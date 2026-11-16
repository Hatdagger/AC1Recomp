/* Master of Arena: Arena Run (roguelike mode), the run's own logic.
 *
 * Everything here is plain data work the launcher does before a session:
 * the seed, the random legal starting AC, the 50-opponent lineup on the
 * difficulty curve, and the run file kept beside memory card 1. The game
 * side (hub, pause menu, results) is a runtime plugin that reads the run file.
 */
#ifndef MOA_ARENA_RUN_H
#define MOA_ARENA_RUN_H

#include <stdint.h>
#include "moa_parts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOA_RUN_SLOTS 50

/* Where an opponent comes from. Each source is one arena block. */
enum moa_run_band {
    MOA_BAND_SUB = 0,     /* sub arena: lowest band, few picks, ranks 50-35 only */
    MOA_BAND_LEG_EX,      /* the four leg-type Ex arenas: one shared middle band */
    MOA_BAND_LATER_EX,    /* later Ex arenas: each harder than the one before */
    MOA_BAND_MASTER,      /* master arena: top band */
    MOA_BAND_PHANTASMA,   /* ranks 11-49 low/middle, top 10 high */
    MOA_BAND_MAIN,        /* the scenario's main arena: its 50 ranks span low to high */
};

struct moa_run_source {
    uint8_t arena;        /* the game's arena number (10 = Project Phantasma) */
    uint8_t band;         /* enum moa_run_band */
    uint8_t order;        /* LATER_EX: 0, 1, 2 ... from easiest */
    uint8_t entries;      /* opponents in that arena */
    uint64_t karasawa;    /* bit r: rank r's build holds the KARASAWA */
};

/* Mark each source's opponents that carry the KARASAWA (their builds in
 * GG/MS/MENU_TIM.T); those are kept to the run's top 25. 1 = read. */
int moa_run_sources_mark(const char *disc, const struct moa_parts *p,
                         struct moa_run_source *src, int nsrc);

struct moa_run_pick { uint8_t arena, rank; };   /* rank 0 = that arena's top */

/* The 64-byte arena record of a ghost (its build, names and the run's paint). */
struct moa_run_ghost;
void moa_run_ghost_record(const struct moa_run_ghost *g, uint64_t seed, uint8_t rec[64]);
/* The attempt's random paint (seed + attempt), as a card's 48 paint bytes. */
void moa_run_paint(uint64_t seed, uint8_t attempt, uint8_t paint[48]);

/* The run's sources from the player's disc: each arena's opponent count from
 * the menu module's table (0x800D5A08), with the band each arena belongs to.
 * Phantasma (arena 10, 50 opponents) is added when `phantasma` is set.
 * Returns the number of sources (0 = the disc could not be read). */
int moa_run_sources_load(const char *disc_path, int phantasma,
                         struct moa_run_source *out, int max);

/* Seeded generator (splitmix64): one seed always gives the same run. */
uint64_t moa_run_next(uint64_t *state);
uint32_t moa_run_below(uint64_t *state, uint32_t n);

/* A seed from text: digits are taken as the number, anything else hashed. */
uint64_t moa_run_seed_from_text(const char *s);
void moa_run_seed_text(uint64_t seed, char out[17]);   /* 16 hex digits */

/* The difficulty score (0-100) of an arena's opponent. */
int moa_run_score(const struct moa_run_source *src, int rank);

/* Fill `out` (ranks 50..1 as out[0]..out[49]: easiest first) from the
 * sources: out[0] is the player's place (MOA_RUN_PLAYER), 49 opponents above. Phantasma sources are skipped unless `phantasma` is set.
 * Returns the number of slots filled, the player's included (50 unless the roster is too small). */
int moa_run_lineup(const struct moa_run_source *src, int nsrc, int phantasma,
                   uint64_t seed, struct moa_run_pick out[MOA_RUN_SLOTS]);

/* A random starting AC that passes the game's own checks (no Human PLUS):
 * a 13-byte card record, options empty. 1 = ok. */
int moa_run_start_ac(const struct moa_parts *p, uint64_t seed, uint8_t rec[13]);

/* ---- the run file -------------------------------------------------------- */

