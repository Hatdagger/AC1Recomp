/* Rogue Arena victory sounds (moa_chime.h).
 *
 * Made here, not taken from the disc. Heavy Japanese rock with a mechanical
 * edge (fighting-game victory sting):
 * - the sting (about 1.85 s, quick): a hit on E, a fast rising lead run
 *   over chugs and a snare build, then at 0.72 s (when YOU WIN lands) a big
 *   E power chord with a lead note bent up into the octave, crash, kick and
 *   anvil clank; it fades out fast and ends on a hydraulic hiss;
 * - the type tick: a short mechanical click for each word of the lines.
 * Guitars are plucked strings (Karplus-Strong) through a hard overdrive and
 * a speaker-cabinet filter. Synthesized at 22050 Hz, encoded once to PS1 SPU
 * ADPCM (every filter and shift tried per block, the closest kept),
 * registered as private sample banks and played on private voices: SPU RAM,
 * the game's voices and its registers are never touched. */
#include "moa_chime.h"
#include "mod_plugins.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RATE        22050
#define BANK_STING  31u            /* private sample banks (1..31) */
#define BANK_TICK   30u
#define SLOT_STING  23u            /* private voice slots (0..23) */
#define SLOT_TICK   22u
#define TAU         6.2831853f

struct snd { uint8_t *adpcm; uint32_t bytes; };
static struct snd s_sting, s_tick;

static uint32_t s_noise = 0x1234567u;
static float noise(void)
{
    s_noise = s_noise * 1664525u + 1013904223u;
    return (float)(int32_t)s_noise / 2147483648.0f;
}

/* ---- guitar ------------------------------------------------------------------- */
/* One plucked string into `o` (clean signal; the amp comes after).
 * damp: 0.5 = open and ringing .. lower = palm muted. len_s: how long it may
 * sound. vib: vibrato depth in semitones (after 0.25 s). */
static void pluck_bend(float *o, int n, float at, float hz, float amp, float damp, float len_s, float vib, float bend)
{
    const int s0 = (int)(at * RATE);
    int d = (int)((float)RATE / hz);
    if (d < 2) d = 2;
    float *line = (float *)calloc((size_t)d + 2u, sizeof *line);
    if (!line) return;
    float lp = 0.0f;
    for (int i = 0; i < d; i++) { lp += 0.6f * (noise() - lp); line[i] = lp; }  /* a softer pick */
    const int end = s0 + (int)(len_s * RATE) < n ? s0 + (int)(len_s * RATE) : n;
    float pos = 0.0f, prev = 0.0f;
    for (int i = s0; i < end; i++) {
        const float t = (float)(i - s0) / RATE;
        /* vibrato by reading the line slightly faster or slower */
        float v = vib > 0.0f && t > 0.25f ? vib * sinf(TAU * 5.5f * t) * fminf(1.0f, (t - 0.25f) * 3.0f) : 0.0f;
        if (bend > 0.0f && t < 0.07f) v -= bend * (1.0f - t / 0.07f);
        const float step = powf(2.0f, v / 12.0f);
        const int k = (int)pos % d, k1 = (k + 1) % d;
        const float cur = line[k];
        const float nxt = (cur + line[k1]) * damp + prev * (1.0f - 2.0f * damp);
        prev = cur;
        line[k] = nxt * 0.999f;
        const float fade = end - i < 400 ? (float)(end - i) / 400.0f : 1.0f;
        o[i] += amp * cur * fade;
        pos += step;
        if (pos >= (float)d * 1000.0f) pos -= (float)d * 1000.0f;
    }
    free(line);
}

static void pluck(float *o, int n, float at, float hz, float amp, float damp, float len_s, float vib)
{
    pluck_bend(o, n, at, hz, amp, damp, len_s, vib, 0.0f);
}

/* A power chord (root, fifth, octave), two takes a hair apart and detuned. */
static void power_chord(float *o, int n, float at, float root_hz, float damp, float len_s, float vib)
{
    static const float k_int[3] = { 1.0f, 1.4983f, 2.0f };
    for (int take = 0; take < 2; take++)
        for (int k = 0; k < 3; k++)
            pluck(o, n, at + 0.004f * (float)take + 0.002f * (float)k, root_hz * k_int[k] * (take ? 1.004f : 0.997f),
                  0.5f, damp, len_s, vib);
}

