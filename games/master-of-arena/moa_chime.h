/* Rogue Arena victory sounds: made at run time (no disc data), played on
 * private SPU voices. Offline only. */
#ifndef MOA_CHIME_H
#define MOA_CHIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The victory sting (heavy rock guitar, mechanical): 1 = started. */
int moa_chime_play(void);
/* The type tick, one per word of the victory lines: 1 = started. */
int moa_chime_tick(void);
/* Encode both now (optional; otherwise on first use). */
void moa_chime_prepare(void);
/* The sounds as 22050 Hz PCM, for an offline preview (caller frees). */
float *moa_chime_sting_pcm(int *n);
float *moa_chime_tick_pcm(int *n);
/* The same with the shared finish, as encoded (caller frees). */
float *moa_chime_sting_finished(int *n);
float *moa_chime_tick_finished(int *n);

/* For other sounds of ours: 22050 Hz PCM (normalised to `peak`, int16 scale)
 * to a one-shot PS1 ADPCM sample (malloc'd), and playing one on a private
 * voice: sample bank 1..31 (31, 30 are the sting and tick), voice slot
 * 0..23 (23, 22 theirs), fixed volume (0x3000 = the sting's). 1 = ok. */
int moa_chime_encode(const float *pcm, int n, float peak, uint8_t **adpcm, uint32_t *bytes);
int moa_chime_play_sample(const uint8_t *adpcm, uint32_t bytes, unsigned bank, unsigned slot, unsigned vol);

#ifdef __cplusplus
}
#endif

#endif
