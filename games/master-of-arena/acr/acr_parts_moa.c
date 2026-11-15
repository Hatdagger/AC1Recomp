/* Master of Arena part tables -> one readable TOML file per part.
 *
 * The tables live in the menu module (GG/COM/FDAT.T entry 206), at fixed
 * offsets in the USA release. Each record: 18-byte name, 18-byte type text
 * ("HUMANOID LEGS", "SNIPER RIFLE"...), then little-endian stats. Field meanings come from the game's own compile routine
 * (docs/research/gen1-movement.md); fields not yet understood are kept in
 * `raw` so nothing is lost. */
#include "acr_gen1_fit.h"
#include "acr_parts_moa.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define acr_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define acr_mkdir(p) mkdir((p), 0755)
#endif

typedef struct table {
    const char *category;
    uint32_t offset;   /* in FDAT entry 206 */
    uint32_t stride;   /* bytes per part */
    uint32_t count;    /* parts in the table (the gap to the next table) */
    const char *first; /* name of part 0, to confirm the layout */
} table;

static const table k_tables[] = {
    { "head",      0x7D8B8u, 60, 18, "HD-01-SRVT" },
    { "core",      0x7DCF0u, 56, 11, "XCA-00" },
    { "legs",      0x7DF58u, 72, 40, "LN-1001" },
    { "arms",      0x7EA98u, 52, 24, "AN-101" },
    { "hand",      0x7EF78u, 44, 26, "NO WEAPON" },
    { "back",      0x7F3F0u, 52, 60, "WM-S40/1" },
    { "blade",     0x80020u, 52,  6, "LS-2001" },
    { "booster",   0x80158u, 48, 14, "B-P320" },
    { "fcs",       0x803F8u, 96, 16, "COMDEX-C7" },
    { "generator", 0x809F8u, 48, 15, "GPS-VVA" },
};

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16); }

static int make_dirs(const char *path) {
    char buf[1024];
    const size_t len = strlen(path);
    if (len >= sizeof buf) return 0;
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i <= len; ++i) {
        if (buf[i] == '/' || buf[i] == '\\' || buf[i] == '\0') {
            const char saved = buf[i];
            buf[i] = '\0';
            if (acr_mkdir(buf) != 0 && errno != EEXIST) return 0;
            buf[i] = saved;
        }
    }
    return 1;
}

/* A part name as a file-name-safe slug ("WM-S40/1" -> "WM-S40_1"). */
static void slug(const char *name, char *out, size_t size) {
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < size; ++p)
        out[n++] = (isalnum((unsigned char)*p) || *p == '-' || *p == '.') ? *p : '_';
    while (n && out[n - 1] == '_') --n;
    out[n] = '\0';
}

/* An 18-byte text field (the name at +0, the type at +18). */
static int text_of(const uint8_t *p, char *out) {
    int n = 0;
    for (; n < 18 && p[n]; ++n) {
        if (p[n] < 32 || p[n] > 126) return 0;
        out[n] = (char)p[n];
    }
    while (n && out[n - 1] == ' ') --n;   /* a few names carry a trailing space */
    out[n] = '\0';
    return n > 0;
}
static int name_of(const uint8_t *rec, char *out) { return text_of(rec, out); }

/* Sub-folder from the game's own type text, so parts sort the way players
 * think of them (back units: missiles, rockets, radars, cannons and guns). */
static const char *sub_folder(const char *category, const char *type) {
    if (strcmp(category, "arms") == 0) return strcmp(type, "ARM UNIT") == 0 ? "regular" : "weapon";
    if (strcmp(category, "legs") == 0) {
        if (strcmp(type, "HUMANOID LEGS") == 0) return "humanoid";
        if (strcmp(type, "REVERSE JOINT") == 0) return "reverse-joint";
        if (strcmp(type, "FOUR LEGS TYPE") == 0) return "four-legs";
        if (strcmp(type, "CATERPILLAR") == 0) return "caterpillar";
        return "other";
    }
    if (strcmp(category, "back") == 0) {
        if (strstr(type, "RADAR")) return "radar";
        if (strstr(type, "ROCKET")) return "rocket";
        if (strstr(type, "MISSILE")) return "missile";
        if (strstr(type, "CANNON") || strstr(type, "GUN")) return "cannon-gun";
        return "other";
    }
    return NULL;
}

