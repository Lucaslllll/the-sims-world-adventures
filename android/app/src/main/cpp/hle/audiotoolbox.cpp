// AudioToolbox HLE: AudioSession, AudioFile (WAV/CAF) and AudioQueue output.
// AAC packets are decoded with Android's AMediaCodec; PCM goes straight to the
// mixer. Each AudioQueue owns a worker thread that consumes enqueued buffers
// at playback pace and invokes the game's output callback, like iOS does.
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/audio_mixer.h"
#include "hle/foundation.h"
#include "objc/runtime.h"

namespace {

typedef int32_t OSStatus;
constexpr uint32_t fourcc(const char s[5]) {
    return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint32_t)s[3];
}
constexpr OSStatus kAudioFileUnspecifiedError = 0x7768743F;     // 'wht?'
constexpr OSStatus kAudioFileUnsupportedFileType = 0x7479703F;  // 'typ?'
constexpr OSStatus kAudioFileUnsupportedProperty = 0x7074793F;  // 'pty?'
constexpr OSStatus kAudioFileEndOfFile = -39;
constexpr OSStatus kAudioQueueErr_InvalidRunState = -66678;
constexpr OSStatus kAudioQueueErr_InvalidProperty = -66684;

struct ASBD {
    double mSampleRate;
    uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame,
        mBitsPerChannel, mReserved;
};
static_assert(sizeof(ASBD) == 40, "ASBD");

struct PacketDesc {
    int64_t mStartOffset;
    uint32_t mVariableFramesInPacket;
    uint32_t mDataByteSize;
};
static_assert(sizeof(PacketDesc) == 16, "packet desc");

// =============================================================================
// AudioFile
// =============================================================================
struct AFile {
    std::mutex m;
    FILE* f = nullptr;
    uint32_t type = 0;
    ASBD fmt{};
    int64_t data_off = 0, data_size = 0;
    std::vector<uint8_t> cookie, chan;
    std::vector<PacketDesc> packets;  // VBR packet table (offsets relative to data start)
    int64_t packet_count = 0;
    uint32_t max_packet = 0;
};

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }

bool parse_wav(AFile* a, const std::vector<uint8_t>& hdr, int64_t file_size) {
    size_t p = 12;
    bool have_fmt = false;
    while (p + 8 <= hdr.size()) {
        uint32_t id = be32(&hdr[p]), sz = le32(&hdr[p + 4]);
        if (id == fourcc("fmt ") && p + 8 + 16 <= hdr.size()) {
            const uint8_t* f = &hdr[p + 8];
            uint16_t tag = le16(f), ch = le16(f + 2), align = le16(f + 12), bits = le16(f + 14);
            uint32_t rate = le32(f + 4);
            if (tag != 1 && tag != 0xFFFE) {
                LOGW("WAV format tag %u unsupported", tag);
                return false;
            }
            a->fmt.mSampleRate = rate;
            a->fmt.mFormatID = fourcc("lpcm");
            a->fmt.mFormatFlags = (bits > 8 ? 0x4 /*signed*/ : 0) | 0x8 /*packed*/;
            a->fmt.mBytesPerPacket = align;
            a->fmt.mFramesPerPacket = 1;
            a->fmt.mBytesPerFrame = align;
            a->fmt.mChannelsPerFrame = ch;
            a->fmt.mBitsPerChannel = bits;
            a->max_packet = align;
            have_fmt = true;
        } else if (id == fourcc("data")) {
            a->data_off = p + 8;
            a->data_size = std::min<int64_t>(sz, file_size - a->data_off);
            break;
        }
        p += 8 + sz + (sz & 1);
    }
    if (!have_fmt || !a->data_off) return false;
    a->type = fourcc("WAVE");
    a->packet_count = a->fmt.mBytesPerPacket ? a->data_size / a->fmt.mBytesPerPacket : 0;
    return true;
}

