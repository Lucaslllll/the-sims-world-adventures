// Android entry point: sets up SDL/EGL, loads the iOS executable and runs it.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <SDL.h>
#include <jni.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cmath>
#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "hle/cg.h"
#include "hle/config.h"
#include "hle/foundation.h"
#include "hle/audio_mixer.h"
#include "hle/gles.h"
#include "hle/media.h"
#include "hle/nsstring.h"
#include "hle/uikit.h"
#include "loader/macho.h"
#include "objc/runtime.h"
#include "platform/platform.h"
#include "platform/viewport.h"

using hle::config;

void openal_pause(bool paused);  // hle/openal.cpp

static void call_activity_static(const char* name, const char* sig, ...);

namespace {

SDL_Window* g_window = nullptr;
SDL_GLContext g_game_ctx = nullptr;
SDL_GLContext g_present_ctx = nullptr;
bool g_running = true;
bool g_crash_test = false;  // config.txt crash_test=1: validate the crash reporter
bool g_paused = false;
int g_win_w = 0, g_win_h = 0;
GLuint g_present_prog = 0;  // ES2 path only
uint64_t g_frames = 0;
double g_fps_t0 = 0;

platform::Viewport g_vp;
std::mutex g_viewport_mutex;
std::atomic<int> g_screen_mode{0};
std::atomic<bool> g_settings_ready{false};

void compute_viewport() {
    std::lock_guard<std::mutex> lock(g_viewport_mutex);
    g_vp = platform::make_viewport(g_win_w, g_win_h, config().screen_w, config().screen_h, config().screen_mode);
}

std::map<std::string, std::string> read_kv(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
            size_t i = 0;
            while (i < s.size() && isspace((unsigned char)s[i])) i++;
            return s.substr(i);
        };
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return kv;
}

void setup_config() {
    auto& c = config();
    const char* ext = SDL_AndroidGetExternalStoragePath();
    c.data_root = ext ? ext : "/sdcard";
    c.bundle_path = c.data_root + "/sims3wa_iphone.app";
    c.home_path = c.data_root + "/home";
    if (SDL_Locale* loc = SDL_GetPreferredLocales()) {
        if (loc[0].language) c.language = loc[0].language;
        if (loc[0].country) c.region = loc[0].country;
        SDL_free(loc);
    }
    auto kv = read_kv(c.data_root + "/config.txt");
    if (kv.count("language")) c.language = kv["language"];
    if (c.language != "en" && c.language != "fr" && c.language != "it" &&
        c.language != "de" && c.language != "es" && c.language != "ja") c.language = "en";
    if (kv.count("gl_api")) c.gl_api = kv["gl_api"];
    if (kv.count("device_model")) c.device_model = kv["device_model"];
    if (kv.count("system_version")) c.system_version = kv["system_version"];
    if (kv.count("render_scale")) c.render_scale = std::clamp(atoi(kv["render_scale"].c_str()), 1, 4);
    if (kv.count("texture_upscale")) c.texture_upscale = std::clamp(atoi(kv["texture_upscale"].c_str()), 1, 4);
    c.trace_animation = kv.count("trace_animation") && kv["trace_animation"] == "1";
    if (kv.count("text_scale")) c.text_scale = std::clamp(atoi(kv["text_scale"].c_str()), 1, 4);
    c.screen_mode = kv["screen_mode"] == "stretch" ? hle::ScreenMode::Stretch : hle::ScreenMode::Fit;
    g_screen_mode = c.screen_mode == hle::ScreenMode::Stretch ? 1 : 0;
    if (kv.count("short_side")) c.screen_w = atoi(kv["short_side"].c_str());
    else c.screen_w = 0;
    if (kv.count("long_side")) c.screen_h = atoi(kv["long_side"].c_str());
    else c.screen_h = 0;
    g_crash_test = kv.count("crash_test") && kv["crash_test"] == "1";
    LOGI("data root %s, language %s_%s, GL %s", c.data_root.c_str(), c.language.c_str(), c.region.c_str(),
         c.gl_api.c_str());
}

