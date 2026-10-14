// PVRTC1 (2bpp / 4bpp) software decoder -> RGBA8.
// Mali/Adreno GPUs don't support PowerVR texture compression, so compressed
// uploads are expanded on the CPU before reaching the driver.
#include "hle/pvrtc.h"

#include <algorithm>
#include <vector>

namespace {

struct C5 {
    int r, g, b, a;  // r,g,b: 5 bit; a: 4 bit
};

C5 color_a(uint32_t c) {
    if (c & 0x8000) {
        return {int((c >> 10) & 0x1f), int((c >> 5) & 0x1f), int((c & 0x1e) | ((c & 0x1e) >> 4)), 0xf};
    }
    return {int(((c >> 7) & 0x1e) | ((c >> 11) & 0x1)), int(((c >> 3) & 0x1e) | ((c >> 7) & 0x1)),
            int(((c << 1) & 0x1c) | ((c >> 2) & 0x3)), int((c >> 11) & 0xe)};
}

C5 color_b(uint32_t c) {
    if (c & 0x80000000u) {
        return {int((c >> 26) & 0x1f), int((c >> 21) & 0x1f), int((c >> 16) & 0x1f), 0xf};
    }
    return {int(((c >> 23) & 0x1e) | ((c >> 27) & 0x1)), int(((c >> 19) & 0x1e) | ((c >> 23) & 0x1)),
            int(((c >> 15) & 0x1e) | ((c >> 19) & 0x1)), int((c >> 27) & 0xe)};
}

uint32_t twiddle(uint32_t xsize, uint32_t ysize, uint32_t x, uint32_t y) {
    uint32_t min_axis = ysize, max_value = x, out = 0, src = 1, dst = 1;
    int shift = 0;
    if (ysize > xsize) {
        min_axis = xsize;
        max_value = y;
    }
    while (src < min_axis) {
        if (y & src) out |= dst;
        if (x & src) out |= dst << 1;
        src <<= 1;
        dst <<= 2;
        shift++;
    }
    max_value >>= shift;
    out |= max_value << (2 * shift);
    return out;
}

}  // namespace