int64_t read_ber(const uint8_t*& p, const uint8_t* end) {
    int64_t v = 0;
    while (p < end) {
        uint8_t b = *p++;
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return v;
}

bool parse_caf(AFile* a, int64_t file_size) {
    int64_t p = 8;
    std::vector<uint8_t> pakt;
    while (p + 12 <= file_size) {
        uint8_t h[12];
        fseek(a->f, p, SEEK_SET);
        if (fread(h, 1, 12, a->f) != 12) break;
        uint32_t id = be32(h);
        int64_t sz = (int64_t)be64(h + 4);
        int64_t body = p + 12;
        if (id == fourcc("data")) {
            a->data_off = body + 4;  // skip edit count
            a->data_size = (sz < 0 ? file_size - body : sz) - 4;
            if (sz < 0) break;
        } else if (sz >= 0 && sz < (64 << 20)) {
            std::vector<uint8_t> buf(sz);
            fseek(a->f, body, SEEK_SET);
            if (fread(buf.data(), 1, sz, a->f) != (size_t)sz) break;
            if (id == fourcc("desc") && sz >= 32) {
                uint64_t r = be64(&buf[0]);
                memcpy(&a->fmt.mSampleRate, &r, 8);
                a->fmt.mFormatID = be32(&buf[8]);
                a->fmt.mFormatFlags = be32(&buf[12]);
                a->fmt.mBytesPerPacket = be32(&buf[16]);
                a->fmt.mFramesPerPacket = be32(&buf[20]);
                a->fmt.mChannelsPerFrame = be32(&buf[24]);
                a->fmt.mBitsPerChannel = be32(&buf[28]);
                if (a->fmt.mFormatID == fourcc("lpcm")) {
                    a->fmt.mBytesPerFrame = a->fmt.mBytesPerPacket;
                    // CAF lpcm flags: bit 1 = little endian; CoreAudio flag 0x2 = big endian
                    a->fmt.mFormatFlags = (a->fmt.mFormatFlags & 1 ? 0x1 : 0x4) | 0x8 | (a->fmt.mFormatFlags & 2 ? 0 : 0x2);
                }
            } else if (id == fourcc("kuki")) {
                a->cookie = std::move(buf);
            } else if (id == fourcc("chan")) {
                a->chan = std::move(buf);
            } else if (id == fourcc("pakt")) {
                pakt = std::move(buf);
            }
        }
        if (sz < 0) break;
        p = body + sz;
    }
    if (!a->data_off || !a->fmt.mFormatID) return false;
    a->type = fourcc("caff");
    if (a->fmt.mBytesPerPacket) {
        a->packet_count = a->data_size / a->fmt.mBytesPerPacket;
        a->max_packet = a->fmt.mBytesPerPacket;
    } else if (pakt.size() >= 24) {
        int64_t n = (int64_t)be64(&pakt[0]);
        const uint8_t* q = &pakt[24];
        const uint8_t* end = pakt.data() + pakt.size();
        int64_t off = 0;
        a->packets.reserve(n);
        for (int64_t i = 0; i < n && q < end; i++) {
            PacketDesc d{};
            d.mStartOffset = off;
            d.mDataByteSize = (uint32_t)read_ber(q, end);
            if (!a->fmt.mFramesPerPacket) d.mVariableFramesInPacket = (uint32_t)read_ber(q, end);
            off += d.mDataByteSize;
            a->max_packet = std::max(a->max_packet, d.mDataByteSize);
            a->packets.push_back(d);
        }
        a->packet_count = (int64_t)a->packets.size();
    }
    return true;
}

OSStatus AudioFileOpenURL(id url, int8_t, uint32_t, AFile** out) {
    *out = nullptr;
    std::string path = foundation::url_path(url);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        LOGW("AudioFileOpenURL: cannot open %s", path.c_str());
        return kAudioFileUnspecifiedError;
    }
    fseek(f, 0, SEEK_END);
    int64_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> hdr(std::min<int64_t>(size, 4096));
    if (fread(hdr.data(), 1, hdr.size(), f) != hdr.size()) {
        fclose(f);
        return kAudioFileUnspecifiedError;
    }
    auto* a = new AFile();
    a->f = f;
    bool ok = false;
    if (hdr.size() >= 12 && be32(&hdr[0]) == fourcc("RIFF") && be32(&hdr[8]) == fourcc("WAVE")) ok = parse_wav(a, hdr, size);
    else if (hdr.size() >= 8 && be32(&hdr[0]) == fourcc("caff")) ok = parse_caf(a, size);
    if (!ok) {
        LOGW("AudioFileOpenURL: unsupported file %s", path.c_str());
        fclose(f);
        delete a;
        return kAudioFileUnsupportedFileType;
    }
    TRACE("AudioFile %s: %.0f Hz %u ch fmt %08x packets %lld", path.c_str(), a->fmt.mSampleRate,
          a->fmt.mChannelsPerFrame, a->fmt.mFormatID, (long long)a->packet_count);
    *out = a;
    return 0;
}

OSStatus AudioFileClose(AFile* a) {
    if (!a) return 0;
    fclose(a->f);
    delete a;
    return 0;
}