void choose_screen_size() {
    auto& c = config();
    // The window may not be immersive yet; size the game for the physical display.
    SDL_DisplayMode dm;
    int lw = std::max(g_win_w, g_win_h), sh = std::min(g_win_w, g_win_h);
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
        lw = std::max(dm.w, dm.h);
        sh = std::min(dm.w, dm.h);
    }
    // Preserve the original UI points; render_scale increases the physical framebuffer.
    if (c.screen_w <= 0) c.screen_w = 320;
    if (c.screen_h <= 0) c.screen_h = c.screen_w * 3 / 2;
    c.window_w = g_win_w;
    c.window_h = g_win_h;
    LOGI("display %dx%d, window %dx%d -> iOS screen %dx%d points (portrait)",
         lw, sh, g_win_w, g_win_h, c.screen_w, c.screen_h);
    compute_viewport();
    LOGI("screen mode: %s", c.screen_mode == hle::ScreenMode::Stretch ? "stretch" : "fit");
}

void set_screen_mode(int mode) {
    if (mode != 0 && mode != 1) return;
    config().screen_mode = mode == 1 ? hle::ScreenMode::Stretch : hle::ScreenMode::Fit;
    compute_viewport();
    g_screen_mode = mode;
    const char* value = mode == 1 ? "stretch" : "fit";
    // Preserve comments and all unrelated configuration lines.
    std::string path = config().data_root + "/config.txt", temporary = path + ".screen-tmp";
    std::ifstream input(path);
    std::ofstream output(temporary);
    std::string line;
    bool written = false;
    while (std::getline(input, line)) {
        auto equal = line.find('=');
        std::string key = line.substr(0, equal);
        auto first = key.find_first_not_of(" \t\r"), last = key.find_last_not_of(" \t\r");
        if (equal != std::string::npos && first != std::string::npos && key.substr(first, last - first + 1) == "screen_mode") {
            if (!written) output << "screen_mode=" << value << '\n';
            written = true;
        } else output << line << '\n';
    }
    if (!written) output << "screen_mode=" << value << '\n';
    output.close();
    if (!output || rename(temporary.c_str(), path.c_str()) != 0) {
        LOGW("could not persist screen mode");
        unlink(temporary.c_str());
    }
    LOGI("screen mode changed: %s", value);
}

void init_sync();

