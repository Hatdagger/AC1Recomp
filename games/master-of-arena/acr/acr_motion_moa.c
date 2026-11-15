/* Master of Arena motion: which animation set an AC uses, and each
 * animation's root motion (how far the AC itself moves), as readable files.
 *
 * Walking, strafing and the jump take-off are driven by these tracks, not by
 * formulas (docs/research/gen1-movement.md). The game picks one of 72
 * animation sets from the core, arms and legs (table in the menu module,
 * FDAT entry 206), and each set lives in GG/P<n>/PA<nn>.T entry 1.
 *
 * Animation set (entry 1, after a 4-byte size): u32 count, 8 bytes, then
 * u16 offsets (from +12) to each animation. Animation: u16 x2 (unknown),
 * u16 length in frames, u16 key count (bit 15: a rotation track follows),
 * then keys of u16 frame + s16 x, y, z: the root's displacement since the
 * start, in 1/4 world units. */
#include "acr_motion_moa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_BASE      0x80050DD8u
#define MODEL_TABLE    0x800D4A64u   /* 12 bytes: core, 3 pad, ptr arms list, ptr legs list */
#define MODEL_COUNT    72u

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }

static uint8_t *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        const size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    fclose(f);
    *size = n;
    return buf;
}

/* Names for the animations seen in play; the others keep their number. */
static const char *anim_name(unsigned i) {
    switch (i) {
    case 0: return "idle";
    case 1: return "walk-start";       case 2: return "walk";
    case 8: return "strafe-left-start"; case 9: return "strafe-left";
    case 11: return "strafe-right-start"; case 12: return "strafe-right";
    case 14: return "back-start";      case 15: return "back";
    case 21: return "turn";
    case 22: return "jump";            case 24: return "air";
    case 48: return "boost-start";     case 49: return "boost";
    default: return NULL;
    }
}

static int write_list(FILE *f, const uint8_t *menu, size_t size, uint32_t ptr) {
    if (ptr < MENU_BASE || ptr - MENU_BASE >= size) return 0;
    size_t o = ptr - MENU_BASE;
    fputc('[', f);
    for (int first = 1; o < size && menu[o] != 0xFF; ++o, first = 0)
        fprintf(f, "%s%u", first ? "" : ", ", (unsigned)menu[o]);
    fputc(']', f);
    return o < size;
}

static int write_model(const char *path, unsigned id, const uint8_t *set, size_t size) {
    if (size < 16) return 0;
    const uint32_t count = u32(set);
    if (count == 0 || count > 256 || 12 + 2 * (size_t)count > size) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "# Master of Arena animation set %u (root motion only), imported from your disc.\n", id);
    fprintf(f, "# keys: [frame, x, y, z] = how far the AC has moved since the animation began,\n");
    fprintf(f, "# in 1/4 world units (z forward is negative, y up is negative).\n");
    fprintf(f, "model = %u\n", id);
    int ok = 1;
    for (uint32_t i = 0; ok && i < count; ++i) {
        const size_t a = 12 + (size_t)u16(set + 12 + 2 * i);
        if (a + 8 > size) { ok = 0; break; }
        const unsigned keys = u16(set + a + 6) & 0x7FFF;
        if (a + 8 + 8 * (size_t)keys > size) { ok = 0; break; }
        fprintf(f, "\n[[anim]]\nindex  = %u\n", (unsigned)i);
        if (anim_name(i)) fprintf(f, "name   = \"%s\"\n", anim_name(i));
        fprintf(f, "length = %u\nheader = [%u, %u]\nkeys   = [", (unsigned)u16(set + a + 4),
                (unsigned)u16(set + a), (unsigned)u16(set + a + 2));
        for (unsigned k = 0; k < keys; ++k) {
            const uint8_t *e = set + a + 8 + 8 * k;
            fprintf(f, "%s[%u, %d, %d, %d]", k ? ", " : "", (unsigned)u16(e),
                    (int)(int16_t)u16(e + 2), (int)(int16_t)u16(e + 4), (int)(int16_t)u16(e + 6));
        }
        fprintf(f, "]\n");
    }
    return fclose(f) == 0 && ok;
}

int acr_export_moa_motion(const char *menu_module, const char *unpacked_dir, const char *out_dir,
                          uint32_t *count, char *err, size_t err_size) {
    *count = 0;
    size_t menu_size = 0;
    uint8_t *menu = read_all(menu_module, &menu_size);
    if (!menu) { snprintf(err, err_size, "cannot read %s", menu_module); return 0; }
    const size_t table = MODEL_TABLE - MENU_BASE;
    if (table + 12 * MODEL_COUNT > menu_size) {
        free(menu);
        snprintf(err, err_size, "the animation set table is not where Master of Arena (USA) keeps it");
        return 0;
    }
    char path[1200];
    snprintf(path, sizeof path, "%s/models.toml", out_dir);
    FILE *f = fopen(path, "wb");
    if (!f) { free(menu); snprintf(err, err_size, "cannot write %s", path); return 0; }
    fprintf(f, "# Which animation set an AC uses: the first model whose core matches and\n"
               "# whose lists hold its arms and legs (game indices). Cores 3 and 4 count as\n"
               "# core 0. Imported from your disc by acr-import.\n");
    int ok = 1;
    for (unsigned i = 0; ok && i < MODEL_COUNT; ++i) {
        const uint8_t *e = menu + table + 12 * i;
        fprintf(f, "\n[[model]]\nid   = %u\ncore = %u\narms = ", i, (unsigned)e[0]);
        ok = write_list(f, menu, menu_size, u32(e + 4));
        fprintf(f, "\nlegs = ");
        ok = ok && write_list(f, menu, menu_size, u32(e + 8));
        fputc('\n', f);
    }
    ok = (fclose(f) == 0) && ok;
    free(menu);
    if (!ok) { snprintf(err, err_size, "the animation set table did not check out"); return 0; }

    for (unsigned i = 0; i < MODEL_COUNT; ++i) {
        char src[1200], dst[1200];
        snprintf(src, sizeof src, "%s/GG/P%u/PA%02u/001.bin", unpacked_dir, i / 20, i);
        size_t size = 0;
        uint8_t *data = read_all(src, &size);
        if (!data) continue;   /* a missing set is skipped, not fatal */
        snprintf(dst, sizeof dst, "%s/model-%02u.toml", out_dir, i);
        if (size > 4 && write_model(dst, i, data + 4, size - 4)) ++*count;
        free(data);
    }
    return 1;
}
