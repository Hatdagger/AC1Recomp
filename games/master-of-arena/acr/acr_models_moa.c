/* Master of Arena AC models as readable files: OBJ per piece, PNG per part,
 * a rig file per animation set and the joint animations as text.
 * Formats: docs/research/gen1-models.md. Everything is read from the user's
 * own import folder; nothing from the disc is built into this program.
 *
 * Space: the game's model space is x right, y down, z forward. The OBJ files
 * use x right, y up, z back: (x, -y, -z), a half turn about x, so faces keep
 * their winding. Rig and animation files keep the game's own numbers. */
#include "acr_models_moa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define acr_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define acr_mkdir(p) mkdir((p), 0755)
#endif

#define MENU_BASE   0x80050DD8u
#define SET_TABLE   0x800D4A64u   /* 12 bytes: core, 3 pad, ptr arms list, ptr legs list */
#define SET_COUNT   72u
#define MAX_NODES   64u
#define MAX_SLOTS   32u

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t s16(const uint8_t *p) { return (int16_t)u16(p); }
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

static int make_dirs(const char *path) {
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; ; ++p) {
        if (*p == '/' || *p == '\\' || *p == 0) {
            const char c = *p;
            *p = 0;
            if (acr_mkdir(buf) != 0 && errno != EEXIST) return 0;
            if (!c) break;
            *p = c;
        }
    }
    return 1;
}

/* ---- PNG (8-bit indexed, stored deflate) -------------------------------- */

uint32_t acr_crc32(const uint8_t *data, size_t n, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t head[8];
    put32(head, n);
    memcpy(head + 4, type, 4);
    uint32_t crc = acr_crc32(head + 4, 4, 0);
    crc = acr_crc32(data, n, crc);
    uint8_t tail[4];
    put32(tail, crc);
    return fwrite(head, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) &&
           fwrite(tail, 1, 4, f) == 4;
}

int acr_write_png8(const char *path, unsigned w, unsigned h, const uint8_t *pixels,
                   const uint8_t *palette, const uint8_t *alpha) {
    if (!w || !h || w > 4096 || h > 4096) return 0;
    const size_t raw_n = (size_t)(w + 1) * h;   /* filter byte per row */
    const size_t blocks = (raw_n + 65534) / 65535;
    const size_t z_n = 2 + raw_n + 5 * blocks + 4;
    uint8_t *raw = malloc(raw_n), *z = malloc(z_n);
    if (!raw || !z) { free(raw); free(z); return 0; }
    for (unsigned y = 0; y < h; ++y) {
        raw[(size_t)y * (w + 1)] = 0;
        memcpy(raw + (size_t)y * (w + 1) + 1, pixels + (size_t)y * w, w);
    }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; ++i) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t at = 0; at < raw_n; at += 65535) {
        const size_t len = raw_n - at < 65535 ? raw_n - at : 65535;
        z[o++] = at + len == raw_n ? 1 : 0;
        z[o++] = (uint8_t)len; z[o++] = (uint8_t)(len >> 8);
        z[o++] = (uint8_t)~len; z[o++] = (uint8_t)(~len >> 8);
        memcpy(z + o, raw + at, len);
        o += len;
    }
    put32(z + o, b << 16 | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    int ok = f != NULL;
    if (ok) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        uint8_t ihdr[13];
        put32(ihdr, w); put32(ihdr + 4, h);
        ihdr[8] = 8; ihdr[9] = 3; ihdr[10] = ihdr[11] = ihdr[12] = 0;
        ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
             chunk(f, "PLTE", palette, 768) && (!alpha || chunk(f, "tRNS", alpha, 256)) &&
             chunk(f, "IDAT", z, (uint32_t)o) && chunk(f, "IEND", NULL, 0);
        ok = (fclose(f) == 0) && ok;
    }
    free(raw); free(z);
    return ok;
}

/* ---- Slot rule ----------------------------------------------------------- */

unsigned acr_moa_slot_roles(unsigned arms_pieces, unsigned legs_pieces, acr_moa_role *r,
                            unsigned max) {
    unsigned n = 0;
#define ADD(p, k) do { if (n < max) { r[n].part = (uint8_t)(p); r[n].piece = (uint8_t)(k); } ++n; } while (0)
    ADD(ACR_MOA_HEAD, 0); ADD(ACR_MOA_CORE, 0); ADD(ACR_MOA_LEGS, 0);
    for (unsigned i = 0; i < arms_pieces; ++i) ADD(ACR_MOA_ARMS, i);
    if (arms_pieces == 7) ADD(ACR_MOA_HAND, 0);             /* weapon arms have no hand */
    const unsigned rest = legs_pieces ? legs_pieces - 1 : 0;
    const unsigned front = rest >= 8 ? 4 : rest;             /* four legs: 4 before, 4 after */
    for (unsigned i = 1; i <= front; ++i) ADD(ACR_MOA_LEGS, i);
    ADD(ACR_MOA_BACK_RIGHT, 0); ADD(ACR_MOA_BACK_LEFT, 0); ADD(ACR_MOA_BACK_LEFT, 1);
    ADD(ACR_MOA_BOOSTER, 0); ADD(ACR_MOA_BOOSTER, 1); ADD(ACR_MOA_BACK_RIGHT, 1);
    for (unsigned i = front + 1; i <= rest; ++i) ADD(ACR_MOA_LEGS, i);
#undef ADD
    return n;
}

/* ---- Part categories ------------------------------------------------------ */

typedef struct cat_info {
    const char *name;     /* folder name */
    unsigned role;        /* ACR_MOA_* */
    unsigned entry, count;
    const char *tex;      /* GG/MS archive */
    unsigned tex_table;   /* entries per colour pattern (patterned) */
    int patterned;        /* 1: palette from the AC's colours */
    unsigned clut_row;    /* player 1's palette row in VRAM */
} cat_info;

static const cat_info CATS[] = {
    { "core",       ACR_MOA_CORE,       2,   13, "CORE_T", 11, 1, 486 },
    { "legs",       ACR_MOA_LEGS,       15,  42, "LEG_T",  40, 1, 488 },
    { "head",       ACR_MOA_HEAD,       57,  24, "HEAD_T", 18, 1, 490 },
    { "booster",    ACR_MOA_BOOSTER,    81,  18, "BST_T",  14, 0, 487 },
    { "hand",       ACR_MOA_HAND,       99,  28, "WER_T",  26, 0, 492 },
    { "arms",       ACR_MOA_ARMS,       127, 28, "ARMS_T", 24, 1, 489 },
    { "back-left",  ACR_MOA_BACK_LEFT,  155, 60, "BWL_T",  60, 0, 493 },
    { "back-right", ACR_MOA_BACK_RIGHT, 215, 60, "BWR_T",  60, 0, 494 },
};
#define CAT_COUNT (sizeof CATS / sizeof CATS[0])
#define FX_CLUT_ROW 510u

static const cat_info *cat_by_clut(unsigned row) {
    for (unsigned i = 0; i < CAT_COUNT; ++i) if (CATS[i].clut_row == row) return &CATS[i];
    return NULL;
}