bool init_video() {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        LOGE("SDL_Init: %s", SDL_GetError());
        return false;
    }
    bool es1 = config().gl_api == "OpenGLES1";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, es1 ? 1 : 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, es1 ? 1 : 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("TS3 World Adventures", 0, 0, 0, 0,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) {
        LOGE("SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    g_game_ctx = SDL_GL_CreateContext(g_window);
    if (!g_game_ctx) {
        LOGE("SDL_GL_CreateContext: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    g_present_ctx = SDL_GL_CreateContext(g_window);
    if (!g_present_ctx) LOGE("present context: %s", SDL_GetError());
    else LOGI("present context created (shared)");
    SDL_GL_MakeCurrent(g_window, g_game_ctx);
    SDL_GL_SetSwapInterval(1);
    SDL_GL_GetDrawableSize(g_window, &g_win_w, &g_win_h);
    if (g_win_w < g_win_h) std::swap(g_win_w, g_win_h);
    gl::load(es1);
    init_sync();
    LOGI("GL_VENDOR=%s GL_RENDERER=%s GL_VERSION=%s", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
         glGetString(GL_VERSION));
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    LOGI("GL_EXTENSIONS=%s", ext ? ext : "");
    return true;
}

// --- cross-context synchronisation ------------------------------------------------
// The present context samples the texture the game just rendered in another
// context. Mali needs explicit sync for that; prefer a GPU-side fence wait.
PFNEGLCREATESYNCKHRPROC p_eglCreateSyncKHR = nullptr;
PFNEGLDESTROYSYNCKHRPROC p_eglDestroySyncKHR = nullptr;
PFNEGLWAITSYNCKHRPROC p_eglWaitSyncKHR = nullptr;
bool g_use_fence = false;

void init_sync() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    const char* ext = eglQueryString(dpy, EGL_EXTENSIONS);
    if (ext && strstr(ext, "EGL_KHR_fence_sync") && strstr(ext, "EGL_KHR_wait_sync")) {
        p_eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        p_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        p_eglWaitSyncKHR = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
        g_use_fence = p_eglCreateSyncKHR && p_eglDestroySyncKHR && p_eglWaitSyncKHR;
    }
    if (std::getenv("TS3WA_NO_FENCE")) g_use_fence = false;
    LOGI("present sync: %s", g_use_fence ? "EGL fence + server wait" : "glFinish");
}

// --- present pass ---------------------------------------------------------------
const char* kVS =
    "attribute vec2 p; attribute vec2 t; varying vec2 v;"
    "void main(){ v = t; gl_Position = vec4(p, 0.0, 1.0); }";
const char* kFS =
    "precision mediump float; varying vec2 v; uniform sampler2D s;"
    "void main(){ gl_FragColor = vec4(texture2D(s, v).rgb, 1.0); }";

GLuint compile(GLenum type, const char* src) {
    GLuint s = gl::fn.glCreateShader(type);
    gl::fn.glShaderSource(s, 1, &src, nullptr);
    gl::fn.glCompileShader(s);
    return s;
}

void draw_present_quad(unsigned tex) {
    // NDC corners -> texture coords. The drawable is portrait with the game's
    // landscape image rotated; see docs/02-ARQUITETURA.md for the derivation.
    static const float pos[] = {-1, 1, 1, 1, -1, -1, 1, -1};
    static const float uv[] = {1, 1, 1, 0, 0, 1, 0, 0};
    platform::Viewport viewport;
    { std::lock_guard<std::mutex> lock(g_viewport_mutex); viewport = g_vp; }
    auto& f = gl::fn;
    f.glViewport(0, 0, viewport.window_w, viewport.window_h);
    f.glDisable(GL_SCISSOR_TEST);
    f.glClearColor(0, 0, 0, 1);
    f.glClear(GL_COLOR_BUFFER_BIT);
    f.glViewport((GLint)viewport.x, (GLint)viewport.y, (GLsizei)viewport.w, (GLsizei)viewport.h);
    f.glActiveTexture(GL_TEXTURE0);
    f.glBindTexture(GL_TEXTURE_2D, tex);
    if (gl::g_es1) {
        f.glDisable(GL_BLEND);
        f.glDisable(GL_DEPTH_TEST);
        f.glDisable(GL_LIGHTING);
        f.glEnable(GL_TEXTURE_2D);
        f.glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        f.glMatrixMode(GL_PROJECTION);
        f.glLoadIdentity();
        f.glMatrixMode(GL_MODELVIEW);
        f.glLoadIdentity();
        f.glEnableClientState(GL_VERTEX_ARRAY);
        f.glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        f.glVertexPointer(2, GL_FLOAT, 0, pos);
        f.glTexCoordPointer(2, GL_FLOAT, 0, uv);
        f.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        if (!g_present_prog) {
            g_present_prog = f.glCreateProgram();
            f.glAttachShader(g_present_prog, compile(GL_VERTEX_SHADER, kVS));
            f.glAttachShader(g_present_prog, compile(GL_FRAGMENT_SHADER, kFS));
            f.glBindAttribLocation(g_present_prog, 0, "p");
            f.glBindAttribLocation(g_present_prog, 1, "t");
            f.glLinkProgram(g_present_prog);
        }
        f.glUseProgram(g_present_prog);
        f.glUniform1i(f.glGetUniformLocation(g_present_prog, "s"), 0);
        f.glDisable(GL_BLEND);
        f.glDisable(GL_DEPTH_TEST);
        f.glEnableVertexAttribArray(0);
        f.glEnableVertexAttribArray(1);
        f.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        f.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
        f.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

// --- accelerometer -------------------------------------------------------------------
SDL_Sensor* g_accel = nullptr;

void init_accelerometer() {
    if (SDL_InitSubSystem(SDL_INIT_SENSOR) != 0) return;
    for (int i = 0; i < SDL_NumSensors(); i++) {
        if (SDL_SensorGetDeviceType(i) == SDL_SENSOR_ACCEL) {
            g_accel = SDL_SensorOpen(i);
            LOGI("accelerometer: %s", SDL_SensorGetDeviceName(i));
            break;
        }
    }
}

void poll_accelerometer() {
    if (!g_accel) return;
    float v[3];
    if (SDL_SensorGetData(g_accel, v, 3) != 0) return;
    // Android/SDL: m/s^2, +g away from the ground. iOS UIAcceleration: g units,
    // pointing towards the ground. Both use the device's natural axes.
    const float g = SDL_STANDARD_GRAVITY;
    uikit::accelerometer_update(-v[0] / g, -v[1] / g, -v[2] / g);
}

// --- input ------------------------------------------------------------------------
// Window pixel -> portrait view point of the iOS screen.
CGPoint window_to_view(float wx, float wy) {
    std::lock_guard<std::mutex> lock(g_viewport_mutex);
    auto point = platform::portrait_point(g_vp, wx, wy, config().screen_w, config().screen_h);
    return {point.x, point.y};
}

void handle_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT:
            g_running = false;
            break;
        case SDL_APP_WILLENTERBACKGROUND:
            LOGI("app will enter background");
            uikit::app_will_resign_active();
            uikit::app_did_enter_background();
            mixer::pause_device(true);
            openal_pause(true);
            g_paused = true;
            break;
        case SDL_APP_DIDENTERFOREGROUND:
            LOGI("app did enter foreground");
            g_paused = false;
            openal_pause(false);
            mixer::pause_device(false);
            uikit::app_will_enter_foreground();
            uikit::app_did_become_active();
            break;
        case SDL_APP_LOWMEMORY:
            uikit::app_memory_warning();
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP: {
            uikit::TouchEvent te;
            te.pointer_id = (long long)e.tfinger.fingerId;
            te.phase = e.type == SDL_FINGERDOWN ? uikit::kBegan : e.type == SDL_FINGERUP ? uikit::kEnded : uikit::kMoved;
            te.location = window_to_view(e.tfinger.x * g_win_w, e.tfinger.y * g_win_h);
            uikit::dispatch_touches({te});
            break;
        }
        case SDL_TEXTINPUT:
            uikit::text_input_insert(ns::utf8_to_u16(e.text.text));
            break;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_BACKSPACE) uikit::text_input_backspace();
            else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) uikit::text_input_return();
            else if (e.key.keysym.sym == SDLK_AC_BACK) {
                if (uikit::text_input_active()) uikit::text_input_return();
                else call_activity_static("confirmExit", "()V");
            }
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                SDL_GL_GetDrawableSize(g_window, &g_win_w, &g_win_h);
                if (g_win_w < g_win_h) std::swap(g_win_w, g_win_h);
                config().window_w = g_win_w; config().window_h = g_win_h;
                compute_viewport();
            }
            break;
        default:
            break;
    }
}

