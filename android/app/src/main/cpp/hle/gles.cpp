// OpenGL ES exports + EAGLContext.
//
// The game renders into its own framebuffer object whose colour renderbuffer
// comes from -[EAGLContext renderbufferStorage:fromDrawable:]. We swap that
// colour attachment for a texture ("drawable texture") so the platform layer
// can draw it rotated/scaled to the Android window on -presentRenderbuffer:.
#include "hle/gles.h"

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_RENDERBUFFER_BINDING
#define GL_RENDERBUFFER_BINDING 0x8CA7
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "hle/config.h"
#include "hle/foundation.h"
#include "hle/nsstring.h"
#include "hle/pvrtc.h"
#include "hle/hd_textures.h"
#include "hle/bitmap_hd.h"
#include "hle/texture_pixels.h"
#include "objc/runtime.h"
#include "platform/platform.h"

#ifdef TS3WA_GL_TRACE
#define GLTRACE(name) LOGD("gl: %s", name)
#else
#define GLTRACE(name) do {} while (0)
#endif

namespace gl {

Fns fn;
bool g_es1 = true;

static void* g_lib_v1 = nullptr;
static void* g_lib_v3 = nullptr;
static void (*g_generate_mipmap)(GLenum) = nullptr;
static GLint g_max_texture_size = 4096;

static void* lookup(const char* name) {
    void* p = nullptr;
    if (g_es1 && g_lib_v1) p = dlsym(g_lib_v1, name);
    if (!p && g_lib_v3) p = dlsym(g_lib_v3, name);
    if (!p && g_lib_v1) p = dlsym(g_lib_v1, name);
    return p;
}

static void missing_fn() { LOGW("call to GL function not available in this context"); }

bool load(bool es1) {
    g_es1 = es1;
    if (!g_lib_v1) g_lib_v1 = dlopen("libGLESv1_CM.so", RTLD_NOW);
    if (!g_lib_v3) g_lib_v3 = dlopen("libGLESv3.so", RTLD_NOW);
    if (!g_lib_v3) g_lib_v3 = dlopen("libGLESv2.so", RTLD_NOW);
    int missing = 0;
#define X(name, ret, params)                          \
    fn.name = (ret(*) params)lookup(#name);           \
    if (!fn.name) {                                   \
        fn.name = (ret(*) params)(void*)missing_fn;   \
        missing++;                                    \
        LOGW("GL function %s not found", #name);      \
    }
    GL_FUNCS(X)
#undef X
    g_generate_mipmap = (void(*)(GLenum))lookup(es1 ? "glGenerateMipmapOES" : "glGenerateMipmap");
    fn.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_max_texture_size);
    LOGI("GL functions loaded for %s (%d missing)", es1 ? "ES1.1" : "ES2/3", missing);
    return true;
}

// --- drawable state --------------------------------------------------------------
static GLuint g_drawable_rb = 0;   // renderbuffer the game thinks is the screen
static GLuint g_drawable_tex = 0;  // texture we attach instead
static GLuint g_game_fbo = 0;
static int g_drawable_w = 0, g_drawable_h = 0;
static int g_logical_w = 0, g_logical_h = 0, g_render_scale = 1;
struct BufferSize { int w, h, scale; };
static std::unordered_map<GLuint, BufferSize> g_buffer_sizes;
static std::unordered_map<GLuint, BufferSize> g_hd_textures;
static std::unordered_map<GLuint, BufferSize> g_texture_sizes;
static GLint g_viewport[4]{}, g_scissor[4]{};
static bool g_have_viewport = false, g_have_scissor = false;

GLuint drawable_texture() { return g_drawable_tex; }
int drawable_width() { return g_drawable_w; }
int drawable_height() { return g_drawable_h; }

}  // namespace gl

using gl::fn;

// Keep game-visible points/texel coordinates unchanged while increasing physical pixels.
static int framebuffer_scale() {
    GLint framebuffer = 0;
    fn.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    return gl::g_game_fbo && framebuffer == (GLint)gl::g_game_fbo ? gl::g_render_scale : 1;
}
static void apply_viewport() {
    if (!gl::g_have_viewport) return;
    int scale = framebuffer_scale();
    auto* rect = gl::g_viewport;
    fn.glViewport(rect[0] * scale, rect[1] * scale, rect[2] * scale, rect[3] * scale);
}
static void apply_scissor() {
    if (!gl::g_have_scissor) return;
    int scale = framebuffer_scale();
    auto* rect = gl::g_scissor;
    fn.glScissor(rect[0] * scale, rect[1] * scale, rect[2] * scale, rect[3] * scale);
}
static void h_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    gl::g_viewport[0] = x; gl::g_viewport[1] = y; gl::g_viewport[2] = w; gl::g_viewport[3] = h;
    gl::g_have_viewport = true;
    apply_viewport();
}
static void h_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    gl::g_scissor[0] = x; gl::g_scissor[1] = y; gl::g_scissor[2] = w; gl::g_scissor[3] = h;
    gl::g_have_scissor = true;
    apply_scissor();
}
static void bind_framebuffer(bool oes, GLenum target, GLuint framebuffer) {
    if (oes) fn.glBindFramebufferOES(target, framebuffer);
    else fn.glBindFramebuffer(target, framebuffer);
    apply_viewport();
    apply_scissor();
}
static void h_glBindFramebuffer(GLenum target, GLuint framebuffer) { bind_framebuffer(false, target, framebuffer); }
static void h_glBindFramebufferOES(GLenum target, GLuint framebuffer) { bind_framebuffer(true, target, framebuffer); }
static void h_glGetIntegerv(GLenum pname, GLint* values) {
    fn.glGetIntegerv(pname, values);
    if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) {
        int scale = framebuffer_scale();
        for (int i = 0; i < 4; ++i) values[i] /= scale;
    }
}
static void h_glGetFloatv(GLenum pname, GLfloat* values) {
    fn.glGetFloatv(pname, values);
    if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) {
        int scale = framebuffer_scale();
        for (int i = 0; i < 4; ++i) values[i] /= scale;
    }
}
static void renderbuffer_storage(bool oes, GLenum target, GLenum format, GLsizei w, GLsizei h) {
    GLint rb = 0;
    fn.glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    int scale = (w == gl::g_logical_w && h == gl::g_logical_h) ? framebuffer_scale() : 1;
    if (scale > 1) gl::g_buffer_sizes[rb] = {w, h, scale};
    else gl::g_buffer_sizes.erase(rb);
    if (oes) fn.glRenderbufferStorageOES(target, format, w * scale, h * scale);
    else fn.glRenderbufferStorage(target, format, w * scale, h * scale);
}
static void h_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { renderbuffer_storage(false, t, f, w, h); }
static void h_glRenderbufferStorageOES(GLenum t, GLenum f, GLsizei w, GLsizei h) { renderbuffer_storage(true, t, f, w, h); }
static void renderbuffer_parameter(bool oes, GLenum target, GLenum pname, GLint* value) {
    if (oes) fn.glGetRenderbufferParameterivOES(target, pname, value);
    else fn.glGetRenderbufferParameteriv(target, pname, value);
    GLint rb = 0;
    fn.glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    auto size = gl::g_buffer_sizes.find(rb);
    if (size != gl::g_buffer_sizes.end()) {
        if (pname == GL_RENDERBUFFER_WIDTH) *value = size->second.w;
        if (pname == GL_RENDERBUFFER_HEIGHT) *value = size->second.h;
    }
}
static void h_glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint* v) { renderbuffer_parameter(false, t, p, v); }
static void h_glGetRenderbufferParameterivOES(GLenum t, GLenum p, GLint* v) { renderbuffer_parameter(true, t, p, v); }
static void delete_renderbuffers(bool oes, GLsizei n, const GLuint* ids) {
    for (int i = 0; i < n; ++i) gl::g_buffer_sizes.erase(ids[i]);
    if (oes) fn.glDeleteRenderbuffersOES(n, ids); else fn.glDeleteRenderbuffers(n, ids);
}
static void h_glDeleteRenderbuffers(GLsizei n, const GLuint* ids) { delete_renderbuffers(false, n, ids); }
static void h_glDeleteRenderbuffersOES(GLsizei n, const GLuint* ids) { delete_renderbuffers(true, n, ids); }

