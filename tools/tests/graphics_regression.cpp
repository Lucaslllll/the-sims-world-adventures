// Verify logical/physical framebuffer separation and texture updates with a recording GL driver.
#include "hle/gles.cpp"
#include <cassert>
#include <array>
#include <cstdio>

namespace hle {
void register_symbol(const char*, void*) {}
Config& config() { static Config value; return value; }
}
namespace objc {
id retain(id object) { return object; }
void release(id) {}
Class define_class(const char*, const char*, size_t) { return nullptr; }
void add_method(Class, const char*, void*) {}
void add_class_method(Class, const char*, void*) {}
}
namespace ns { id make_constant(const char*) { return nil; } }
namespace foundation {
bool is_main_thread() { return true; }
void run_on_main(std::function<void()> fn, bool) { fn(); }
}
namespace platform {
void gl_make_current(bool) {}
void drawable_size(id, int* w, int* h) { *w = 320; *h = 480; }
void present_frame(unsigned, int, int) {}
}
namespace hd { bool replacement(uint64_t, int, int, int*, std::vector<uint8_t>&) { return false; } }

namespace {
GLint framebuffer = 7, renderbuffer = 1, texture = 99, alignment = 4;
std::array<GLint, 4> viewport{}, scissor{}, texture_upload{}, texture_update{};
std::unordered_map<GLuint, std::array<GLint, 2>> buffers;
int uploads = 0, mip_generations = 0;
void get_integer(GLenum key, GLint* value) {
    if (key == GL_FRAMEBUFFER_BINDING) *value = framebuffer;
    else if (key == GL_RENDERBUFFER_BINDING) *value = renderbuffer;
    else if (key == GL_TEXTURE_BINDING_2D) *value = texture;
    else if (key == GL_MAX_RENDERBUFFER_SIZE || key == GL_MAX_TEXTURE_SIZE) *value = 4096;
    else if (key == GL_UNPACK_ALIGNMENT) *value = alignment;
    else if (key == GL_VIEWPORT) std::copy(viewport.begin(), viewport.end(), value);
    else if (key == GL_SCISSOR_BOX) std::copy(scissor.begin(), scissor.end(), value);
    else assert(false);
}
void set_viewport(GLint x, GLint y, GLsizei w, GLsizei h) { viewport = {x, y, w, h}; }
void set_scissor(GLint x, GLint y, GLsizei w, GLsizei h) { scissor = {x, y, w, h}; }
void bind_frame(GLenum, GLuint id) { framebuffer = id; }
void bind_texture(GLenum, GLuint id) { texture = id; }
void buffer_storage(GLenum, GLenum, GLsizei w, GLsizei h) { buffers[renderbuffer] = {w, h}; }
void get_buffer(GLenum, GLenum key, GLint* value) {
    *value = buffers.at(renderbuffer)[key == GL_RENDERBUFFER_WIDTH ? 0 : 1];
}
void gen_textures(GLsizei count, GLuint* output) { for (int i = 0; i < count; ++i) output[i] = 100 + i; }
void tex_parameter(GLenum, GLenum, GLint) {}
void tex_image(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum, GLenum, const void*) {
    texture_upload = {0, 0, w, h}; ++uploads;
}
void tex_update(GLenum, GLint, GLint x, GLint y, GLsizei w, GLsizei h, GLenum, GLenum, const void*) {
    texture_update = {x, y, w, h};
}
void pixel_store(GLenum, GLint value) { alignment = value; }
void generate(GLenum) { ++mip_generations; }
void attach(GLenum, GLenum, GLenum, GLuint, GLint) {}
void delete_textures(GLsizei, const GLuint*) {}
}

