// Load but never execute ARM code on the host. Exercise actual loader fixups.
#include "loader/macho.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include "hle/config.h"
#include <cstdlib>
#include <sys/mman.h>

namespace hle { void* find_symbol(const char*) { static uint32_t data[128]; return data; } }
namespace hle { Config& config() { static Config value; return value; } }
namespace objc { void* resolve_class_symbol(const char*) { return nullptr; } }
void install_animation_trace(MachOImage&) {}
void* arm32_wrap_function(void* function) { return function; }
void* make_unimplemented_stub(const char*) { return nullptr; }

int main(int argc, char** argv) {
    assert(argc == 2);
    MachOImage image;
    assert(macho_load(argv[1], image));
    auto word = [&](uint32_t address) { return *(uint32_t*)(uintptr_t)image.slid(address); };
    // M3G node ID 10049 must not be confused with ARM function 0x2740.
    assert(word(0x5e11c) == 0x2741);
    // Read-only packed resource IDs must not be treated as pointer candidates.
    assert(word(0x136cc4) == 0x00140013);
    assert(word(0x136f88) == 0x00140013);
    assert(word(0x136f8c) == 0x00160015);
    // The VertexArray absolute switch table must be slid, including interior labels.
    assert(word(0xdf308) == uint32_t(image.slide + 0xdf34c));
    // Thumb PIC literal is a delta; ADD PC will already account for the slide.
    assert(word(0x4318) == 0x133db4);
    // GL_MATRIX_PALETTE_OES is a numeric constant, not the reset() code pointer.
    assert(word(0xe06dc) == 0x8840);
    assert(word(0x138b10) == uint32_t(image.slide + 0x8840));
    assert(word(0x126cb4) == 0xc000); // Exhausted animation multiplier 0.75 in 16.16 fixed point.
    const auto* constants = image.section("__TEXT", "__const");
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> file{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto* text = image.section("__TEXT", "__text");
    (void)text;
    uint64_t original_constants = constants->addr - image.slide;
    assert(!memcmp((void*)(uintptr_t)constants->addr, file.data() + original_constants - 0x1000, constants->size));
    assert(image.entry == image.slid(0xcb1f0));
    const auto* init = image.section("__DATA", "__mod_init_func");
    assert(init && init->size == 28 * 4);
    std::puts("Loader regression: numeric IDs, packed tables, ARM switch and Thumb PIC OK");
    munmap(image.map_base, image.map_size);
    g_game_image = nullptr;
}
