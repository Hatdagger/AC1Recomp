/* Master of Arena: Project Phantasma arena package generator. See the header.
 *
 * The package adds a tenth Ex Arena to Master of Arena's menu file
 * (GG/MS/MENU_TIM.T): a copy of the file with a block of 204 entries added,
 * written into the disc's spare area, plus a few small patches to the menu
 * code (the arena list entry, its name and the arena tables). The block's
 * descriptions, emblems, portraits, AC builds and names come from the
 * Phantasma disc; the AI settings and prize money are tuning of our own. The
 * game plug-in (psxrecomp/runtime/src/mod_builtin_moa_phantasma.c) supplies the
 * logic that does not fit in those patches.
 */
#include "moa_phantasma.h"

#include "psx_sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define SECTOR 2048u

/* ---- disc images -------------------------------------------------------------- */

struct disc {
    FILE    *f;
    uint32_t stride;    /* bytes per sector in the image */
    uint32_t skip;      /* offset of the 2048 data bytes in a sector */
    uint32_t sectors;
};

static int has_ext(const char *p, const char *ext) {
    const size_t n = strlen(p), e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; ++i) {
        char a = p[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* The first FILE of a cue sheet, as a path beside the sheet. */
static int cue_image(const char *cue, char *out, size_t cap) {
    FILE *f = fopen(cue, "rb");
    if (!f) return 0;
    char line[600];
    int ok = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "FILE", 4) != 0) continue;
        char *a = strchr(p, '"'), *b = a ? strchr(a + 1, '"') : NULL;
        if (!a || !b) continue;
        *b = '\0';
        const char *name = a + 1;
        const char *slash = cue + strlen(cue);
        while (slash > cue && slash[-1] != '/' && slash[-1] != '\\') --slash;
        const size_t dir = (size_t)(slash - cue);
        if (dir + strlen(name) + 1 > cap) break;
        memcpy(out, cue, dir);
        strcpy(out + dir, name);
        ok = 1;
        break;
    }
    fclose(f);
    return ok;
}

static void disc_close(struct disc *d) {
    if (d->f) fclose(d->f);
    d->f = NULL;
}

static int disc_open(struct disc *d, const char *path) {
    char image[1100];
    memset(d, 0, sizeof *d);
    if (has_ext(path, ".cue")) {
        if (!cue_image(path, image, sizeof image)) return 0;
        path = image;
    }
    d->f = fopen(path, "rb");
    if (!d->f) return 0;
    uint8_t head[24];
    if (fread(head, 1, sizeof head, d->f) != sizeof head) { disc_close(d); return 0; }
    static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    if (memcmp(head, sync, 12) == 0) {
        d->stride = 2352;
        d->skip = head[15] == 2 ? 24u : 16u;
    } else {
        d->stride = 2048;
        d->skip = 0;
    }
    fseek(d->f, 0, SEEK_END);
    const long long size = ftell(d->f);
    d->sectors = size > 0 ? (uint32_t)(size / d->stride) : 0;
    return 1;
}