// --- game boot -------------------------------------------------------------------
MachOImage g_image;

void run_static_initializers() {
    auto* s = g_image.section("__DATA", "__mod_init_func");
    if (!s) return;
    typedef void (*Init)(int, char**, char**, char**, void*);
    auto** fns = (Init*)s->addr;
    size_t n = s->size / sizeof(void*);
    static char* argv[] = {(char*)"sims3wa_iphone", nullptr};
    static char* envp[] = {nullptr};
    for (size_t i = 0; i < n; i++) {
        LOGI("static initializer %zu/%zu at +%#lx", i + 1, n, (unsigned long)((uint64_t)fns[i] - (uint64_t)g_image.map_base));
        fns[i](1, argv, envp, envp, nullptr);
    }
}

void* game_thread(void*) {
    setup_config();
    platform::install_crash_handler();
    if (!init_video()) return nullptr;
    choose_screen_size();
    foundation::set_main_thread();

    foundation::init();
    uikit::init();
    gl::init_classes();
    cg::init();
    media::init();

    std::string exe = config().bundle_path + "/sims3wa_iphone";
    if (access(exe.c_str(), R_OK) != 0) {
        LOGE("game executable not found at %s", exe.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TS3 World Adventures",
                                 ("Arquivos do jogo nao encontrados em\n" + config().bundle_path).c_str(), g_window);
        return nullptr;
    }
    if (!macho_load(exe, g_image)) {
        LOGE("failed to load %s", exe.c_str());
        return nullptr;
    }
    objc::load_image(g_image);
    run_static_initializers();

    typedef int (*MainFn)(int, char**);
    static char* argv[] = {(char*)exe.c_str(), nullptr};
    LOGI("calling game main() at %p", (void*)g_image.entry);
    int r = ((MainFn)g_image.entry)(1, argv);
    LOGI("game main returned %d", r);
    return nullptr;
}

}  // namespace