/* ---- Sets ------------------------------------------------------------------ */

typedef struct set_info {
    uint8_t core, arms[32], legs[64];
    unsigned n_arms, n_legs;
    uint8_t *anim; size_t anim_size;       /* PA entry 0 */
    unsigned n_nodes, node[MAX_NODES];
    int parent[MAX_NODES], slot[MAX_NODES];  /* by node id; -1 none */
    int has_rot[MAX_NODES], has_pos[MAX_NODES];
    int rot[MAX_NODES][3], pos[MAX_NODES][3];  /* bind pose: animation 0, frame 0 */
    unsigned n_slots;
    int slot_node[MAX_SLOTS];
    acr_moa_role role[MAX_SLOTS];
    unsigned n_roles;
} set_info;

static void read_list(const uint8_t *menu, size_t size, uint32_t ptr, uint8_t *out, unsigned *n,
                      unsigned max) {
    *n = 0;
    if (ptr < MENU_BASE || ptr - MENU_BASE >= size) return;
    for (size_t o = ptr - MENU_BASE; o < size && menu[o] != 0xFF && *n < max; ++o) out[(*n)++] = menu[o];
}

static uint8_t *read_entry(const char *unpacked, unsigned set, unsigned entry, size_t *size) {
    char path[1200];
    snprintf(path, sizeof path, "%s/GG/P%u/PA%02u/%03u.bin", unpacked, set / 20, set, entry);
    uint8_t *d = read_all(path, size);
    if (d && *size < 32) { free(d); return NULL; }
    return d;
}

/* Animation entry header: size, ?, ?, joint table, list, a, b (u32 each). */
static int parse_joint_table(set_info *s) {
    const uint8_t *d = s->anim;
    if (s->anim_size < 28) return 0;
    const uint32_t jt = u32(d + 12), list = u32(d + 16);
    if (jt >= list || list > s->anim_size) return 0;
    for (unsigned i = 0; i < MAX_NODES; ++i) s->parent[i] = s->slot[i] = -1;
    size_t p = jt + 2;
    s->n_nodes = 0;
    while (p + 2 <= list && d[p] == 8 && s->n_nodes < MAX_NODES) {
        if (d[p + 1] >= MAX_NODES) return 0;
        s->node[s->n_nodes++] = d[p + 1];
        p += 2;
    }
    while (p + 4 <= list && (d[p] == 2 || d[p] == 3)) {
        const unsigned a = d[p + 1], b = u16(d + p + 2);
        if (a >= MAX_NODES) return 0;
        if (d[p] == 2) s->slot[a] = (int)b;
        else s->parent[a] = b == 0xFFFF ? -1 : (int)b;
        p += 4;
    }
    s->n_slots = 0;
    for (unsigned i = 0; i < MAX_SLOTS; ++i) s->slot_node[i] = -1;
    for (unsigned i = 0; i < s->n_nodes; ++i) {
        const int sl = s->slot[s->node[i]];
        if (sl >= 0 && sl < (int)MAX_SLOTS) {
            s->slot_node[sl] = (int)s->node[i];
            if ((unsigned)sl + 1 > s->n_slots) s->n_slots = (unsigned)sl + 1;
        }
    }
    return s->n_nodes > 0;
}

/* Byte range of animation `a` (0 = absent), records end at the next start. */
static int anim_range(const set_info *s, unsigned a, size_t *start, size_t *end) {
    const uint8_t *d = s->anim;
    const uint32_t list = u32(d + 16), stop = u32(d + 20);
    if (list + 57 * 4 > s->anim_size || stop > s->anim_size) return 0;
    const uint32_t off = u32(d + list + 4 * a);
    if (!off && a) return 0;
    uint32_t next = stop;
    for (unsigned k = 0; k < 57; ++k) {
        const uint32_t o = u32(d + list + 4 * k);
        if (o > off && list + o < next) next = list + o;
    }
    *start = list + off;
    *end = next;
    return *start < *end && *end <= s->anim_size;
}

/* Calls `node_cb` for every node record of every frame; returns 0 if the
 * record does not parse to its end. */
typedef void (*node_fn)(void *ctx, unsigned frame, unsigned node, unsigned rot_mode,
                        const int *rot, unsigned pos_mode, const int *pos);

static int walk_anim(const set_info *s, size_t p, size_t end, unsigned *frames, int *holds,
                     node_fn cb, void *ctx) {
    const uint8_t *d = s->anim;
    if (p + 6 > end) return 0;
    const int count = s16(d + p + 2);
    if (count < 1 || count > 512) return 0;
    *frames = (unsigned)count;
    p += 6;
    for (int i = 0; i < count - 1; ++i) {
        if (p + 2 > end) return 0;
        if (holds) holds[i] = s16(d + p);
        p += 2;
    }
    for (int f = 0; f < count; ++f) {
        if (p + 2 > end) return 0;
        const int n = s16(d + p);
        p += 2;
        if (n < 0 || n > (int)MAX_NODES) return 0;
        for (int k = 0; k < n; ++k) {
            if (p + 2 > end) return 0;
            const unsigned desc = u16(d + p), fl = desc & 0xFF, node = desc >> 8;
            const unsigned rm = (fl >> 2) & 3, pm = (fl >> 6) & 3;
            p += 2;
            int rot[3] = { 0, 0, 0 }, pos[3] = { 0, 0, 0 };
            for (int pass = 0; pass < 2; ++pass) {
                const unsigned m = pass ? pm : rm;
                int *v = pass ? pos : rot;
                if (m == 1) {
                    if (p + 6 > end) return 0;
                    for (int c = 0; c < 3; ++c) v[c] = s16(d + p + 2 * c);
                    p += 6;
                } else if (m == 2) {
                    if (p + 4 > end) return 0;
                    for (int c = 0; c < 3; ++c) v[c] = (int8_t)d[p + c];
                    p += 4;
                }
            }
            if (node >= MAX_NODES) return 0;
            if (cb) cb(ctx, (unsigned)f, node, rm, rot, pm, pos);
        }
    }
    return end - p <= 4;
}

static void bind_cb(void *ctx, unsigned frame, unsigned node, unsigned rm, const int *rot,
                    unsigned pm, const int *pos) {
    set_info *s = ctx;
    if (frame) return;
    if (rm == 1 || rm == 2) { s->has_rot[node] = 1; memcpy(s->rot[node], rot, sizeof s->rot[node]); }
    if (pm == 1 || pm == 2) { s->has_pos[node] = 1; memcpy(s->pos[node], pos, sizeof s->pos[node]); }
}

static unsigned mesh_pieces(const uint8_t *m, size_t size) {
    return size >= 24 ? u32(m + 12) : 0;
}

