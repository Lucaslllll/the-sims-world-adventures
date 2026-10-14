#include "loader/abi32.h"
#include "hle/common.h"
#include <sys/mman.h>
#include <mutex>
#include <unordered_map>

extern "C" void arm32_abi_bridge();

void* arm32_wrap_function(void* function) {
    static std::mutex mutex;
    static std::unordered_map<void*, void*> wrappers;
    static uint32_t* current = nullptr;
    static uint32_t* end = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = wrappers.find(function); it != wrappers.end()) return it->second;
    if (!current || current + 8 > end) {
        size_t bytes = 64 * 1024;
        void* arena = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (arena == MAP_FAILED) { LOGE("ARM32 ABI bridge allocation failed"); abort(); }
        current = (uint32_t*)arena;
        end = current + bytes / 4;
    }
    uint32_t* stub = current;
    stub[0] = 0xe59fc004;  // LDR ip, [pc,#4] -> target
    stub[1] = 0xe59ff004;  // LDR pc, [pc,#4] -> bridge
    stub[2] = 0xe1a00000;
    stub[3] = (uintptr_t)function;
    stub[4] = (uintptr_t)&arm32_abi_bridge;
    current += 8;
    __builtin___clear_cache((char*)stub, (char*)(stub + 8));
    wrappers.emplace(function, stub);
    return stub;
}
