#pragma once
#include <cstdint>
#include <vector>

namespace hd {
// Bound cache of native high-resolution CoreGraphics output, keyed by the logical pixels.
void publish_bitmap(int w, int h, int scale, const uint8_t* logical_rgba, const uint8_t* physical_rgba);
bool bitmap_replacement(uint64_t hash, int w, int h, int* scale, std::vector<uint8_t>& rgba);
}