static int load_set(set_info *s, const uint8_t *menu, size_t menu_size, const char *unpacked,
                    unsigned id) {
    memset(s, 0, sizeof *s);
    const uint8_t *e = menu + (SET_TABLE - MENU_BASE) + 12 * id;
    s->core = e[0];
    read_list(menu, menu_size, u32(e + 4), s->arms, &s->n_arms, sizeof s->arms);
    read_list(menu, menu_size, u32(e + 8), s->legs, &s->n_legs, sizeof s->legs);
    s->anim = read_entry(unpacked, id, 0, &s->anim_size);
    if (!s->anim || !parse_joint_table(s)) return 0;
    size_t a, b;
    unsigned frames;
    if (!anim_range(s, 0, &a, &b) || !walk_anim(s, a, b, &frames, NULL, bind_cb, s)) return 0;
    /* Piece counts of this set's arms and legs decide the slot order. */
    unsigned arms = 7, legs = 7;
    size_t n;
    if (s->n_arms) {
        uint8_t *m = read_entry(unpacked, id, 127 + s->arms[0], &n);
        if (m) { arms = mesh_pieces(m, n); free(m); }
    }
    if (s->n_legs) {
        uint8_t *m = read_entry(unpacked, id, 15 + s->legs[0], &n);
        if (m) { legs = mesh_pieces(m, n); free(m); }
    }
    s->n_roles = acr_moa_slot_roles(arms, legs, s->role, MAX_SLOTS);
    return s->n_roles == s->n_slots;
}

static int set_lists(const set_info *s, unsigned role, unsigned i) {
    const uint8_t *list = role == ACR_MOA_ARMS ? s->arms : s->legs;
    const unsigned n = role == ACR_MOA_ARMS ? s->n_arms : s->n_legs;
    for (unsigned k = 0; k < n; ++k) if (list[k] == i) return 1;
    return 0;
}

/* Node that draws piece `piece` of a `role` part in set `s` (-1 if none). */
static int piece_node(const set_info *s, unsigned role, unsigned piece) {
    for (unsigned k = 0; k < s->n_roles; ++k)
        if (s->role[k].part == role && s->role[k].piece == piece)
            return s->slot_node[s->n_slots - 1 - k];
    return -1;
}

/* ---- Textures ------------------------------------------------------------- */

typedef struct tim_info {
    unsigned x, y, w, h;      /* VRAM position, width in halfwords */
    const uint8_t *pix;       /* 8-bit pixels, w*2 x h */
    const uint8_t *clut;      /* 256 x u16 or NULL */
} tim_info;

static int parse_tim(const uint8_t *d, size_t n, tim_info *t) {
    if (n < 20 || u32(d) != 0x10) return 0;
    const uint32_t fl = u32(d + 4);
    if ((fl & 3) != 1) return 0;                 /* 8-bit only */
    size_t q = 8;
    t->clut = NULL;
    if (fl & 8) {
        const uint32_t bl = u32(d + q);
        if (q + bl > n || bl < 12 + 512) return 0;
        t->clut = d + q + 12;
        q += bl;
    }
    if (q + 12 > n) return 0;
    const uint32_t bl = u32(d + q);
    t->x = u16(d + q + 4); t->y = u16(d + q + 6); t->w = u16(d + q + 8); t->h = u16(d + q + 10);
    if (q + bl > n || (size_t)t->w * 2 * t->h + 12 > bl) return 0;
    t->pix = d + q + 12;
    return 1;
}

static uint8_t *read_tex_entry(const char *unpacked, const char *archive, unsigned entry,
                               size_t *n) {
    char path[1200];
    snprintf(path, sizeof path, "%s/GG/MS/%s/%03u.bin", unpacked, archive, entry);
    return read_all(path, n);
}

/* The "DEFAULT" colour preset in the menu module: 4 slots of RGB, 0..63. */
static int default_colours(const uint8_t *menu, size_t size, uint8_t c[4][3]) {
    static const char key[] = "DEFAULT";
    for (size_t i = 0; i + sizeof key + 12 <= size; ++i) {
        if (memcmp(menu + i, key, sizeof key) == 0) {
            const uint8_t *p = menu + i + sizeof key;
            for (int s = 0; s < 4; ++s)
                for (int k = 0; k < 3; ++k) {
                    if (p[3 * s + k] > 63) return 0;
                    c[s][k] = p[3 * s + k];
                }
            return 1;
        }
    }
    return 0;
}

/* Palette for patterned parts: index = slot x 64 + shade. The game ramps
 * each slot colour down to black; this is a close fit to what it builds
 * (colour x (64 - even shade) / 64, rounded, capped at 31 in 5-bit steps). */
static void ramp_palette(uint8_t (*c)[3], uint8_t pal[768]) {
    for (int s = 0; s < 4; ++s)
        for (int k = 0; k < 64; ++k)
            for (int ch = 0; ch < 3; ++ch) {
                int v = (c[s][ch] * (64 - (k & ~1)) + 32) / 64;
                if (v > 31) v = 31;
                pal[3 * (s * 64 + k) + ch] = (uint8_t)(v << 3 | v >> 2);
            }
}

static void clut_palette(const uint8_t *clut, uint8_t pal[768], uint8_t alpha[256]) {
    for (int i = 0; i < 256; ++i) {
        const unsigned v = u16(clut + 2 * i);
        const unsigned r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
        pal[3 * i] = (uint8_t)(r << 3 | r >> 2);
        pal[3 * i + 1] = (uint8_t)(g << 3 | g >> 2);
        pal[3 * i + 2] = (uint8_t)(b << 3 | b >> 2);
        alpha[i] = v == 0 ? 0 : 255;   /* 0x0000 is see-through on the PS1 */
    }
}

/* ---- Mesh export ----------------------------------------------------------- */

typedef struct out_ctx {
    const char *unpacked, *out_dir;
    uint8_t colours[4][3];
    tim_info cat_tim[CAT_COUNT];      /* first texture of each category: VRAM place */
    int cat_tim_ok[CAT_COUNT];
} out_ctx;

typedef struct material {
    char name[56];
    unsigned clut_row;   /* 0 = untextured */
    unsigned rgb;        /* untextured colour */
    int semi;            /* semi-transparent (abr + 1), 0 = opaque */
    int two_sided;
} material;

#define MAX_MATS 64

static int find_material(material *mats, unsigned *n, const material *m) {
    for (unsigned i = 0; i < *n; ++i)
        if (mats[i].clut_row == m->clut_row && mats[i].rgb == m->rgb && mats[i].semi == m->semi &&
            mats[i].two_sided == m->two_sided)
            return (int)i;
    if (*n >= MAX_MATS) return -1;
    mats[*n] = *m;
    return (int)(*n)++;
}

static void material_name(material *m) {
    char base[40];
    if (!m->clut_row) snprintf(base, sizeof base, "rgb-%06X", m->rgb & 0xFFFFFFu);
    else if (m->clut_row == FX_CLUT_ROW) snprintf(base, sizeof base, "fx");
    else {
        const cat_info *c = cat_by_clut(m->clut_row);
        snprintf(base, sizeof base, "%s", c ? c->name : "other");
    }
    snprintf(m->name, sizeof m->name, "%s%s%s", base, m->semi ? "-semi" : "",
             m->two_sided ? "-2side" : "");
}

