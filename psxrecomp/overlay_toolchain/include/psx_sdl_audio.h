#pragma once

#include "psx_sdl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (SDLCALL *PsxSdlAudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct PsxSdlAudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint16 samples;
    int allow_frequency_change;
    PsxSdlAudioCallback callback;
    void *userdata;
    /* Output device by its SDL name (as SDL lists it); NULL or empty = the
     * system default. A name that is not connected falls back to the default. */
    const char *device_name;
    /* Out (have): 1 when the named device was the one opened. */
    int named_device_used;
} PsxSdlAudioSpec;

SDL_AudioDeviceID psx_sdl_audio_open(
    const PsxSdlAudioSpec *want, PsxSdlAudioSpec *have);
int psx_sdl_audio_resume(SDL_AudioDeviceID device);
void psx_sdl_audio_close(SDL_AudioDeviceID device);
void psx_sdl_audio_clear(SDL_AudioDeviceID device);
Uint32 psx_sdl_audio_queued_size(SDL_AudioDeviceID device);
int psx_sdl_audio_queue(
    SDL_AudioDeviceID device, const void *data, Uint32 bytes);
void psx_sdl_audio_lock(SDL_AudioDeviceID device);
void psx_sdl_audio_unlock(SDL_AudioDeviceID device);

#ifdef __cplusplus
}
#endif
