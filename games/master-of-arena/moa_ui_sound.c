/* Rogue Arena overlay sounds from the game's menu bank (moa_ui_sound.h). */
#include "moa_ui_sound.h"
#include "moa_chime.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Menu bank (GG/MS/MENU_VAB.T entry 0, program 0): the overlay's tones.
 * 0 back (sample 10), 1 cursor (1), 2 OK (2): what the arena list plays.
 * 10 (sample 9, centre note 76): what the garage's part-writing code plays
 * (menu 0x80088340, next to the back-weapon slot writes at 0x800881E8). */
static const uint8_t k_tone[MOA_UI__COUNT] = { 0, 1, 2, 10 };
#define BANK0      23u           /* private sample banks 23..26 (27..31 are the VS and victory sounds) */
#define SLOT_UI    19u           /* private voice slot (20..23: VS and victory) */

struct ui_snd { uint8_t *adpcm; uint32_t bytes; float *pcm; int n; unsigned vol; };
static struct ui_snd s_snd[MOA_UI__COUNT];
static int s_ready;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* PS1 ADPCM to float PCM, up to the end flag (or the data's end). */
static float *adpcm_decode(const uint8_t *b, uint32_t bytes, int *n_out)
{
    static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
    const uint32_t blocks = bytes / 16u;
    if (!blocks) return NULL;
    float *o = (float *)malloc((size_t)blocks * 28u * sizeof *o);
    if (!o) return NULL;
    int h1 = 0, h2 = 0, n = 0;
    for (uint32_t k = 0; k < blocks; k++) {
        const uint8_t *q = b + 16u * k;
        const int sh = q[0] & 15;
        int f = q[0] >> 4;
        if (f > 4) f = 0;
        for (int i = 0; i < 28; i++) {
            int nib = (q[2 + i / 2] >> ((i & 1) * 4)) & 15;
            if (nib & 8) nib -= 16;
            int v = (int)(((unsigned)nib << 12) & 0xFFFFu);
            v = (int16_t)v >> sh;
            v += (h1 * f0[f] + h2 * f1[f] + 32) >> 6;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            h2 = h1; h1 = v;
            o[n++] = (float)v / 32768.0f;
        }
        if (q[1] & 1u) break;                         /* end flag */
    }
    *n_out = n;
    return o;
}

int moa_ui_sound_ready(void) { return s_ready; }
int moa_ui_sound_tone(int k) { return k >= 0 && k < MOA_UI__COUNT ? k_tone[k] : -1; }

static void clear(void)
{
    for (int k = 0; k < MOA_UI__COUNT; k++) { free(s_snd[k].adpcm); free(s_snd[k].pcm); memset(&s_snd[k], 0, sizeof s_snd[k]); }
}

int moa_ui_sound_build(const uint8_t *vh, uint32_t vh_bytes, const uint8_t *vb, uint32_t vb_bytes)
{
    if (s_ready) return 1;
    if (!vh || vh_bytes < 32u || memcmp(vh, "pBAV", 4) != 0) return 0;
    const uint32_t ps = rd16(vh + 0x12u), vs = rd16(vh + 0x16u);
    const uint32_t tones_at = 32u + 128u * 16u, sizes_at = tones_at + 512u * ps;
    if (sizes_at + 512u > vh_bytes) return 0;
    const unsigned master = vh[0x18u] ? vh[0x18u] : 127u, prog_vol = vh[32u + 1u] ? vh[32u + 1u] : 127u;
    for (int k = 0; k < MOA_UI__COUNT; k++) {
        const uint8_t *t = vh + tones_at + 32u * k_tone[k];         /* program 0's tone */
        const unsigned tvol = t[2], centre = t[4], fine = t[5];
        const uint32_t v = rd16(t + 22u);
        if (!v || v > vs) { clear(); return 0; }
        uint32_t at = 0;                                            /* the VB holds the bodies only */
        for (uint32_t i = 1; i < v; i++) at += rd16(vh + sizes_at + 2u * i) << 3;
        const uint32_t len = rd16(vh + sizes_at + 2u * v) << 3;
        if (!len || at + len > vb_bytes) { clear(); return 0; }
        int n = 0;
        float *raw = adpcm_decode(vb + at, len, &n);
        if (!raw || n < 28) { free(raw); clear(); return 0; }
        /* Note 60 on a sample centred at `centre` (+ fine / 128 semitone) plays
         * at 44,100 Hz x 2^((60 - centre - fine/128) / 12); to 22,050 Hz. */
        const double rate = 44100.0 * pow(2.0, (60.0 - (double)centre - (double)fine / 128.0) / 12.0);
        const double step = rate / 22050.0;
        const int m = (int)((double)n / step);
        float *pcm = (float *)malloc((size_t)(m > 0 ? m : 1) * sizeof *pcm);
        float peak = 0.0f;
        for (int i = 0; pcm && i < m; i++) {
            const double x = (double)i * step;
            const int a = (int)x;
            const float f = (float)(x - a), y0 = raw[a], y1 = a + 1 < n ? raw[a + 1] : 0.0f;
            pcm[i] = y0 + (y1 - y0) * f;
            if (fabsf(pcm[i]) > peak) peak = fabsf(pcm[i]);
        }
        free(raw);
        struct ui_snd *s = &s_snd[k];
        s->pcm = pcm;
        s->n = m;
        /* The level the game gives it: master x program x tone x key-on (127),
         * on the SPU's 0..0x3FFF voice volume; the sample keeps its own peak. */
        s->vol = (unsigned)(0x3FFF * (double)master / 127.0 * (double)prog_vol / 127.0 * (double)tvol / 127.0);
        if (!pcm || peak <= 0.0f || !moa_chime_encode(pcm, m, peak * 32767.0f, &s->adpcm, &s->bytes)) { clear(); return 0; }
    }
    s_ready = 1;
    return 1;
}

int moa_ui_sound_play(int k)
{
    if (!s_ready || k < 0 || k >= MOA_UI__COUNT) return 0;
    const struct ui_snd *s = &s_snd[k];
    return moa_chime_play_sample(s->adpcm, s->bytes, BANK0 + (unsigned)k, SLOT_UI, s->vol);
}

float *moa_ui_sound_pcm(int k, int *n)
{
    if (!s_ready || k < 0 || k >= MOA_UI__COUNT) return NULL;
    float *o = (float *)malloc((size_t)s_snd[k].n * sizeof *o);
    if (!o) return NULL;
    memcpy(o, s_snd[k].pcm, (size_t)s_snd[k].n * sizeof *o);
    *n = s_snd[k].n;
    return o;
}