static GLuint bound_texture() {
    GLint texture = 0;
    fn.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    return texture;
}
static bool hd_texture_bound(GLenum target, GLint level) {
    return target == GL_TEXTURE_2D && level > 0 && gl::g_hd_textures.count(bound_texture());
}
static void generate_mips(GLenum target, const uint8_t* rgba, int w, int h) {
    if ((w & (w - 1)) || (h & (h - 1))) return;
    if (gl::g_generate_mipmap) { gl::g_generate_mipmap(target); return; }
    std::vector<uint8_t> pixels;
    for (int level = 1; w > 1 || h > 1; ++level) {
        pixels = hd::mip_rgba(level == 1 ? rgba : pixels.data(), w, h);
        w = std::max(1, w / 2); h = std::max(1, h / 2);
        fn.glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
}
static bool upload_hd(GLenum target, GLint level, GLint border, int w, int h, uint64_t hash,
                       const std::vector<uint8_t>& original) {
    int wanted = hle::config().texture_upscale;
    if (target != GL_TEXTURE_2D || level != 0 || border || wanted <= 1 || w <= 0 || h <= 0) return false;
    int scale = std::min({wanted, gl::g_max_texture_size / w, gl::g_max_texture_size / h});
    if (scale <= 1) return false;
    std::vector<uint8_t> pixels;
    bool native_bitmap = hd::bitmap_replacement(hash, w, h, &scale, pixels);
    if (!native_bitmap && !original.empty())
        native_bitmap = hd::bitmap_replacement(hd::texture_hash(w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                                                               original.data(), original.size()), w, h, &scale, pixels);
    bool ai = !native_bitmap && hd::replacement(hash, w, h, &scale, pixels);
    if ((native_bitmap || ai) && (w * scale > gl::g_max_texture_size || h * scale > gl::g_max_texture_size)) {
        native_bitmap = ai = false;
        scale = std::min({wanted, gl::g_max_texture_size / w, gl::g_max_texture_size / h});
        pixels.clear();
    }
    if (!ai && !native_bitmap) {
        if (std::min(w, h) < 8 || original.empty()) return false;
        pixels = hd::resize_rgba(original.data(), w, h, scale);
    }
    GLint alignment = 4;
    fn.glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    fn.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    fn.glTexImage2D(target, 0, GL_RGBA, w * scale, h * scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    generate_mips(target, pixels.data(), w * scale, h * scale);
    fn.glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    GLuint texture = bound_texture();
    gl::g_hd_textures[texture] = {w, h, scale};
    static int ai_logs = 0, fallback_logs = 0, glyph_logs = 0;
    if ((native_bitmap && glyph_logs++ < 20) || (ai && ai_logs++ < 30) || (!ai && !native_bitmap && fallback_logs++ < 8))
        LOGI("HD %s texture %u key %016llx: %dx%d -> %dx%d", native_bitmap ? "glyph" : ai ? "AI" : "filtered", texture,
             (unsigned long long)hash, w, h, w * scale, h * scale);
    return true;
}
static void h_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                           GLint border, GLenum format, GLenum type, const void* data) {
    if (hd_texture_bound(target, level)) return;  // The HD base has its own complete mip chain.
    if (target == GL_TEXTURE_2D && level == 0) {
        gl::g_hd_textures.erase(bound_texture());
        if (!border && w > 0 && h > 0) gl::g_texture_sizes[bound_texture()] = {w, h, 1};
        else gl::g_texture_sizes.erase(bound_texture());
        int bpp = hd::bytes_per_pixel(format, type);
        if (data && bpp && w > 0 && h > 0 && w <= 2048 && h <= 2048 && hle::config().texture_upscale > 1) {
            GLint align = 4;
            fn.glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
            size_t row = size_t(w) * bpp, stride = (row + align - 1) & ~(size_t(align) - 1);
            uint64_t hash = hd::texture_hash_rows(w, h, format, type, data, row, stride);
            std::vector<uint8_t> rgba;
            if (hd::decode_rgba(w, h, format, type, align, data, rgba) && upload_hd(target, level, border, w, h, hash, rgba)) return;
        }
    }
    fn.glTexImage2D(target, level, internal, w, h, border, format, type, data);
}
static void h_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                               GLenum format, GLenum type, const void* data) {
    // ImageIPhone allocates empty storage, draws text, then sends a full subimage.
    // Promote only that complete bitmap; partial uploads must preserve existing pixels.
    GLuint texture = bound_texture();
    auto base = gl::g_texture_sizes.find(texture);
    if (target == GL_TEXTURE_2D && level == 0 && x == 0 && y == 0 && data &&
        base != gl::g_texture_sizes.end() && base->second.w == w && base->second.h == h &&
        hle::config().texture_upscale > 1) {
        GLint alignment = 4;
        fn.glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        std::vector<uint8_t> rgba, physical;
        int scale = 1;
        if (hd::decode_rgba(w, h, format, type, alignment, data, rgba)) {
            uint64_t hash = hd::texture_hash(w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data(), rgba.size());
            if (hd::bitmap_replacement(hash, w, h, &scale, physical) &&
                w * scale <= gl::g_max_texture_size && h * scale <= gl::g_max_texture_size &&
                upload_hd(target, 0, 0, w, h, hash, rgba)) return;
        }
    }
    auto found = gl::g_hd_textures.find(bound_texture());
    if (target == GL_TEXTURE_2D && found != gl::g_hd_textures.end()) {
        if (level > 0) return;
        GLint align = 4;
        fn.glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        std::vector<uint8_t> rgba;
        if (hd::decode_rgba(w, h, format, type, align, data, rgba)) {
            int scale = found->second.scale;
            std::vector<uint8_t> enlarged;
            int native_scale = 1;
            bool native = hd::bitmap_replacement(hd::texture_hash(w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                                                                 rgba.data(), rgba.size()), w, h, &native_scale, enlarged);
            if (!native || native_scale != scale) enlarged = hd::resize_rgba(rgba.data(), w, h, scale);
            fn.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            fn.glTexSubImage2D(target, level, x * scale, y * scale, w * scale, h * scale, GL_RGBA, GL_UNSIGNED_BYTE, enlarged.data());
            if (gl::g_generate_mipmap) gl::g_generate_mipmap(target);
            fn.glPixelStorei(GL_UNPACK_ALIGNMENT, align);
            return;
        }
        LOGW("HD texture update has unsupported pixel format %#x/%#x", format, type);
    }
    fn.glTexSubImage2D(target, level, x, y, w, h, format, type, data);
}
static void h_glDeleteTextures(GLsizei n, const GLuint* textures) {
    for (int i = 0; i < n; ++i) { gl::g_hd_textures.erase(textures[i]); gl::g_texture_sizes.erase(textures[i]); }
    fn.glDeleteTextures(n, textures);
}

// --- intercepted functions ----------------------------------------------------------
static GLenum h_glGetError() {
    GLenum e = fn.glGetError();
    if (e) LOGW("glGetError -> %#x", e);
    return e;
}

static bool is_pvrtc(GLenum f, bool* two) {
    switch (f) {
        case 0x8C00: case 0x8C02: *two = false; return true;  // 4bpp
        case 0x8C01: case 0x8C03: *two = true; return true;   // 2bpp
    }
    return false;
}

static void h_glCompressedTexImage2D(GLenum target, GLint level, GLenum fmt, GLsizei w, GLsizei h, GLint border,
                                     GLsizei size, const void* data) {
    if (hd_texture_bound(target, level)) return;
    if (target == GL_TEXTURE_2D && level == 0) {
        gl::g_hd_textures.erase(bound_texture());
        gl::g_texture_sizes.erase(bound_texture());
    }
    bool two;
    if (is_pvrtc(fmt, &two)) {
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        if (data && w > 0 && h > 0) pvrtc_decode((const uint8_t*)data, w, h, two, rgba.data());
        if (fmt == 0x8c00 || fmt == 0x8c01) for (size_t p = 3; p < rgba.size(); p += 4) rgba[p] = 255;
        size_t base_size = size_t(std::max(w / (two ? 8 : 4), 2)) * std::max(h / 4, 2) * 8;
        if (data && size >= (GLsizei)base_size && upload_hd(target, level, border, w, h,
                                                          hd::texture_hash(w, h, fmt, 0, data, base_size), rgba)) return;
        GLint align;
        fn.glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        fn.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        fn.glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, data ? rgba.data() : nullptr);
        fn.glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        return;
    }
    fn.glCompressedTexImage2D(target, level, fmt, w, h, border, size, data);
}