/* Overdrive and a speaker cabinet over the whole guitar track. */
static void amp_cab(float *g, int n)
{
    float hp = 0.0f, hpx = 0.0f, l1 = 0.0f, l2 = 0.0f, l3 = 0.0f;
    for (int i = 0; i < n; i++) {
        float x = g[i];
        const float y = x - hpx + 0.995f * hp;              /* low cut before the drive */
        hpx = x; hp = y;
        x = tanhf(y * 9.0f);                                 /* hard drive */
        x = 0.7f * x + 0.3f * tanhf(x * 3.0f);
        l1 += 0.42f * (x - l1);                              /* cabinet: ~3.5 kHz roll-off */
        l2 += 0.42f * (l1 - l2);
        l3 += 0.55f * (l2 - l3);
        g[i] = l3;
    }
}

/* ---- drums and machines -------------------------------------------------------- */
static void kick(float *o, int n, float at, float gain)
{
    const int s0 = (int)(at * RATE);
    float ph = 0.0f;
    for (int i = s0; i < n; i++) {
        const float t = (float)(i - s0) / RATE;
        if (t > 0.35f) break;
        ph += (50.0f + 120.0f * expf(-t * 35.0f)) / RATE;
        o[i] += gain * (sinf(TAU * ph) * expf(-t * 11.0f) + 0.3f * noise() * expf(-t * 300.0f));
    }
}

static void crash(float *o, int n, float at, float gain)
{
    const int s0 = (int)(at * RATE);
    float a = 0.0f;
    for (int i = s0; i < n; i++) {
        const float t = (float)(i - s0) / RATE;
        if (t > 1.6f) break;
        const float w = noise();
        a += 0.25f * (w - a);
        o[i] += gain * (w - a) * expf(-t * 3.6f) * (t < 0.003f ? t / 0.003f : 1.0f);
    }
}

static void snare(float *o, int n, float at, float gain)
{
    const int s0 = (int)(at * RATE);
    float ph = 0.0f, a = 0.0f;
    for (int i = s0; i < n; i++) {
        const float t = (float)(i - s0) / RATE;
        if (t > 0.25f) break;
        ph += (190.0f + 60.0f * expf(-t * 60.0f)) / RATE;
        const float w = noise();
        a += 0.35f * (w - a);
        o[i] += gain * (0.55f * sinf(TAU * ph) * expf(-t * 30.0f) + 0.8f * (w - 0.5f * a) * expf(-t * 18.0f));
    }
}

/* A hydraulic hiss with a clunk in front: the machine settling. */
static void hiss(float *o, int n, float at, float len, float gain)
{
    const int s0 = (int)(at * RATE);
    float a = 0.0f, b = 0.0f;
    for (int i = s0; i < n; i++) {
        const float t = (float)(i - s0) / RATE;
        if (t > len) break;
        const float w = noise();
        a += 0.6f * (w - a);
        b += 0.6f * (a - b);
        const float env = (t < 0.01f ? t / 0.01f : 1.0f) * expf(-t * 4.0f / len);
        o[i] += gain * (w - b) * env;
        o[i] += gain * 1.6f * sinf(TAU * 70.0f * t) * expf(-t * 40.0f);
    }
}

/* Metal: inharmonic clang partials and a click. */
static void clank(float *o, int n, float at, float gain)
{
    static const float k_part[5] = { 1.0f, 1.52f, 2.39f, 3.17f, 4.41f };
    const int s0 = (int)(at * RATE);
    for (int i = s0; i < n; i++) {
        const float t = (float)(i - s0) / RATE;
        if (t > 0.9f) break;
        float v = 0.5f * noise() * expf(-t * 120.0f);
        for (int k = 0; k < 5; k++)
            v += 0.2f * sinf(TAU * 420.0f * k_part[k] * t + (float)k) * expf(-t * (8.0f + 4.0f * (float)k));
        o[i] += gain * v;
    }
}

/* The pinch harmonic: a high squeal sliding up a step, with wide vibrato. */
static void squeal(float *o, int n, float at, float hz, float gain)
{
    const int s0 = (int)(at * RATE);
    float ph = 0.0f;
    for (int i = s0; i < n; i++) {
        const float t = (float)(i - s0) / RATE;
        if (t > 1.0f) break;
        const float slide = 1.0f + 0.1225f * fminf(1.0f, t / 0.12f);            /* up a whole step */
        const float vib = 1.0f + 0.03f * sinf(TAU * 7.0f * t) * fminf(1.0f, t * 4.0f);
        ph += hz * slide * vib / RATE;
        const float env = (t < 0.01f ? t / 0.01f : 1.0f) * expf(-t * 2.8f);
        o[i] += gain * env * tanhf(4.0f * (sinf(TAU * ph) + 0.4f * sinf(2.0f * TAU * ph)));
    }
}

