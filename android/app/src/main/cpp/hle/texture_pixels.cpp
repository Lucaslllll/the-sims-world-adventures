#include "hle/texture_pixels.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hd {
namespace {
constexpr uint64_t prime = 1099511628211ull;
uint64_t header(int w, int h, uint32_t format, uint32_t type) {
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t value : {uint32_t(w), uint32_t(h), format, type})
        for (unsigned shift = 0; shift < 32; shift += 8) hash = (hash ^ ((value >> shift) & 255)) * prime;
    return hash;
}
uint64_t append(uint64_t hash, const uint8_t* pixels, size_t size) {
    for (size_t i = 0; i < size; ++i) hash = (hash ^ pixels[i]) * prime;
    return hash;
}
}
uint64_t texture_hash(int w, int h, uint32_t format, uint32_t type, const void* data, size_t size) {
    return append(header(w, h, format, type), (const uint8_t*)data, size);
}
uint64_t texture_hash_rows(int w, int h, uint32_t format, uint32_t type, const void* data, size_t row, size_t stride) {
    uint64_t hash = header(w, h, format, type);
    for (int y = 0; y < h; ++y) hash = append(hash, (const uint8_t*)data + y * stride, row);
    return hash;
}
int bytes_per_pixel(uint32_t format, uint32_t type) {
    if (type == 0x8363) return format == 0x1907 ? 2 : 0;
    if (type == 0x8033 || type == 0x8034) return format == 0x1908 ? 2 : 0;
    if (type != 0x1401) return 0;
    switch (format) {
        case 0x1908: return 4;
        case 0x1907: return 3;
        case 0x190a: return 2;
        case 0x1906: case 0x1909: return 1;
        default: return 0;
    }
}
bool decode_rgba(int w, int h, uint32_t format, uint32_t type, int alignment, const void* pixels,
                 std::vector<uint8_t>& rgba) {
    int bpp = bytes_per_pixel(format, type);
    if (!pixels || !bpp || w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
        (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)) return false;
    size_t stride = (size_t(w) * bpp + alignment - 1) & ~(size_t(alignment) - 1);
    rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = (const uint8_t*)pixels + y * stride;
        uint8_t* dst = rgba.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x, src += bpp, dst += 4) {
            if (type == 0x1401) {
                if (format == 0x1908) memcpy(dst, src, 4);
                else if (format == 0x1907) { memcpy(dst, src, 3); dst[3] = 255; }
                else if (format == 0x1906) { dst[0] = dst[1] = dst[2] = 255; dst[3] = src[0]; }
                else { dst[0] = dst[1] = dst[2] = src[0]; dst[3] = format == 0x190a ? src[1] : 255; }
            } else {
                uint16_t value;
                memcpy(&value, src, 2);
                if (type == 0x8363) {
                    int r = value >> 11, g = (value >> 5) & 63, b = value & 31;
                    dst[0] = (r << 3) | (r >> 2); dst[1] = (g << 2) | (g >> 4);
                    dst[2] = (b << 3) | (b >> 2); dst[3] = 255;
                } else if (type == 0x8033) {
                    dst[0] = ((value >> 12) & 15) * 17; dst[1] = ((value >> 8) & 15) * 17;
                    dst[2] = ((value >> 4) & 15) * 17; dst[3] = (value & 15) * 17;
                } else {
                    int r = value >> 11, g = (value >> 6) & 31, b = (value >> 1) & 31;
                    dst[0] = (r << 3) | (r >> 2); dst[1] = (g << 3) | (g >> 2);
                    dst[2] = (b << 3) | (b >> 2); dst[3] = (value & 1) ? 255 : 0;
                }
            }
        }
    }
    return true;
}
std::vector<uint8_t> resize_rgba(const uint8_t* pixels, int w, int h, int scale) {
    if (!pixels || w <= 0 || h <= 0 || scale < 1 || scale > 4) return {};
    int nw = w * scale, nh = h * scale;
    std::vector<uint8_t> result(size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y) {
        float sy = (y + 0.5f) / scale - 0.5f;
        int iy = (int)std::floor(sy), y0 = std::clamp(iy, 0, h - 1), y1 = std::clamp(iy + 1, 0, h - 1);
        float fy = sy - iy;
        for (int x = 0; x < nw; ++x) {
            float sx = (x + 0.5f) / scale - 0.5f;
            int ix = (int)std::floor(sx), x0 = std::clamp(ix, 0, w - 1), x1 = std::clamp(ix + 1, 0, w - 1);
            float fx = sx - ix;
            for (int c = 0; c < 4; ++c) {
                float a = pixels[(size_t(y0) * w + x0) * 4 + c] * (1 - fx) + pixels[(size_t(y0) * w + x1) * 4 + c] * fx;
                float b = pixels[(size_t(y1) * w + x0) * 4 + c] * (1 - fx) + pixels[(size_t(y1) * w + x1) * 4 + c] * fx;
                result[(size_t(y) * nw + x) * 4 + c] = uint8_t(std::lround(a * (1 - fy) + b * fy));
            }
        }
    }
    return result;
}
std::vector<uint8_t> mip_rgba(const uint8_t* pixels, int w, int h) {
    int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    std::vector<uint8_t> result(size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y) for (int x = 0; x < nw; ++x) for (int c = 0; c < 4; ++c) {
        int sum = 0;
        for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
            sum += pixels[(size_t(std::min(y * 2 + dy, h - 1)) * w + std::min(x * 2 + dx, w - 1)) * 4 + c];
        result[(size_t(y) * nw + x) * 4 + c] = (sum + 2) / 4;
    }
    return result;
}
}