#define MOA_RUN_MAGIC   0x4E555241u   /* "ARUN" */
#define MOA_RUN_VERSION 3u

/* A lineup slot whose arena is MOA_RUN_GHOST is a ghost: `rank` is its
 * index in moa_run_lineup.ghost. */
#define MOA_RUN_GHOST   0xFFu
/* Slot 0 (rank 50) is the player's own place at the bottom of the ladder:
 * 49 opponents above it, as in the game's arena 0. */
#define MOA_RUN_PLAYER  0xFEu

/* A losing AC from an earlier attempt, kept on the ladder for Retry. */
struct moa_run_ghost {
    uint8_t  rec[13];              /* the build, as moa_run_state.start_ac */
    uint8_t  used;                 /* 1 = on the ladder */
    uint8_t  attempt;              /* the attempt it came from */
    uint8_t  pad;
    char     pilot[17];
    char     ac[17];
    struct moa_run_pick ai_from;   /* AI: copied from the opponent it displaced */
};

/* What Retry keeps: the arena itself. The seed made it; ghosts pile up. */
struct moa_run_lineup {
    uint64_t seed;
    uint8_t  phantasma;            /* the lineup used Phantasma opponents */
    uint8_t  ghosts;               /* ghosts on the ladder now */
    uint8_t  pad[2];
    uint32_t menu_lba;             /* GG/MS/MENU_TIM.T's first sector on the disc */
    struct moa_run_pick slot[MOA_RUN_SLOTS];          /* ranks 50..1 */
    struct moa_run_ghost ghost[MOA_RUN_SLOTS];
};

/* What Retry resets: this attempt's player. */
struct moa_run_progress {
    uint8_t  start_ac[13];
    uint8_t  attempt;              /* 0 = the first; a Retry adds one */
    uint8_t  wins;                 /* opponents beaten; rank = 50 - wins */
    uint8_t  points;               /* P: one per win */
    uint8_t  over;                 /* 1 = lost: Retry or New Run only */
    uint8_t  best;                 /* most wins of any attempt */
    uint8_t  pad[2];
    char     pilot[17];            /* ASCII, as typed in the launcher */
    char     ac[17];               /* may be empty */
    /* step 2 adds shop stock and owned parts here. */
};

struct moa_run_state {
    uint32_t magic, version;
    struct moa_run_lineup   lineup;
    struct moa_run_progress run;
};

/* Path of the run file for a memory card file: "<card>.arenarun". */
void moa_run_path(const char *card_path, char *out, unsigned out_size);

int moa_run_new(const struct moa_parts *p, const struct moa_run_source *src, int nsrc,
                int phantasma, uint64_t seed, struct moa_run_state *out);

/* Retry a lost run: the ghost of the losing AC takes the last rank it held
 * (nothing if it never won), then a new random legal AC at 0 P. 1 = ok. */
int moa_run_retry(const struct moa_parts *p, struct moa_run_state *s);

/* A new win (the caller checks it beat someone not beaten before). */
int moa_run_win(struct moa_run_state *s);
/* A loss: the run is over (kept for Retry). */
void moa_run_lose(struct moa_run_state *s);

/* 1 = a live run, 2 = a lost run (Retry), 0 = none. */
int moa_run_load(const char *path, struct moa_run_state *out);
int moa_run_save(const char *path, const struct moa_run_state *s);
void moa_run_wipe(const char *path);

/* ---- the run's memory card -------------------------------------------------
 *
 * The run is played from its own memory card in slot 1, beside the player's
 * card ("<card>.arenarun.mcd"): one scenario save the game loads itself (LOAD
 * GAME), with the run's AC, pilot and AC names, P as credits, and the owned
 * parts. The player's own card is never touched. */

/* "<card>.arenarun.mcd". */
void moa_run_card_path(const char *card_path, char *out, unsigned out_size);

/* The run as a scenario save block (8192 bytes). */
void moa_run_make_save(const struct moa_run_state *s, uint8_t block[8192]);

/* Write the run's memory card. 1 = ok. */
int moa_run_write_card(const char *card_path, const struct moa_run_state *s);

#ifdef __cplusplus
}
#endif

#endif
