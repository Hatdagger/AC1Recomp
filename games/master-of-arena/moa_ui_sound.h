/* Rogue Arena overlay sounds: the game's own menu clicks (cursor, OK, back)
 * and the garage's part-equip sound, taken from the menu sound bank on the
 * player's disc (GG/MS/MENU_VAB.T entries 0 = header, 1 = samples) and played
 * on a private SPU voice. They no longer go through the game's menu, so they
 * play from the first overlay on, and over screens where the menu isn't
 * running (the loss screen). Offline only. */
#ifndef MOA_UI_SOUND_H
#define MOA_UI_SOUND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The menu bank's tones the overlay uses (program 0, played at note 60). */
enum { MOA_UI_BACK = 0, MOA_UI_CURSOR = 1, MOA_UI_OK = 2, MOA_UI_EQUIP = 3, MOA_UI__COUNT };

/* Build from the bank's header (VH) and sample (VB) bytes. 1 = ready. */
int moa_ui_sound_build(const uint8_t *vh, uint32_t vh_bytes, const uint8_t *vb, uint32_t vb_bytes);
int moa_ui_sound_ready(void);
/* Play sound k (MOA_UI_*). 1 = started. */
int moa_ui_sound_play(int k);
/* Offline: sound k as 22050 Hz float PCM (caller frees). */
float *moa_ui_sound_pcm(int k, int *n);
/* Which tone of the menu bank each sound is (for logs and tests). */
int moa_ui_sound_tone(int k);

#ifdef __cplusplus
}
#endif

#endif