/* Writes piece<K>.obj files and part.mtl for one part. `offset` per piece is
 * added to the vertices to move them into their joint's space. */
static int write_part_mesh(out_ctx *c, const cat_info *cat, unsigned index, const uint8_t *m,
                           size_t size, const char *dir, int (*offset)[3],
                           unsigned *semi_out) {
    const unsigned pieces = mesh_pieces(m, size);
    material mats[MAX_MATS];
    unsigned n_mats = 0;
    *semi_out = 0;
    for (unsigned pc = 0; pc < pieces; ++pc) {
        const size_t ob = 0x18 + 28 * (size_t)pc;
        if (ob + 28 > size) return 0;
        const uint32_t vt = u32(m + ob), nv = u32(m + ob + 4) & 0xFFFF, nt = u32(m + ob + 8),
                       nn = u32(m + ob + 12) & 0xFFFF, pt = u32(m + ob + 16),
                       np = u32(m + ob + 20) & 0xFFFF;
        if (0x18 + (size_t)vt + 8 * nv > size || 0x18 + (size_t)nt + 8 * nn > size) return 0;
        char path[1400];
        snprintf(path, sizeof path, "%s/piece%u.obj", dir, pc);
        FILE *f = fopen(path, "wb");
        if (!f) return 0;
        fprintf(f, "# Master of Arena %s %02u, piece %u, imported from your disc by acr-import.\n"
                   "# Joint space, x right, y up, z back (game units).\nmtllib part.mtl\no piece%u\n",
                cat->name, index, pc, pc);
        for (uint32_t i = 0; i < nv; ++i) {
            const uint8_t *v = m + 0x18 + vt + 8 * i;
            fprintf(f, "v %d %d %d\n", s16(v) + offset[pc][0], -(s16(v + 2) + offset[pc][1]),
                    -(s16(v + 4) + offset[pc][2]));
        }
        for (uint32_t i = 0; i < nn; ++i) {
            const uint8_t *v = m + 0x18 + nt + 8 * i;
            fprintf(f, "vn %.5f %.5f %.5f\n", s16(v) / 4096.0, -s16(v + 2) / 4096.0, -s16(v + 4) / 4096.0);
        }
        size_t p = 0x18 + pt;
        unsigned vt_count = 0;
        int cur_mat = -1;
        int ok = 1;
        for (uint32_t k = 0; ok && k < np; ++k) {
            if (p + 4 > size) { ok = 0; break; }
            const uint32_t h = u32(m + p);
            const unsigned ilen = (h >> 8) & 0xFF, flag = (h >> 16) & 0xFF, mode = h >> 24;
            if (p + 4 + 4 * (size_t)ilen > size) { ok = 0; break; }
            const uint8_t *w = m + p + 4;
            p += 4 + 4 * (size_t)ilen;
            const int quad = (mode & 8) != 0, tex = (mode & 4) != 0, gour = (mode & 0x10) != 0;
            const unsigned nvx = quad ? 4u : 3u;
            const unsigned hw = ilen * 2;
            const unsigned idx_at = tex ? nvx * 2 : 2;          /* halfwords before the indices */
            if (idx_at + (gour ? 2 * nvx : 1 + nvx) > hw) { ok = 0; break; }
            unsigned vi[4], ni[4];
            for (unsigned j = 0; j < nvx; ++j) {
                if (gour) { ni[j] = u16(w + 2 * (idx_at + 2 * j)); vi[j] = u16(w + 2 * (idx_at + 2 * j + 1)); }
                else { ni[j] = u16(w + 2 * idx_at); vi[j] = u16(w + 2 * (idx_at + 1 + j)); }
            }
            int bad = 0;
            for (unsigned j = 0; j < nvx; ++j) if (vi[j] >= nv || ni[j] >= nn) bad = 1;
            if (bad) continue;
            material mt = { "", 0, 0, 0, (flag & 2) != 0 };
            unsigned tsb = 0, cba = 0;
            if (tex) {
                cba = u16(w + 2); tsb = u16(w + 6);
                mt.clut_row = cba >> 6;
                if (mode & 2) mt.semi = (int)((tsb >> 5) & 3) + 1;
            } else {
                mt.rgb = (unsigned)w[0] << 16 | (unsigned)w[1] << 8 | w[2];
                if (mode & 2) mt.semi = 1;
            }
            if (mt.semi) ++*semi_out;
            material_name(&mt);
            const int mi = find_material(mats, &n_mats, &mt);
            if (mi < 0) { ok = 0; break; }
            if (mi != cur_mat) { fprintf(f, "usemtl %s\n", mats[mi].name); cur_mat = mi; }
            unsigned t0 = 0;
            if (tex) {
                /* UV -> pixel in the texture of the palette's category. */
                const unsigned page_x = (tsb & 15) * 64, page_y = ((tsb >> 4) & 1) * 256;
                const cat_info *tc = cat_by_clut(mt.clut_row);
                const tim_info *ti = NULL;
                if (tc) {
                    const unsigned ci = (unsigned)(tc - CATS);
                    if (c->cat_tim_ok[ci]) ti = &c->cat_tim[ci];
                }
                for (unsigned j = 0; j < nvx; ++j) {
                    const unsigned uv = u16(w + 4 * j);
                    double px = uv & 0xFF, py = uv >> 8, W = 256, H = 256;
                    if (ti) {
                        px -= 2.0 * ((double)ti->x - page_x);
                        py -= (double)ti->y - page_y;
                        W = ti->w * 2.0; H = ti->h;
                    }
                    fprintf(f, "vt %.5f %.5f\n", (px + 0.5) / W, 1.0 - (py + 0.5) / H);
                }
                t0 = vt_count + 1;
                vt_count += nvx;
            }
            const unsigned order[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
            for (unsigned tri = 0; tri < (quad ? 2u : 1u); ++tri) {
                fputc('f', f);
                for (unsigned j = 0; j < 3; ++j) {
                    const unsigned q = order[tri][j];
                    if (tex) fprintf(f, " %u/%u/%u", vi[q] + 1, t0 + q, ni[q] + 1);
                    else fprintf(f, " %u//%u", vi[q] + 1, ni[q] + 1);
                }
                fputc('\n', f);
            }
        }
        ok = (fclose(f) == 0) && ok;
        if (!ok) return 0;
    }
    /* Materials. Textured ones point at the texture of the part that owns the
     * palette: this part, or (hands, some back units) the equipped arms /
     * left back unit, shown here with the same index or 00. */
    char path[1400];
    snprintf(path, sizeof path, "%s/part.mtl", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "# Materials for %s %02u. Textured materials are named after the part type\n"
               "# whose texture and colours they use; \"fx\" is a shared effects texture\n"
               "# that is not exported yet. -semi = semi-transparent, -2side = no culling.\n",
            cat->name, index);
    for (unsigned i = 0; i < n_mats; ++i) {
        const material *mt = &mats[i];
        fprintf(f, "\nnewmtl %s\n", mt->name);
        if (!mt->clut_row) {
            fprintf(f, "Kd %.4f %.4f %.4f\n", ((mt->rgb >> 16) & 255) / 255.0,
                    ((mt->rgb >> 8) & 255) / 255.0, (mt->rgb & 255) / 255.0);
        } else {
            fprintf(f, "Kd 1 1 1\n");
            const cat_info *tc = cat_by_clut(mt->clut_row);
            if (tc == cat) fprintf(f, "map_Kd texture.png\n");
            else if (tc) {
                char other[1200];
                snprintf(other, sizeof other, "%s/%s/%02u/texture.png", c->out_dir, tc->name, index);
                FILE *probe = fopen(other, "rb");
                if (probe) fclose(probe);
                fprintf(f, "map_Kd ../../%s/%02u/texture.png\n", tc->name, probe ? index : 0u);
            }
        }
        if (mt->semi) fprintf(f, "d 0.5\n");
    }
    return fclose(f) == 0;
}

static int write_texture(out_ctx *c, const cat_info *cat, unsigned index, const char *dir) {
    size_t n = 0;
    uint8_t *d = read_tex_entry(c->unpacked, cat->tex, index, &n);
    if (!d) return 0;
    tim_info t;
    int ok = parse_tim(d, n, &t);
    if (ok) {
        uint8_t pal[768], alpha[256];
        if (cat->patterned || !t.clut) ramp_palette(c->colours, pal);
        else clut_palette(t.clut, pal, alpha);
        char path[1400];
        snprintf(path, sizeof path, "%s/texture.png", dir);
        ok = acr_write_png8(path, t.w * 2, t.h, t.pix, pal,
                            (cat->patterned || !t.clut) ? NULL : alpha);
    }
    free(d);
    return ok;
}

static int export_part(out_ctx *c, const cat_info *cat, unsigned index, const set_info *sets,
                       uint32_t *count) {
    /* The set the game would load it from. */
    int src = -1;
    for (unsigned s = 0; s < SET_COUNT && src < 0; ++s) {
        if (!sets[s].anim) continue;
        if ((cat->role == ACR_MOA_ARMS || cat->role == ACR_MOA_LEGS) && !set_lists(&sets[s], cat->role, index))
            continue;
        size_t n;
        uint8_t *m = read_entry(c->unpacked, s, cat->entry + index, &n);
        if (m) { free(m); src = (int)s; }
    }
    if (src < 0) return 1;   /* no model: not an error */
    size_t size;
    uint8_t *m = read_entry(c->unpacked, (unsigned)src, cat->entry + index, &size);
    if (!m) return 1;
    const set_info *s = &sets[src];
    const unsigned pieces = mesh_pieces(m, size);
    int ok = pieces > 0 && pieces <= 16 && 0x18 + 28 * (size_t)pieces * 3 <= size;
    int offset[16][3];
    int nodes[16];
    memset(offset, 0, sizeof offset);
    for (unsigned p = 0; ok && p < pieces; ++p) {
        nodes[p] = piece_node(s, cat->role, p);
        if (nodes[p] < 0 || !s->has_pos[nodes[p]]) { ok = 0; break; }
        memcpy(offset[p], s->pos[nodes[p]], sizeof offset[p]);   /* pivot = minus the joint */
    }
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/%s/%02u", c->out_dir, cat->name, index);
    unsigned semi = 0;
    if (ok) ok = make_dirs(dir) && write_part_mesh(c, cat, index, m, size, dir,
                                                   offset, &semi);
    const int tex_ok = ok && write_texture(c, cat, index, dir);
    if (ok) {
        char path[1400];
        snprintf(path, sizeof path, "%s/part.toml", dir);
        FILE *f = fopen(path, "wb");
        ok = f != NULL;
        if (ok) {
            fprintf(f, "# Master of Arena %s %02u model, imported from your disc by acr-import.\n"
                       "# Each piece is drawn with its node's matrix from the set's skeleton\n"
                       "# (sets/set-NN.toml); vertices are in that node's joint space.\n"
                       "category   = \"%s\"\ngame_index = %u\nsource_set = %d\npieces     = %u\n"
                       "texture    = %s\nsemi_transparent_faces = %u\n",
                    cat->name, index, cat->name, index, src, pieces,
                    tex_ok ? "\"texture.png\"" : "\"\"", semi);
            if (cat->patterned)
                fprintf(f, "# texture.png holds colour slot x 64 + shade; its palette is the\n"
                           "# DEFAULT colour scheme (close to, not exactly, the game's ramp).\n"
                           "colour_pattern = 0\n");
            for (unsigned p = 0; p < pieces; ++p)
                fprintf(f, "\n[[piece]]\nindex = %u\nfile  = \"piece%u.obj\"\nnode  = %d\n"
                           "joint = [%d, %d, %d]   # bind position, game space\n",
                        p, p, nodes[p], -offset[p][0], -offset[p][1], -offset[p][2]);
            ok = fclose(f) == 0;
        }
    }
    free(m);
    if (ok) ++*count;
    return ok;
}


/* ---- Garage models (MENU_TMD) ------------------------------------------------ */

/* The garage's Assembly screen shows each part on its own from GG/MS/MENU_TMD:
 * one TMD per owned-part number (same ranges as the save's part counts, in a
 * different order). They are whole models in the part's own space, so FCS,
 * generators, option parts and blades, which have no AC model, get one too.
 * Textures come from the part type's own archive (entry = part number), or
 * for a few shared ones from MENU_TIM. */
typedef struct menu_cat {
    const char *name;      /* folder under models/menu */
    unsigned base, count;  /* MENU_TMD entries */
    const char *tex;       /* archive with one texture per part */
    const char *tex2;      /* second archive (right-hand back units) or NULL */
    int patterned;         /* AC colours (core, legs, arms, head) */
    unsigned clut_row;     /* patterned: the row the game builds the AC's palette in */
} menu_cat;

static const menu_cat MENU_CATS[] = {
    { "core",      3,   11, "CORE_T", NULL,    1, 486 },
    { "legs",      14,  40, "LEG_T",  NULL,    1, 488 },
    { "arms",      54,  24, "ARMS_T", NULL,    1, 489 },
    { "head",      78,  18, "HEAD_T", NULL,    1, 490 },
    { "booster",   96,  14, "BST_T",  NULL,    0, 0 },
    { "back",      110, 60, "BWL_T",  "BWR_T", 0, 0 },
    { "blade",     170, 6,  "WEL_T",  NULL,    0, 0 },
    { "hand",      176, 26, "WER_T",  NULL,    0, 0 },
    { "fcs",       202, 16, "COMP_T", NULL,    0, 0 },
    { "option",    218, 19, "SPEC_T", NULL,    0, 0 },
    { "generator", 237, 15, "GENE_T", NULL,    0, 0 },
};
#define MENU_CAT_COUNT (sizeof MENU_CATS / sizeof MENU_CATS[0])
#define MENU_TEX_MAX 8

typedef struct menu_tex {
    uint8_t *data;         /* owns the TIM */
    tim_info t;
    int patterned;
    char file[32];
    int used;
    unsigned row;          /* CLUT row it answers to */
} menu_tex;

/* The TIM for CLUT row `row`, among the part's own textures and MENU_TIM's
 * shared ones (entries 0 and 1). */
static menu_tex *menu_find_tex(menu_tex *tx, unsigned n, unsigned row) {
    for (unsigned i = 0; i < n; ++i) {
        if (tx[i].data && tx[i].row == row) return &tx[i];
    }
    return NULL;
}

static unsigned menu_load_texs(const char *unpacked, const menu_cat *mc, unsigned index, menu_tex *tx) {
    unsigned n = 0;
    const char *arcs[4] = { mc->tex, mc->tex2, NULL, NULL };
    const unsigned ents[4] = { index, index, 0, 0 };
    /* the part's own textures, then any other row its archive has (blades
       share WEL_T 4), then MENU_TIM's shared ones */
    for (unsigned k = 0; k < 4 + 64 && n < MENU_TEX_MAX; ++k) {
        const char *arc = k < 4 ? arcs[k] : mc->tex;
        const unsigned ent = k < 4 ? ents[k] : k - 4;
        if (!arc || (k >= 4 && ent == index)) continue;
        size_t sz = 0;
        uint8_t *d = read_tex_entry(unpacked, arc, ent, &sz);
        if (!d) continue;
        menu_tex *m = &tx[n];
        memset(m, 0, sizeof *m);
        if (!parse_tim(d, sz, &m->t)) { free(d); continue; }
        m->data = d;
        m->patterned = mc->patterned && k == 0;
        if (m->patterned) m->row = mc->clut_row;
        else if (m->t.clut) m->row = u16(m->t.clut - 12 + 6);   /* CLUT block header: y */
        else { free(d); continue; }
        int dup = 0;
        for (unsigned i = 0; i < n; ++i) if (tx[i].row == m->row) dup = 1;
        if (dup) { free(d); continue; }
        snprintf(m->file, sizeof m->file, k == 0 ? "texture.png" : "texture-%u.png", n);
        ++n;
    }
    for (unsigned e = 0; e < 2 && n < MENU_TEX_MAX; ++e) {
        size_t sz = 0;
        uint8_t *d = read_tex_entry(unpacked, "MENU_TIM", e, &sz);
        if (!d) continue;
        menu_tex *m = &tx[n];
        memset(m, 0, sizeof *m);
        if (!parse_tim(d, sz, &m->t) || !m->t.clut) { free(d); continue; }
        m->data = d;
        m->row = u16(m->t.clut - 12 + 6);
        int dup = 0;
        for (unsigned i = 0; i < n; ++i) if (tx[i].row == m->row) dup = 1;
        if (dup) { free(d); continue; }
        snprintf(m->file, sizeof m->file, "texture-%u.png", n);
        ++n;
    }
    return n;
}

/* One MENU_TMD model as model.obj + part.mtl + textures + part.toml. TMD
 * here: header 0x41, flags, object count; objects of 7 words (vertex top,
 * count, normal top, count, packet top, packet group count, scale), tops
 * relative to the object table. Packets come in groups: the first word is
 * olen, ilen, u16 count, then the first packet's words; every further packet
 * has its own olen, ilen, flag, mode word. Vertex indices are x4. */
static int export_menu_part(const out_ctx *c, const menu_cat *mc, unsigned index, uint32_t *count) {
    char path[1400];
    snprintf(path, sizeof path, "%s/GG/MS/MENU_TMD/%03u.bin", c->unpacked, mc->base + index);
    size_t size = 0;
    uint8_t *m = read_all(path, &size);
    if (!m) return 1;
    if (size < 40 || u32(m) != 0x41 || u32(m + 8) == 0 || u32(m + 8) > 16) { free(m); return 1; }
    const unsigned nobj = u32(m + 8);
    menu_tex tx[MENU_TEX_MAX];
    const unsigned ntx = menu_load_texs(c->unpacked, mc, index, tx);
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/menu/%s/%02u", c->out_dir, mc->name, index);
    int ok = make_dirs(dir);
    FILE *f = NULL;
    unsigned tris = 0;
    char used_mats[64][48];
    unsigned n_used = 0;
    for (unsigned o = 0; ok && o < nobj; ++o) {
        const size_t ob = 12 + 28 * (size_t)o;
        if (ob + 28 > size) { ok = 0; break; }
        unsigned vbase = 0, nbase = 0, tbase = 0;
        snprintf(path, sizeof path, "%s/object%u.obj", dir, o);
        f = fopen(path, "wb");
        if (!f) { ok = 0; break; }
        fprintf(f, "# Master of Arena garage model, %s %02u object %u, imported from your disc by\n"
                   "# acr-import. Part space, x right, y up, z back (game units).\nmtllib part.mtl\n",
                mc->name, index, o);
        const uint32_t vt = u32(m + ob), nv = u32(m + ob + 4), nt = u32(m + ob + 8),
                       nn = u32(m + ob + 12), pt = u32(m + ob + 16), ng = u32(m + ob + 20);
        if (nv > 4096 || nn > 4096 || 12 + (size_t)vt + 8 * nv > size || 12 + (size_t)nt + 8 * nn > size) {
            fclose(f); f = NULL; ok = 0; break;
        }
        for (uint32_t i = 0; i < nv; ++i) {
            const uint8_t *v = m + 12 + vt + 8 * i;
            fprintf(f, "v %d %d %d\n", s16(v), -s16(v + 2), -s16(v + 4));
        }
        for (uint32_t i = 0; i < nn; ++i) {
            const uint8_t *v = m + 12 + nt + 8 * i;
            fprintf(f, "vn %.5f %.5f %.5f\n", s16(v) / 4096.0, -s16(v + 2) / 4096.0, -s16(v + 4) / 4096.0);
        }
        size_t p = 12 + (size_t)pt;
        for (uint32_t g = 0; ok && g < ng; ++g) {
            if (p + 4 > size) { ok = 0; break; }
            const uint32_t h = u32(m + p);
            const unsigned ilen = (h >> 8) & 0xFF, cnt = h >> 16;
            p += 4;
            if (!ilen || cnt > 4096) { ok = 0; break; }
            unsigned mode = 0, flag = 0;
            if (cnt > 1 && p + 4 * (size_t)ilen + 4 <= size) {
                flag = m[p + 4 * ilen + 2];
                mode = m[p + 4 * ilen + 3];
            }
            for (unsigned k = 0; ok && k < cnt; ++k) {
                if (k) {
                    if (p + 4 > size) { ok = 0; break; }
                    flag = m[p + 2]; mode = m[p + 3];
                    p += 4;
                }
                if (p + 4 * (size_t)ilen > size) { ok = 0; break; }
                const uint8_t *w = m + p;
                p += 4 * (size_t)ilen;
                if (cnt == 1) mode = (w[3] >> 5) == 1 ? w[3] : (ilen >= 6 ? 0x24 : 0x20);
                if ((mode >> 5) != 1) continue;               /* not a polygon */
                const int quad = (mode & 8) != 0, tex = (mode & 4) != 0, gour = (mode & 0x10) != 0;
                const unsigned nvx = quad ? 4u : 3u, hw = ilen * 2;
                const unsigned idx_at = tex ? nvx * 2 : 2;
                if (idx_at + (gour ? 2 * nvx : 1 + nvx) > hw) continue;
                unsigned vi[4], ni[4];
                int bad = 0;
                for (unsigned j = 0; j < nvx; ++j) {
                    if (gour) { ni[j] = u16(w + 2 * (idx_at + 2 * j)); vi[j] = u16(w + 2 * (idx_at + 2 * j + 1)); }
                    else { ni[j] = u16(w + 2 * idx_at); vi[j] = u16(w + 2 * (idx_at + 1 + j)); }
                    vi[j] /= 4;
                    if (vi[j] >= nv || ni[j] >= nn) bad = 1;
                }
                if (bad) continue;
                char mat[48];
                menu_tex *mt = NULL;
                unsigned tsb = 0;
                if (tex) {
                    const unsigned row = u16(w + 2) >> 6;
                    tsb = u16(w + 6);
                    mt = menu_find_tex(tx, ntx, row);
                }
                const int semi = (mode & 2) != 0, two = (flag & 2) != 0;
                if (mt) {
                    snprintf(mat, sizeof mat, "%s%s%s", mt->file, semi ? "-semi" : "", two ? "-2side" : "");
                    mt->used = 1;
                } else {
                    unsigned rgb = tex ? 0x808080u : ((unsigned)w[0] << 16 | (unsigned)w[1] << 8 | w[2]);
                    snprintf(mat, sizeof mat, "rgb-%06X%s%s", rgb, semi ? "-semi" : "", two ? "-2side" : "");
                }
                unsigned seen = 0;
                for (; seen < n_used; ++seen) if (strcmp(used_mats[seen], mat) == 0) break;
                if (seen == n_used && n_used < 64) snprintf(used_mats[n_used++], sizeof used_mats[0], "%s", mat);
                fprintf(f, "usemtl %s\n", mat);
                unsigned t0 = 0;
                if (mt) {
                    const unsigned page_x = (tsb & 15) * 64, page_y = ((tsb >> 4) & 1) * 256;
                    for (unsigned j = 0; j < nvx; ++j) {
                        const unsigned uv = u16(w + 4 * j);
                        const double px = (uv & 0xFF) - 2.0 * ((double)mt->t.x - page_x);
                        const double py = (uv >> 8) - ((double)mt->t.y - page_y);
                        const double W = mt->t.w * 2.0, H = mt->t.h;
                        fprintf(f, "vt %.5f %.5f\n", (px + 0.5) / W, 1.0 - (py + 0.5) / H);
                    }
                    t0 = tbase + 1;
                    tbase += nvx;
                }
                static const unsigned order[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
                for (unsigned tri = 0; tri < (quad ? 2u : 1u); ++tri) {
                    fputc('f', f);
                    for (unsigned j = 0; j < 3; ++j) {
                        const unsigned q = order[tri][j];
                        if (mt) fprintf(f, " %u/%u/%u", vbase + vi[q] + 1, t0 + q, nbase + ni[q] + 1);
                        else fprintf(f, " %u//%u", vbase + vi[q] + 1, nbase + ni[q] + 1);
                    }
                    fputc('\n', f);
                    ++tris;
                }
            }
        }
        ok = (fclose(f) == 0) && ok;
        f = NULL;
    }
    /* materials and textures */
    if (ok) {
        snprintf(path, sizeof path, "%s/part.mtl", dir);
        f = fopen(path, "wb");
        ok = f != NULL;
        if (ok) {
            fprintf(f, "# Materials for the garage model %s %02u.\n", mc->name, index);
            for (unsigned i = 0; i < n_used; ++i) {
                fprintf(f, "\nnewmtl %s\n", used_mats[i]);
                if (strncmp(used_mats[i], "rgb-", 4) == 0) {
                    const unsigned rgb = (unsigned)strtoul(used_mats[i] + 4, NULL, 16);
                    fprintf(f, "Kd %.4f %.4f %.4f\n", ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0,
                            (rgb & 255) / 255.0);
                } else {
                    char file[48];
                    memcpy(file, used_mats[i], sizeof file);
                    file[sizeof file - 1] = 0;
                    char *dash = strstr(file, ".png");
                    if (dash) dash[4] = 0;
                    fprintf(f, "Kd 1 1 1\nmap_Kd %s\n", file);
                }
                if (strstr(used_mats[i], "-semi")) fprintf(f, "d 0.5\n");
            }
            ok = fclose(f) == 0;
        }
    }
    for (unsigned i = 0; ok && i < ntx; ++i) {
        if (!tx[i].used) continue;
        uint8_t pal[768], alpha[256];
        if (tx[i].patterned || !tx[i].t.clut) ramp_palette((uint8_t (*)[3])c->colours, pal);
        else clut_palette(tx[i].t.clut, pal, alpha);
        snprintf(path, sizeof path, "%s/%s", dir, tx[i].file);
        ok = acr_write_png8(path, tx[i].t.w * 2, tx[i].t.h, tx[i].t.pix, pal,
                            (tx[i].patterned || !tx[i].t.clut) ? NULL : alpha);
    }
    if (ok) {
        snprintf(path, sizeof path, "%s/part.toml", dir);
        f = fopen(path, "wb");
        ok = f != NULL;
        if (ok) {
            fprintf(f, "# Master of Arena garage model (the part on its own, as the Assembly\n"
                       "# screen shows it), imported from your disc by acr-import.\n"
                       "category   = \"%s\"\ngame_index = %u\nmenu_entry = %u\nobjects    = %u\n"
                       "triangles  = %u\ncolour_pattern = %d   # 1: texture.png is slot x 64 + shade\n",
                    mc->name, index, mc->base + index, nobj, tris, mc->patterned);
            ok = fclose(f) == 0;
        }
    }
    for (unsigned i = 0; i < ntx; ++i) free(tx[i].data);
    free(m);
    if (ok && tris) ++*count;
    return ok;
}

/* ---- Set files -------------------------------------------------------------- */

static const char *ROLE_NAME[] = { "head", "core", "legs", "arms", "hand", "back-left",
                                   "back-right", "booster" };

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

typedef struct text_ctx { FILE *f; int cur; } text_ctx;

static void text_cb(void *ctx, unsigned frame, unsigned node, unsigned rm, const int *rot,
                    unsigned pm, const int *pos) {
    text_ctx *t = ctx;
    if ((int)frame != t->cur) { fprintf(t->f, "frame %u\n", frame); t->cur = (int)frame; }
    fprintf(t->f, "  node %u", node);
    if (rm == 3) fprintf(t->f, " rot zero");
    else if (rm) fprintf(t->f, " rot %d %d %d", rot[0], rot[1], rot[2]);
    if (pm == 3) fprintf(t->f, " pos zero");
    else if (pm) fprintf(t->f, " pos %d %d %d", pos[0], pos[1], pos[2]);
    fputc('\n', t->f);
}

static int write_set(const char *out_dir, unsigned id, const set_info *s) {
    char path[1200];
    snprintf(path, sizeof path, "%s/sets/set-%02u.toml", out_dir, id);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "# Master of Arena animation set %u skeleton, imported from your disc.\n"
               "# Game space (x right, y down, z forward), 4096 = a full turn.\n"
               "# A node's matrix = parent's matrix x T(position) x Rx x Ry x Rz.\n"
               "# Pivots carry a slot and draw one piece; joints rotate.\n"
               "# Bind pose = animation 0, frame 0 (other animations set their own values).\n"
               "set = %u\ncore = %u\n", id, id, s->core);
    fprintf(f, "arms = [");
    for (unsigned i = 0; i < s->n_arms; ++i) fprintf(f, "%s%u", i ? ", " : "", s->arms[i]);
    fprintf(f, "]\nlegs = [");
    for (unsigned i = 0; i < s->n_legs; ++i) fprintf(f, "%s%u", i ? ", " : "", s->legs[i]);
    fprintf(f, "]\nanimations = \"set-%02u-anims.txt\"\n", id);
    for (unsigned i = 0; i < s->n_nodes; ++i) {
        const unsigned n = s->node[i];
        fprintf(f, "\n[[node]]\nid       = %u\nparent   = %d\n", n, s->parent[n]);
        if (s->slot[n] >= 0) {
            const unsigned k = s->n_slots - 1 - (unsigned)s->slot[n];
            fprintf(f, "slot     = %d\ndraws    = \"%s %u\"\n", s->slot[n],
                    ROLE_NAME[s->role[k].part], s->role[k].piece);
        }
        if (s->has_pos[n]) fprintf(f, "position = [%d, %d, %d]\n", s->pos[n][0], s->pos[n][1], s->pos[n][2]);
        if (s->has_rot[n]) fprintf(f, "rotation = [%d, %d, %d]\n", s->rot[n][0], s->rot[n][1], s->rot[n][2]);
    }
    if (fclose(f) != 0) return 0;

    snprintf(path, sizeof path, "%s/sets/set-%02u-anims.txt", out_dir, id);
    f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "# Master of Arena animation set %u, joint animations, imported from your disc.\n"
               "# Each frame lists only the nodes that change; values are absolute.\n"
               "# rot / pos: new value, \"zero\": back to 0, missing: keep the last one.\n"
               "# hold = ticks each frame is shown before the next (one fewer than frames).\n", id);
    int ok = 1;
    for (unsigned a = 0; ok && a < 57; ++a) {
        size_t start, end;
        if (!anim_range(s, a, &start, &end)) continue;
        unsigned frames = 0;
        int holds[512];
        if (!walk_anim(s, start, end, &frames, holds, NULL, NULL)) { ok = 0; break; }
        fprintf(f, "\nanim %u%s%s\nlast_tick %d\nframes %u\nhold", a, anim_name(a) ? " " : "",
                anim_name(a) ? anim_name(a) : "", s16(s->anim + start), frames);
        for (unsigned i = 0; i + 1 < frames; ++i) fprintf(f, " %d", holds[i]);
        fputc('\n', f);
        text_ctx t = { f, -1 };
        ok = walk_anim(s, start, end, &frames, NULL, text_cb, &t);
    }
    return (fclose(f) == 0) && ok;
}

