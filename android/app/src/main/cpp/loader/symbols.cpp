// Registry of HLE symbols + auto-generated stubs for anything unimplemented.
#include "loader/symbols.h"

#include <sys/mman.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "hle/common.h"

namespace {

std::unordered_map<std::string, void*>& registry() {
    static std::unordered_map<std::string, void*> r;
    return r;
}

// Unimplemented-function stubs live in an executable arena. Each stub is:
//   ldr x17, #16   ; x17 = symbol name
//   ldr x16, #20   ; x16 = handler
//   br  x16
//   nop
//   .quad name
//   .quad handler
struct StubArena {
    uint32_t* cur = nullptr;
    uint32_t* end = nullptr;
};
StubArena g_arena;
std::mutex g_arena_mutex;

}  // namespace

extern "C" void ts3a_unimplemented_stub();  // in stubs.S

extern "C" uint64_t ts3a_unimplemented_report(const char* name) {
    static std::mutex m;
    static std::unordered_map<std::string, int> counts;
    std::lock_guard<std::mutex> lk(m);
    int& n = counts[name];
    if (n < 3)
        LOGW("UNIMPLEMENTED call: %s%s", name, n == 2 ? " (further calls muted)" : "");
    n++;
    if (getenv("TS3WA_STRICT")) abort();
    return 0;
}

namespace hle {

void register_symbol(const char* name, void* addr) { registry()[name] = addr; }

void* find_symbol(const char* name) {
    auto& r = registry();
    auto it = r.find(name);
    return it == r.end() ? nullptr : it->second;
}

}  // namespace hle

void* make_unimplemented_stub(const char* name) {
    std::lock_guard<std::mutex> lk(g_arena_mutex);
    if (!g_arena.cur || g_arena.cur + 8 > g_arena.end) {
        size_t sz = 64 * 1024;
        void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            LOGE("stub arena mmap failed");
            abort();
        }
        g_arena.cur = (uint32_t*)p;
        g_arena.end = (uint32_t*)((char*)p + sz);
    }
    uint32_t* s = g_arena.cur;
    s[0] = 0xe59fc004;  // ldr ip, [pc, #4] = name
    s[1] = 0xe59ff004;  // ldr pc, [pc, #4] = handler
    s[2] = 0xe1a00000;  // nop
    char* persistent = strdup(name);
    memcpy(&s[3], &persistent, 4);
    void* handler = (void*)&ts3a_unimplemented_stub;
    memcpy(&s[4], &handler, 4);
    g_arena.cur += 8;
    __builtin___clear_cache((char*)s, (char*)(s + 8));
    return s;
}
