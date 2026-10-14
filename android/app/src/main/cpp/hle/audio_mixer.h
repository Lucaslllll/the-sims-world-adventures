// Software mixer for AudioQueue output, on top of an SDL audio device.
#pragma once

#include <cstdint>

namespace mixer {

struct Voice;

// Voice fed with interleaved signed 16-bit PCM at (rate, channels).
Voice* create_voice(int rate, int channels);
void destroy_voice(Voice* v);
void push(Voice* v, const int16_t* pcm, int frames);
void clear(Voice* v);
void set_paused(Voice* v, bool paused);
void set_volume(Voice* v, float volume);
// Milliseconds of audio queued but not yet played.
int queued_ms(Voice* v);
// Source frames played since the last reset_clock().
int64_t played_frames(Voice* v);
void reset_clock(Voice* v);

// Pause/resume the whole device (app lifecycle).
void pause_device(bool paused);

}  // namespace mixer
