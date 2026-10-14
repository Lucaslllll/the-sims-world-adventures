#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hd {
uint64_t texture_hash(int w, int h, uint32_t format, uint32_t type, const void* data, size_t size);
uint64_t texture_hash_rows(int w, int h, uint32_t format, uint32_t type, const void* data, size_t row, size_t stride);
int bytes_per_pixel(uint32_t format, uint32_t type);
bool decode_rgba(int w, int h, uint32_t format, uint32_t type, int alignment, const void* pixels,
                 std::vector<uint8_t>& rgba);
std::vector<uint8_t> resize_rgba(const uint8_t* pixels, int w, int h, int scale);
std::vector<uint8_t> mip_rgba(const uint8_t* pixels, int w, int h);
}
