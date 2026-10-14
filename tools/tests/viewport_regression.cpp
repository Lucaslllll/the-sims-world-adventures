#include "platform/viewport.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <utility>

int main() {
    for (auto mode : {hle::ScreenMode::Fit, hle::ScreenMode::Stretch}) {
        for (auto size : {std::pair{1520, 720}, std::pair{1382, 666}, std::pair{960, 640}, std::pair{800, 800}}) {
            auto v = platform::make_viewport(size.first, size.second, 320, 480, mode);
            if (mode == hle::ScreenMode::Fit) assert(std::abs(v.w / v.h - 1.5f) < 0.0001);
            else assert(v.x == 0 && v.y == 0 && v.w == size.first && v.h == size.second);
            // Invert real logical button positions at the corners and middle.
            for (auto point : {std::pair{0.0, 0.0}, std::pair{320.0, 480.0}, std::pair{160.0, 240.0}, std::pair{28.0, 450.0}}) {
                float x = v.x + point.second / 480 * v.w;
                float y = v.y + (320 - point.first) / 320 * v.h;
                auto mapped = platform::portrait_point(v, x, y, 320, 480);
                assert(std::abs(mapped.x - point.first) < 0.001 && std::abs(mapped.y - point.second) < 0.001);
            }
        }
    }
    auto invalid = platform::make_viewport(0, 0, 0, 0, hle::ScreenMode::Fit);
    assert(invalid.w > 0 && invalid.h > 0);
    std::puts("Viewport regression OK: fit/stretch, resize, rotated touches and button anchors");
}