/* ---- PS1 SPU ADPCM ---------------------------------------------------------- */
static const int k_f0[5] = { 0, 60, 115, 98, 122 };
static const int k_f1[5] = { 0, 0, -52, -55, -60 };

static void encode_block(const int16_t *in, int *h1, int *h2, uint8_t out[16], uint8_t flags)
{
    int64_t best = -1;
    uint8_t best_out[16];
    int best_h1 = 0, best_h2 = 0;
    for (int f = 0; f < 5; f++)
        for (int shift = 0; shift <= 12; shift++) {
            uint8_t o[16];
            memset(o, 0, sizeof o);
            o[0] = (uint8_t)(shift | f << 4);
            o[1] = flags;
            int a = *h1, b = *h2;
            int64_t err = 0;
            for (int i = 0; i < 28; i++) {
                const int pred = (a * k_f0[f] + b * k_f1[f] + 32) >> 6;
                int nib = (int)lroundf((float)(in[i] - pred) / (float)(1 << (12 - shift)));
                if (nib > 7) nib = 7;
                if (nib < -8) nib = -8;
                int s = (nib << (12 - shift)) + pred;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                const int64_t d = (int64_t)(s - in[i]);
                err += d * d;
                b = a;
                a = s;
                o[2 + i / 2] |= (uint8_t)((nib & 15) << ((i & 1) * 4));
            }
            if (best < 0 || err < best) {
                best = err;
                memcpy(best_out, o, 16);
                best_h1 = a;
                best_h2 = b;
            }
        }
    memcpy(out, best_out, 16);
    *h1 = best_h1;
    *h2 = best_h2;
}

/* PCM (normalised to `peak`) into a one-shot ADPCM sample ending in silence. */
static int encode(const float *pcm, int n, float peak, struct snd *out)
{
    float top = 0.0f;
    for (int i = 0; i < n; i++) if (fabsf(pcm[i]) > top) top = fabsf(pcm[i]);
    const int blocks = (n + 27) / 28;
    const uint32_t bytes = (uint32_t)(blocks + 1) * 16u;          /* + a silent end block */
    uint8_t *a = (uint8_t *)calloc(bytes, 1);
    int16_t *q = (int16_t *)calloc((size_t)blocks * 28u, sizeof *q);
    if (!a || !q) { free(a); free(q); return 0; }
    const float k = top > 0.0f ? peak / top : 0.0f;
    for (int i = 0; i < n; i++) q[i] = (int16_t)lroundf(pcm[i] * k);
    int h1 = 0, h2 = 0;
    for (int bl = 0; bl < blocks; bl++) encode_block(q + bl * 28, &h1, &h2, a + bl * 16, bl == 0 ? 0x04u : 0x00u);
    a[blocks * 16 + 1] = 0x01u;                /* end, no repeat: the voice falls silent */
    free(q);
    out->adpcm = a;
    out->bytes = bytes;
    return 1;
}

/* The sting as PCM (also used by the offline preview). About 1.85 s, quick:
 * a hit on E to open, a fast rising run on the lead guitar over chugs and a
 * snare build, then at 0.72 s (when YOU WIN lands) a big E power chord with a
 * lead note bent up into the octave, crash, kick and anvil clank; it fades out
 * fast and the machine settles with a hydraulic hiss. */