int acr_export_moa_models(const char *menu_module, const char *unpacked_dir, const char *out_dir,
                          uint32_t *parts, uint32_t *sets_out, char *err, size_t err_size) {
    *parts = *sets_out = 0;
    size_t menu_size = 0;
    uint8_t *menu = read_all(menu_module, &menu_size);
    if (!menu) { snprintf(err, err_size, "cannot read %s", menu_module); return 0; }
    if ((SET_TABLE - MENU_BASE) + 12 * SET_COUNT > menu_size) {
        free(menu);
        snprintf(err, err_size, "the animation set table is not where Master of Arena (USA) keeps it");
        return 0;
    }
    out_ctx c;
    memset(&c, 0, sizeof c);
    c.unpacked = unpacked_dir;
    c.out_dir = out_dir;
    if (!default_colours(menu, menu_size, c.colours)) {
        /* Not fatal: grey ramps keep the shading readable. */
        for (int s = 0; s < 4; ++s) c.colours[s][0] = c.colours[s][1] = c.colours[s][2] = (uint8_t)(32 - 6 * s);
    }
    {   /* the starting colour scheme, for new saves */
        char cpath[1200];
        snprintf(cpath, sizeof cpath, "%s/colours.toml", out_dir);
        FILE *f = fopen(cpath, "w");
        if (f) {
            fprintf(f, "# Master of Arena colour scheme, imported from your disc by acr-import.\n"
                       "# 4 colour slots as red, green, blue 0..63.\n"
                       "default = [");
            for (int k = 0; k < 12; ++k) fprintf(f, k ? ", %u" : "%u", c.colours[k / 3][k % 3]);
            fprintf(f, "]\n");
            fclose(f);
        }
    }
    set_info *sets = calloc(SET_COUNT, sizeof *sets);
    if (!sets) { free(menu); snprintf(err, err_size, "out of memory"); return 0; }
    char path[1200];
    snprintf(path, sizeof path, "%s/sets", out_dir);
    int ok = make_dirs(path);
    for (unsigned i = 0; ok && i < SET_COUNT; ++i) {
        if (!load_set(&sets[i], menu, menu_size, unpacked_dir, i)) {
            free(sets[i].anim);
            sets[i].anim = NULL;     /* skipped: missing or did not check out */
            continue;
        }
        if (write_set(out_dir, i, &sets[i])) ++*sets_out;
    }
    for (unsigned k = 0; k < CAT_COUNT; ++k) {
        size_t n = 0;
        uint8_t *d = read_tex_entry(unpacked_dir, CATS[k].tex, 0, &n);
        if (d) {
            tim_info t;
            if (parse_tim(d, n, &t)) {
                t.pix = NULL; t.clut = NULL;   /* only the VRAM place is kept */
                c.cat_tim[k] = t;
                c.cat_tim_ok[k] = 1;
            }
            free(d);
        }
    }
    for (unsigned k = 0; ok && k < CAT_COUNT; ++k)
        for (unsigned i = 0; ok && i < CATS[k].count; ++i)
            ok = export_part(&c, &CATS[k], i, sets, parts);
    for (unsigned k = 0; ok && k < MENU_CAT_COUNT; ++k)
        for (unsigned i = 0; ok && i < MENU_CATS[k].count; ++i)
            ok = export_menu_part(&c, &MENU_CATS[k], i, parts);
    for (unsigned i = 0; i < SET_COUNT; ++i) free(sets[i].anim);
    free(sets);
    free(menu);
    if (!ok) { snprintf(err, err_size, "a model did not check out while exporting"); return 0; }
    return 1;
}
