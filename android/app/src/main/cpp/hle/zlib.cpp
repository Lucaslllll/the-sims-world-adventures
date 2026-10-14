// libz: forwarded to the system zlib. z_stream has the same LP64 layout (112
// bytes) as Apple's zlib 1.2.5, so inflate* pass through directly. The one
// difference is get_crc_table(): zlib 1.2.5 returns `unsigned long` entries
// (8 bytes on arm64) while modern zlib returns 32-bit z_crc_t, and the game's
// minizip indexes the table with 8-byte stride.
#include <zlib.h>

#include <cstdint>

#include "hle/common.h"

namespace {

uint32_t g_crc_table64[256];

const uint32_t* get_crc_table64() {
    if (!g_crc_table64[1]) {
        const z_crc_t* t = get_crc_table();
        for (int i = 0; i < 256; i++) g_crc_table64[i] = t[i];
    }
    return g_crc_table64;
}

unsigned long w_crc32(unsigned long crc, const Bytef* buf, uInt len) { return crc32(crc, buf, len); }
int w_inflate(z_streamp s, int flush) { return inflate(s, flush); }
int w_inflateEnd(z_streamp s) { return inflateEnd(s); }
int w_inflateInit2_(z_streamp s, int bits, const char* version, int size) {
    return inflateInit2_(s, bits, version, size);
}

}  // namespace

HLE_EXPORT(crc32, w_crc32);
HLE_EXPORT(get_crc_table, get_crc_table64);
HLE_EXPORT(inflate, w_inflate);
HLE_EXPORT(inflateEnd, w_inflateEnd);
HLE_EXPORT(inflateInit2_, w_inflateInit2_);
HLE_EXPORT(deflate, deflate);
HLE_EXPORT(deflateEnd, deflateEnd);
HLE_EXPORT(deflateInit2_, deflateInit2_);
