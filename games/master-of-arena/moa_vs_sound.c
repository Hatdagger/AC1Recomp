/* Rogue Arena VS screen sounds (moa_vs_sound.h). */
#include "moa_vs_sound.h"
#include "moa_sfx.h"
#include "moa_chime.h"

#include <stdlib.h>
#include <string.h>

#define VAB_AT      25652u        /* the shared fight bank's VAB header in entry 205 */
#define VAG_SHOT    21u           /* the rifle */
#define VAG_DOOR    1u
#define BANK_SHOT   29u           /* private sample banks (31, 30: the victory sounds) */
#define BANK_LAST   28u
#define BANK_DOOR   27u
#define SLOT_SHOT   21u           /* private voice slots (23, 22: the victory sounds) */
#define SLOT_DOOR   20u

struct vs_snd { uint8_t *adpcm; uint32_t bytes; float *pcm; int n; };
static struct vs_snd s_snd[3];
static int s_ready;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* PS1 ADPCM to float PCM, dropping the last (end) block. */
static float *adpcm_decode(const uint8_t *b, uint32_t bytes, int *n_out)
{
    static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
    const uint32_t blocks = bytes / 16u;
    if (blocks < 2u) return NULL;
    float *o = (float *)malloc((size_t)(blocks - 1u) * 28u * sizeof *o);
    if (!o) return NULL;
    int h1 = 0, h2 = 0, n = 0;
    for (uint32_t k = 0; k + 1u < blocks; k++) {
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
    }
    *n_out = n;
    return o;
}

/* VAG `k` of the bank whose header is at e + p. */
static float *vag(const uint8_t *e, uint32_t bytes, uint32_t p, unsigned k, int *n)
{
    if (p + 0x20u > bytes || memcmp(e + p, "pBAV", 4) != 0) return NULL;
    const uint32_t ps = rd16(e + p + 0x12u), vs = rd16(e + p + 0x16u);
    const uint32_t hl = 32u + 128u * 16u + 512u * ps + 512u;
    if (k == 0u || k > vs || p + hl > bytes) return NULL;
    const uint8_t *sizes = e + p + 32u + 2048u + 512u * ps;
    uint32_t at = p + hl + 4u;                       /* the bodies follow the header (+4) */
    for (unsigned i = 1; i < k; i++) at += rd16(sizes + 2u * i) << 3;
    const uint32_t len = rd16(sizes + 2u * k) << 3;
    if (at + len > bytes) return NULL;
    return adpcm_decode(e + at, len, n);
}

int moa_vs_sound_ready(void) { return s_ready; }

int moa_vs_sound_build(const uint8_t *entry, uint32_t bytes)
{
    if (s_ready) return 1;
    int ns = 0, nd = 0;
    float *shot = vag(entry, bytes, VAB_AT, VAG_SHOT, &ns), *door = vag(entry, bytes, VAB_AT, VAG_DOOR, &nd);
    int ok = shot && door;
    for (int k = 0; k < 2 && ok; k++) {
        struct vs_snd *s = &s_snd[k];
        s->pcm = moa_sfx_count_shot(shot, ns, k, &s->n);
        ok = s->pcm && moa_chime_encode(s->pcm, s->n, 29000.0f, &s->adpcm, &s->bytes);
    }
    if (ok) {
        static const struct moa_sfx_room k_door_room = { 0.28f, 1.0f, 3800.0f, 1.3f, 8500.0f };
        struct vs_snd *s = &s_snd[2];
        s->pcm = moa_sfx_finish(door, nd, 1.0f, &k_door_room, &s->n);
        ok = s->pcm && moa_chime_encode(s->pcm, s->n, 29000.0f, &s->adpcm, &s->bytes);
    }
    free(shot);
    free(door);
    if (!ok) {
        for (int k = 0; k < 3; k++) { free(s_snd[k].adpcm); free(s_snd[k].pcm); memset(&s_snd[k], 0, sizeof s_snd[k]); }
        return 0;
    }
    s_ready = 1;
    return 1;
}

int moa_vs_sound_shot(int last)
{
    const struct vs_snd *s = &s_snd[last ? 1 : 0];
    return s_ready && moa_chime_play_sample(s->adpcm, s->bytes, last ? BANK_LAST : BANK_SHOT, SLOT_SHOT, 0x3000u);
}

int moa_vs_sound_door(void)
{
    const struct vs_snd *s = &s_snd[2];
    return s_ready && moa_chime_play_sample(s->adpcm, s->bytes, BANK_DOOR, SLOT_DOOR, 0x3000u);
}

float *moa_vs_sound_pcm(int k, int *n)
{
    if (!s_ready || k < 0 || k > 2) return NULL;
    float *o = (float *)malloc((size_t)s_snd[k].n * sizeof *o);
    if (!o) return NULL;
    memcpy(o, s_snd[k].pcm, (size_t)s_snd[k].n * sizeof *o);
    *n = s_snd[k].n;
    return o;
}