static int disc_read(struct disc *d, uint32_t lba, uint32_t count, uint8_t *out) {
    if (lba + count > d->sectors) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const long long at = (long long)(lba + i) * d->stride + d->skip;
#ifdef _WIN32
        if (_fseeki64(d->f, at, SEEK_SET) != 0) return 0;
#else
        if (fseeko(d->f, (off_t)at, SEEK_SET) != 0) return 0;
#endif
        if (fread(out + (size_t)i * SECTOR, 1, SECTOR, d->f) != SECTOR) return 0;
    }
    return 1;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int name_is(const uint8_t *rec, const char *want) {
    const unsigned n = rec[32];
    unsigned len = n;
    for (unsigned i = 0; i < n; ++i) if (rec[33 + i] == ';') { len = i; break; }
    if (len != strlen(want)) return 0;
    for (unsigned i = 0; i < len; ++i) {
        char a = (char)rec[33 + i], b = want[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* A file by path ("GG/MS/MENU_TIM.T"): its first sector and size. */
static int disc_find(struct disc *d, const char *path, uint32_t *lba, uint32_t *size) {
    uint8_t pvd[SECTOR];
    if (!disc_read(d, 16, 1, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) return 0;
    uint32_t dir = le32(pvd + 156 + 2), dsize = le32(pvd + 156 + 10);
    const char *p = path;
    for (;;) {
        char part[40];
        size_t n = 0;
        while (p[n] && p[n] != '/' && n + 1 < sizeof part) { part[n] = p[n]; ++n; }
        part[n] = '\0';
        const int last = p[n] == '\0';
        if (dsize == 0 || dsize > 4u * 1024 * 1024) return 0;
        const uint32_t secs = (dsize + SECTOR - 1) / SECTOR;
        uint8_t *buf = (uint8_t *)malloc((size_t)secs * SECTOR);
        if (!buf) return 0;
        int found = 0;
        if (disc_read(d, dir, secs, buf)) {
            uint32_t i = 0;
            while (i < dsize) {
                const uint8_t len = buf[i];
                if (len == 0) { i = (i / SECTOR + 1) * SECTOR; continue; }
                if (i + len > dsize) break;
                if (name_is(buf + i, part)) {
                    dir = le32(buf + i + 2);
                    dsize = le32(buf + i + 10);
                    found = 1;
                    break;
                }
                i += len;
            }
        }
        free(buf);
        if (!found) return 0;
        if (last) { *lba = dir; *size = dsize; return 1; }
        p += n + 1;
    }
}

static uint8_t *disc_file(struct disc *d, const char *path, uint32_t *size) {
    uint32_t lba, bytes;
    if (!disc_find(d, path, &lba, &bytes) || bytes == 0 || bytes > 64u * 1024 * 1024) return NULL;
    const uint32_t secs = (bytes + SECTOR - 1) / SECTOR;
    uint8_t *buf = (uint8_t *)malloc((size_t)secs * SECTOR);
    if (!buf) return NULL;
    if (!disc_read(d, lba, secs, buf)) { free(buf); return NULL; }
    *size = bytes;
    return buf;
}

int moa_phantasma_is_disc(const char *path) {
    struct disc d;
    uint32_t lba, size;
    if (!path || !disc_open(&d, path)) return 0;
    const int ok = disc_find(&d, "SLUS_006.70", &lba, &size);
    disc_close(&d);
    return ok;
}

/* ---- the arena block ------------------------------------------------------------ */

/* Tuning of our own: AI settings (36 bytes a rival; byte 0 is the Human PLUS
 * level) and prize money by rank. */
#include "moa_phantasma_tables.inc"

#define STOCK_SECTORS   2906u    /* MENU_TIM.T in Master of Arena */
#define STOCK_BYTES     (STOCK_SECTORS * SECTOR)
#define PP_ENTRIES      524u     /* entries in Phantasma's MENU_TIM.T */
#define BLOCK_SECTORS   604u

/* Phantasma entries that make up the block. */
#define PP_DESC1        232u
#define PP_EMBLEM       282u
#define PP_PORTRAIT     332u
#define PP_BUILDS       382u
#define PP_DESC2        432u
#define PP_NAMES        482u

/* Every entry ends with a checksum of the rest of it: the sum of its 32-bit
 * words plus 0x12345678. */
static void seal(uint8_t *entry, size_t bytes) {
    uint32_t sum = 0x12345678u;
    for (size_t i = 0; i + 4 < bytes; i += 4) sum += le32(entry + i);
    entry[bytes - 4] = (uint8_t)sum;
    entry[bytes - 3] = (uint8_t)(sum >> 8);
    entry[bytes - 2] = (uint8_t)(sum >> 16);
    entry[bytes - 1] = (uint8_t)(sum >> 24);
}

struct entries {
    const uint8_t *data;
    uint32_t size;
    uint32_t start[PP_ENTRIES + 1];
};

static int entries_load(struct entries *e, const uint8_t *data, uint32_t size) {
    e->data = data;
    e->size = size;
    if (size < SECTOR || (data[0] | data[1] << 8) != (int)PP_ENTRIES) return 0;
    for (unsigned i = 0; i <= PP_ENTRIES; ++i) {
        e->start[i] = (uint32_t)(data[2 + 2 * i] | data[3 + 2 * i] << 8);
        if (i && e->start[i] < e->start[i - 1]) return 0;
        if ((uint64_t)e->start[i] * SECTOR > size) return 0;
    }
    return 1;
}

static const uint8_t *entry_at(const struct entries *e, unsigned k, uint32_t *sectors) {
    *sectors = e->start[k + 1] - e->start[k];
    return e->data + (size_t)e->start[k] * SECTOR;
}

/* A description: the text without the Phantasma end marker ('>' and the
 * ^Z after it). */
static void put_text(uint8_t *out, const uint8_t *in) {
    size_t o = 0;
    for (size_t i = 0; i < SECTOR - 4; ++i)
        if (in[i] != '>' && in[i] != 0x1A) out[o++] = in[i];
    seal(out, SECTOR);
}

static int build_block(const struct entries *pp, uint8_t *block, char *err, unsigned errn) {
    uint8_t *at = block;
    uint32_t n;
    memset(block, 0, (size_t)BLOCK_SECTORS * SECTOR);
    for (unsigned k = 0; k < 50; ++k, at += SECTOR) {
        const uint8_t *s = entry_at(pp, PP_DESC1 + k, &n);
        if (n != 1) goto bad;
        put_text(at, s);
    }
    for (unsigned k = 0; k < 49; ++k, at += 2 * SECTOR) {
        const uint8_t *s = entry_at(pp, PP_EMBLEM + k, &n);
        if (n != 2) goto bad;
        memcpy(at, s, 2 * SECTOR);
    }
    for (unsigned k = 0; k < 50; ++k, at += 8 * SECTOR) {
        const uint8_t *s = entry_at(pp, PP_PORTRAIT + k, &n);
        if (n != 8) goto bad;
        memcpy(at, s, 8 * SECTOR);
    }
    for (unsigned k = 0; k < 50; ++k, at += SECTOR) {
        const uint8_t *s = entry_at(pp, PP_DESC2 + k, &n);
        if (n != 1) goto bad;
        put_text(at, s);
    }
    {   /* AC builds: Master of Arena has one more back weapon, from slot 17 on. */
        const uint8_t *s = entry_at(pp, PP_BUILDS, &n);
        if (n != 2) goto bad;
        memcpy(at, s, 2 * SECTOR);
        for (unsigned r = 0; r < 50; ++r)
            for (unsigned c = 6; c <= 7; ++c) {
                uint8_t *v = at + r * 64 + c;
                if (*v >= 17 && *v != 255) ++*v;
            }
        seal(at, 2 * SECTOR);
        at += 2 * SECTOR;
    }
    {   /* Names. */
        const uint8_t *s = entry_at(pp, PP_NAMES, &n);
        if (n != 2) goto bad;
        for (size_t i = 0; i < 2 * SECTOR - 4; ++i) at[i] = s[i] == '>' ? 0 : s[i];
        seal(at, 2 * SECTOR);
        at += 2 * SECTOR;
    }
    for (unsigned r = 0; r < 50; ++r) {     /* Prize money; no unlocks. */
        const uint32_t m = k_money[r];
        for (unsigned b = 0; b < 4; ++b) {
            at[r * 8 + b] = (uint8_t)(m >> (8 * b));
            at[r * 8 + 4 + b] = 0xFF;
        }
    }
    seal(at, SECTOR);
    at += SECTOR;
    for (unsigned r = 0; r < 50; ++r) memcpy(at + r * 36, k_ai_template[k_ai_for[r]], 36);
    seal(at, SECTOR);
    at += SECTOR;
    if ((size_t)(at - block) != (size_t)BLOCK_SECTORS * SECTOR) goto bad;
    return 1;
bad:
    snprintf(err, errn, "The Project Phantasma disc does not have the expected arena data.");
    return 0;
}

/* ---- menu code patches ---------------------------------------------------------- */

struct patch {
    uint32_t offset;     /* byte offset on the disc (data bytes only) */
    uint8_t  length;
    uint8_t  old_bytes[17];
    uint8_t  new_bytes[17];
};

static const struct patch k_patches[] = {
#include "moa_phantasma_patches.inc"
};
#define PATCH_COUNT (sizeof k_patches / sizeof k_patches[0])

/* Where the menu file's copy goes (the disc's spare area), and its directory
 * record: both-endian start sector, then both-endian size. */
#define COPY_SECTOR       282670u
#define DIR_RECORD_OFFSET 299833956u
#define CHECKSUM_OFFSET   287711228u

static int read_bytes(struct disc *d, uint32_t offset, uint32_t n, uint8_t *out) {
    uint8_t sec[2 * SECTOR];
    const uint32_t lba = offset / SECTOR, at = offset % SECTOR;
    const uint32_t cnt = (at + n + SECTOR - 1) / SECTOR;
    if (cnt > 2 || !disc_read(d, lba, cnt, sec)) return 0;
    memcpy(out, sec + at, n);
    return 1;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- manifest and archive ---------------------------------------------------------- */

struct text { char *s; size_t n, cap; };

static void text_add(struct text *t, const char *s) {
    const size_t l = strlen(s);
    if (t->n + l + 1 > t->cap) {
        t->cap = (t->n + l + 1) * 2;
        t->s = (char *)realloc(t->s, t->cap);
    }
    memcpy(t->s + t->n, s, l + 1);
    t->n += l;
}

static void text_hex(struct text *t, const uint8_t *p, size_t n) {
    char b[3];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof b, "%02x", p[i]);
        text_add(t, b);
        if (i + 1 < n) text_add(t, " ");
    }
}

static void text_patch(struct text *t, uint32_t offset, const uint8_t *was, const uint8_t *now, size_t n) {
    char b[64];
    text_add(t, "\n[[patch]]\nfeature = \"arena\"\ntarget = \"disc_user\"\n");
    snprintf(b, sizeof b, "offset = %u\n", offset);
    text_add(t, b);
    text_add(t, "expected = \"");
    text_hex(t, was, n);
    text_add(t, "\"\nreplace = \"");
    text_hex(t, now, n);
    text_add(t, "\"\n");
}

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

static void w16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f); }
static void w32(FILE *f, uint32_t v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

/* A zip with two stored (uncompressed) files. */
static int write_archive(const char *path, const char *const names[2],
                         const uint8_t *const data[2], const size_t size[2]) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t crc[2], where[2], central = 0;
    for (int i = 0; i < 2; ++i) {
        crc[i] = crc32_of(data[i], size[i]);
        where[i] = (uint32_t)ftell(f);
        w32(f, 0x04034B50u); w16(f, 20); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0x21);
        w32(f, crc[i]); w32(f, (uint32_t)size[i]); w32(f, (uint32_t)size[i]);
        w16(f, (unsigned)strlen(names[i])); w16(f, 0);
        fwrite(names[i], 1, strlen(names[i]), f);
        fwrite(data[i], 1, size[i], f);
    }
    central = (uint32_t)ftell(f);
    for (int i = 0; i < 2; ++i) {
        w32(f, 0x02014B50u); w16(f, 20); w16(f, 20); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0x21);
        w32(f, crc[i]); w32(f, (uint32_t)size[i]); w32(f, (uint32_t)size[i]);
        w16(f, (unsigned)strlen(names[i])); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0);
        w32(f, 0); w32(f, where[i]);
        fwrite(names[i], 1, strlen(names[i]), f);
    }
    const uint32_t end = (uint32_t)ftell(f);
    w32(f, 0x06054B50u); w16(f, 0); w16(f, 0); w16(f, 2); w16(f, 2);
    w32(f, end - central); w32(f, central); w16(f, 0);
    const int ok = fflush(f) == 0 && ferror(f) == 0;
    fclose(f);
    return ok;
}

