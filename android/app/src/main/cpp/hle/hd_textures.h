#pragma once
#include <cstdint>
#include <vector>

namespace hd {
// Return 2x RGBA pixels when an exact original upload matches the local AI pack.
bool replacement(uint64_t hash, int w, int h, int* scale, std::vector<uint8_t>& rgba);
}