OSStatus file_property(AFile* a, uint32_t prop, uint32_t* size, void* data, bool info_only) {
    auto put = [&](const void* src, uint32_t n) -> OSStatus {
        if (info_only || !data) {
            *size = n;
            return 0;
        }
        uint32_t k = std::min(*size, n);
        memcpy(data, src, k);
        *size = k;
        return 0;
    };
    switch (prop) {
        case fourcc("ffmt"): return put(&a->type, 4);
        case fourcc("dfmt"): return put(&a->fmt, sizeof(ASBD));
        case fourcc("bcnt"): {
            uint64_t v = a->data_size;
            return put(&v, 8);
        }
        case fourcc("pcnt"): {
            uint64_t v = a->packet_count;
            return put(&v, 8);
        }
        case fourcc("pkub"):
        case fourcc("psze"): {
            uint32_t v = a->max_packet;
            return put(&v, 4);
        }
        case fourcc("mgic"):
            if (a->cookie.empty()) return kAudioFileUnsupportedProperty;
            return put(a->cookie.data(), (uint32_t)a->cookie.size());
        case fourcc("cmap"):
            if (a->chan.empty()) return kAudioFileUnsupportedProperty;
            return put(a->chan.data(), (uint32_t)a->chan.size());
        case fourcc("dofs"): {
            int64_t v = a->data_off;
            return put(&v, 8);
        }
        default: {
            char s[5] = {(char)(prop >> 24), (char)(prop >> 16), (char)(prop >> 8), (char)prop, 0};
            LOGW("AudioFile property '%s' unsupported", s);
            return kAudioFileUnsupportedProperty;
        }
    }
}
OSStatus AudioFileGetProperty(AFile* a, uint32_t prop, uint32_t* size, void* data) {
    return a ? file_property(a, prop, size, data, false) : kAudioFileUnspecifiedError;
}
OSStatus AudioFileGetPropertyInfo(AFile* a, uint32_t prop, uint32_t* size, uint32_t* writable) {
    if (!a) return kAudioFileUnspecifiedError;
    uint32_t sz = 0;
    OSStatus r = file_property(a, prop, &sz, nullptr, true);
    if (size) *size = sz;
    if (writable) *writable = 0;
    return r;
}

OSStatus AudioFileReadBytes(AFile* a, bool, int64_t start, uint32_t* io_bytes, void* out) {
    if (!a) return kAudioFileUnspecifiedError;
    std::lock_guard<std::mutex> lk(a->m);
    int64_t avail = a->data_size - start;
    if (avail <= 0) {
        *io_bytes = 0;
        return kAudioFileEndOfFile;
    }
    uint32_t n = (uint32_t)std::min<int64_t>(*io_bytes, avail);
    fseek(a->f, a->data_off + start, SEEK_SET);
    *io_bytes = (uint32_t)fread(out, 1, n, a->f);
    return *io_bytes < n ? kAudioFileEndOfFile : 0;
}

OSStatus AudioFileReadPackets(AFile* a, bool, uint32_t* out_bytes, PacketDesc* descs, int64_t start,
                              uint32_t* io_packets, void* out) {
    if (!a) return kAudioFileUnspecifiedError;
    std::lock_guard<std::mutex> lk(a->m);
    uint32_t want = *io_packets;
    if (start >= a->packet_count) {
        *io_packets = 0;
        if (out_bytes) *out_bytes = 0;
        return kAudioFileEndOfFile;
    }
    uint32_t n = (uint32_t)std::min<int64_t>(want, a->packet_count - start);
    int64_t off, len;
    if (!a->packets.empty()) {
        off = a->packets[start].mStartOffset;
        const PacketDesc& last = a->packets[start + n - 1];
        len = last.mStartOffset + last.mDataByteSize - off;
        if (descs)
            for (uint32_t i = 0; i < n; i++) {
                descs[i] = a->packets[start + i];
                descs[i].mStartOffset -= off;
            }
    } else {
        off = start * a->fmt.mBytesPerPacket;
        len = (int64_t)n * a->fmt.mBytesPerPacket;
        if (descs)
            for (uint32_t i = 0; i < n; i++) descs[i] = {(int64_t)i * a->fmt.mBytesPerPacket, 0, a->fmt.mBytesPerPacket};
    }
    fseek(a->f, a->data_off + off, SEEK_SET);
    size_t got = fread(out, 1, len, a->f);
    if (out_bytes) *out_bytes = (uint32_t)got;
    *io_packets = n;
    return 0;
}