static void field(FILE *f, const char *key, uint32_t value, const char *note) {
    fprintf(f, "%-14s = %u", key, (unsigned)value);
    if (note) fprintf(f, "   # %s", note);
    fputc('\n', f);
}

/* Fitting-rule flags (src/gen1/acr_gen1_fit.h), so mods can opt new parts in. */
static void write_fit(FILE *f, const char *c, uint32_t index) {
    acr_gen1_slot slot;
    if (!strcmp(c, "arms")) slot = ACR_GEN1_SLOT_ARMS;
    else if (!strcmp(c, "legs")) slot = ACR_GEN1_SLOT_LEGS;
    else if (!strcmp(c, "hand")) slot = ACR_GEN1_SLOT_HAND;
    else if (!strcmp(c, "back")) slot = ACR_GEN1_SLOT_BACK_L;
    else return;
    static const struct { uint16_t bit; const char *name; } k[] = {
        { ACR_GEN1_FIT_ARM_WEAPON, "arm-weapon" }, { ACR_GEN1_FIT_CATERPILLAR, "caterpillar" },
        { ACR_GEN1_FIT_RADAR, "radar" }, { ACR_GEN1_FIT_TWO_SLOT, "two-slot" },
        { ACR_GEN1_FIT_HEAVY_HAND, "heavy-hand" }, { ACR_GEN1_FIT_NO_HEAVY_HAND, "no-heavy-hand" },
        { ACR_GEN1_FIT_NO_BACK_WEAPONS, "no-back-weapons" }, { ACR_GEN1_FIT_BACK_WEAPON, "back-weapon" },
    };
    uint16_t flags = acr_gen1_fit_flags_moa(slot, index);
    fprintf(f, "fit            = [");
    int first = 1;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i) {
        if (!(flags & k[i].bit)) continue;
        fprintf(f, "%s\"%s\"", first ? "" : ", ", k[i].name);
        first = 0;
    }
    fprintf(f, "]   # fitting rules, see docs/research/gen1-fitting-rules.md\n");
}

/* Weapon details live in separate 24-byte tables (docs/research/gen1-weapons.md). */
#define HAND_DETAIL 0x8118Cu   /* 0x800D1F64 in the menu module */
#define BACK_DETAIL 0x813FCu   /* 0x800D21D4 */
#define ARMS_DETAIL 0x8100Cu   /* 0x800D1DE4: weapon arms (arms 12 and up), same layout */
static const uint8_t *g_menu;
static size_t g_menu_size;
static uint8_t *g_vs;          /* VS engine (FDAT 209): shot speeds */
static size_t g_vs_size;

/* Shot speed (world units per Gen 1 step) for a gun's model id (detail
 * byte +1): VS engine 0x80080E38 picks one of five tables by id range. */
static int vs_u16(uint32_t addr, uint16_t *out) {
    const uint32_t off = addr - 0x80050DD8u;
    if (!g_vs || off + 2 > g_vs_size) return 0;
    *out = (uint16_t)(g_vs[off] | g_vs[off + 1] << 8);
    return 1;
}
static int gun_speed(unsigned id, uint16_t *out) {
    if (id < 0x14) return vs_u16(0x80091198u + 28 * id + 8, out);
    if (id < 0x28) return vs_u16(0x80092980u + 40 * (id - 0x14), out);
    if (id < 0x3C) return vs_u16(0x80092C20u + 36 * (id - 0x28), out);
    if (id < 0x5A) return vs_u16(0x80092824u + 24 * (id - 0x3C), out);
    return vs_u16(0x800928E4u + 22 * (id - 0x5A), out);
}

