/* Host music channel for offline title mods (mod_plugins.h, psx_mod_music_*).
 *
 * A stereo 16-bit PCM track mixed into the speakers on the audio device's own
 * clock (main.cpp: the bridge's pull callback, or the legacy push pump). It
 * touches no emulated state: no SPU register, no SPU or guest RAM, no guest
 * cycles. So it keeps playing through loads, fast forward, the turbo mute and
 * stalls, and savestates, rewind and netplay never see it.
 *
 * Two decks. play() on a busy channel puts the new track on the other deck
 * and crossfades; the outgoing deck is freed once its fade is over (on the
 * next control call, never on the audio thread).
 *
 * Threads: control calls from any thread; psx_host_music_mix() from the audio
 * thread. A spinlock guards the deck table; nothing allocates or frees while
 * holding it. */
#include "mod_plugins.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct HostMusicDeck {
    int16_t *pcm;              /* interleaved L/R, owned */
    uint32_t frames, rate, loop_start;
    uint64_t pos;              /* 32.32 fixed point, in source frames */
    float gain, gain_to, gain_step;   /* fade; step per output frame */
    int stop_at_zero;          /* free the deck once the fade reaches 0 */
    int live;
} HostMusicDeck;

static HostMusicDeck s_deck[2];
static float s_level = 1.0f, s_level_to = 1.0f, s_level_step = 0.0f;
static uint32_t s_out_rate = 44100u;
static volatile int s_lock;

static void lock(void)   { while (__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE)) { } }
static void unlock(void) { __atomic_clear(&s_lock, __ATOMIC_RELEASE); }

static float fade_step(float from, float to, uint32_t ms)
{
    const float n = (float)s_out_rate * (float)ms / 1000.0f;
    return n < 1.0f ? (to - from) : (to - from) / n;
}

/* Decks that finished fading out: detach under the lock, free outside it. */
static void reap(void)
{
    int16_t *dead[2] = { NULL, NULL };
    lock();
    for (int i = 0; i < 2; i++) {
        HostMusicDeck *d = &s_deck[i];
        if (d->live && d->stop_at_zero && d->gain <= 0.0f) {
            dead[i] = d->pcm;
            memset(d, 0, sizeof *d);
        }
    }
    unlock();
    free(dead[0]);
    free(dead[1]);
}

int psx_mod_music_play(const int16_t *pcm, uint32_t frames, uint32_t rate,
                       uint32_t loop_start, uint32_t fade_ms)
{
    if (!pcm || frames == 0u || rate < 8000u || rate > 192000u) return 0;
    if (frames > (UINT32_MAX / 4u)) return 0;
    reap();
    int16_t *copy = (int16_t *)malloc((size_t)frames * 4u);
    if (!copy) return 0;
    memcpy(copy, pcm, (size_t)frames * 4u);
    int16_t *dead = NULL;
    lock();
    /* The quieter deck takes the new track (a free one if there is one). */
    int k = !s_deck[0].live ? 0 : !s_deck[1].live ? 1 : (s_deck[0].gain <= s_deck[1].gain ? 0 : 1);
    HostMusicDeck *d = &s_deck[k];
    dead = d->pcm;
    memset(d, 0, sizeof *d);
    d->pcm = copy;
    d->frames = frames;
    d->rate = rate;
    d->loop_start = loop_start;
    d->gain = fade_ms ? 0.0f : 1.0f;
    d->gain_to = 1.0f;
    d->gain_step = fade_step(d->gain, 1.0f, fade_ms);
    d->live = 1;
    HostMusicDeck *o = &s_deck[!k];
    if (o->live) {
        o->gain_to = 0.0f;
        o->gain_step = fade_step(o->gain, 0.0f, fade_ms);
        o->stop_at_zero = 1;
    }
    unlock();
    free(dead);
    return 1;
}

void psx_mod_music_stop(uint32_t fade_ms)
{
    reap();
    lock();
    for (int i = 0; i < 2; i++) {
        HostMusicDeck *d = &s_deck[i];
        if (!d->live) continue;
        d->gain_to = 0.0f;
        d->gain_step = fade_step(d->gain, 0.0f, fade_ms);
        d->stop_at_zero = 1;
        if (!fade_ms) d->gain = 0.0f;
    }
    unlock();
    if (!fade_ms) reap();
}

void psx_mod_music_level(uint32_t permille, uint32_t fade_ms)
{
    if (permille > 1000u) permille = 1000u;
    lock();
    s_level_to = (float)permille / 1000.0f;
    s_level_step = fade_step(s_level, s_level_to, fade_ms);
    if (!fade_ms) s_level = s_level_to;
    unlock();
}

int psx_mod_music_active(void)
{
    reap();
    lock();
    const int on = (s_deck[0].live && !(s_deck[0].stop_at_zero && s_deck[0].gain <= 0.0f)) ||
                   (s_deck[1].live && !(s_deck[1].stop_at_zero && s_deck[1].gain <= 0.0f));
    unlock();
    return on;
}

static float approach(float v, float to, float step)
{
    if (step == 0.0f) return to;
    v += step;
    if ((step > 0.0f && v > to) || (step < 0.0f && v < to)) v = to;
    return v;
}

/* Audio thread: add the channel to `out` (frames at rate Hz, interleaved
 * stereo) at the player's volume (0..100). */
void psx_host_music_mix(int16_t *out, int frames, int rate, int volume_pct)
{
    if (!out || frames <= 0 || rate <= 0) return;
    lock();
    s_out_rate = (uint32_t)rate;
    if (!s_deck[0].live && !s_deck[1].live) { unlock(); return; }
    const float vol = (float)(volume_pct < 0 ? 0 : volume_pct > 100 ? 100 : volume_pct) / 100.0f;
    for (int f = 0; f < frames; f++) {
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < 2; i++) {
            HostMusicDeck *d = &s_deck[i];
            if (!d->live || (d->stop_at_zero && d->gain <= 0.0f)) continue;
            uint32_t at = (uint32_t)(d->pos >> 32);
            if (at >= d->frames) {
                if (d->loop_start < d->frames) {
                    d->pos = ((uint64_t)d->loop_start << 32) | (d->pos & 0xFFFFFFFFu);
                    at = d->loop_start;
                } else {                      /* played once: silent until reaped */
                    d->gain = 0.0f;
                    d->stop_at_zero = 1;
                    continue;
                }
            }
            uint32_t nx = at + 1u;
            if (nx >= d->frames) nx = d->loop_start < d->frames ? d->loop_start : at;
            const float t = (float)(uint32_t)(d->pos & 0xFFFFFFFFu) * (1.0f / 4294967296.0f);
            const int16_t *a = d->pcm + 2u * at, *b = d->pcm + 2u * nx;
            const float g = d->gain;
            l += ((float)a[0] + ((float)b[0] - (float)a[0]) * t) * g;
            r += ((float)a[1] + ((float)b[1] - (float)a[1]) * t) * g;
            d->pos += ((uint64_t)d->rate << 32) / (uint64_t)rate;
            if (d->gain != d->gain_to) d->gain = approach(d->gain, d->gain_to, d->gain_step);
        }
        if (s_level != s_level_to) s_level = approach(s_level, s_level_to, s_level_step);
        const float k = s_level * vol;
        int32_t ol = (int32_t)out[2 * f] + (int32_t)(l * k);
        int32_t orr = (int32_t)out[2 * f + 1] + (int32_t)(r * k);
        out[2 * f] = (int16_t)(ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol);
        out[2 * f + 1] = (int16_t)(orr > 32767 ? 32767 : orr < -32768 ? -32768 : orr);
    }
    unlock();
}