// =============================================================================
// AAC decoding (AMediaCodec)
// =============================================================================
// Extract the AudioSpecificConfig (DecoderSpecificInfo, tag 0x05) from an
// MPEG-4 ESDS descriptor as stored in CAF 'kuki' chunks.
std::vector<uint8_t> esds_asc(const std::vector<uint8_t>& c) {
    for (size_t i = 0; i + 2 < c.size(); i++) {
        if (c[i] != 0x05) continue;
        size_t p = i + 1, len = 0;
        for (int k = 0; k < 4 && p < c.size(); k++) {
            uint8_t b = c[p++];
            len = (len << 7) | (b & 0x7f);
            if (!(b & 0x80)) break;
        }
        if (len > 0 && len <= 8 && p + len <= c.size()) return std::vector<uint8_t>(c.begin() + p, c.begin() + p + len);
    }
    return {};
}

struct AacDecoder {
    AMediaCodec* codec = nullptr;
    int out_rate = 0, out_channels = 0;
    bool started = false;
    int64_t pts = 0;

    bool init(const ASBD& fmt, const std::vector<uint8_t>& cookie) {
        std::vector<uint8_t> asc = esds_asc(cookie);
        if (asc.empty()) {
            // Build an AAC-LC config from the stream description.
            static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
            int idx = 4;
            for (int i = 0; i < 12; i++)
                if ((int)fmt.mSampleRate == rates[i]) idx = i;
            asc = {(uint8_t)((2 << 3) | (idx >> 1)), (uint8_t)(((idx & 1) << 7) | (fmt.mChannelsPerFrame << 3))};
        }
        codec = AMediaCodec_createDecoderByType("audio/mp4a-latm");
        if (!codec) {
            LOGE("no AAC decoder available");
            return false;
        }
        AMediaFormat* f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, (int32_t)fmt.mSampleRate);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, (int32_t)fmt.mChannelsPerFrame);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_IS_ADTS, 0);
        AMediaFormat_setBuffer(f, "csd-0", asc.data(), asc.size());
        media_status_t st = AMediaCodec_configure(codec, f, nullptr, nullptr, 0);
        AMediaFormat_delete(f);
        if (st != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
            LOGE("AAC decoder configure/start failed (%d)", st);
            AMediaCodec_delete(codec);
            codec = nullptr;
            return false;
        }
        out_rate = (int)fmt.mSampleRate;
        out_channels = (int)fmt.mChannelsPerFrame;
        started = true;
        return true;
    }

    // Feed one packet (or EOS when data == nullptr) and collect decoded PCM.
    void feed(const uint8_t* data, size_t n, std::vector<int16_t>& pcm) {
        if (!codec) return;
        for (int tries = 0; tries < 50; tries++) {
            ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 2000);
            if (in >= 0) {
                size_t cap;
                uint8_t* buf = AMediaCodec_getInputBuffer(codec, in, &cap);
                size_t k = data ? std::min(n, cap) : 0;
                if (k) memcpy(buf, data, k);
                AMediaCodec_queueInputBuffer(codec, in, 0, k, pts, data ? 0 : AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                pts += 1024 * 1000000LL / std::max(out_rate, 1);
                break;
            }
            drain(pcm, 0);
        }
        drain(pcm, 0);
    }

    bool drain(std::vector<int16_t>& pcm, int64_t timeout_us) {
        for (;;) {
            AMediaCodecBufferInfo info;
            ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout_us);
            if (out >= 0) {
                size_t sz;
                uint8_t* buf = AMediaCodec_getOutputBuffer(codec, out, &sz);
                if (buf && info.size > 0) {
                    const int16_t* s = (const int16_t*)(buf + info.offset);
                    pcm.insert(pcm.end(), s, s + info.size / 2);
                }
                AMediaCodec_releaseOutputBuffer(codec, out, false);
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return true;
            } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
                int32_t r = 0, c = 0;
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &r);
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &c);
                AMediaFormat_delete(f);
                if (r) out_rate = r;
                if (c) out_channels = c;
            } else if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                return false;
            }
        }
    }

    void end_of_stream(std::vector<int16_t>& pcm) {
        if (!codec) return;
        feed(nullptr, 0, pcm);
        for (int i = 0; i < 20; i++)
            if (drain(pcm, 5000)) break;
        AMediaCodec_flush(codec);
        pts = 0;
    }

    void reset() {
        if (codec) AMediaCodec_flush(codec);
        pts = 0;
    }

    ~AacDecoder() {
        if (codec) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
        }
    }
};