static void write_weapon(FILE *f, const char *c, uint32_t index) {
    uint32_t base = !strcmp(c, "hand") ? HAND_DETAIL : !strcmp(c, "back") ? BACK_DETAIL : 0;
    if (!strcmp(c, "arms")) {
        if (index < 12) return;
        base = ARMS_DETAIL;
        index -= 12;
    }
    if (!base || !g_menu || base + 24 * (index + 1) > g_menu_size) return;
    const uint8_t *w = g_menu + base + 24 * index;
    if (w[6] == 0 && u16(w + 0x14) == 0 && u16(w + 8) == 0) return;   /* radars and the like */
    field(f, "attack", u16(w + 0x14), NULL);
    field(f, "ammo", u16(w + 8), NULL);
    field(f, "range", u16(w + 0xE), "the FCS may cap it");
    field(f, "lock_class", w[6], "picks the FCS's range, lock box and lock time; 4 = none");
    field(f, "energy_damage", (w[0x12] & 0x20) ? 1 : 0, "0 = shell damage");
    field(f, "energy_per_shot", u16(w + 0xA), NULL);
    field(f, "ammo_price", u16(w + 0xC), NULL);
    field(f, "salvo", w[0x10], "missiles locked at once");
    field(f, "fire_interval", w[0x11], "the garage shows it as RELOAD TIME");
    field(f, "ammo_flags", w[0x12], "garage AMMO TYPE: 0x10 SOLID, 0x20 ENERGY");
    field(f, "impact", w[0x16], NULL);
    field(f, "type_id", w[7], NULL);
    /* Back cannons need a steady stance (kneel / on the ground) to fire. */
    if (!strcmp(c, "back") && (w[0] == 2 || (w[0x12] & 0x20 && w[5] == 3)))
        field(f, "stance", 1, "cannon: bipeds kneel, four legs on the ground only, caterpillars free");
    uint16_t v, a;
    if (w[0] == 0 && gun_speed(w[1], &v))
        field(f, "shot_speed", v, "world units per Gen 1 step");
    /* Cannons: VS engine 0x80081B18, table 0x80091310 (6 bytes per model). */
    if (w[0] == 2 && vs_u16(0x80091310u + 6 * w[1], &v))
        field(f, "shot_speed", v, "world units per Gen 1 step (read from code, not traced)");
    /* Missiles: VS engine table 0x80093114 (40 bytes per missile model):
     * steering push per step (+0xC) and top speed (+0xE). */
    if (w[0] == 1 && vs_u16(0x80093114u + 40 * w[1] + 0xE, &v) && vs_u16(0x80093114u + 40 * w[1] + 0xC, &a)) {
        field(f, "shot_speed", v, "top speed, world units per Gen 1 step");
        field(f, "homing", a, "pushed toward the target this much each step");
    }
}

static void write_fcs(FILE *f, const uint8_t *rec) {
    field(f, "max_locks", rec[0x2C], NULL);
    for (int k = 0; k < 4; ++k) {
        const uint8_t *g = rec + 0x2E + 12 * k;
        fprintf(f, "lock_class_%d   = [%u, %u, %u, %u, %u, %u]   # range, box w, h, inner w, h, lock time\n", k,
                (unsigned)u16(g), (unsigned)u16(g + 2), (unsigned)u16(g + 4), (unsigned)u16(g + 6),
                (unsigned)u16(g + 8), (unsigned)u16(g + 10));
    }
}

