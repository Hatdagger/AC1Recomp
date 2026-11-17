/* Master of Arena: start a versus match with as little menu as possible.
 *
 * Shared by every feature that runs a versus match for the player: AI Battle
 * today, netplay next. How the game itself does it is mapped in
 * docs/mapping/mode-launch.md; this module calls the game's own routines in
 * the same order, so every engine (split screen, link, arena) gets the match
 * the versus menu would have built.
 *
 *   1. Boot: the first title of a session is replaced by the versus menu
 *      (no logos, intro, title or mode choice).
 *   2. Units: each player's build, names, paint, options, Human PLUS and
 *      emblem are written straight into the versus menu's staging area
 *      (no AC SETUP, no DATA LOAD, no memory card).
 *   3. Graphics: at VS START each player's part files and textures are
 *      loaded with the garage's own rebuild, then VS START goes ahead and
 *      compiles both stat blocks as usual.
 *
 * The caller keeps the decision of which engine runs (the mode byte) and of
 * the stage: those differ per feature.
 */
#ifndef MOA_VS_DIRECT_H
#define MOA_VS_DIRECT_H

#include <stdint.h>

struct CPUState;
struct moa_card;

struct moa_vs_unit {
    uint8_t parts[12];      /* garage order (0x80036FB0) */
    uint8_t paint[48];
    uint16_t options;       /* optional-parts mask */
    uint8_t plus[8];        /* 0x8004A7D0 layout: [0] Human PLUS level, [2] limits waived */
    uint8_t has_names;
    uint8_t names[36];      /* AC name (18), pilot name (18), the game's two-byte text */
    uint8_t has_emblem;
    uint16_t clut[16];      /* 15-bit colours */
    uint8_t emblem[2048];   /* 64 x 64, 4 bits per pixel, low nibble first */
};

/* A card as a unit. `plus_level` and `waive` come from the caller (AI
 * Battle takes the level from the card's AI record). */
void moa_vs_unit_from_card(const struct moa_card *card, uint8_t plus_level, int waive,
                           struct moa_vs_unit *out);

/* 1. Boot. Armed, the next title the menu would open becomes the versus
 * menu instead; it disarms itself once used. */
void moa_vs_direct_arm_boot(int on);
int moa_vs_direct_boot_armed(void);

/* 2. Units: write player `player`'s unit into the versus staging area and
 * keep its emblem for step 3. */
void moa_vs_direct_set_unit(unsigned player, const struct moa_vs_unit *unit);
/* Forget kept emblems (a new match). */
void moa_vs_direct_clear(void);

/* 3. Graphics. Call from a VS START filter every time VS START runs, with
 * the menu state VS START was given (a0). Returns 1 while loading (the filter
 * then returns 1 so VS START runs again next frame), 0 when done (or when the
 * loader routines are not in memory: the match then uses default colours).
 * `*timed_out` is set if the disc never went quiet. */
int moa_vs_direct_load_frame(struct CPUState *cpu, uint32_t vs_state, int *timed_out);
int moa_vs_direct_loading(void);

#endif