// =============================================================================
// AudioQueue
// =============================================================================
struct AQBuffer {
    uint32_t mAudioDataBytesCapacity;
    void* mAudioData;
    uint32_t mAudioDataByteSize;
    void* mUserData;
    uint32_t mPacketDescriptionCapacity;
    PacketDesc* mPacketDescriptions;
    uint32_t mPacketDescriptionCount;
};
static_assert(offsetof(AQBuffer, mAudioDataByteSize) == 8 && offsetof(AQBuffer, mPacketDescriptions) == 20 &&
                  offsetof(AQBuffer, mPacketDescriptionCount) == 24,
              "ARM32 AudioQueueBuffer layout");

typedef void (*AQOutputCallback)(void* user, void* aq, AQBuffer* buf);
typedef void (*AQPropertyListener)(void* user, void* aq, uint32_t prop);

struct AQueue {
    ASBD fmt{};
    AQOutputCallback cb = nullptr;
    void* user = nullptr;
    std::mutex m;
    std::condition_variable cv;
    std::deque<AQBuffer*> queued;
    std::vector<uint8_t> cookie;
    std::vector<std::pair<AQPropertyListener, void*>> listeners;
    std::recursive_mutex listener_call;  // held while a listener runs
    bool running = false, paused = false, stop_pending = false, quit = false;
    bool notify_running = false;  // pending IsRunning change notification
    float volume = 1.0f;
    std::thread worker;
    std::thread::id worker_id;
    AacDecoder* aac = nullptr;
    mixer::Voice* voice = nullptr;
    int64_t time_base = 0;  // source frames already played before the last clock reset
};

constexpr int kTargetBufferMs = 350;

void decode_buffer(AQueue* q, AQBuffer* b, std::vector<int16_t>& pcm) {
    const uint8_t* data = (const uint8_t*)b->mAudioData;
    if (q->fmt.mFormatID == fourcc("aac ")) {
        if (!q->aac) {
            q->aac = new AacDecoder();
            if (!q->aac->init(q->fmt, q->cookie)) return;
        }
        if (b->mPacketDescriptionCount && b->mPacketDescriptions) {
            for (uint32_t i = 0; i < b->mPacketDescriptionCount; i++) {
                const PacketDesc& d = b->mPacketDescriptions[i];
                if (d.mStartOffset + d.mDataByteSize <= b->mAudioDataByteSize)
                    q->aac->feed(data + d.mStartOffset, d.mDataByteSize, pcm);
            }
        } else if (b->mAudioDataByteSize) {
            q->aac->feed(data, b->mAudioDataByteSize, pcm);
        }
    } else if (q->fmt.mFormatID == fourcc("lpcm")) {
        uint32_t bits = q->fmt.mBitsPerChannel;
        if (bits == 16) {
            const int16_t* s = (const int16_t*)data;
            pcm.insert(pcm.end(), s, s + b->mAudioDataByteSize / 2);
        } else if (bits == 8) {
            for (uint32_t i = 0; i < b->mAudioDataByteSize; i++) pcm.push_back((int16_t)((data[i] - 128) << 8));
        }
    } else {
        static bool warned = false;
        if (!warned) LOGW("AudioQueue: unsupported format %08x", q->fmt.mFormatID);
        warned = true;
    }
}

void notify_listeners(AQueue* q) {
    std::lock_guard<std::recursive_mutex> call(q->listener_call);
    std::vector<std::pair<AQPropertyListener, void*>> ls;
    {
        std::lock_guard<std::mutex> lk(q->m);
        ls = q->listeners;
    }
    void* pool = objc::pool_push();
    for (auto& l : ls) l.first(l.second, q, fourcc("aqrn"));
    objc::pool_pop(pool);
}

void worker_main(AQueue* q) {
    std::vector<int16_t> pcm;
    std::unique_lock<std::mutex> lk(q->m);
    for (;;) {
        q->cv.wait_for(lk, std::chrono::milliseconds(20), [q] {
            return q->quit || q->notify_running ||
                   (q->running && !q->paused && !q->queued.empty() && mixer::queued_ms(q->voice) < kTargetBufferMs) ||
                   (q->stop_pending && q->queued.empty());
        });
        if (q->quit) break;
        if (q->notify_running) {
            q->notify_running = false;
            lk.unlock();
            notify_listeners(q);
            lk.lock();
            continue;
        }
        if (q->stop_pending && q->queued.empty()) {
            // Play out what is buffered, then report that the queue stopped.
            pcm.clear();
            if (q->aac) q->aac->end_of_stream(pcm);
            int ch = q->aac ? q->aac->out_channels : (int)q->fmt.mChannelsPerFrame;
            if (!pcm.empty()) mixer::push(q->voice, pcm.data(), (int)pcm.size() / std::max(ch, 1));
            lk.unlock();
            while (mixer::queued_ms(q->voice) > 0 && !q->quit) usleep(10000);
            lk.lock();
            q->stop_pending = false;
            q->running = false;
            lk.unlock();
            notify_listeners(q);
            lk.lock();
            continue;
        }
        if (!(q->running && !q->paused && !q->queued.empty() && mixer::queued_ms(q->voice) < kTargetBufferMs)) continue;
        AQBuffer* b = q->queued.front();
        q->queued.pop_front();
        lk.unlock();
        pcm.clear();
        decode_buffer(q, b, pcm);
        int ch = q->aac ? q->aac->out_channels : (int)q->fmt.mChannelsPerFrame;
        if (!pcm.empty()) mixer::push(q->voice, pcm.data(), (int)pcm.size() / std::max(ch, 1));
        void* pool = objc::pool_push();
        q->cb(q->user, q, b);  // the game refills and re-enqueues the buffer
        objc::pool_pop(pool);
        lk.lock();
    }
}

