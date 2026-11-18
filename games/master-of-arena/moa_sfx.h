/* Rogue Arena sound finish: one shared "90s CD-era" treatment for every
 * sound we add (victory sting, type tick, VS screen sounds), so they sit
 * with the game's own: a little drive, a dull top end like a 22 kHz sample,
 * and a dark hall echo baked into the sample (private voices get no SPU
 * reverb). Mono, 22050 Hz float PCM. */
#ifndef MOA_SFX_H
#define MOA_SFX_H

#ifdef __cplusplus
extern "C" {
#endif

#define MOA_SFX_RATE 22050

struct moa_sfx_room {
    float wet;     /* echo level (0.25 typical) */
    float size;    /* hall size (1.0 = the VS sounds' hall) */
    float damp;    /* echo darkness, Hz (3800 typical) */
    float drive;   /* 0 = clean, 1.5 = hot sample */
    float top;     /* top-end roll-off, Hz (0 = none) */
};

extern const struct moa_sfx_room moa_sfx_hall;    /* the house sound */

/* `in` (n samples) with the finish and `tail_s` seconds of echo after it.
 * Returns a new buffer (caller frees) of *n_out samples, or NULL. */
float *moa_sfx_finish(const float *in, int n, float tail_s, const struct moa_sfx_room *r, int *n_out);

/* 2-pole low-pass in place. */
void moa_sfx_lowpass(float *x, int n, float hz);

/* VS screen count-in shot from a game sample (22050 Hz; the rifle, shared
 * fight bank sound 21). The count crosses out the last three beaten ACs on
 * counts 3, 2 and 1, one shot each; last = the final cross-out, which plays a
 * little lower and duller. Finished with the house sound. Returns a new
 * buffer (caller frees). */
float *moa_sfx_count_shot(const float *src, int n, int last, int *n_out);

#ifdef __cplusplus
}
#endif

#endif
