// libSystem (Darwin libc) shims.
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "hle/config.h"
#include "hle/format.h"

namespace {

// --- Darwin FILE ------------------------------------------------------------
// The game inlines Darwin's getc(): `--fp->_r < 0 ? __srget(fp) : *fp->_p++`.
// We hand out FILE objects with Darwin's layout and keep _r at 0 so every
// getc() falls through to __srget, which reads from the real bionic FILE.
struct DarwinFILE {
    unsigned char* _p;   // 0
    int _r;              // 4
    int _w;              // 8
    short _flags;        // 12
    short _file;         // 14
    uint8_t _bf[8];      // 16
    int _lbfsize;        // 24
    void* _cookie;       // 28 -> real FILE*
    uint8_t rest[88 - 32];
};
static_assert(sizeof(DarwinFILE) == 88, "Darwin32 FILE size");
static_assert(offsetof(DarwinFILE, _r) == 4, "Darwin32 FILE _r offset");

inline FILE* real(void* f) { return f ? (FILE*)((DarwinFILE*)f)->_cookie : nullptr; }

void* d_fopen(const char* path, const char* mode) {
    // This older game constructs bundle-parent/Documents paths itself.
    std::string mapped = path;
    const auto& config = hle::config();
    for (const char* directory : {"Documents", "Library", "tmp"}) {
        std::string prefix = config.data_root + "/" + directory;
        if (mapped == prefix || mapped.starts_with(prefix + "/")) {
            mapped = config.home_path + mapped.substr(config.data_root.size());
            break;
        }
    }
    FILE* f = fopen(mapped.c_str(), mode);
    TRACE("fopen(%s, %s) -> %p", path, mode, f);
    if (!f) {
        LOGD("fopen failed: %s (%s)", path, strerror(errno));
        return nullptr;
    }
    auto* d = (DarwinFILE*)calloc(1, sizeof(DarwinFILE));
    d->_cookie = f;
    d->_file = (short)fileno(f);
    return d;
}
int d_fclose(void* f) {
    if (!f) return -1;
    int r = fclose(real(f));
    free(f);
    return r;
}
size_t d_fread(void* buf, size_t sz, size_t n, void* f) { return fread(buf, sz, n, real(f)); }
size_t d_fwrite(const void* buf, size_t sz, size_t n, void* f) { return fwrite(buf, sz, n, real(f)); }
int d_fseek(void* f, long off, int whence) { return fseek(real(f), off, whence); }
long d_ftell(void* f) { return ftell(real(f)); }
int d_feof(void* f) { return feof(real(f)); }
int d_ferror(void* f) { return ferror(real(f)); }
int d_fileno(void* f) { return fileno(real(f)); }
void d_flockfile(void* f) { flockfile(real(f)); }
void d_funlockfile(void* f) { funlockfile(real(f)); }
int d_srget(void* f) {
    auto* d = (DarwinFILE*)f;
    d->_r = 0;
    return getc_unlocked(real(f));
}

// --- Darwin ARM32 stat with 64-bit inodes (game reads st_size at offset 60) ---
struct __attribute__((packed, aligned(4))) DarwinStat {
    int32_t st_dev;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint64_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t st_rdev;
    struct timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    int64_t st_size;
    int64_t st_blocks;
    int32_t st_blksize;
    uint32_t st_flags;
    uint32_t st_gen;
    int32_t st_lspare;
    int64_t st_qspare[2];
};
static_assert(sizeof(DarwinStat) == 108, "Darwin32 stat64 size");
static_assert(offsetof(DarwinStat, st_size) == 60, "st_size offset");

void to_darwin(const struct stat& s, DarwinStat* d) {
    memset(d, 0, sizeof *d);
    d->st_dev = (int32_t)s.st_dev;
    d->st_mode = (uint16_t)s.st_mode;
    d->st_nlink = (uint16_t)s.st_nlink;
    d->st_ino = s.st_ino;
    d->st_uid = s.st_uid;
    d->st_gid = s.st_gid;
    d->st_rdev = (int32_t)s.st_rdev;
    d->st_atimespec = s.st_atim;
    d->st_mtimespec = s.st_mtim;
    d->st_ctimespec = s.st_ctim;
    d->st_birthtimespec = s.st_ctim;
    d->st_size = s.st_size;
    d->st_blocks = s.st_blocks;
    d->st_blksize = (int32_t)s.st_blksize;
}
int d_fstat(int fd, DarwinStat* out) {
    struct stat s;
    int r = fstat(fd, &s);
    if (r == 0) to_darwin(s, out);
    return r;
}
int d_stat(const char* path, DarwinStat* out) {
    struct stat s;
    int r = stat(path, &s);
    if (r == 0) to_darwin(s, out);
    return r;
}

// --- misc ------------------------------------------------------------------------
int32_t d_OSAtomicAdd32(int32_t amount, volatile int32_t* value) {
    return __atomic_add_fetch(value, amount, __ATOMIC_SEQ_CST);
}
void* d_valloc(size_t n) { return memalign(sysconf(_SC_PAGESIZE), n); }
void d_perror(const char* s) { LOGW("perror: %s: %s", s ? s : "", strerror(errno)); }
int d_atexit(void (*)(void*), void*, void*) { return 0; }

[[noreturn]] void d_stack_chk_fail() {
    LOGE("stack smashing detected in game code");
    abort();
}
[[noreturn]] void d_unwind_resume(void*) {
    LOGE("_Unwind_Resume called (C++ exception in game code) - aborting");
    abort();
}
[[noreturn]] void d_dyld_stub_binder() {
    LOGE("dyld_stub_binder called: a lazy symbol was not bound");
    abort();
}

uintptr_t g_stack_chk_guard = 0xe3d1b2f1u;

// pthread: Darwin uses 1/2 for JOINABLE/DETACHED, bionic 0/1.
int d_pthread_attr_setdetachstate(pthread_attr_t* a, int state) {
    return pthread_attr_setdetachstate(a, state == 2 ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
}
struct ThreadStart {
    void* (*fn)(void*);
    void* arg;
};
void* thread_trampoline(void* p) {
    ThreadStart ts = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    LOGI("game thread %ld started (entry %p)", (long)gettid(), (void*)ts.fn);
    return ts.fn(ts.arg);
}
int d_pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    // Give game threads a generous stack: iOS secondary threads default to 512 KB,
    // but the game may rely on more.
    pthread_attr_t a;
    if (attr) a = *attr;
    else pthread_attr_init(&a);
    size_t ss = 0;
    pthread_attr_getstacksize(&a, &ss);
    if (ss < 1024 * 1024) pthread_attr_setstacksize(&a, 1024 * 1024);
    int r = pthread_create(t, &a, thread_trampoline, new ThreadStart{fn, arg});
    if (!attr) pthread_attr_destroy(&a);
    return r;
}

// sysctl: CTL_HW = 6
int d_sysctl(int* name, unsigned namelen, void* oldp, size_t* oldlenp, void*, size_t) {
    if (namelen >= 2 && name[0] == 6) {
        struct sysinfo si;
        sysinfo(&si);
        uint64_t v = 0;
        switch (name[1]) {
            case 3: v = sysconf(_SC_NPROCESSORS_ONLN); break;  // HW_NCPU
            case 5:                                            // HW_PHYSMEM
            case 24:                                           // HW_MEMSIZE
                v = (uint64_t)si.totalram * si.mem_unit;
                break;
            case 6: v = (uint64_t)si.freeram * si.mem_unit + (uint64_t)si.bufferram * si.mem_unit; break;
            case 7: v = sysconf(_SC_PAGESIZE); break;
            default:
                LOGW("sysctl hw.%d unsupported", name[1]);
                errno = ENOENT;
                return -1;
        }
        // Old iOS games use 32-bit "int" sized results for some; honour the buffer size.
        if (oldlenp) {
            if (oldp) memcpy(oldp, &v, std::min<size_t>(*oldlenp, 8));
            if (*oldlenp > 8) *oldlenp = 8;
        }
        LOGI("sysctl hw.%d -> %llu", name[1], (unsigned long long)v);
        return 0;
    }
    LOGW("sysctl {%d,%d} unsupported", namelen > 0 ? name[0] : -1, namelen > 1 ? name[1] : -1);
    errno = ENOENT;
    return -1;
}
int d_sysctlbyname(const char* name, void* oldp, size_t* oldlenp, void*, size_t) {
    std::string val;
    if (!strcmp(name, "hw.machine")) val = hle::config().device_model;
    else if (!strcmp(name, "hw.model")) val = "N61AP";
    else {
        LOGW("sysctlbyname(%s) unsupported", name);
        errno = ENOENT;
        return -1;
    }
    size_t need = val.size() + 1;
    if (oldp && oldlenp) {
        if (*oldlenp < need) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(oldp, val.c_str(), need);
    }
    if (oldlenp) *oldlenp = need;
    return 0;
}

}  // namespace