/* ---- the mission pack ---------------------------------------------------------------- */

/* The 17 Project Phantasma missions, ported to Master of Arena's mission engine. Four
 * blocks a mission (textures, common battle data with the mission's messages, mission
 * code, map), each sealed like an archive entry. The recipe says, for every block, which
 * bytes to copy from which disc entry and which XOR corrections to apply on top (the
 * port: relocated code, Master of Arena struct offsets, message control codes). */
#include <zlib.h>
#include "moa_phantasma_missions.inc"

#define PPM_BLOCKS      68u
#define PPM_PACK_SECTORS 10455u

/* A T archive: u16 count, then count+1 u16 start sectors. */
struct tarc {
    const uint8_t *data;
    uint32_t size, count;
};

static int tarc_load(struct tarc *t, const uint8_t *data, uint32_t size) {
    t->data = data;
    t->size = size;
    if (!data || size < SECTOR) return 0;
    t->count = (uint32_t)(data[0] | data[1] << 8);
    if (2u + 2u * (t->count + 1u) > SECTOR) return 0;
    return 1;
}

static const uint8_t *tarc_entry(const struct tarc *t, unsigned k, uint32_t *bytes) {
    if (k >= t->count) return NULL;
    const uint32_t a = (uint32_t)(t->data[2 + 2 * k] | t->data[3 + 2 * k] << 8);
    const uint32_t b = (uint32_t)(t->data[4 + 2 * k] | t->data[5 + 2 * k] << 8);
    if (b < a || (uint64_t)b * SECTOR > t->size) return NULL;
    *bytes = (b - a) * SECTOR;
    return t->data + (size_t)a * SECTOR;
}

