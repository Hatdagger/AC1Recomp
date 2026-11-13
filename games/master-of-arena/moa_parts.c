/* Master of Arena: part tables and build rules. See moa_parts.h. */
#include "moa_parts.h"
#include "moa_phantasma.h"

#include <stdlib.h>
#include <string.h>

#define MENU_BASE   0x80050DD8u
#define MENU_ENTRY  206u            /* FDAT entry of the menu module */

/* Table start and record size in the menu module (see the AI Battle notes). */
static const struct { uint32_t addr; uint32_t stride; uint32_t end; } k_tables[MOA_CAT__COUNT] = {
    { 0x800CE690u, 60, 0x800CEAC8u },   /* head */
    { 0x800CEAC8u, 56, 0x800CED30u },   /* core */
    { 0x800CED30u, 72, 0x800CF870u },   /* legs */
    { 0x800CF870u, 52, 0x800CFD50u },   /* arms */
    { 0x800CFD50u, 44, 0x800D01C8u },   /* hand weapon */
    { 0x800D01C8u, 52, 0x800D0DF8u },   /* back */
    { 0x800D0DF8u, 52, 0x800D0F30u },   /* blade */
    { 0x800D0F30u, 48, 0x800D11D0u },   /* booster */
    { 0x800D11D0u, 96, 0x800D17D0u },   /* FCS */
    { 0x800D17D0u, 48, 0x800D1AA0u },   /* generator */
};
#define OPTION_TABLE 0x800D1AA0u        /* 44-byte entries: name 18, what 18, price, slots */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void copy_name(char out[19], const uint8_t *p, int trim_lower)
{
    int n = 0;
    while (n < 18 && p[n] >= 32 && p[n] < 127) { out[n] = (char)p[n]; n++; }
    while (n > 0 && out[n - 1] == ' ') n--;
    if (trim_lower) while (n > 0 && out[n - 1] >= 'a' && out[n - 1] <= 'z') n--;  /* "SP-DEtq" */
    out[n] = 0;
}

int moa_parts_load(const char *disc, struct moa_parts *out)
{
    memset(out, 0, sizeof *out);
    /* FDAT.T: a table of u16 sector numbers after two header words. */
    unsigned int fsize = 0;
    uint8_t *head = moa_disc_read_range(disc, "GG/COM/FDAT.T", 0, 2048, &fsize);
    if (!head) return 0;
    uint32_t start = 0, end = 0, prev = 0;
    int ok = 0;
    for (unsigned k = 0; k + 1 < 1022; k++) {
        const uint32_t a = rd16(head + 4 + 2 * k), b = rd16(head + 6 + 2 * k);
        if (a < prev || (uint64_t)b * 2048u > fsize) break;
        prev = a;
        if (k + 1 == MENU_ENTRY) { start = a * 2048u; end = b * 2048u; ok = b > a; break; }
    }
    free(head);
    if (!ok) return 0;
    const uint32_t need = k_tables[0].addr - MENU_BASE, last = OPTION_TABLE + 13u * 44u - MENU_BASE;
    if (end - start < last) return 0;
    const uint32_t from = need & ~2047u;
    uint8_t *m = moa_disc_read_range(disc, "GG/COM/FDAT.T", start + from, last - from, NULL);
    if (!m) return 0;
#define AT(addr) (m + ((addr) - MENU_BASE - from))
    if (memcmp(AT(k_tables[0].addr), "HD-01-SRVT", 10) != 0) { free(m); return 0; }
    for (int c = 0; c < MOA_CAT__COUNT; c++) {
        const uint32_t n = (k_tables[c].end - k_tables[c].addr) / k_tables[c].stride;
        for (uint32_t i = 0; i < n && out->count[c] < MOA_PARTS_MAX; i++) {
            const uint8_t *r = AT(k_tables[c].addr + i * k_tables[c].stride);
            if (memcmp(r, "DUMMY", 5) == 0) continue;
            struct moa_part *p = &out->part[c][out->count[c]++];
            p->id = (uint8_t)i;
            copy_name(p->name, r, 0);
            p->weight = rd16(r + 40);
            p->drain = rd16(r + 42);
            if (c == MOA_CAT_HEAD || c == MOA_CAT_CORE || c == MOA_CAT_LEGS || c == MOA_CAT_ARMS) {
                p->ap = rd16(r + 44); p->def_shell = rd16(r + 46); p->def_energy = rd16(r + 48);
            }
            if (c == MOA_CAT_ARMS && i >= 12) { p->ap = rd16(r + 44); p->def_shell = p->def_energy = 0; }
            if (c == MOA_CAT_CORE || c == MOA_CAT_LEGS) p->extra = rd16(r + 50);
            if (c == MOA_CAT_CORE) p->slots = (uint8_t)rd16(r + 54);
            if (c == MOA_CAT_BOOST) p->extra = rd16(r + 44);
            if (c == MOA_CAT_GEN) { p->extra = rd16(r + 42); p->drain = 0; }
        }
    }
    for (int i = 0; i < MOA_OPTIONS; i++) {
        const uint8_t *r = AT(OPTION_TABLE + 44u * (uint32_t)i);
        copy_name(out->option[i].name, r, 1);
        copy_name(out->option[i].what, r + 18, 0);
        out->option[i].slots = r[40];
    }
#undef AT
    free(m);
    return out->count[MOA_CAT_HEAD] > 0 && out->count[MOA_CAT_GEN] > 0;
}

