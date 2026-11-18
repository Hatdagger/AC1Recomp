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

/* The opaque backdrop of the tile, choice and list screens: a tile of
 * 0xRRGGBB pixels (64x64 at most), repeated at 3x. NULL: the steel plate. */
void moa_screen_set_backdrop(const uint32_t *rgb_tile, int w, int h);

/* The steps a session goes through, shown in a row under the bar. */
enum { MOA_STEP_SETUP = 0, MOA_STEP_STAGING, MOA_STEP_GATHERING, MOA_STEP_COMPLETE, MOA_STEP_COUNT };

/* One frame. win_w x win_h: the window (0 = unknown, 4:3). status: the
 * glowing line. step: MOA_STEP_*, lit in the row of steps. permille: bar fill 0..1000 (< 0: no bar). limit_secs: shown
 * under the bar when > 0. fade: 0 = full picture .. 1 = black. now_ms: a
 * wall clock for the pulse. Returns 0xAARRGGBB pixels owned here, valid
 * until the next call. */
const uint32_t *moa_screen_render(int win_w, int win_h, const char *status,
                                  int step, int permille, int limit_secs, float fade,
                                  uint64_t now_ms, int *out_w, int *out_h);

/* A screen of part tiles (unused since the prize screen; kept): a title, a line
 * under it, up to 8 tiles in rows of 4 and a footer. A tile shows its
 * thumbnail (128x128 0xAARRGGBB, NULL = the category in large text), the
 * part's name and its label (price). Same canvas and font as above;
 * returns NULL until the font is loaded. see_through: no steel backdrop, a
 * dark veil instead (alpha 0xB4), for psx_mod_set_overlay_image. */
struct moa_screen_tile {
    const char *category;     /* "HEAD", "BACK WEAPON" ... */
    const char *name;
    const char *label;        /* e.g. "3 P" */
    const uint32_t *thumb;    /* 128 x 128, or NULL */
};
const uint32_t *moa_screen_render_tiles(int win_w, int win_h, const char *title,
                                        const char *line, const struct moa_screen_tile *tile,
                                        int count, const char *footer, int see_through,
                                        int *out_w, int *out_h);

/* A choice screen (Rogue Arena prize picks): a title, a line under it, up
 * to 4 boxes stacked in the middle, one shared width that fits the longest
 * label (16 characters at most), the box at `cursor` highlighted, and a
 * footer. Same canvas and font as above; NULL until the font is loaded.
 * see_through: as for the tiles. */
const uint32_t *moa_screen_render_choice(int win_w, int win_h, const char *title,
                                         const char *line, const char *const *label,
                                         int count, int cursor, const char *footer,
                                         int see_through, int *out_w, int *out_h);

/* A list screen (Rogue Arena hub and its stage picker): a title, up to 3
 * lines under it, then rows like the launcher's lists. `visible` rows show
 * at a time (8 at most), from `top`; arrows above and below show there is
 * more. A row may carry a value on its right (gold); `marked` (-1 none)
 * gets a gold dot (the remembered pick). Same canvas and font as above;
 * NULL until the font is loaded. see_through: as for the tiles. */
struct moa_screen_list {
    const char *title;
    const char *info[3];
    const char *const *row;      /* 26 characters at most */
    const char *const *value;    /* NULL, or one per row (NULL entries allowed) */
    int count, cursor, top, visible, marked;
    const char *footer;
};
const uint32_t *moa_screen_render_list(int win_w, int win_h, const struct moa_screen_list *l,
                                       int see_through, int *out_w, int *out_h);

/* The Rogue Arena prize screen after every win: up to 3 part offers (cheap,
 * mid, expensive) and PASS, one row each. An offer row shows the part's
 * picture (128x128 0xAARRGGBB, NULL = the category in large text), its
 * price tier tag, category and name, and its cost on the right; a locked
 * row (not enough P) is dimmed and shows `cost` in red. The PASS row has no
 * picture; with `alt_name` it is split in two (RE-ROLL | PASS). When `ask_n` > 0 a box opens over the rows (EQUIP NOW / SAVE FOR
 * LATER ...) with `ask_line` (what an equip replaces) under its title.
 * see_through: as for the tiles. */
struct moa_screen_prize_row {
    const char *tag;          /* "CHEAP", "MID", "EXPENSIVE", or NULL (PASS) */
    const char *category;     /* "BACK WEAPON  MISSILES" */
    const char *name;         /* "PASS" on the pass row */
    const char *cost;         /* "3 P", "+1 P" */
    const uint32_t *thumb;    /* 128 x 128, or NULL */
    int locked;
    /* A set cycling through its parts: the whole box is wiped from the top
     * to the next part (`category_next`, `name_next`, `thumb_next`), `wipe`
     * of 128 done, under a bright scan line; `dots` (2..3) small marks under
     * the picture, `dot_on` lit. `special`: the set's bronze frame. */
    const char *category_next, *name_next;
    const uint32_t *thumb_next;
    int wipe, dots, dot_on, special;
};
struct moa_screen_prize {
    const char *title, *line;
    const struct moa_screen_prize_row *row;
    int count, cursor;
    const char *ask_title, *ask_line;
    const char *const *ask;
    int ask_n, ask_cursor;
    const char *footer;
    /* An optional second button on the left half of the last (plain) row,
     * e.g. RE-ROLL; alt_on: the cursor (on the last row) is on it. */
    const char *alt_name, *alt_cost;
    int alt_locked, alt_on;
};
const uint32_t *moa_screen_render_prize(int win_w, int win_h, const struct moa_screen_prize *p,
                                        int see_through, int *out_w, int *out_h);

/* The victory screen after a run win (fighting-game style). winner and
 * loser: RGBA renders (rows top first, transparent background) of the
 * player's AC and of the beaten one (drawn on its back); either may be NULL.
 * band: the belt colour 0xRRGGBB; the belt pattern is the backdrop tile
 * (moa_screen_set_backdrop). The layers are built once per `key`; each call
 * draws the frame at time `t`: the beaten AC slides in from the left, the
 * winner from the right, YOU WIN stamps in with a flash at 0.72 s. */
struct moa_screen_victory {
    const uint8_t *winner; int winner_w, winner_h;
    const uint8_t *loser;  int loser_w, loser_h;
    uint32_t band;
    const char *rank, *pay, *beaten;
    const char *line[2];
    const char *status;
    float t;                  /* seconds since the screen opened (the animation) */
    unsigned key;
};
const uint32_t *moa_screen_render_victory(int win_w, int win_h, const struct moa_screen_victory *v,
                                          int *out_w, int *out_h);
/* How many words of the two lines are shown at v->t (typed in one at a
 * time once the chime is over): a caller plays a tick each time it grows. */
int moa_screen_victory_words(const struct moa_screen_victory *v);
/* 1 = the lines are fully typed (the prompt may show). */
int moa_screen_victory_typed(const struct moa_screen_victory *v);

#ifdef __cplusplus
}
#endif

#endif