static void h_glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                        GLenum fmt, GLsizei size, const void* data) {
    bool two;
    if (is_pvrtc(fmt, &two)) {
        if (hd_texture_bound(target, level)) return;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        pvrtc_decode((const uint8_t*)data, w, h, two, rgba.data());
        GLint align = 4;
        fn.glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        fn.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        h_glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        fn.glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        return;
    }
    fn.glCompressedTexSubImage2D(target, level, x, y, w, h, fmt, size, data);
}

static void framebuffer_renderbuffer(bool oes, GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    if (rb && rb == gl::g_drawable_rb && attachment == GL_COLOR_ATTACHMENT0) {
        GLint cur = 0;
        fn.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
        gl::g_game_fbo = cur;
        apply_viewport();
        apply_scissor();
        LOGI("attaching drawable texture %u to game FBO %d", gl::g_drawable_tex, cur);
        if (oes) fn.glFramebufferTexture2DOES(target, attachment, GL_TEXTURE_2D, gl::g_drawable_tex, 0);
        else fn.glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, gl::g_drawable_tex, 0);
        return;
    }
    if (oes) fn.glFramebufferRenderbufferOES(target, attachment, rbtarget, rb);
    else fn.glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
}
static void h_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb) {
    framebuffer_renderbuffer(false, t, a, rt, rb);
}
static void h_glFramebufferRenderbufferOES(GLenum t, GLenum a, GLenum rt, GLuint rb) {
    framebuffer_renderbuffer(true, t, a, rt, rb);
}