int main() {
    fn.glGetIntegerv = get_integer;
    fn.glViewport = set_viewport; fn.glScissor = set_scissor;
    fn.glBindFramebuffer = fn.glBindFramebufferOES = bind_frame;
    fn.glBindTexture = bind_texture;
    fn.glRenderbufferStorage = fn.glRenderbufferStorageOES = buffer_storage;
    fn.glGetRenderbufferParameteriv = fn.glGetRenderbufferParameterivOES = get_buffer;
    fn.glGenTextures = gen_textures; fn.glTexParameteri = tex_parameter;
    fn.glTexImage2D = tex_image; fn.glTexSubImage2D = tex_update;
    fn.glPixelStorei = pixel_store;
    fn.glFramebufferTexture2DOES = fn.glFramebufferTexture2D = attach;
    fn.glDeleteTextures = delete_textures;
    gl::g_generate_mipmap = generate;
    hle::config().render_scale = 2;
    hle::config().texture_upscale = 2;
    assert(EAGL_renderbufferStorage_fromDrawable(nil, nullptr, GL_RENDERBUFFER, nil));
    assert((buffers[1] == std::array<GLint, 2>{640, 960}));
    GLint width = 0;
    h_glGetRenderbufferParameterivOES(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
    assert(width == 320);
    h_glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 1);
    renderbuffer = 2;
    h_glRenderbufferStorageOES(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 320, 480);
    assert((buffers[2] == std::array<GLint, 2>{640, 960}));
    h_glViewport(0, 0, 320, 480);
    h_glScissor(4, 6, 64, 32);
    assert((viewport == std::array<GLint, 4>{0, 0, 640, 960}));
    assert((scissor == std::array<GLint, 4>{8, 12, 128, 64}));
    GLint query[4]; h_glGetIntegerv(GL_VIEWPORT, query);
    assert(query[2] == 320 && query[3] == 480);
    h_glBindFramebufferOES(GL_FRAMEBUFFER, 9);
    assert((viewport == std::array<GLint, 4>{0, 0, 320, 480}));
    assert((scissor == std::array<GLint, 4>{4, 6, 64, 32}));
    renderbuffer = 3;
    h_glRenderbufferStorageOES(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 96, 96);
    assert((buffers[3] == std::array<GLint, 2>{96, 96}));
    h_glViewport(0, 0, 96, 96);
    h_glBindFramebufferOES(GL_FRAMEBUFFER, 7);
    assert(viewport[2] == 192);

    std::vector<uint8_t> rgba(8 * 8 * 4, 255);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    assert(texture_upload[2] == 16 && texture_upload[3] == 16);
    int previous = uploads;
    h_glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    assert(uploads == previous);  // Original low-resolution mip cannot corrupt HD completeness.
    uint8_t rgb[16] = {255, 0, 0, 0, 255, 0, 99, 99, 0, 0, 255, 255, 255, 255, 99, 99};
    h_glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 2, 2, 2, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    assert((texture_update == std::array<GLint, 4>{2, 4, 4, 4}));
    assert(mip_generations >= 2);
    GLuint id = texture;
    h_glDeleteTextures(1, &id);
    assert(!gl::g_hd_textures.count(id));

    std::vector<uint8_t> decoded;
    uint8_t padded565[12] = {0, 0xf8, 77, 88, 0xe0, 7, 77, 88, 31, 0, 77, 88};
    assert(hd::decode_rgba(1, 3, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 4, padded565, decoded));
    assert(decoded[0] == 255 && decoded[5] == 255 && decoded[10] == 255);
    auto tight_hash = hd::texture_hash(2, 2, GL_RGB, GL_UNSIGNED_BYTE,
                                      std::array<uint8_t, 12>{255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}.data(), 12);
    assert(hd::texture_hash_rows(2, 2, GL_RGB, GL_UNSIGNED_BYTE, rgb, 6, 8) == tight_hash);
    uint8_t associated[8] = {128, 0, 0, 128, 0, 0, 0, 0};
    auto resized = hd::resize_rgba(associated, 2, 1, 2);
    for (size_t p = 0; p < resized.size(); p += 4) assert(resized[p] <= resized[p + 3]);
    auto mip = hd::mip_rgba(associated, 2, 1);
    assert(mip[0] == 64 && mip[3] == 64);
    std::vector<uint8_t> low(16 * 16 * 4, 64), high(32 * 32 * 4, 64), snapshot;
    hd::publish_bitmap(16, 16, 2, low.data(), high.data());
    int snapshot_scale = 0;
    assert(hd::bitmap_replacement(hd::texture_hash(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, low.data(), low.size()),
                                  16, 16, &snapshot_scale, snapshot));
    assert(snapshot_scale == 2 && snapshot == high);
    low[0] = 255;
    assert(!hd::bitmap_replacement(hd::texture_hash(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, low.data(), low.size()),
                                    16, 16, &snapshot_scale, snapshot));
    // The actual game allocates a blank POT texture and uses subimages for text.
    texture = 150;
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    assert(!gl::g_hd_textures.count(texture));
    low[0] = 64;
    h_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, low.data());
    assert(gl::g_hd_textures.at(texture).scale == 2);
    assert(texture_upload[2] == 32 && texture_upload[3] == 32);
    // Nonmatching updates and partial uploads cannot resize or erase the texture.
    texture = 151;
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    previous = uploads;
    h_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, low.data());
    assert(uploads == previous && !gl::g_hd_textures.count(texture));
    id = texture;
    h_glDeleteTextures(1, &id);
    assert(!gl::g_texture_sizes.count(texture));
    // A cached 4x bitmap must still obey a GPU that can only fit the 2x upload.
    low.assign(256 * 64 * 4, 128); high.assign(1024 * 256 * 4, 128);
    hd::publish_bitmap(256, 64, 4, low.data(), high.data());
    gl::g_max_texture_size = 512; texture = 152;
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, low.data());
    assert(texture_upload[2] == 512 && texture_upload[3] == 128);
    gl::g_max_texture_size = 4096;
    std::puts("Graphics regression OK: virtual UI, physical depth/color, FBO switches, HD mips, subupdates, alpha, packed pixels");
}
