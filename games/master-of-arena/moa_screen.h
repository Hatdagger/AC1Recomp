/* AI Battle loading screen: the picture shown while the session runs
 * unattended (setup and the fight in fast forward).
 *
 * Steel panel background, both units' emblems, AC and pilot names and AI
 * style, "VS", a status line and a progress bar with the glow of the game's
 * own Now Loading screen, pulsing about once a second. Text uses the game's
 * menu font, read from the player's disc (MENU_TIM.T); nothing from the disc
 * is kept in the source. The picture is made at the window's aspect: the
 * background fills the width, the content stays in a centred 4:3 area. */
#ifndef MOA_SCREEN_H
#define MOA_SCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct moa_screen_unit {
    int have;                 /* 0: drawn as an empty frame, no names */
    char ac[18];
    char pilot[18];
    char style[16];           /* AI preset name, e.g. "AGGRESSOR" */
    uint32_t emblem[64 * 64]; /* 0xRRGGBB */
};

/* The game font from MENU_TIM.T's bytes. 1 = found. Without it nothing is
 * drawn (the caller keeps the plain loading screen). */
int moa_screen_load_font(const uint8_t *menu_tim, uint32_t bytes);
int moa_screen_ready(void);

void moa_screen_set_units(const struct moa_screen_unit *unit1,
                          const struct moa_screen_unit *unit2);

/* One frame. win_w x win_h: the window (0 = unknown, 4:3). status: the
 * glowing line. permille: bar fill 0..1000 (< 0: no bar). limit_secs: shown
 * under the bar when > 0. fade: 0 = full picture .. 1 = black. now_ms: a
 * wall clock for the pulse. Returns 0xAARRGGBB pixels owned here, valid
 * until the next call. */
const uint32_t *moa_screen_render(int win_w, int win_h, const char *status,
                                  int permille, int limit_secs, float fade,
                                  uint64_t now_ms, int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif
