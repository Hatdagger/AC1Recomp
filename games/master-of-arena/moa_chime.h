/* Rogue Arena victory sounds: made at run time (no disc data), played on
 * private SPU voices. Offline only. */
#ifndef MOA_CHIME_H
#define MOA_CHIME_H

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

#ifdef __cplusplus
}
#endif

#endif