#define STING_HIT 0.72f
float *moa_chime_sting_pcm(int *n_out)
{
    const int n = (int)(1.85f * RATE);
    float *g = (float *)calloc((size_t)n, sizeof *g);       /* guitars */
    float *o = (float *)calloc((size_t)n, sizeof *o);       /* the mix */
    if (!g || !o) { free(g); free(o); return NULL; }
    s_noise = 0x1234567u;
    const float E2 = 82.41f;
    /* the opening hit and the chugs under the run */
    power_chord(g, n, 0.00f, E2, 0.45f, 0.16f, 0.0f);
    for (int k = 1; k < 8; k++) power_chord(g, n, 0.08f * (float)k + 0.08f, E2, 0.36f, 0.06f, 0.0f);
    /* the run: E minor pentatonic up two octaves, double-tracked */
    static const float k_run[8] = { 196.00f, 220.00f, 246.94f, 293.66f, 329.63f, 392.00f, 440.00f, 493.88f };
    for (int k = 0; k < 8; k++)
        for (int take = 0; take < 2; take++)
            pluck(g, n, 0.08f + 0.08f * (float)k + 0.003f * (float)take, k_run[k] * (take ? 1.003f : 0.998f),
                  0.55f, 0.47f, 0.085f, 0.0f);
    /* the big one */
    power_chord(g, n, STING_HIT, E2, 0.4985f, 1.1f, 0.25f);
    for (int take = 0; take < 2; take++)
        pluck_bend(g, n, STING_HIT + 0.003f * (float)take, 659.26f * (take ? 1.003f : 0.998f), 0.6f, 0.4985f, 1.1f,
                   0.4f, 2.0f);
    squeal(g, n, STING_HIT + 0.01f, 1318.5f, 0.035f);
    amp_cab(g, n);
    for (int i = 0; i < n; i++) o[i] = 0.85f * g[i];
    kick(o, n, 0.00f, 1.0f);
    clank(o, n, 0.00f, 0.5f);
    crash(o, n, 0.00f, 0.12f);
    kick(o, n, 0.24f, 0.55f);
    kick(o, n, 0.40f, 0.55f);
    for (int k = 0; k < 6; k++) snare(o, n, 0.44f + 0.05f * (float)k, 0.18f + 0.07f * (float)k);   /* the build */
    kick(o, n, STING_HIT, 1.0f);
    snare(o, n, STING_HIT, 0.5f);
    clank(o, n, STING_HIT, 0.6f);
    crash(o, n, STING_HIT, 0.24f);
    hiss(o, n, 1.30f, 0.5f, 0.10f);
    /* starts strong, fades out fast after the hit */
    for (int i = (int)(1.05f * RATE); i < n; i++) {
        const float u = (float)(i - (int)(1.05f * RATE)) / (float)(n - (int)(1.05f * RATE));
        o[i] *= (1.0f - u) * (1.0f - u);
    }
    free(g);
    *n_out = n;
    return o;
}

static int build_sting(void)
{
    if (s_sting.adpcm) return 1;
    int n = 0;
    float *o = moa_chime_sting_pcm(&n);
    if (!o) return 0;
    const int ok = encode(o, n, 29000.0f, &s_sting);
    free(o);
    return ok;
}

float *moa_chime_tick_pcm(int *n_out)
{
    const int n = (int)(0.045f * RATE);
    float *o = (float *)calloc((size_t)n, sizeof *o);
    if (!o) return NULL;
    float lp = 0.0f;
    for (int i = 0; i < n; i++) {
        const float t = (float)i / RATE;
        lp += 0.5f * (noise() - lp);
        o[i] = (0.7f * (noise() - lp) + 0.6f * sinf(TAU * 2200.0f * t)) * expf(-t * 160.0f) +
               0.5f * sinf(TAU * 180.0f * t) * expf(-t * 90.0f);
    }
    *n_out = n;
    return o;
}

static int build_tick(void)
{
    if (s_tick.adpcm) return 1;
    int n = 0;
    float *o = moa_chime_tick_pcm(&n);
    if (!o) return 0;
    const int ok = encode(o, n, 14000.0f, &s_tick);
    free(o);
    return ok;
}

static int play(const struct snd *s, uint32_t bank, uint32_t slot, uint16_t vol)
{
    /* Banks are cleared at every session start (spu_init); registering the
     * same bytes again is accepted. */
    if (!psx_mod_spu_sample_bank(bank, s->adpcm, s->bytes)) return 0;
    const uint16_t regs[8] = {
        vol, vol,              /* fixed volume L / R */
        0x0800u,               /* pitch: 22050 Hz */
        0x0000u,               /* start: the bank's first block */
        0x80FFu, 0x1FC0u,      /* ADSR: instant attack, hold; the sample decays itself */
        0x0000u,
        0x0000u,               /* repeat address (unused) */
    };
    return psx_mod_spu_private_voice_play(slot, bank, regs, 0u);
}

int moa_chime_play(void)
{
    return build_sting() && play(&s_sting, BANK_STING, SLOT_STING, 0x3000u);
}

int moa_chime_tick(void)
{
    return build_tick() && play(&s_tick, BANK_TICK, SLOT_TICK, 0x1C00u);
}

/* Both encoded up front (about 0.1 s of host time), so the first win does
 * not wait for them. */
void moa_chime_prepare(void)
{
    (void)build_sting();
    (void)build_tick();
}
