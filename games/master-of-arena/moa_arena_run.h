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
};

struct moa_run_source {
    uint8_t arena;        /* the game's arena number (10 = Project Phantasma) */
    uint8_t band;         /* enum moa_run_band */
    uint8_t order;        /* LATER_EX: 0, 1, 2 ... from easiest */
    uint8_t entries;      /* opponents in that arena */
};

struct moa_run_pick { uint8_t arena, rank; };   /* rank 0 = that arena's top */

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
 * sources. Phantasma sources are skipped unless `phantasma` is set.
 * Returns the number of slots filled (50 unless the roster is too small). */
int moa_run_lineup(const struct moa_run_source *src, int nsrc, int phantasma,
                   uint64_t seed, struct moa_run_pick out[MOA_RUN_SLOTS]);

/* A random starting AC that passes the game's own checks (no Human PLUS):
 * a 13-byte card record, options empty. 1 = ok. */
int moa_run_start_ac(const struct moa_parts *p, uint64_t seed, uint8_t rec[13]);

/* ---- the run file -------------------------------------------------------- */

#define MOA_RUN_MAGIC   0x4E555241u   /* "ARUN" */
#define MOA_RUN_VERSION 1u

struct moa_run_state {
    uint32_t magic, version;
    uint64_t seed;
    uint8_t  start_ac[13];
    uint8_t  phantasma;            /* the lineup used Phantasma opponents */
    uint8_t  wins;                 /* opponents beaten; rank = 50 - wins */
    uint8_t  points;               /* P: one per win */
    uint8_t  over;                 /* 1 = lost; the file is wiped on read */
    uint8_t  pad[3];
    struct moa_run_pick lineup[MOA_RUN_SLOTS];
    /* step 2 adds shop stock and owned parts here (version 2). */
};

/* Path of the run file for a memory card file: "<card>.arenarun". */
void moa_run_path(const char *card_path, char *out, unsigned out_size);

int moa_run_new(const struct moa_parts *p, const struct moa_run_source *src, int nsrc,
                int phantasma, uint64_t seed, struct moa_run_state *out);
int moa_run_load(const char *path, struct moa_run_state *out);   /* 1 = a live run */
int moa_run_save(const char *path, const struct moa_run_state *s);
void moa_run_wipe(const char *path);

#ifdef __cplusplus
}
#endif

#endif