/* Build the pack (PPM_PACK_SECTORS sectors) into `pack`. */
static int build_missions(const struct tarc src[3], uint8_t *pack, char *err, unsigned errn) {
    uint8_t *r = (uint8_t *)malloc(PPM_RECIPE_RAW);
    if (!r) { snprintf(err, errn, "Out of memory."); return 0; }
    uLongf rn = PPM_RECIPE_RAW;
    int ok = 0;
    if (uncompress(r, &rn, k_ppm_recipe, sizeof k_ppm_recipe) != Z_OK || rn != PPM_RECIPE_RAW ||
        memcmp(r, "PPM1", 4) != 0 || le32(r + 4) != PPM_BLOCKS) {
        snprintf(err, errn, "The mission recipe is damaged.");
        goto out;
    }
    {
        const uint8_t *p = r + 8, *end = r + rn;
        size_t at = 0;
        for (unsigned b = 0; b < PPM_BLOCKS; ++b) {
            if (end - p < 10) goto bad;
            const uint32_t bytes = le32(p) * SECTOR;
            const unsigned nseg = (unsigned)(p[4] | p[5] << 8);
            const uint32_t nrun = le32(p + 6);
            p += 10;
            if (at + bytes > (size_t)PPM_PACK_SECTORS * SECTOR || bytes < 4) goto bad;
            uint8_t *blk = pack + at;
            memset(blk, 0, bytes);
            for (unsigned s = 0; s < nseg; ++s, p += 15) {
                if (end - p < 15) goto bad;
                const uint32_t dst = le32(p), src_off = le32(p + 7), len = le32(p + 11);
                const unsigned arc = p[4], ent = (unsigned)(p[5] | p[6] << 8);
                uint32_t have = 0;
                const uint8_t *e = arc < 3 ? tarc_entry(&src[arc], ent, &have) : NULL;
                if (!e || (uint64_t)src_off + len > have || (uint64_t)dst + len > bytes) goto bad;
                memcpy(blk + dst, e + src_off, len);
            }
            for (uint32_t k = 0; k < nrun; ++k) {
                if (end - p < 6) goto bad;
                const uint32_t off = le32(p);
                const unsigned len = (unsigned)(p[4] | p[5] << 8);
                p += 6;
                if (end - p < (ptrdiff_t)len || (uint64_t)off + len > bytes) goto bad;
                for (unsigned i = 0; i < len; ++i) blk[off + i] ^= p[i];
                p += len;
            }
            seal(blk, bytes);
            at += bytes;
        }
        if (at != (size_t)PPM_PACK_SECTORS * SECTOR || p != end) goto bad;
    }
    {   /* Exactly the pack the missions were tested with. */
        uint8_t hash[32];
        char hex[65];
        psx_sha256_compute(pack, (size_t)PPM_PACK_SECTORS * SECTOR, hash);
        for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
        if (strcmp(hex, PPM_PACK_SHA256) != 0) {
            snprintf(err, errn, "This Project Phantasma or Master of Arena disc is not the one "
                                "the missions were made for (US discs).");
            goto out;
        }
    }
    ok = 1;
    goto out;
bad:
    snprintf(err, errn, "This Project Phantasma or Master of Arena disc is not the one the "
                        "missions were made for (US discs).");
out:
    free(r);
    return ok;
}