OSStatus AudioQueueNewOutput(const ASBD* fmt, AQOutputCallback cb, void* user, void*, void*, uint32_t, AQueue** out) {
    auto* q = new AQueue();
    q->fmt = *fmt;
    q->cb = cb;
    q->user = user;
    q->voice = mixer::create_voice((int)fmt->mSampleRate, (int)fmt->mChannelsPerFrame);
    mixer::set_paused(q->voice, true);
    q->worker = std::thread(worker_main, q);
    q->worker_id = q->worker.get_id();
    LOGI("AudioQueueNewOutput %p: %.0f Hz, %u ch, format %08x", q, fmt->mSampleRate, fmt->mChannelsPerFrame,
         fmt->mFormatID);
    *out = q;
    return 0;
}

OSStatus AudioQueueDispose(AQueue* q, bool) {
    if (!q) return 0;
    {
        std::lock_guard<std::mutex> lk(q->m);
        q->quit = true;
    }
    q->cv.notify_all();
    if (std::this_thread::get_id() == q->worker_id) q->worker.detach();
    else q->worker.join();
    mixer::destroy_voice(q->voice);
    delete q->aac;
    delete q;
    return 0;
}

OSStatus AudioQueueAllocateBufferWithPacketDescriptions(AQueue*, uint32_t size, uint32_t ndescs, AQBuffer** out) {
    auto* b = (AQBuffer*)calloc(1, sizeof(AQBuffer));
    b->mAudioDataBytesCapacity = size;
    b->mAudioData = calloc(1, size ? size : 1);
    b->mPacketDescriptionCapacity = ndescs;
    b->mPacketDescriptions = ndescs ? (PacketDesc*)calloc(ndescs, sizeof(PacketDesc)) : nullptr;
    *out = b;
    return 0;
}
OSStatus AudioQueueAllocateBuffer(AQueue* q, uint32_t size, AQBuffer** out) {
    return AudioQueueAllocateBufferWithPacketDescriptions(q, size, 0, out);
}
OSStatus AudioQueueFreeBuffer(AQueue* q, AQBuffer* b) {
    if (!b) return 0;
    if (q) {
        std::lock_guard<std::mutex> lk(q->m);
        for (size_t i = 0; i < q->queued.size(); i++)
            if (q->queued[i] == b) {
                q->queued.erase(q->queued.begin() + i);
                break;
            }
    }
    free(b->mAudioData);
    free(b->mPacketDescriptions);
    free(b);
    return 0;
}

OSStatus AudioQueueEnqueueBuffer(AQueue* q, AQBuffer* b, uint32_t ndescs, const PacketDesc* descs) {
    if (!q || !b) return -50;
    if (ndescs && descs) {
        if (!b->mPacketDescriptions || b->mPacketDescriptionCapacity < ndescs) {
            free(b->mPacketDescriptions);
            b->mPacketDescriptions = (PacketDesc*)calloc(ndescs, sizeof(PacketDesc));
            b->mPacketDescriptionCapacity = ndescs;
        }
        memcpy(b->mPacketDescriptions, descs, ndescs * sizeof(PacketDesc));
        b->mPacketDescriptionCount = ndescs;
    }
    {
        std::lock_guard<std::mutex> lk(q->m);
        q->queued.push_back(b);
    }
    q->cv.notify_all();
    return 0;
}

OSStatus AudioQueueStart(AQueue* q, const void*) {
    if (!q) return -50;
    bool changed;
    {
        std::lock_guard<std::mutex> lk(q->m);
        changed = !q->running;
        if (changed) {
            mixer::reset_clock(q->voice);
            q->time_base = 0;
        }
        q->running = true;
        q->paused = false;
        q->stop_pending = false;
        if (changed) q->notify_running = true;
    }
    mixer::set_volume(q->voice, q->volume);
    mixer::set_paused(q->voice, false);
    q->cv.notify_all();
    return 0;
}