// --- variadic implementations (see varargs.S) -----------------------------------
extern "C" {
int hle_printf_impl(const char* fmt, const uintptr_t* va) {
    std::string s = hle::apple_format(fmt, {va}, false);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    __android_log_print(ANDROID_LOG_INFO, "TS3WA-printf", "%s", s.c_str());
    return (int)s.size();
}
int hle_snprintf_impl(char* buf, size_t n, const char* fmt, const uintptr_t* va) {
    std::string s = hle::apple_format(fmt, {va}, false);
    if (n > 0) {
        size_t k = std::min(n - 1, s.size());
        memcpy(buf, s.data(), k);
        buf[k] = 0;
    }
    return (int)s.size();
}
int hle_sprintf_impl(char* buf, const char* fmt, const uintptr_t* va) {
    std::string s = hle::apple_format(fmt, {va}, false);
    memcpy(buf, s.c_str(), s.size() + 1);
    return (int)s.size();
}
int hle_fprintf_impl(void* f, const char* fmt, const uintptr_t* va) {
    std::string s = hle::apple_format(fmt, {va}, false);
    return (int)fwrite(s.data(), 1, s.size(), real(f));
}
int hle_sscanf_impl(const char* str, const char* fmt, const uintptr_t* va) {
    // Pointers only: forward up to 8 of them.
    return sscanf(str, fmt, (void*)va[0], (void*)va[1], (void*)va[2], (void*)va[3], (void*)va[4],
                  (void*)va[5], (void*)va[6], (void*)va[7]);
}
int hle_open_impl(const char* path, int flags, const uintptr_t* va) {
    // Darwin O_CREAT=0x200, O_TRUNC=0x400, O_EXCL=0x800, O_APPEND=0x8
    int f = flags & 3;
    if (flags & 0x8) f |= O_APPEND;
    if (flags & 0x200) f |= O_CREAT;
    if (flags & 0x400) f |= O_TRUNC;
    if (flags & 0x800) f |= O_EXCL;
    return open(path, f, (int)va[0]);
}
int hle_printf(const char*, ...);
int hle_snprintf(char*, size_t, const char*, ...);
}