/* ---- the package ----------------------------------------------------------------- */

static const char k_manifest_head[] =
    "format_version = 5\n"
    "id = \"community.moa.phantasma-arena\"\n"
    "version = \"1.1.0\"\n"
    "name = \"Master of Arena: Project Phantasma Arena\"\n"
    "author = \"AC1Recomp community\"\n"
    "description = \"Adds a tenth Ex Arena, 'Project Phantasma', with the 49 arena opponents of "
    "Armored Core: Project Phantasma and its own rank starting at 50. Built from your own Master of "
    "Arena and Project Phantasma discs; requires the moa.phantasma-arena plugin in the game build.\"\n"
    "resolver = \"declarative\"\n"
    "save_compatibility = \"shared\"\n"
    "\n[[target]]\ngame_id = \"SLUS-01030\"\n"
    "disc_sha256 = \"9e18bb329c8c35555fafbefd2ef370c67fedec164497e5bf9f25c848a97b921e\"\n"
    "\n[[target]]\ngame_id = \"SLUS-01081\"\n"
    "disc_sha256 = \"40be1b9ac8b66de3866a8d9700a5f053d8e5de2abffbede96b290570147c83e2\"\n"
    "\n[[feature]]\nid = \"arena\"\nname = \"Project Phantasma arena\"\n"
    "description = \"Tenth entry in the Ex Arena list. Turn off to restore the original game.\"\n"
    "group = \"Gameplay\"\ndefault_enabled = false\n"
    "\n[[plugin]]\nfeature = \"arena\"\nid = \"moa.phantasma-arena\"\n";

