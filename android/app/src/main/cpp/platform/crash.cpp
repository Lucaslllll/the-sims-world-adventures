// Crash reporter: on a fatal signal, writes the faulting location and a
// frame-pointer backtrace to logcat and <data_root>/crash.txt, translating
// addresses inside the game image to the executable's own addresses
// (0x1000xxxxx) so they can be looked up in Ghidra. Then chains to the
// previous handler so Android still produces its tombstone.
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "hle/common.h"
#include "hle/config.h"
#include "loader/macho.h"

namespace {

struct sigaction g_old[32];
int g_fd = -1;
char g_path[512];

void emit(const char* line) {
    __android_log_write(ANDROID_LOG_FATAL, "TS3WA-crash", line);
    if (g_fd >= 0) {
        write(g_fd, line, strlen(line));
        write(g_fd, "\n", 1);
    }
}

void describe(char* out, size_t n, uint64_t addr) {
    MachOImage* img = g_game_image;
    if (img && img->contains(addr)) {
        snprintf(out, n, "%#llx  game %#llx", (unsigned long long)addr,
                 (unsigned long long)(addr - (uint64_t)img->slide));
        return;
    }
    Dl_info info;
    if (dladdr((void*)addr, &info) && info.dli_fname) {
        const char* lib = strrchr(info.dli_fname, '/');
        lib = lib ? lib + 1 : info.dli_fname;
        if (info.dli_sname)
            snprintf(out, n, "%#llx  %s %s+%#llx", (unsigned long long)addr, lib, info.dli_sname,
                     (unsigned long long)(addr - (uint64_t)info.dli_saddr));
        else
            snprintf(out, n, "%#llx  %s+%#llx", (unsigned long long)addr, lib,
                     (unsigned long long)(addr - (uint64_t)info.dli_fbase));
        return;
    }
    snprintf(out, n, "%#llx", (unsigned long long)addr);
}

bool readable(uint64_t p) {
    // Cheap sanity check for stack walking: aligned and in user space.
    return p && !(p & 7) && p < 0x0000800000000000ULL;
}

void handler(int sig, siginfo_t* info, void* uctx) {
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    auto* uc = (ucontext_t*)uctx;
    char line[256], where[192];
    snprintf(line, sizeof line, "fatal signal %d (code %d) fault addr %p tid %d", sig, info->si_code, info->si_addr,
             gettid());
    emit(line);
    if (g_game_image) {
        snprintf(line, sizeof line, "game image at %p (slide %#llx)", g_game_image->map_base,
                 (unsigned long long)g_game_image->slide);
        emit(line);
    }
    uint64_t pc = uc->uc_mcontext.arm_pc, lr = uc->uc_mcontext.arm_lr;
    describe(where, sizeof where, pc);
    snprintf(line, sizeof line, "pc %s", where);
    emit(line);
    describe(where, sizeof where, lr);
    snprintf(line, sizeof line, "lr %s", where);
    emit(line);
    auto* regs = &uc->uc_mcontext.arm_r0;
    for (int i = 0; i < 16; i += 4) {
        snprintf(line, sizeof line, "r%-2d %08lx %08lx %08lx %08lx", i,
                 regs[i], regs[i + 1], regs[i + 2], regs[i + 3]);
        emit(line);
    }
    // ARMv6 uses r7 frames; NDK ARM/Thumb may use r11. Do not blindly walk memory.
    if (g_fd >= 0) close(g_fd);
    // chain to the previous handler (debuggerd) or default action
    struct sigaction& old = g_old[sig];
    if (old.sa_flags & SA_SIGINFO) {
        if (old.sa_sigaction) old.sa_sigaction(sig, info, uctx);
    } else if (old.sa_handler != SIG_IGN && old.sa_handler != SIG_DFL && old.sa_handler) {
        old.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

}  // namespace

namespace platform {

void install_crash_handler() {
    snprintf(g_path, sizeof g_path, "%s/crash.txt", hle::config().data_root.c_str());
    struct sigaction sa {};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, &g_old[sig]);
}

}  // namespace platform