// The game's ES2 skinning shader uses HLSL-style int4/float4 attributes, which
// GLSL ES rejects; rewrite them into valid GLSL.
static void replace_all(std::string& s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
}
static void h_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    std::string src;
    for (GLsizei i = 0; i < count; i++) src.append(strings[i], lengths && lengths[i] >= 0 ? lengths[i] : strlen(strings[i]));
    if (src.find("int4") != std::string::npos || src.find("float4") != std::string::npos) {
        replace_all(src, "highp int4 vs_SkinIndices", "highp vec4 vs_SkinIndices");
        replace_all(src, "float4", "vec4");
        for (const char* c : {"x", "y", "z", "w"}) {
            replace_all(src, std::string("g_SkinPalette[vs_SkinIndices.") + c + "]",
                        std::string("g_SkinPalette[int(vs_SkinIndices.") + c + ")]");
        }
    }
    const GLchar* p = src.c_str();
    fn.glShaderSource(shader, 1, &p, nullptr);
}

// Pass-through wrappers and the HLE_EXPORT table.
#include "hle/gl_exports.inc"

// =====================================================================================
// EAGLContext
// =====================================================================================
namespace {

struct EAGLContextObj {
    Class isa;
    int api;
    id sharegroup;
};

// EAGL keeps a current context per thread; EGL lets a context be current on
// only one thread at a time, so ownership of the single game context moves to
// whichever thread asks for it (the main thread is asked to let go if needed).
thread_local id t_current_context = nil;
std::mutex g_owner_mutex;
std::condition_variable g_owner_cv;
bool g_owned = true;  // platform makes the context current on the main thread at start
pthread_t g_owner = 0;
bool g_owner_init = false;

void acquire_context() {
    pthread_t self = pthread_self();
    std::unique_lock<std::mutex> lk(g_owner_mutex);
    if (!g_owner_init) {
        g_owner_init = true;
        g_owner = self;  // first caller is the thread that created the context
    }
    if (g_owned && pthread_equal(g_owner, self)) return;
    if (g_owned) {
        pthread_t owner = g_owner;
        lk.unlock();
        LOGI("GL context requested by thread %ld; asking owner to release", (long)gettid());
        if (foundation::is_main_thread()) {
            LOGE("main thread wants the GL context but a game thread holds it");
        } else {
            foundation::run_on_main(
                [owner]() {
                    std::lock_guard<std::mutex> g(g_owner_mutex);
                    if (g_owned && pthread_equal(g_owner, pthread_self())) {
                        platform::gl_make_current(false);
                        g_owned = false;
                        LOGI("main thread released the GL context");
                    }
                },
                true);
        }
        lk.lock();
        g_owner_cv.wait_for(lk, std::chrono::seconds(5), [] { return !g_owned; });
        if (g_owned) LOGE("timed out waiting for the GL context");
    }
    platform::gl_make_current(true);
    g_owned = true;
    g_owner = self;
}

void release_context() {
    std::lock_guard<std::mutex> lk(g_owner_mutex);
    if (g_owned && pthread_equal(g_owner, pthread_self())) {
        platform::gl_make_current(false);
        g_owned = false;
        g_owner_cv.notify_all();
    }
}

id EAGL_initWithAPI(id self, SEL, int api) {
    bool want_es1 = hle::config().gl_api == "OpenGLES1";
    if ((api == 1) != want_es1) {
        LOGW("EAGLContext initWithAPI:%d refused (platform context is %s)", api, want_es1 ? "ES1" : "ES2");
        objc::release(self);
        return nil;
    }
    ((EAGLContextObj*)self)->api = api;
    LOGI("EAGLContext created with API %d", api);
    return self;
}
id EAGL_initWithAPI_sharegroup(id self, SEL s, int api, id) { return EAGL_initWithAPI(self, s, api); }
int EAGL_API(id self, SEL) { return ((EAGLContextObj*)self)->api; }
id EAGL_sharegroup(id, SEL) { return nil; }
BOOL EAGL_setCurrentContext(Class, SEL, id ctx) {
    LOGI("setCurrentContext:%p on thread %ld", ctx, (long)gettid());
    if (ctx) acquire_context();
    else release_context();
    if (ctx != t_current_context) {
        objc::retain(ctx);
        objc::release(t_current_context);
        t_current_context = ctx;
    }
    return YES;
}
id EAGL_currentContext(Class, SEL) { return t_current_context; }

BOOL EAGL_renderbufferStorage_fromDrawable(id, SEL, GLenum target, id layer) {
    int w = 0, h = 0;
    platform::drawable_size(layer, &w, &h);
    gl::g_logical_w = w;
    gl::g_logical_h = h;
    GLint max_buffer = 4096;
    fn.glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_buffer);
    gl::g_render_scale = std::max(1, std::min({hle::config().render_scale, max_buffer / std::max(w, 1),
                                            max_buffer / std::max(h, 1), gl::g_max_texture_size / std::max(w, 1),
                                            gl::g_max_texture_size / std::max(h, 1)}));
    GLint rb = 0;
    fn.glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    bool es1 = gl::g_es1;
    gl::g_buffer_sizes[rb] = {w, h, gl::g_render_scale};
    LOGI("HD framebuffer rb=%d logical %dx%d -> physical %dx%d", rb, w, h, w * gl::g_render_scale, h * gl::g_render_scale);
    w *= gl::g_render_scale;
    h *= gl::g_render_scale;
    if (es1) fn.glRenderbufferStorageOES(target, GL_RGBA8_OES, w, h);
    else fn.glRenderbufferStorage(target, GL_RGBA8, w, h);
    gl::g_drawable_rb = rb;
    gl::g_drawable_w = w;
    gl::g_drawable_h = h;
    // Texture we render into instead of the renderbuffer.
    GLint prev_tex = 0;
    fn.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    if (!gl::g_drawable_tex) fn.glGenTextures(1, &gl::g_drawable_tex);
    fn.glBindTexture(GL_TEXTURE_2D, gl::g_drawable_tex);
    fn.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    fn.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    fn.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    fn.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    fn.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    fn.glBindTexture(GL_TEXTURE_2D, prev_tex);
    return YES;
}

