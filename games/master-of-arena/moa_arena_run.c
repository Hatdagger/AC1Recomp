/* Master of Arena: Arena Run. See moa_arena_run.h. */
#include "moa_arena_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "moa_phantasma.h"

/* ---- sources ------------------------------------------------------------------
 *
 * The arena numbers, from the Ex Arena list in the menu module (0x800D5A20)
 * and the opponent counts beside it (0x800D5A08):
 *   0 main arena (50)   1 Humanoid  2 Four Legs  3 Reverse Joint  4 Caterpillar (10 each)
 *   5 Champion (16)     6 Guest (31)   7 FromSoft (10)   8 Master (20, "Extra difficult")
 *   9 Custom (player-made, not used)   10 Project Phantasma (when installed).
 * The sub arena is not among them; where its opponents live is still to find. */
static const struct { uint8_t band, order; } k_arena_band[9] = {
    { MOA_BAND_MAIN, 0 },
    { MOA_BAND_LEG_EX, 0 }, { MOA_BAND_LEG_EX, 0 }, { MOA_BAND_LEG_EX, 0 }, { MOA_BAND_LEG_EX, 0 },
    { MOA_BAND_LATER_EX, 0 }, { MOA_BAND_LATER_EX, 1 }, { MOA_BAND_LATER_EX, 2 },
    { MOA_BAND_MASTER, 0 },
};
#define MENU_BASE     0x80050DD8u
#define MENU_ENTRY    206u
#define ARENA_ENTRIES 0x800D5A08u

int moa_run_sources_load(const char *disc, int phantasma, struct moa_run_source *out, int max)
{
    unsigned int fsize = 0;
    uint8_t *head = moa_disc_read_range(disc, "GG/COM/FDAT.T", 0, 2048, &fsize);
    if (!head) return 0;
    const uint32_t a = (uint32_t)(head[4 + 2 * (MENU_ENTRY - 1)] | head[5 + 2 * (MENU_ENTRY - 1)] << 8);
    free(head);
    const uint32_t off = ARENA_ENTRIES - MENU_BASE;
    uint8_t *m = moa_disc_read_range(disc, "GG/COM/FDAT.T", a * 2048u + (off & ~2047u), 2048, NULL);
    if (!m) return 0;
    int n = 0;
    for (int i = 0; i < 9 && n < max; ++i) {
        const uint8_t e = m[(off & 2047u) + (uint32_t)i];
        if (!e || e > 50) continue;
        out[n].arena = (uint8_t)i;
        out[n].band = k_arena_band[i].band;
        out[n].order = k_arena_band[i].order;
        out[n].entries = e;
        ++n;
    }
    free(m);
    if (phantasma && n < max) {
        out[n].arena = 10; out[n].band = MOA_BAND_PHANTASMA; out[n].order = 0; out[n].entries = 50;
        ++n;
    }
    return n;
}

/* ---- seed ------------------------------------------------------------------ */

uint64_t moa_run_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint32_t moa_run_below(uint64_t *s, uint32_t n)
{
    return n ? (uint32_t)(moa_run_next(s) % n) : 0;
}

uint64_t moa_run_seed_from_text(const char *t)
{
    uint64_t v = 0;
    int hex = t && *t;
    for (const char *c = t; c && *c; ++c)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F')))
            hex = 0;
    if (hex && strlen(t) <= 16) return strtoull(t, NULL, 16);
    for (const char *c = t; c && *c; ++c) v = (v ^ (uint8_t)*c) * 0x100000001B3ull;  /* FNV-1a */
    return v;
}

void moa_run_seed_text(uint64_t seed, char out[17])
{
    snprintf(out, 17, "%016llX", (unsigned long long)seed);
}

/* ---- difficulty curve ---------------------------------------------------------- */

int moa_run_score(const struct moa_run_source *src, int rank)
{
    const int n = src->entries > 1 ? src->entries : 2;
    const int up = n - 1 - rank;                 /* 0 at the bottom, n-1 at the top */
    int base = 0, width = 0;
    switch (src->band) {
    case MOA_BAND_SUB:      base = 0;  width = 15; break;
    case MOA_BAND_LEG_EX:   base = 30; width = 30; break;
    case MOA_BAND_LATER_EX: base = 50 + 8 * src->order; width = 20; break;
    case MOA_BAND_MASTER:   base = 72; width = 25; break;
    case MOA_BAND_MAIN:     base = 8;  width = 70; break;
    case MOA_BAND_PHANTASMA:
        if (rank < 10) return 85 + (9 - rank) * 15 / 9;     /* top 10: high band */
        return 5 + (n - 1 - rank) * 50 / (n > 11 ? n - 11 : 1);  /* 11-49: low/middle */
    }
    int s = base + width * up / (n - 1);
    return s > 100 ? 100 : s;
}

struct cand { struct moa_run_pick p; int score; uint32_t jitter; };

static int cand_cmp(const void *a, const void *b)
{
    const struct cand *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    return x->jitter < y->jitter ? -1 : x->jitter > y->jitter;
}

#define SUB_LOWEST 16          /* sub arena picks only in ranks 50-35 */

