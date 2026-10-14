#include "hle/bitmap_hd.h"
#include "hle/texture_pixels.h"
#include <list>
#include <mutex>
#include <unordered_map>

namespace hd {
namespace {
struct Bitmap { int w, h, scale; std::vector<uint8_t> rgba; std::list<uint64_t>::iterator age; };
std::unordered_map<uint64_t, Bitmap> bitmaps;
std::list<uint64_t> ages;
std::mutex mutex;
size_t memory = 0;
constexpr size_t budget = 32 * 1024 * 1024;
}
void publish_bitmap(int w, int h, int scale, const uint8_t* logical_rgba, const uint8_t* physical_rgba) {
    size_t size = size_t(w) * h * scale * scale * 4;
    if (scale <= 1 || size > budget) return;
    uint64_t hash = texture_hash(w, h, 0x1908, 0x1401, logical_rgba, size_t(w) * h * 4);
    std::lock_guard<std::mutex> lock(mutex);
    auto previous = bitmaps.find(hash);
    if (previous != bitmaps.end()) {
        memory -= previous->second.rgba.size(); ages.erase(previous->second.age); bitmaps.erase(previous);
    }
    while (memory + size > budget && !ages.empty()) {
        auto found = bitmaps.find(ages.front());
        memory -= found->second.rgba.size(); bitmaps.erase(found); ages.pop_front();
    }
    ages.push_back(hash);
    Bitmap bitmap{w, h, scale, std::vector<uint8_t>(physical_rgba, physical_rgba + size), std::prev(ages.end())};
    memory += size;
    bitmaps.emplace(hash, std::move(bitmap));
}
bool bitmap_replacement(uint64_t hash, int w, int h, int* scale, std::vector<uint8_t>& rgba) {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = bitmaps.find(hash);
    if (found == bitmaps.end() || found->second.w != w || found->second.h != h) return false;
    ages.splice(ages.end(), ages, found->second.age);
    rgba = found->second.rgba;
    *scale = found->second.scale;
    return true;
}
}