HLE_EXPORT(printf, hle_printf);
HLE_EXPORT(snprintf, hle_snprintf);

HLE_EXPORT(fopen, d_fopen);
HLE_EXPORT(fclose, d_fclose);
HLE_EXPORT(fread, d_fread);
HLE_EXPORT(fwrite, d_fwrite);
HLE_EXPORT(fseek, d_fseek);
HLE_EXPORT(ftell, d_ftell);
HLE_EXPORT(feof, d_feof);
HLE_EXPORT(ferror, d_ferror);
HLE_EXPORT(fileno, d_fileno);
HLE_EXPORT(flockfile, d_flockfile);
HLE_EXPORT(funlockfile, d_funlockfile);
HLE_EXPORT(__srget, d_srget);
HLE_EXPORT(fstat, d_fstat);
HLE_EXPORT(stat, d_stat);

HLE_EXPORT(OSAtomicAdd32, d_OSAtomicAdd32);
HLE_EXPORT(OSAtomicAdd32Barrier, d_OSAtomicAdd32);
HLE_EXPORT(valloc, d_valloc);
HLE_EXPORT(perror, d_perror);
HLE_EXPORT(__cxa_atexit, d_atexit);
HLE_EXPORT(__stack_chk_fail, d_stack_chk_fail);
HLE_EXPORT(__stack_chk_guard, &g_stack_chk_guard);
HLE_EXPORT(_Unwind_Resume, d_unwind_resume);
HLE_EXPORT_AS("dyld_stub_binder", d_dyld_stub_binder);
HLE_EXPORT(sysctl, d_sysctl);
HLE_EXPORT(sysctlbyname, d_sysctlbyname);
HLE_EXPORT(pthread_attr_init, pthread_attr_init);
HLE_EXPORT(pthread_attr_destroy, pthread_attr_destroy);
HLE_EXPORT(pthread_attr_setdetachstate, d_pthread_attr_setdetachstate);
HLE_EXPORT(pthread_create, d_pthread_create);

