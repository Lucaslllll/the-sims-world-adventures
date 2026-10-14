// OpenAL: forwarded to OpenAL Soft (identical API/ABI). Apple's
// alBufferDataStatic extension is emulated with a copying alBufferData.
#include <AL/al.h>
#include <AL/alc.h>

#include <cstring>

#include "hle/common.h"

namespace {

void AL_APIENTRY buffer_data_static(ALuint buffer, ALenum format, const ALvoid* data, ALsizei size, ALsizei freq) {
    alBufferData(buffer, format, data, size, freq);
}

void* AL_APIENTRY get_proc_address(const ALchar* name) {
    if (!strcmp(name, "alBufferDataStatic")) return (void*)buffer_data_static;
    void* p = alGetProcAddress(name);
    if (!p) LOGW("alGetProcAddress(%s) -> NULL", name);
    return p;
}

ALCdevice* g_device = nullptr;

ALCdevice* ALC_APIENTRY open_device(const ALCchar* name) {
    ALCdevice* d = alcOpenDevice(nullptr);
    if (!g_device) g_device = d;
    LOGI("alcOpenDevice(%s) -> %p", name ? name : "default", d);
    return d;
}

}  // namespace

// Pause/resume OpenAL output while the app is in the background.
void openal_pause(bool paused) {
    if (!g_device || !alcIsExtensionPresent(g_device, "ALC_SOFT_pause_device")) return;
    typedef void(ALC_APIENTRY * PauseFn)(ALCdevice*);
    auto fn = (PauseFn)alcGetProcAddress(g_device, paused ? "alcDevicePauseSOFT" : "alcDeviceResumeSOFT");
    if (fn) fn(g_device);
}

HLE_EXPORT(alDeleteBuffers, alDeleteBuffers);
HLE_EXPORT(alDeleteSources, alDeleteSources);
HLE_EXPORT(alGenBuffers, alGenBuffers);
HLE_EXPORT(alGenSources, alGenSources);
HLE_EXPORT(alGetError, alGetError);
HLE_EXPORT(alGetProcAddress, get_proc_address);
HLE_EXPORT(alGetSourcef, alGetSourcef);
HLE_EXPORT(alGetSourcei, alGetSourcei);
HLE_EXPORT(alListener3f, alListener3f);
HLE_EXPORT(alListenerfv, alListenerfv);
HLE_EXPORT(alSourcePlay, alSourcePlay);
HLE_EXPORT(alSourceRewind, alSourceRewind);
HLE_EXPORT(alSourceStop, alSourceStop);
HLE_EXPORT(alSourcePause, alSourcePause);
HLE_EXPORT(alSourcef, alSourcef);
HLE_EXPORT(alSourcefv, alSourcefv);
HLE_EXPORT(alSourcei, alSourcei);
HLE_EXPORT(alSource3f, alSource3f);
HLE_EXPORT(alBufferData, alBufferData);
HLE_EXPORT(alcCloseDevice, alcCloseDevice);
HLE_EXPORT(alcCreateContext, alcCreateContext);
HLE_EXPORT(alcDestroyContext, alcDestroyContext);
HLE_EXPORT(alcGetCurrentContext, alcGetCurrentContext);
HLE_EXPORT(alcMakeContextCurrent, alcMakeContextCurrent);
HLE_EXPORT(alcOpenDevice, open_device);
HLE_EXPORT(alcProcessContext, alcProcessContext);
HLE_EXPORT(alcSuspendContext, alcSuspendContext);
