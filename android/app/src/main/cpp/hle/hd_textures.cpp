#include "hle/hd_textures.h"
#include "hle/common.h"
#include "hle/config.h"
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#define STBI_NO_STDIO
#include "stb_image.h"

namespace hd {
namespace {
struct Entry { int w, h; bool premultiplied; std::string filename; };
std::unordered_map<uint64_t, Entry> entries;
bool loaded = false;
void load_index() {
    if (loaded) return;
    loaded = true;
    std::ifstream input(hle::config().data_root + "/hdpack/manifest.tsv");
    std::string line;
    if (!std::getline(input, line) || line != "TS3WA_HD1") { LOGI("HD texture pack not installed; using filtered upscale"); return; }
    while (std::getline(input, line)) {
        std::istringstream row(line);
        uint64_t hash;
        Entry entry;
        int premultiplied;
        if (!(row >> std::hex >> hash >> std::dec >> entry.w >> entry.h >> premultiplied >> entry.filename)) continue;
        if (entry.w <= 0 || entry.h <= 0 || entry.w > 2048 || entry.h > 2048 ||
            entry.filename.find("..") != std::string::npos || !entry.filename.starts_with("images/")) continue;
        entry.premultiplied = premultiplied != 0;
        entries[hash] = entry;
    }
    LOGI("HD texture pack: %zu original upload keys", entries.size());
}
}
bool replacement(uint64_t hash, int w, int h, int* scale, std::vector<uint8_t>& rgba) {
    load_index();
    auto found = entries.find(hash);
    if (found == entries.end() || found->second.w != w || found->second.h != h) return false;
    auto& entry = found->second;
    std::ifstream file(hle::config().data_root + "/hdpack/" + entry.filename, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || file.tellg() > 64 * 1024 * 1024) return false;
    size_t size = file.tellg();
    std::vector<uint8_t> compressed(size);
    file.seekg(0);
    if (!file.read((char*)compressed.data(), size)) return false;
    int nw, nh, channels;
    stbi_uc* pixels = stbi_load_from_memory(compressed.data(), compressed.size(), &nw, &nh, &channels, 4);
    if (!pixels) { LOGW("HD image decode failed: %s", entry.filename.c_str()); return false; }
    if (nw != w * 2 || nh != h * 2) { stbi_image_free(pixels); LOGW("HD image dimensions rejected"); return false; }
    rgba.assign(pixels, pixels + size_t(nw) * nh * 4);
    stbi_image_free(pixels);
    if (entry.premultiplied) for (size_t p = 0; p < rgba.size(); p += 4)
        for (int c = 0; c < 3; ++c) rgba[p + c] = (rgba[p + c] * rgba[p + 3] + 127) / 255;
    *scale = 2;
    return true;
}
}