// Plain C functions with identical ABI on both platforms.
static int i_abs(int v) { return abs(v); }
HLE_EXPORT(abs, i_abs);
HLE_EXPORT(acosf, (float (*)(float))acosf);
HLE_EXPORT(asinf, (float (*)(float))asinf);
HLE_EXPORT(atan2, (double (*)(double, double))atan2);
HLE_EXPORT(atan2f, (float (*)(float, float))atan2f);
HLE_EXPORT(cos, (double (*)(double))cos);
HLE_EXPORT(cosf, (float (*)(float))cosf);
HLE_EXPORT(sin, (double (*)(double))sin);
HLE_EXPORT(sinf, (float (*)(float))sinf);
HLE_EXPORT(tanf, (float (*)(float))tanf);
HLE_EXPORT(expf, (float (*)(float))expf);
HLE_EXPORT(logf, (float (*)(float))logf);
HLE_EXPORT(sqrtf, (float (*)(float))sqrtf);
HLE_EXPORT(sqrt, (double (*)(double))sqrt);
static void w_free(void* p) { free(p); }
static void* w_malloc(size_t n) { return malloc(n); }
static void* w_calloc(size_t a, size_t b) { return calloc(a, b); }
static void* w_realloc(void* p, size_t n) { return realloc(p, n); }
static int w_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
static void* w_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
static void* w_memmove(void* d, const void* s, size_t n) { return memmove(d, s, n); }
static void* w_memset(void* d, int c, size_t n) { return memset(d, c, n); }
static int w_strcmp(const char* a, const char* b) { return strcmp(a, b); }
static size_t w_strlen(const char* s) { return strlen(s); }
static int w_usleep(unsigned us) { return usleep(us); }
static int w_gettimeofday(struct timeval* tv, void* tz) { return gettimeofday(tv, (struct timezone*)tz); }
HLE_EXPORT(free, w_free);
HLE_EXPORT(malloc, w_malloc);
HLE_EXPORT(calloc, w_calloc);
HLE_EXPORT(realloc, w_realloc);
HLE_EXPORT(memcmp, w_memcmp);
HLE_EXPORT(memcpy, w_memcpy);
HLE_EXPORT(memmove, w_memmove);
HLE_EXPORT(memset, w_memset);
HLE_EXPORT(strcmp, w_strcmp);
HLE_EXPORT(strlen, w_strlen);
HLE_EXPORT(usleep, w_usleep);
HLE_EXPORT(gettimeofday, w_gettimeofday);
