// Optional startup-only hooks for original ARM animation functions. The IPA stays untouched.
#include "loader/animation_trace.h"
#include "loader/macho.h"
#include "hle/common.h"
#include <sys/mman.h>
#include <cstring>

namespace {
void report_start(uint32_t* self, int animation, int flags, int) {
    static int calls = 0;
    if (++calls > 80) return;
    auto* manager = (uint32_t*)(uintptr_t)self[2];
    LOGI("ANIM start self=%p request=%d flags=%#x available=%u current=%d node=%#x", self,
         animation, flags, manager ? manager[2] : 0, (int)self[3], self[10]);
}
void report_update(uint32_t* self, int delta, int, int) {
    static int calls = 0;
    if (++calls % 300) return;
    LOGI("ANIM update self=%p delta=%d current=%d time=%u start=%u end=%u rate=%u flags=%#x node=%#x", self,
         delta, (int)self[3], self[5], self[6], self[7], self[8], self[9], self[10]);
}
void report_time(uint32_t* self, int time, int, int) {
    static int calls = 0;
    if (++calls > 80) return;
    LOGI("ANIM set time self=%p value=%d current=%d start=%u flags=%#x node=%#x", self,
         time, (int)self[3], self[6], self[9], self[10]);
}
void hook(MachOImage& image, uint32_t address, void* report) {
    auto* original = (uint32_t*)(uintptr_t)image.slid(address);
    void* arena = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) return;
    auto* code = (uint32_t*)arena;
    code[0] = 0xe92d401f;  // push r0-r3,r4,lr
    code[1] = 0xe1a0400d;  // mov r4,sp
    code[2] = 0xe3cdd007;  // bic sp,sp,#7
    code[3] = 0xe59fc018;  // ldr ip, report literal
    code[4] = 0xe12fff3c;  // blx ip
    code[5] = 0xe1a0d004;  // mov sp,r4
    code[6] = 0xe8bd401f;  // pop r0-r3,r4,lr
    code[7] = original[0];
    code[8] = original[1];
    code[9] = 0xe51ff004;  // ldr pc,[pc,#-4]
    code[10] = (uintptr_t)(original + 2);
    code[11] = (uintptr_t)report;
    original[0] = 0xe51ff004;
    original[1] = (uintptr_t)code;
    __builtin___clear_cache((char*)code, (char*)(code + 12));
    LOGI("animation trace hooked original %#x", address);
}
}
void install_animation_trace(MachOImage& image) {
    hook(image, 0x7a80, (void*)report_start);
    hook(image, 0x7890, (void*)report_update);
    hook(image, 0x7840, (void*)report_time);
    hook(image, 0x7860, (void*)report_time);
}