static void call_activity_static(const char* name, const char* sig, ...) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetStaticMethodID(cls, name, sig);
    if (m) {
        va_list ap;
        va_start(ap, sig);
        env->CallStaticVoidMethodV(cls, m, ap);
        va_end(ap);
    } else {
        env->ExceptionClear();
        LOGE("GameActivity.%s%s not found", name, sig);
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

// --- platform API -------------------------------------------------------------------
namespace platform {

void drawable_size(id layer, int* w, int* h) {
    CGRect b = uikit::layer_bounds(layer);
    double s = uikit::layer_contents_scale(layer);
    *w = (int)std::lround(b.size.width * s);
    *h = (int)std::lround(b.size.height * s);
}

void gl_make_current(bool current) {
    if (SDL_GL_MakeCurrent(g_window, current ? g_game_ctx : nullptr) != 0)
        LOGE("SDL_GL_MakeCurrent(%d) failed: %s", current, SDL_GetError());
}

void present_frame(unsigned tex, int w, int h) {
    if (g_paused) return;
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (g_use_fence) sync = p_eglCreateSyncKHR(dpy, EGL_SYNC_FENCE_KHR, nullptr);
    if (sync != EGL_NO_SYNC_KHR) glFlush();
    else glFinish();
    if (g_present_ctx && SDL_GL_MakeCurrent(g_window, g_present_ctx) != 0) LOGE("make present current: %s", SDL_GetError());
    if (sync != EGL_NO_SYNC_KHR) {
        p_eglWaitSyncKHR(dpy, sync, 0);
        p_eglDestroySyncKHR(dpy, sync);
    }
    draw_present_quad(tex);
    if (g_frames % 120 == 0) {
        GLenum err = glGetError();
        if (err) LOGW("present ctx GL error %#x", err);
    }
    SDL_GL_SwapWindow(g_window);
    if (g_present_ctx) SDL_GL_MakeCurrent(g_window, g_game_ctx);
    g_frames++;
    double now = foundation::now_seconds();
    if (now - g_fps_t0 >= 5.0) {
        if (g_fps_t0 > 0) LOGI("fps: %.1f (drawable %dx%d)", g_frames / (now - g_fps_t0), w, h);
        g_fps_t0 = now;
        g_frames = 0;
    }
}

void run_main_loop() {
    LOGI("entering main loop");
    g_settings_ready = true;
    init_accelerometer();
    while (g_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(e);
        if (g_paused) {
            SDL_Delay(50);
            continue;
        }
        poll_accelerometer();
        if (g_crash_test && foundation::now_seconds() - g_fps_t0 > 3.0 && g_frames > 100) {
            LOGW("crash_test: dereferencing NULL on purpose");
            *(volatile int*)nullptr = 0;
        }
        double wait = foundation::runloop_run_once();
        uikit::layout_pass();
        if (wait > 0.0005) SDL_Delay((Uint32)std::min(wait * 1000.0, 8.0));
    }
}

void open_url(const std::string& url) { SDL_OpenURL(url.c_str()); }

void start_text_input(const std::u16string& text, const std::u16string& placeholder, bool secure) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jstring jt = env->NewString((const jchar*)text.data(), (jsize)text.size());
    jstring jp = env->NewString((const jchar*)placeholder.data(), (jsize)placeholder.size());
    call_activity_static("showTextInput", "(Ljava/lang/String;Ljava/lang/String;Z)V", jt, jp, (jboolean)secure);
    env->DeleteLocalRef(jt);
    env->DeleteLocalRef(jp);
}
void stop_text_input() { call_activity_static("hideTextInput", "()V"); }

void pick_image() { call_activity_static("pickImage", "()V"); }


}  // namespace platform