const struct moa_part *moa_parts_find(const struct moa_parts *p, int cat, uint8_t id)
{
    if (cat < 0 || cat >= MOA_CAT__COUNT) return NULL;
    for (int i = 0; i < p->count[cat]; i++)
        if (p->part[cat][i].id == id) return &p->part[cat][i];
    return NULL;
}

const char *moa_part_group(int cat, const char *n)
{
    if (cat == MOA_CAT_ARMS) return strncmp(n, "AW", 2) == 0 ? "WEAPON ARMS" : "REGULAR ARMS";
    if (cat == MOA_CAT_LEGS)
        return strncmp(n, "LB", 2) == 0 ? "REVERSE JOINT" : strncmp(n, "LF", 2) == 0 ? "FOUR LEGS"
             : strncmp(n, "LC", 2) == 0 ? "CATERPILLAR" : "HUMANOID";
    if (cat == MOA_CAT_BACK)
        return (strncmp(n, "WM", 2) == 0 || strncmp(n, "M118", 4) == 0) ? "MISSILES"
             : strncmp(n, "WR", 2) == 0 ? "ROCKETS"
             : (n[0] == 'R' && (n[1] == 'X' || n[1] == 'Z')) ? "RADARS" : "CANNONS AND GUNS";
    return "";
}

int moa_record_cat(int b)
{
    static const signed char cat[13] = {
        MOA_CAT_CORE, MOA_CAT_HEAD, MOA_CAT_ARMS, MOA_CAT_LEGS, MOA_CAT_HAND, MOA_CAT_BLADE,
        MOA_CAT_BACK, MOA_CAT_BACK, MOA_CAT_FCS, MOA_CAT_GEN, -1, -1, MOA_CAT_BOOST };
    return (b >= 0 && b < 13) ? cat[b] : -1;
}

void moa_build_normalise(uint8_t rec[13])
{
    if (rec[2] >= 12 && rec[2] != 255) { rec[4] = 0; rec[5] = 255; }   /* weapon arms */
    if (rec[3] >= 27 && rec[3] != 255) rec[12] = 255;                 /* caterpillar */
    rec[11] &= 0x0F;                                                  /* 12 option bits */
}

void moa_build_check(const struct moa_parts *p, const uint8_t rec[13], int plus,
                     struct moa_build_check *t)
{
    memset(t, 0, sizeof *t);
    const int weapon_arms = rec[2] >= 12 && rec[2] != 255;
    int leg_w = 0;
    for (int b = 0; b < 13; b++) {
        const int cat = moa_record_cat(b);
        if (cat < 0) continue;
        const struct moa_part *q = moa_parts_find(p, cat, rec[b]);
        if (!q) continue;
        t->weight += q->weight; t->drain += q->drain; t->ap += q->ap;
        t->def_shell += q->def_shell; t->def_energy += q->def_energy;
        if ((cat == MOA_CAT_HAND || cat == MOA_CAT_BLADE) && !weapon_arms) t->arm_load += q->weight;
        if (cat == MOA_CAT_LEGS) { t->leg_max = q->extra; leg_w = q->weight; }
        if (cat == MOA_CAT_CORE) { t->arm_max = q->extra; t->slots_max = q->slots; }
        if (cat == MOA_CAT_GEN) t->output = q->extra;
    }
    t->leg_load = t->weight - leg_w;
    const unsigned mask = rec[10] | (rec[11] << 8);
    for (int i = 0; i < MOA_OPTIONS; i++)
        if (mask & (1u << i)) t->slots_used += p->option[i].slots;
    t->leg_over = t->leg_load > t->leg_max;
    t->arm_over = t->arm_load > t->arm_max;
    t->energy_over = t->drain > t->output;
    t->slots_over = t->slots_used > t->slots_max;
    t->missing_booster = rec[3] < 27 && rec[12] == 255;
    t->blocked = t->missing_booster || t->slots_over ||
                 (!plus && (t->leg_over || t->arm_over || t->energy_over));
}

const char *moa_option_effect(int i)
{
    static const char *const k[MOA_OPTIONS] = {
        "Adds a missile indicator to the radar.", "Interferes with enemy missile lock-on.",
        "Launches missiles automatically.", "Decreases impact shock.", "Decreases cannon recoil.",
        "Increases energy output.", "Accelerates lock-on time.", "Decreases projectile damage.",
        "Decreases energy damage.", "Increases energy weapon recharge.",
        "Increases energy weapon power.", "Decreases energy used." };
    return (i >= 0 && i < MOA_OPTIONS) ? k[i] : "";
}