BOOL EAGL_presentRenderbuffer(id, SEL, GLenum) {
    platform::present_frame(gl::g_drawable_tex, gl::g_drawable_w, gl::g_drawable_h);
    return YES;
}

}  // namespace

namespace gl {

static id g_kEAGLColorFormatRGB565 = nil, g_kEAGLColorFormatRGBA8 = nil,
          g_kEAGLDrawablePropertyColorFormat = nil, g_kEAGLDrawablePropertyRetainedBacking = nil;

void init_classes() {
    g_kEAGLColorFormatRGB565 = ns::make_constant("EAGLColorFormatRGB565");
    g_kEAGLColorFormatRGBA8 = ns::make_constant("EAGLColorFormatRGBA8");
    g_kEAGLDrawablePropertyColorFormat = ns::make_constant("EAGLDrawablePropertyColorFormat");
    g_kEAGLDrawablePropertyRetainedBacking = ns::make_constant("EAGLDrawablePropertyRetainedBacking");
    hle::register_symbol("_kEAGLColorFormatRGB565", &g_kEAGLColorFormatRGB565);
    hle::register_symbol("_kEAGLColorFormatRGBA8", &g_kEAGLColorFormatRGBA8);
    hle::register_symbol("_kEAGLDrawablePropertyColorFormat", &g_kEAGLDrawablePropertyColorFormat);
    hle::register_symbol("_kEAGLDrawablePropertyRetainedBacking", &g_kEAGLDrawablePropertyRetainedBacking);

    Class C = objc::define_class("EAGLContext", "NSObject", sizeof(EAGLContextObj));
    objc::add_method(C, "initWithAPI:", (void*)EAGL_initWithAPI);
    objc::add_method(C, "initWithAPI:sharegroup:", (void*)EAGL_initWithAPI_sharegroup);
    objc::add_method(C, "API", (void*)EAGL_API);
    objc::add_method(C, "sharegroup", (void*)EAGL_sharegroup);
    objc::add_method(C, "renderbufferStorage:fromDrawable:", (void*)EAGL_renderbufferStorage_fromDrawable);
    objc::add_method(C, "presentRenderbuffer:", (void*)EAGL_presentRenderbuffer);
    objc::add_class_method(C, "setCurrentContext:", (void*)EAGL_setCurrentContext);
    objc::add_class_method(C, "currentContext", (void*)EAGL_currentContext);
}

}  // namespace gl
