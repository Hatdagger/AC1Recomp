/* Rogue Arena VS screen sounds, made from the game's own samples on the
 * player's disc (the shared fight bank in FDAT entry 205): the rifle shot
 * (sound 21) for the count's cross-outs, the last one lower and duller, and
 * sound 1 when the hangar shutters slam shut, both with the house finish
 * (moa_sfx.h). Played on private SPU voices. Offline only. */
#ifndef MOA_VS_SOUND_H
#define MOA_VS_SOUND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOA_VS_FDAT_ENTRY  205u
#define MOA_VS_DOOR_PEAK   0.156f   /* the shutter sound's peak, s into it: start it this early */

/* Build the three sounds from FDAT entry 205's bytes. 1 = ready (also when
 * built already). */
int moa_vs_sound_build(const uint8_t *entry, uint32_t bytes);
int moa_vs_sound_ready(void);
/* Play: a count shot (last = the final one), or the shutters. 1 = started. */
int moa_vs_sound_shot(int last);
int moa_vs_sound_door(void);
/* Offline preview: sound k (0 shot, 1 last shot, 2 door) as finished 22050 Hz
 * PCM (caller frees). Needs moa_vs_sound_build first. */
float *moa_vs_sound_pcm(int k, int *n);

#ifdef __cplusplus
}
#endif

#endif
