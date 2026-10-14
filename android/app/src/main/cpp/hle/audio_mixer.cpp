#include "hle/audio_mixer.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "hle/common.h"

namespace mixer {

struct Voice {
    std::mutex m;
    SDL_AudioStream* stream = nullptr;
    int rate = 0, channels = 0;
    std::atomic<float> volume{1.0f};
    std::atomic<bool> paused{false};
    std::atomic<int64_t> out_frames{0};  // device frames played since reset
};

namespace {

std::mutex g_mutex;  // protects g_voices
std::vector<Voice*> g_voices;
SDL_AudioDeviceID g_dev = 0;
SDL_AudioSpec g_spec;
std::vector<float> g_tmp;

void SDLCALL callback(void*, Uint8* stream, int len) {
    float* out = (float*)stream;
    int samples = len / (int)sizeof(float);
    std::fill(out, out + samples, 0.0f);
    if ((int)g_tmp.size() < samples) g_tmp.resize(samples);
    std::lock_guard<std::mutex> lk(g_mutex);
    for (Voice* v : g_voices) {
        if (v->paused) continue;
        int got;
        {
            std::lock_guard<std::mutex> vl(v->m);
            got = SDL_AudioStreamGet(v->stream, g_tmp.data(), len);
        }
        if (got <= 0) continue;
        int n = got / (int)sizeof(float);
        float vol = v->volume;
        for (int i = 0; i < n; i++) out[i] += g_tmp[i] * vol;
        v->out_frames += n / g_spec.channels;
    }
    for (int i = 0; i < samples; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

bool ensure_device() {
    if (g_dev) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOGE("SDL audio init failed: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &g_spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_dev) {
        LOGE("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    LOGI("music mixer: %d Hz, %d ch, %d samples (%s)", g_spec.freq, g_spec.channels, g_spec.samples,
         SDL_GetCurrentAudioDriver());
    SDL_PauseAudioDevice(g_dev, 0);
    return true;
}

}  // namespace

Voice* create_voice(int rate, int channels) {
    if (!ensure_device()) return nullptr;
    auto* v = new Voice();
    v->rate = rate;
    v->channels = channels;
    v->stream = SDL_NewAudioStream(AUDIO_S16SYS, (Uint8)channels, rate, g_spec.format, g_spec.channels, g_spec.freq);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_voices.push_back(v);
    return v;
}

void destroy_voice(Voice* v) {
    if (!v) return;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_voices.erase(std::remove(g_voices.begin(), g_voices.end(), v), g_voices.end());
    }
    SDL_FreeAudioStream(v->stream);
    delete v;
}

void push(Voice* v, const int16_t* pcm, int frames) {
    if (!v || frames <= 0) return;
    std::lock_guard<std::mutex> vl(v->m);
    SDL_AudioStreamPut(v->stream, pcm, frames * v->channels * (int)sizeof(int16_t));
}

void clear(Voice* v) {
    if (!v) return;
    std::lock_guard<std::mutex> vl(v->m);
    SDL_AudioStreamClear(v->stream);
}

void set_paused(Voice* v, bool p) {
    if (v) v->paused = p;
}
void set_volume(Voice* v, float vol) {
    if (v) v->volume = vol;
}

int queued_ms(Voice* v) {
    if (!v) return 0;
    int bytes;
    {
        std::lock_guard<std::mutex> vl(v->m);
        bytes = SDL_AudioStreamAvailable(v->stream);
    }
    int frames = bytes / (int)(sizeof(float) * g_spec.channels);
    return (int)((int64_t)frames * 1000 / g_spec.freq);
}

int64_t played_frames(Voice* v) {
    if (!v) return 0;
    return v->out_frames.load() * v->rate / g_spec.freq;
}

void reset_clock(Voice* v) {
    if (v) v->out_frames = 0;
}

void pause_device(bool paused) {
    if (g_dev) SDL_PauseAudioDevice(g_dev, paused ? 1 : 0);
}

}  // namespace mixer