static int write_part(const char *dir, const table *t, uint32_t index, const uint8_t *rec,
                      const char *name, const char *type) {
    char file_name[64], path[1200];
    slug(name, file_name, sizeof file_name);
    snprintf(path, sizeof path, "%s/%03u-%s.toml", dir, (unsigned)index, file_name);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const char *c = t->category;
    fprintf(f, "# Master of Arena part, imported from your disc by acr-import.\n");
    fprintf(f, "name           = \"%s\"\n", name);
    fprintf(f, "category       = \"%s\"\n", c);
    fprintf(f, "type           = \"%s\"\n", type);
    fprintf(f, "generation     = \"gen1\"\n");
    fprintf(f, "game_index     = %u\n", (unsigned)index);
    field(f, "price", u32(rec + 0x24), NULL);
    field(f, "weight", u16(rec + 0x28), NULL);
    if (strcmp(c, "generator") == 0) {
        field(f, "output", u16(rec + 0x2A), "energy supply");
        field(f, "capacity", u16(rec + 0x2C), "energy store");
        field(f, "red_zone", u16(rec + 0x2E), NULL);
    } else if (strcmp(c, "booster") == 0) {
        field(f, "energy_drain", u16(rec + 0x2A), NULL);
        field(f, "power", u16(rec + 0x2C), NULL);
        field(f, "boost_drain", u16(rec + 0x2E), "energy per step while boosting = this / 10");
    } else {
        field(f, "energy_drain", u16(rec + 0x2A),
              strcmp(c, "legs") == 0 ? "while moving" : NULL);
    }
    if (!strcmp(c, "head") || !strcmp(c, "core") || !strcmp(c, "arms") || !strcmp(c, "legs")) {
        field(f, "ap", u16(rec + 0x2C), NULL);
        field(f, "defense_shell", u16(rec + 0x2E), NULL);
        field(f, "defense_energy", u16(rec + 0x30), NULL);
    }
    if (!strcmp(c, "core")) {
        field(f, "arm_load_limit", u16(rec + 0x32), "arms + hand weapon + blade weight (weapon arms: arms alone)");
        field(f, "option_slots", u16(rec + 0x36), NULL);
    }
    if (!strcmp(c, "legs")) {
        field(f, "carry_limit", u16(rec + 0x32), NULL);
        field(f, "drain_standing", u16(rec + 0x34), NULL);
        field(f, "stability", u16(rec + 0x40), "resistance to impact shock");
        field(f, "boost_drain", u16(rec + 0x44), "used when no booster is fitted");
        field(f, "turn_accel", u16(rec + 0x38), "turn acceleration, divided by carried weight");
        field(f, "turn_speed", rec[0x3A], "top turn speed, divided by carried weight");
        if (acr_gen1_fixed_boost_moa(index))
            field(f, "fixed_boost", acr_gen1_fixed_boost_moa(index), "ground boost set by the game for these legs");
    }
    if (!strcmp(c, "blade")) {
        field(f, "charge_drain", u16(rec + 0x2C), "energy a swing takes (garage CHARGE DRAIN)");
        field(f, "attack", u16(rec + 0x30), NULL);
    }
    if (!strcmp(c, "hand") || !strcmp(c, "back") || !strcmp(c, "arms")) write_weapon(f, c, index);
    if (!strcmp(c, "fcs")) write_fcs(f, rec);
    write_fit(f, c, index);
    /* Everything after the type text as 16-bit words, so fields not yet
     * named stay available. */
    fprintf(f, "raw            = [");
    for (uint32_t o = 36; o + 1 < t->stride; o += 2)
        fprintf(f, "%s%u", o == 36 ? "" : ", ", (unsigned)u16(rec + o));
    fprintf(f, "]   # 16-bit words from byte 36 (price is the first two)\n");
    return fclose(f) == 0;
}

/* Optional parts: 44-byte records (code name, full name, u32 price, u16 slot
 * cost) at 0x800D1AA0; bit n of an AC's option mask fits option n. Their
 * descriptions sit in reverse order from 0x80052D58, one per option 11..0;
 * option 12 is a second Balancer the shop never sells. */
#define OPTION_TABLE      0x80CC8u   /* 0x800D1AA0 - load address 0x80050DD8 */
#define OPTION_STRIDE     44u
#define OPTION_COUNT      13u
#define OPTION_TEXT       0x1F80u    /* 0x80052D58 */