bool pvrtc_decode(const uint8_t* src, int width, int height, bool is2bpp, uint8_t* out_rgba) {
    const int bw = is2bpp ? 8 : 4, bh = 4;
    const int bx_count = std::max(width / bw, 2);
    const int by_count = std::max(height / bh, 2);
    const int pw = bx_count * bw, ph = by_count * bh;  // padded size

    auto block = [&](int bx, int by) -> const uint32_t* {
        bx = (bx % bx_count + bx_count) % bx_count;
        by = (by % by_count + by_count) % by_count;
        return (const uint32_t*)(src + 8 * twiddle(bx_count, by_count, bx, by));
    };

    // 1. Modulation values (already mapped to 0..8 weights, +16 for punch-through)
    //    and modes over the padded texture.
    std::vector<int8_t> modval(pw * ph), modmode(pw * ph);
    for (int by = 0; by < by_count; by++) {
        for (int bx = 0; bx < bx_count; bx++) {
            const uint32_t* w = block(bx, by);
            uint32_t mod = w[0], col = w[1];
            int mode = col & 1;
            if (is2bpp) {
                if (mode) {
                    if (mod & 1) {
                        mode = (mod & (1u << 20)) ? 3 : 2;  // V-only : H-only
                        if (mod & (1u << 21)) mod |= 1u << 20;
                        else mod &= ~(1u << 20);
                    }
                    if (mod & 2) mod |= 1;
                    else mod &= ~1u;
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 8; x++) {
                            int i = (by * 4 + y) * pw + bx * 8 + x;
                            modmode[i] = (int8_t)mode;
                            if (((x ^ y) & 1) == 0) {
                                modval[i] = (int8_t)(mod & 3);
                                mod >>= 2;
                            }
                        }
                } else {
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 8; x++) {
                            int i = (by * 4 + y) * pw + bx * 8 + x;
                            modmode[i] = 0;
                            modval[i] = (mod & 1) ? 3 : 0;
                            mod >>= 1;
                        }
                }
            } else {
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int i = (by * 4 + y) * pw + bx * 4 + x;
                        modmode[i] = (int8_t)mode;
                        modval[i] = (int8_t)(mod & 3);
                        mod >>= 2;
                    }
            }
        }
    }
    static const int rep0[4] = {0, 3, 5, 8};
    static const int rep1[4] = {0, 4, 4 + 16, 8};  // 2 = punch-through alpha
    auto mv = [&](int x, int y) {
        x = (x % pw + pw) % pw;
        y = (y % ph + ph) % ph;
        return (int)modval[y * pw + x];
    };
    auto weight = [&](int x, int y) -> int {
        int i = y * pw + x;
        int mode = modmode[i];
        if (!is2bpp) return mode ? rep1[modval[i]] : rep0[modval[i]];
        if (mode == 0 || ((x ^ y) & 1) == 0) return rep0[modval[i]];
        if (mode == 1)
            return (rep0[mv(x, y - 1)] + rep0[mv(x, y + 1)] + rep0[mv(x - 1, y)] + rep0[mv(x + 1, y)] + 2) / 4;
        if (mode == 2) return (rep0[mv(x - 1, y)] + rep0[mv(x + 1, y)] + 1) / 2;
        return (rep0[mv(x, y - 1)] + rep0[mv(x, y + 1)] + 1) / 2;
    };

    // 2. Bilinear upscale of colour A/B images (block colours sit at block
    //    centres, wrapping at the edges), then modulate.
    const int hx = bw / 2, hy = bh / 2;
    const int wsum = bw * bh;  // 16 (4bpp) or 32 (2bpp)
    for (int y = 0; y < height; y++) {
        int ty = y - hy;
        int by0 = (ty >= 0 ? ty : ty - bh + 1) / bh;  // floor division
        int ry = ty - by0 * bh;
        for (int x = 0; x < width; x++) {
            int tx = x - hx;
            int bx0 = (tx >= 0 ? tx : tx - bw + 1) / bw;
            int rx = tx - bx0 * bw;
            uint32_t cP = block(bx0, by0)[1], cQ = block(bx0 + 1, by0)[1];
            uint32_t cR = block(bx0, by0 + 1)[1], cS = block(bx0 + 1, by0 + 1)[1];
            int wP = (bw - rx) * (bh - ry), wQ = rx * (bh - ry), wR = (bw - rx) * ry, wS = rx * ry;
            C5 aP = color_a(cP), aQ = color_a(cQ), aR = color_a(cR), aS = color_a(cS);
            C5 bP = color_b(cP), bQ = color_b(cQ), bR = color_b(cR), bS = color_b(cS);
            int A[4] = {aP.r * wP + aQ.r * wQ + aR.r * wR + aS.r * wS, aP.g * wP + aQ.g * wQ + aR.g * wR + aS.g * wS,
                        aP.b * wP + aQ.b * wQ + aR.b * wR + aS.b * wS, aP.a * wP + aQ.a * wQ + aR.a * wR + aS.a * wS};
            int B[4] = {bP.r * wP + bQ.r * wQ + bR.r * wR + bS.r * wS, bP.g * wP + bQ.g * wQ + bR.g * wR + bS.g * wS,
                        bP.b * wP + bQ.b * wQ + bR.b * wR + bS.b * wS, bP.a * wP + bQ.a * wQ + bR.a * wR + bS.a * wS};
            // expand to 8 bit (sum = value * wsum)
            int a8[4], b8[4];
            for (int k = 0; k < 3; k++) {
                if (wsum == 16) {
                    a8[k] = (A[k] >> 6) + (A[k] >> 1);
                    b8[k] = (B[k] >> 6) + (B[k] >> 1);
                } else {
                    a8[k] = (A[k] >> 7) + (A[k] >> 2);
                    b8[k] = (B[k] >> 7) + (B[k] >> 2);
                }
            }
            if (wsum == 16) {
                a8[3] = (A[3] >> 4) + A[3];
                b8[3] = (B[3] >> 4) + B[3];
            } else {
                a8[3] = (A[3] >> 5) + (A[3] >> 1);
                b8[3] = (B[3] >> 5) + (B[3] >> 1);
            }
            int m = weight(x, y);
            bool punch = m >= 16;
            if (punch) m -= 16;
            uint8_t* o = out_rgba + (y * width + x) * 4;
            for (int k = 0; k < 4; k++) {
                int v = (a8[k] * (8 - m) + b8[k] * m) / 8;
                o[k] = (uint8_t)std::clamp(v, 0, 255);
            }
            if (punch) o[3] = 0;
        }
    }
    return true;
}

size_t pvrtc_data_size(int width, int height, bool is2bpp) {
    int bx = std::max(width / (is2bpp ? 8 : 4), 2);
    int by = std::max(height / 4, 2);
    return (size_t)bx * by * 8;
}
