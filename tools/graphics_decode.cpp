// Reuse the port's exact PNG/PVRTC decoders for offline texture extraction.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "hle/pvrtc.h"
#include <cstdint>
#include <cstdlib>
#include <initializer_list>

extern "C" uint8_t* ts3wa_decode_image(const uint8_t* data, int count, int unpremultiply, int* width, int* height) {
    stbi_convert_iphone_png_to_rgb(1);
    stbi_set_unpremultiply_on_load(unpremultiply);
    int channels = 0;
    return stbi_load_from_memory(data, count, width, height, &channels, 4);
}
extern "C" void ts3wa_image_free(void* image) { stbi_image_free(image); }
extern "C" int ts3wa_decode_pvrtc(const uint8_t* data, int width, int height, int two_bpp, uint8_t* output) {
    return pvrtc_decode(data, width, height, two_bpp != 0, output);
}
extern "C" uint64_t ts3wa_texture_hash(uint32_t width, uint32_t height, uint32_t format, uint32_t type,
                                       const uint8_t* data, size_t count) {
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t field : {width, height, format, type})
        for (unsigned shift = 0; shift < 32; shift += 8) hash = (hash ^ ((field >> shift) & 255)) * 1099511628211ull;
    for (size_t i = 0; i < count; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}