OSStatus AudioQueuePrime(AQueue*, uint32_t, uint32_t* prepared) {
    if (prepared) *prepared = 0;
    return 0;
}

OSStatus AudioQueuePause(AQueue* q) {
    if (!q) return -50;
    std::lock_guard<std::mutex> lk(q->m);
    q->paused = true;
    mixer::set_paused(q->voice, true);
    return 0;
}

OSStatus AudioQueueStop(AQueue* q, bool immediate) {
    if (!q) return -50;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(q->m);
        if (!q->running && !q->stop_pending) return 0;
        if (immediate) {
            q->queued.clear();
            q->stop_pending = false;
            q->running = false;
            q->paused = false;
            mixer::clear(q->voice);
            mixer::set_paused(q->voice, true);
            if (q->aac) q->aac->reset();
            q->notify_running = true;
            notify = true;
        } else {
            q->stop_pending = true;
        }
    }
    q->cv.notify_all();
    (void)notify;
    return 0;
}

OSStatus AudioQueueFlush(AQueue*) { return 0; }

OSStatus AudioQueueSetParameter(AQueue* q, uint32_t param, float value) {
    if (!q) return -50;
    if (param == 1) {  // kAudioQueueParam_Volume
        q->volume = value;
        mixer::set_volume(q->voice, value);
    }
    return 0;
}
OSStatus AudioQueueGetParameter(AQueue* q, uint32_t param, float* value) {
    if (!q || !value) return -50;
    *value = param == 1 ? q->volume : 0.0f;
    return 0;
}

OSStatus AudioQueueSetProperty(AQueue* q, uint32_t prop, const void* data, uint32_t size) {
    if (!q) return -50;
    if (prop == fourcc("aqmc")) {
        std::lock_guard<std::mutex> lk(q->m);
        q->cookie.assign((const uint8_t*)data, (const uint8_t*)data + size);
    }
    return 0;
}
OSStatus AudioQueueGetProperty(AQueue* q, uint32_t prop, void* data, uint32_t* size) {
    if (!q) return -50;
    if (prop == fourcc("aqrn")) {
        uint32_t v;
        {
            std::lock_guard<std::mutex> lk(q->m);
            v = q->running ? 1 : 0;
        }
        memcpy(data, &v, std::min<uint32_t>(*size, 4));
        *size = 4;
        return 0;
    }
    if (prop == fourcc("aqsr")) {
        double v = q->fmt.mSampleRate;
        memcpy(data, &v, std::min<uint32_t>(*size, 8));
        *size = 8;
        return 0;
    }
    return kAudioQueueErr_InvalidProperty;
}
OSStatus AudioQueueAddPropertyListener(AQueue* q, uint32_t, AQPropertyListener fn, void* user) {
    if (!q) return -50;
    std::lock_guard<std::mutex> lk(q->m);
    q->listeners.push_back({fn, user});
    return 0;
}
OSStatus AudioQueueRemovePropertyListener(AQueue* q, uint32_t, AQPropertyListener fn, void* user) {
    if (!q) return -50;
    // Wait for an in-flight notification so the listener's owner can be freed.
    std::lock_guard<std::recursive_mutex> call(q->listener_call);
    std::lock_guard<std::mutex> lk(q->m);
    q->notify_running = false;
    for (size_t i = 0; i < q->listeners.size(); i++)
        if (q->listeners[i].first == fn && q->listeners[i].second == user) {
            q->listeners.erase(q->listeners.begin() + i);
            break;
        }
    return 0;
}

int g_timeline_dummy;
OSStatus AudioQueueCreateTimeline(AQueue*, void** out) {
    *out = &g_timeline_dummy;
    return 0;
}
OSStatus AudioQueueDisposeTimeline(AQueue*, void*) { return 0; }

struct AudioTimeStamp {
    double mSampleTime;
    uint64_t mHostTime;
    double mRateScalar;
    uint64_t mWordClockTime;
    uint8_t mSMPTETime[24];
    uint32_t mFlags;
    uint32_t mReserved;
};
static_assert(sizeof(AudioTimeStamp) == 64, "AudioTimeStamp");

