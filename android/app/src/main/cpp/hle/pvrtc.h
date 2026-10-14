#pragma once

#include <cstddef>
#include <cstdint>

// Decode a PVRTC1 texture level into RGBA8 (width*height*4 bytes).
bool pvrtc_decode(const uint8_t* src, int width, int height, bool is2bpp, uint8_t* out_rgba);
size_t pvrtc_data_size(int width, int height, bool is2bpp);