int moa_phantasma_build(const char *moa_disc, const char *pp_disc, const char *out_path,
                        char *error, unsigned error_size) {
    struct disc moa, pp;
    uint8_t *stock = NULL, *ppmenu = NULL, *asset = NULL;
    uint32_t stock_bytes = 0, pp_bytes = 0;
    struct text manifest = {0};
    int ok = 0;
    memset(&moa, 0, sizeof moa);
    memset(&pp, 0, sizeof pp);
    if (!moa_disc || !pp_disc || !out_path) return 0;

    if (!disc_open(&moa, moa_disc)) {
        snprintf(error, error_size, "The Master of Arena disc could not be opened.");
        goto done;
    }
    if (!disc_open(&pp, pp_disc)) {
        snprintf(error, error_size, "The Project Phantasma disc could not be opened.");
        goto done;
    }
    stock = disc_file(&moa, "GG/MS/MENU_TIM.T", &stock_bytes);
    if (!stock || stock_bytes != STOCK_BYTES || le32(stock) == 0 ||
        (stock[0] | stock[1] << 8) != 885) {
        snprintf(error, error_size,
                 "This is not the US Armored Core: Master of Arena disc 1 (menu file not found).");
        goto done;
    }
    ppmenu = disc_file(&pp, "GG/MS/MENU_TIM.T", &pp_bytes);
    struct entries ents;
    if (!ppmenu || !entries_load(&ents, ppmenu, pp_bytes)) {
        snprintf(error, error_size, "This is not the US Armored Core: Project Phantasma disc.");
        goto done;
    }

    asset = (uint8_t *)malloc((size_t)(STOCK_SECTORS + BLOCK_SECTORS) * SECTOR);
    if (!asset) { snprintf(error, error_size, "Out of memory."); goto done; }
    memcpy(asset, stock, STOCK_BYTES);
    if (!build_block(&ents, asset + STOCK_BYTES, error, error_size)) goto done;
    const size_t asset_bytes = (size_t)(STOCK_SECTORS + BLOCK_SECTORS) * SECTOR;

    /* The Master of Arena disc must carry the bytes the patches expect. */
    uint32_t menu_lba = 0, menu_size = 0;
    disc_find(&moa, "GG/MS/MENU_TIM.T", &menu_lba, &menu_size);
    uint8_t dir_was[16], dir_now[16];
    if (!read_bytes(&moa, DIR_RECORD_OFFSET, 16, dir_was)) goto wrong_disc;
    put_le32(dir_now, COPY_SECTOR); put_be32(dir_now + 4, COPY_SECTOR);
    put_le32(dir_now + 8, (uint32_t)asset_bytes); put_be32(dir_now + 12, (uint32_t)asset_bytes);
    {
        uint8_t stock_rec[16];
        put_le32(stock_rec, menu_lba); put_be32(stock_rec + 4, menu_lba);
        put_le32(stock_rec + 8, STOCK_BYTES); put_be32(stock_rec + 12, STOCK_BYTES);
        if (memcmp(dir_was, stock_rec, 16) != 0) goto wrong_disc;
    }
    text_add(&manifest, k_manifest_head);

    /* The menu file's copy, in the disc's spare area. */
    uint8_t hash[32];
    char line[160];
    psx_sha256_compute(asset, asset_bytes, hash);
    snprintf(line, sizeof line,
             "\n[[overlay]]\nfeature = \"arena\"\ntarget = \"disc_user\"\noffset = %llu\n"
             "file = \"assets/menu_tim.bin\"\nsha256 = \"",
             (unsigned long long)COPY_SECTOR * SECTOR);
    text_add(&manifest, line);
    for (int i = 0; i < 32; ++i) { snprintf(line, 8, "%02x", hash[i]); text_add(&manifest, line); }
    text_add(&manifest, "\"\n");

    text_patch(&manifest, DIR_RECORD_OFFSET, dir_was, dir_now, 16);

    uint32_t delta = 0;
    for (size_t i = 0; i < PATCH_COUNT; ++i) {
        const struct patch *p = &k_patches[i];
        uint8_t was[17];
        if (!read_bytes(&moa, p->offset, p->length, was) || memcmp(was, p->old_bytes, p->length) != 0)
            goto wrong_disc;
        text_patch(&manifest, p->offset, was, p->new_bytes, p->length);
        for (unsigned b = 0; b < p->length; ++b)
            delta += ((uint32_t)p->new_bytes[b] - (uint32_t)p->old_bytes[b]) << (8 * ((p->offset + b) & 3));
    }
    {   /* The menu code's own checksum follows its changes. */
        uint8_t was[4], now[4];
        if (!read_bytes(&moa, CHECKSUM_OFFSET, 4, was)) goto wrong_disc;
        put_le32(now, le32(was) + delta);
        text_patch(&manifest, CHECKSUM_OFFSET, was, now, 4);
    }

    {
        const char *names[2] = { "manifest.toml", "assets/menu_tim.bin" };
        const uint8_t *data[2] = { (const uint8_t *)manifest.s, asset };
        const size_t size[2] = { manifest.n, asset_bytes };
        if (!write_archive(out_path, names, data, size)) {
            snprintf(error, error_size, "The arena package could not be written.");
            goto done;
        }
    }
    ok = 1;
    goto done;
wrong_disc:
    snprintf(error, error_size,
             "This Master of Arena disc is not the one the arena was made for (US, disc 1).");
done:
    free(stock);
    free(ppmenu);
    free(asset);
    free(manifest.s);
    disc_close(&moa);
    disc_close(&pp);
    return ok;
}
