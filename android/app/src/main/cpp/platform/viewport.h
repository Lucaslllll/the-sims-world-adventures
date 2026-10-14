// Presentation and its inverse touch transform share the same rectangle.
#pragma once
#include <algorithm>
#include "hle/config.h"

namespace platform {
struct Viewport {
    float x = 0, y = 0, w = 1, h = 1;
    int window_w = 1, window_h = 1;
};
struct ViewPoint { double x, y; };

inline Viewport make_viewport(int window_w, int window_h, int portrait_w, int portrait_h, hle::ScreenMode mode) {
    if (window_w <= 0 || window_h <= 0 || portrait_w <= 0 || portrait_h <= 0) return {};
    Viewport v{0, 0, float(window_w), float(window_h), window_w, window_h};
    if (mode == hle::ScreenMode::Fit) {
        float scale = std::min(float(window_w) / portrait_h, float(window_h) / portrait_w);
        v.w = portrait_h * scale; v.h = portrait_w * scale;
        v.x = (window_w - v.w) / 2; v.y = (window_h - v.h) / 2;
    }
    return v;
}
inline ViewPoint portrait_point(const Viewport& v, float x, float y, int portrait_w, int portrait_h) {
    return {portrait_w - double(y - v.y) / v.h * portrait_w,
            double(x - v.x) / v.w * portrait_h};
}
}  // namespace platform