int moa_run_lineup(const struct moa_run_source *src, int nsrc, int phantasma,
                   uint64_t seed, struct moa_run_pick out[MOA_RUN_SLOTS])
{
    uint64_t rng = seed ^ 0x4C494E45ull;          /* own stream: "LINE" */
    int total = 0;
    for (int i = 0; i < nsrc; ++i) total += src[i].entries;
    struct cand *subs = calloc((size_t)total + 1, sizeof *subs);
    struct cand *rest = calloc((size_t)total + 1, sizeof *rest);
    int nsub = 0, nrest = 0, filled = 0;
    if (!subs || !rest) goto done;
    for (int i = 0; i < nsrc; ++i) {
        if (src[i].band == MOA_BAND_PHANTASMA && !phantasma) continue;
        for (int r = 0; r < src[i].entries; ++r) {
            struct cand c = { { src[i].arena, (uint8_t)r }, moa_run_score(&src[i], r),
                              (uint32_t)moa_run_next(&rng) };
            if (src[i].band == MOA_BAND_SUB) subs[nsub++] = c; else rest[nrest++] = c;
        }
    }
    /* Sub arena: 6-10 random picks, never every low slot. */
    int take_sub = 6 + (int)moa_run_below(&rng, 5);
    if (take_sub > nsub) take_sub = nsub;
    for (int i = 0; i < take_sub; ++i) {            /* partial shuffle */
        int j = i + (int)moa_run_below(&rng, (uint32_t)(nsub - i));
        struct cand t = subs[i]; subs[i] = subs[j]; subs[j] = t;
    }
    int need = MOA_RUN_SLOTS - take_sub;
    if (need > nrest) need = nrest;
    /* The rest: sorted by score, cut into `need` equal buckets, one random
     * pick from each, so the ramp holds and the lineup varies. */
    qsort(rest, (size_t)nrest, sizeof *rest, cand_cmp);
    struct cand chosen[MOA_RUN_SLOTS];
    for (int b = 0; b < need; ++b) {
        const int lo = (int)((int64_t)nrest * b / need);
        const int hi = (int)((int64_t)nrest * (b + 1) / need);
        chosen[b] = rest[lo + (int)moa_run_below(&rng, (uint32_t)(hi - lo))];
    }
    /* Sub picks go to random slots among the lowest 16. */
    uint8_t is_sub[MOA_RUN_SLOTS] = {0};
    const int low = SUB_LOWEST < need + take_sub ? SUB_LOWEST : need + take_sub;
    for (int i = 0; i < take_sub && i < low; ) {
        int k = (int)moa_run_below(&rng, (uint32_t)low);
        if (!is_sub[k]) { is_sub[k] = 1; ++i; }
    }
    int si = 0, ri = 0;
    for (int k = 0; k < need + take_sub; ++k)
        out[filled++] = is_sub[k] ? subs[si++].p : chosen[ri++].p;
done:
    free(subs);
    free(rest);
    return filled;
}

/* ---- starting AC ------------------------------------------------------------------ */

int moa_run_start_ac(const struct moa_parts *p, uint64_t seed, uint8_t rec[13])
{
    uint64_t rng = seed ^ 0x53544152ull;          /* own stream: "STAR" */
    for (int tries = 0; tries < 20000; ++tries) {
        for (int b = 0; b < 13; ++b) {
            const int cat = moa_record_cat(b);
            rec[b] = 255;
            if (cat < 0) { rec[b] = 0; continue; }          /* options: none */
            const int n = p->count[cat];
            if (n <= 0) continue;
            /* Weapons may be left empty; the frame parts never are. */
            if ((cat == MOA_CAT_HAND || cat == MOA_CAT_BLADE || cat == MOA_CAT_BACK) &&
                moa_run_below(&rng, 4) == 0)
                continue;
            rec[b] = p->part[cat][moa_run_below(&rng, (uint32_t)n)].id;
        }
        moa_build_normalise(rec);
        const int armed = rec[4] != 255 || rec[5] != 255 || rec[6] != 255 ||
                          rec[7] != 255 || (rec[2] >= 12 && rec[2] != 255);
        struct moa_build_check t;
        moa_build_check(p, rec, 0, &t);
        if (armed && !t.blocked && !t.leg_over && !t.arm_over && !t.energy_over &&
            !t.slots_over && !t.missing_booster)
            return 1;
    }
    return 0;
}

/* ---- run file ------------------------------------------------------------------------ */

void moa_run_path(const char *card, char *out, unsigned n)
{
    snprintf(out, n, "%s.arenarun", card ? card : "memcard1");
}

int moa_run_new(const struct moa_parts *p, const struct moa_run_source *src, int nsrc,
                int phantasma, uint64_t seed, struct moa_run_state *s)
{
    memset(s, 0, sizeof *s);
    s->magic = MOA_RUN_MAGIC;
    s->version = MOA_RUN_VERSION;
    s->seed = seed;
    s->phantasma = (uint8_t)(phantasma != 0);
    if (!moa_run_start_ac(p, seed, s->start_ac)) return 0;
    return moa_run_lineup(src, nsrc, phantasma, seed, s->lineup) == MOA_RUN_SLOTS;
}

int moa_run_load(const char *path, struct moa_run_state *s)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t got = fread(s, 1, sizeof *s, f);
    fclose(f);
    if (got != sizeof *s || s->magic != MOA_RUN_MAGIC || s->version != MOA_RUN_VERSION)
        return 0;
    if (s->over) { moa_run_wipe(path); return 0; }   /* a lost run never continues */
    return 1;
}

int moa_run_save(const char *path, const struct moa_run_state *s)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    const int ok = fwrite(s, 1, sizeof *s, f) == sizeof *s;
    if (fclose(f) != 0 || !ok) { remove(tmp); return 0; }
    remove(path);                                       /* Windows rename won't replace */
    return rename(tmp, path) == 0;
}

void moa_run_wipe(const char *path)
{
    remove(path);
}