static int export_options(const uint8_t *data, size_t size, const char *out_dir, uint32_t *count) {
    if (OPTION_TABLE + OPTION_COUNT * OPTION_STRIDE > size || OPTION_TEXT + 0x200 > size) return 0;
    /* descriptions: consecutive strings, each padded to a 4-byte boundary */
    const char *desc[12];
    size_t at = OPTION_TEXT;
    for (int i = 11; i >= 0; --i) {
        desc[i] = (const char *)data + at;
        size_t len = 0;
        while (len < 64 && desc[i][len]) ++len;
        if (len == 0 || len == 64) return 0;
        at += (len + 4) & ~(size_t)3;
    }
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/option", out_dir);
    if (!make_dirs(dir)) return 0;
    for (uint32_t i = 0; i < OPTION_COUNT; ++i) {
        const uint8_t *rec = data + OPTION_TABLE + i * OPTION_STRIDE;
        char code[19], full[19], file_name[64], path[1200];
        if (!text_of(rec, code) || !text_of(rec + 18, full)) return 0;
        slug(code, file_name, sizeof file_name);
        snprintf(path, sizeof path, "%s/%02u-%s.toml", dir, (unsigned)i, file_name);
        FILE *f = fopen(path, "wb");
        if (!f) return 0;
        fprintf(f, "# Master of Arena optional part, imported from your disc by acr-import.\n");
        fprintf(f, "name           = \"%s\"\n", code);
        fprintf(f, "full_name      = \"%s\"\n", full);
        fprintf(f, "category       = \"option\"\n");
        fprintf(f, "generation     = \"gen1\"\n");
        fprintf(f, "bit            = %u   # bit in the AC's option mask\n", (unsigned)i);
        fprintf(f, "price          = %u\n", (unsigned)u32(rec + 36));
        fprintf(f, "slots          = %u   # core option slots it uses\n", (unsigned)u16(rec + 40));
        if (i < 12) fprintf(f, "description    = \"%s\"\n", desc[i]);
        else fprintf(f, "description    = \"Decreases Impact shock. (Not sold in the shop.)\"\n");
        if (fclose(f) != 0) return 0;
        ++*count;
    }
    return 1;
}

int acr_export_moa_parts(const char *menu_module, const char *out_dir, uint32_t *count,
                         char *err, size_t err_size) {
    *count = 0;
    FILE *f = fopen(menu_module, "rb");
    if (!f) { snprintf(err, err_size, "cannot open %s", menu_module); return 0; }
    uint8_t *data = malloc(1u << 20);
    if (!data) { fclose(f); snprintf(err, err_size, "out of memory"); return 0; }
    const size_t size = fread(data, 1, 1u << 20, f);
    fclose(f);
    g_menu = data;
    g_menu_size = size;
    {   /* the VS engine sits next to the menu module: .../FDAT/209.bin */
        char vs_path[1200];
        snprintf(vs_path, sizeof vs_path, "%s", menu_module);
        char *slash = strrchr(vs_path, '/');
        if (slash && (size_t)(slash - vs_path) + 9 < sizeof vs_path) {
            strcpy(slash + 1, "209.bin");
            FILE *vf = fopen(vs_path, "rb");
            if (vf) {
                g_vs = malloc(1u << 20);
                if (g_vs) g_vs_size = fread(g_vs, 1, 1u << 20, vf);
                fclose(vf);
            }
        }
    }
    int ok = 1;
    for (size_t t = 0; ok && t < sizeof k_tables / sizeof k_tables[0]; ++t) {
        const table *tb = &k_tables[t];
        char name[19];
        if (tb->offset + tb->stride > size || !name_of(data + tb->offset, name) ||
            strcmp(name, tb->first) != 0) {
            snprintf(err, err_size, "the %s table is not where Master of Arena (USA) keeps it",
                     tb->category);
            ok = 0;
            break;
        }
        for (uint32_t i = 0; i < tb->count && tb->offset + (i + 1) * tb->stride <= size; ++i) {
            const uint8_t *rec = data + tb->offset + i * tb->stride;
            char type[19] = "";
            if (!name_of(rec, name)) continue;
            text_of(rec + 18, type);
            /* placeholder slots and "no weapon" are not parts */
            if (strcmp(name, "DUMMY") == 0 || strcmp(type, "DUMMY PARTS") == 0 ||
                strcmp(name, "NO WEAPON") == 0) continue;
            char dir[1100];
            const char *sub = sub_folder(tb->category, type);
            snprintf(dir, sizeof dir, "%s/%s%s%s", out_dir, tb->category, sub ? "/" : "",
                     sub ? sub : "");
            if (!make_dirs(dir) || !write_part(dir, tb, i, rec, name, type)) {
                snprintf(err, err_size, "cannot write parts into %s", dir);
                ok = 0;
                break;
            }
            ++*count;
        }
    }
    if (ok && !export_options(data, size, out_dir, count)) {
        snprintf(err, err_size, "the optional parts table is not where Master of Arena (USA) keeps it");
        ok = 0;
    }
    g_menu = NULL;
    free(g_vs);
    g_vs = NULL;
    g_vs_size = 0;
    free(data);
    return ok;
}