OSStatus AudioQueueGetCurrentTime(AQueue* q, void*, AudioTimeStamp* t, uint8_t* discontinuity) {
    if (!q) return -50;
    {
        std::lock_guard<std::mutex> lk(q->m);
        if (!q->running) return kAudioQueueErr_InvalidRunState;
    }
    if (t) {
        memset(t, 0, sizeof *t);
        t->mSampleTime = (double)(q->time_base + mixer::played_frames(q->voice));
        t->mRateScalar = 1.0;
        t->mFlags = 1;  // sample time valid
    }
    if (discontinuity) *discontinuity = 0;
    return 0;
}

// =============================================================================
// AudioSession
// =============================================================================
typedef void (*InterruptionListener)(void* user, uint32_t state);
InterruptionListener g_interruption = nullptr;
void* g_interruption_user = nullptr;

OSStatus AudioSessionInitialize(void*, void*, InterruptionListener l, void* user) {
    g_interruption = l;
    g_interruption_user = user;
    return 0;
}
OSStatus AudioSessionAddPropertyListener(uint32_t, void*, void*) { return 0; }
OSStatus AudioSessionSetProperty(uint32_t, uint32_t, const void*) { return 0; }
OSStatus AudioSessionSetActive(bool) { return 0; }
OSStatus AudioSessionGetProperty(uint32_t prop, uint32_t* size, void* data) {
    if (!size || !data) return 0;
    memset(data, 0, *size);
    if (prop == fourcc("chan") && *size >= 4) *(uint32_t*)data = 2;  // output channels
    if (prop == fourcc("cshw") && *size >= 8) {
        double sr = 48000;
        memcpy(data, &sr, 8);
    }
    return 0;
}

}  // namespace

namespace audiotoolbox {
// Called by the platform on app pause/resume (kAudioSessionBegin/EndInterruption).
void interruption(bool begin) {
    if (g_interruption) g_interruption(g_interruption_user, begin ? 1 : 0);
    mixer::pause_device(begin);
}
}  // namespace audiotoolbox

HLE_EXPORT(AudioSessionInitialize, AudioSessionInitialize);
HLE_EXPORT(AudioSessionAddPropertyListener, AudioSessionAddPropertyListener);
HLE_EXPORT(AudioSessionSetProperty, AudioSessionSetProperty);
HLE_EXPORT(AudioSessionSetActive, AudioSessionSetActive);
HLE_EXPORT(AudioSessionGetProperty, AudioSessionGetProperty);
HLE_EXPORT(AudioFileOpenURL, AudioFileOpenURL);
HLE_EXPORT(AudioFileClose, AudioFileClose);
HLE_EXPORT(AudioFileGetProperty, AudioFileGetProperty);
HLE_EXPORT(AudioFileGetPropertyInfo, AudioFileGetPropertyInfo);
HLE_EXPORT(AudioFileReadBytes, AudioFileReadBytes);
HLE_EXPORT(AudioFileReadPackets, AudioFileReadPackets);
HLE_EXPORT(AudioQueueNewOutput, AudioQueueNewOutput);
HLE_EXPORT(AudioQueueDispose, AudioQueueDispose);
HLE_EXPORT(AudioQueueAllocateBuffer, AudioQueueAllocateBuffer);
HLE_EXPORT(AudioQueueAllocateBufferWithPacketDescriptions, AudioQueueAllocateBufferWithPacketDescriptions);
HLE_EXPORT(AudioQueueFreeBuffer, AudioQueueFreeBuffer);
HLE_EXPORT(AudioQueueEnqueueBuffer, AudioQueueEnqueueBuffer);
HLE_EXPORT(AudioQueueStart, AudioQueueStart);
HLE_EXPORT(AudioQueuePrime, AudioQueuePrime);
HLE_EXPORT(AudioQueuePause, AudioQueuePause);
HLE_EXPORT(AudioQueueStop, AudioQueueStop);
HLE_EXPORT(AudioQueueFlush, AudioQueueFlush);
HLE_EXPORT(AudioQueueSetParameter, AudioQueueSetParameter);
HLE_EXPORT(AudioQueueGetParameter, AudioQueueGetParameter);
HLE_EXPORT(AudioQueueSetProperty, AudioQueueSetProperty);
HLE_EXPORT(AudioQueueGetProperty, AudioQueueGetProperty);
HLE_EXPORT(AudioQueueAddPropertyListener, AudioQueueAddPropertyListener);
HLE_EXPORT(AudioQueueRemovePropertyListener, AudioQueueRemovePropertyListener);
HLE_EXPORT(AudioQueueCreateTimeline, AudioQueueCreateTimeline);
HLE_EXPORT(AudioQueueDisposeTimeline, AudioQueueDisposeTimeline);
HLE_EXPORT(AudioQueueGetCurrentTime, AudioQueueGetCurrentTime);
