/* Rogue Arena music (moa_music.cpp). Offline only.
 *
 * Two sources:
 * - The game's own menu song (a sequence on the SPU, started by the menu at
 *   0x80064F24(2)). The run starts it early in the direct boot, holds it
 *   silent under the victory screen, fades it back in after X, and starts it
 *   again at the hub if it never began.
 * - The player's own music files, by scene and belt, from the music folder
 *   (README.txt there). A file for the scene replaces the game's song; scene
 *   and belt changes crossfade. These play on the runtime's host music channel
 *   (psx_mod_music_*), so they also play through loads and fast forward. */
#ifndef MOA_MUSIC_H
#define MOA_MUSIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

enum moa_music_scene {
    MOA_MUSIC_OFF = 0,      /* not a run, or not started */
    MOA_MUSIC_BOOT,         /* the boot, until the player has control */
    MOA_MUSIC_HUB,          /* the hub and its overlays (shop, garage, stage, next battle) */
    MOA_MUSIC_VICTORY,      /* the victory screen, until X */
    MOA_MUSIC_LOSS,         /* RUN OVER: retry / new / quit */
    MOA_MUSIC_FIGHT         /* the game's loading screen and the fight: the game's own music */
};

typedef void (*moa_music_log_fn)(const char *fmt, ...);

/* A new session: everything stops and starts over. */
void moa_music_reset(moa_music_log_fn log);
/* Every frame: what is on screen, and the run's rank (1..50) for the belt. */
void moa_music_frame(int scene, int rank);
/* The menu module started (its song is gone: the game clears its flag). */
void moa_music_menu_entry(void);
/* From the menu's controller sampling, where a guest call is safe. */
void moa_music_menu_frame(struct CPUState *cpu);
/* Filter on 0x80064F24 (the menu song's start): 1 = skipped (already playing). */
int moa_music_start_filter(struct CPUState *cpu, uint32_t address);
/* The direct boot: start the menu song now. 1 = started. */
int moa_music_boot_start(struct CPUState *cpu, uint32_t ra);
/* 1 while a custom file plays for the boot (the boot may keep fast forward). */
int moa_music_custom_on(void);

#ifdef __cplusplus
}
#endif

#endif