extern "C" JNIEXPORT jint JNICALL Java_com_ts3wa_port_GameActivity_nativeGetScreenMode(JNIEnv*, jclass) {
    return g_settings_ready.load() ? g_screen_mode.load() : -1;
}
extern "C" JNIEXPORT void JNICALL Java_com_ts3wa_port_GameActivity_nativeSetScreenMode(JNIEnv*, jclass, jint mode) {
    foundation::run_on_main([mode]() { set_screen_mode(mode); }, false);
}

extern "C" JNIEXPORT void JNICALL Java_com_ts3wa_port_GameActivity_nativeTextInputDone(JNIEnv* env, jclass,
                                                                                         jstring text, jboolean ok) {
    std::u16string s;
    if (text) {
        const jchar* c = env->GetStringChars(text, nullptr);
        s.assign((const char16_t*)c, env->GetStringLength(text));
        env->ReleaseStringChars(text, c);
    }
    bool accepted = ok;
    foundation::run_on_main([s, accepted]() { uikit::text_input_finish(s, accepted); }, false);
}

extern "C" JNIEXPORT void JNICALL Java_com_ts3wa_port_GameActivity_nativeImagePicked(JNIEnv* env, jclass, jint w,
                                                                                       jint h, jintArray argb) {
    id img = nil;
    if (argb && w > 0 && h > 0) {
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        jint* px = env->GetIntArrayElements(argb, nullptr);
        for (int i = 0; i < w * h; i++) {
            uint32_t p = (uint32_t)px[i];
            uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            // CGImage pixels are premultiplied RGBA
            rgba[i * 4 + 0] = (uint8_t)(r * a / 255);
            rgba[i * 4 + 1] = (uint8_t)(g * a / 255);
            rgba[i * 4 + 2] = (uint8_t)(b * a / 255);
            rgba[i * 4 + 3] = (uint8_t)a;
        }
        env->ReleaseIntArrayElements(argb, px, JNI_ABORT);
        img = cg::image_from_rgba(rgba.data(), w, h);
    }
    foundation::run_on_main(
        [img]() {
            uikit::image_picked(img);
            objc::release(img);
        },
        false);
}

extern "C" int SDL_main(int argc, char* argv[]) {
    LOGI("TS3 World Adventures port starting");
    // Run the game on a thread with a large stack (iOS main thread: 1 MB+,
    // the EA engine recurses deeply while loading M3G scenes).
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);
    pthread_t t;
    pthread_create(&t, &attr, game_thread, nullptr);
    pthread_join(t, nullptr);
    LOGI("game thread finished");
    return 0;
}
