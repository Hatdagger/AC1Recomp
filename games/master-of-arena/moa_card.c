/* Master of Arena: AI Battle card reader. See moa_card.h. */
#include "moa_card.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "moa_card_font.h"

/* stb_image is compiled into the runtime (psx_window_icon.cpp: PNG + JPEG). */
#define STBI_NO_STDIO
#include "../../psxrecomp/runtime/third_party/stb_image.h"

/* ---- card geometry (builder: N, SW, CELL, Y_SW, Y_DATA, ROWS, EX, EY) ---- */
#define CARD_N      640
#define CARD_SW     8
#define CARD_CELL   8
#define CARD_Y_SW   8
#define CARD_Y_DATA (CARD_Y_SW + 16 * CARD_SW + 8)
#define CARD_ROWS   ((CARD_N - CARD_Y_DATA) / CARD_CELL)
#define CARD_MAXCOLS 6   /* format 3 cards use 2..4; AI overrides can need 6 */
#define CARD_EX     72
#define CARD_EY     64
#define CARD_PMAX   64

struct card_image {
    const uint8_t *rgb;
    int w, h;
};

/* Mean colour of a rectangle given in card pixels (the card scaled to 640). */
static void card_avg(const struct card_image *im, int rx, int ry, int rw, int rh,
                     double out[3])
{
    int x0 = (int)((long)rx * im->w / CARD_N), x1 = (int)((long)(rx + rw) * im->w / CARD_N);
    int y0 = (int)((long)ry * im->h / CARD_N), y1 = (int)((long)(ry + rh) * im->h / CARD_N);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    if (x1 > im->w) x1 = im->w;
    if (y1 > im->h) y1 = im->h;
    double r = 0, g = 0, b = 0;
    long n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const uint8_t *p = im->rgb + ((size_t)y * im->w + x) * 3;
            r += p[0]; g += p[1]; b += p[2]; n++;
        }
    if (!n) n = 1;
    out[0] = r / n; out[1] = g / n; out[2] = b / n;
}

static int card_cap(int cols) { return CARD_ROWS * cols * 3 / 8; }

static int js_round(double v) { return (int)floor(v + 0.5); }

static int q5(double v)
{
    int q = js_round(v * 31.0 / 255.0);
    return q < 0 ? 0 : q > 31 ? 31 : q;
}

static uint16_t c15(double r, double g, double b)
{
    return (uint16_t)(q5(r) | (q5(g) << 5) | (q5(b) << 10));
}

static void c15_rgb(uint16_t c, int out[3])
{
    out[0] = (c & 31) * 255 / 31;
    out[1] = ((c >> 5) & 31) * 255 / 31;
    out[2] = ((c >> 10) & 31) * 255 / 31;
}

static uint16_t crc16_ccitt(const uint8_t *b, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)(b[i] << 8);
        for (int k = 0; k < 8; k++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

struct bit_reader { const uint8_t *b; int bytes, pos; int bad; };

static unsigned get_bits(struct bit_reader *r, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        const int byte = r->pos >> 3;
        if (byte >= r->bytes) { r->bad = 1; return 0; }
        v = (v << 1) | ((r->b[byte] >> (7 - (r->pos & 7))) & 1u);
        r->pos++;
    }
    return v;
}

static int card_unpack(const uint8_t *p, int count, struct moa_card *u, int *has_paint)
{
    const int n = p[0];
    if (!n || n < 18 || n + 1 > count) return 0;
    const uint8_t *body = p + 1;
    const int body_bytes = n - 2;
    if (crc16_ccitt(body, body_bytes) != (uint16_t)((p[n - 1] << 8) | p[n])) return 0;
    struct bit_reader r = { body, body_bytes, 0, 0 };
    const unsigned fmt = get_bits(&r, 4);
    if (fmt < 2 || fmt > 4) return 0;
    u->format = (uint8_t)fmt;
    for (int i = 0; i < 13; i++) u->rec[i] = (uint8_t)get_bits(&r, 8);
    *has_paint = 0;
    if (fmt == 2) {
        (void)get_bits(&r, 2);                 /* old skill band */
    } else {
        u->flag = (uint8_t)get_bits(&r, 1);
        u->pattern = (uint8_t)get_bits(&r, 4);
        u->variant = (uint8_t)get_bits(&r, 2);
        u->plus = (uint8_t)get_bits(&r, 4);
        const unsigned uni = get_bits(&r, 1);
        if (uni) {
            uint8_t c[12];
            for (int i = 0; i < 12; i++) c[i] = (uint8_t)get_bits(&r, 7);
            for (int i = 0; i < 48; i++) u->paint[i] = c[i % 12];
        } else {
            for (int i = 0; i < 48; i++) u->paint[i] = (uint8_t)get_bits(&r, 7);
        }
        *has_paint = 1;
    }
    const unsigned l0 = get_bits(&r, 5), l1 = get_bits(&r, 5);
    if (l0 > 16 || l1 > 16) return 0;
    for (unsigned i = 0; i < l0; i++) u->pilot[i] = (char)get_bits(&r, 7);
    u->pilot[l0] = 0;
    for (unsigned i = 0; i < l1; i++) u->ac[i] = (char)get_bits(&r, 7);
    u->ac[l1] = 0;
    if (fmt == 4 && get_bits(&r, 1)) {
        u->has_ai = 1;
        u->ai_preset = (uint8_t)get_bits(&r, 3);
        if (u->ai_preset >= MOA_AI_PRESETS) u->ai_preset = MOA_AI_PRESET_CUSTOM;
        for (int i = 0; i < MOA_AI_SLIDERS; i++) {
            const unsigned v = get_bits(&r, 7);
            u->ai_slider[i] = (uint8_t)(v > 100 ? 100 : v);
        }
        if (get_bits(&r, 1)) {
            for (int i = 0; i < MOA_AI_RECORD_SIZE; i++) u->ai_set[i] = (uint8_t)get_bits(&r, 1);
            for (int i = 0; i < MOA_AI_RECORD_SIZE; i++)
                if (u->ai_set[i]) u->ai_raw[i] = (uint8_t)get_bits(&r, 8);
        }
    }
    return !r.bad;
}

/* Format 2 cards carry no paint: the emblem's four most used colours (near
 * identical entries counted together) become base, detail, joint and
 * optional, on every section (builder: paintScheme / paintFromEmblem). */
static void card_paint_from_emblem(struct moa_card *u)
{
    int count[16] = { 0 };
    for (int i = 0; i < 4096; i++) count[u->emblem[i] & 15]++;
    struct paint_group { double c[3]; int n; } g[16];
    int groups = 0;
    for (int i = 0; i < 16; i++) {
        if (!count[i]) continue;
        int c[3];
        c15_rgb(u->palette[i], c);
        int k = 0;
        for (; k < groups; k++)
            if (fabs(g[k].c[0] - c[0]) + fabs(g[k].c[1] - c[1]) + fabs(g[k].c[2] - c[2]) <= 60)
                break;
        if (k < groups) {
            const int t = g[k].n + count[i];
            for (int j = 0; j < 3; j++)
                g[k].c[j] = js_round((g[k].c[j] * g[k].n + c[j] * count[i]) / (double)t);
            g[k].n = t;
        } else {
            for (int j = 0; j < 3; j++) g[groups].c[j] = c[j];
            g[groups].n = count[i];
            groups++;
        }
    }
    /* Stable sort, most used first. */
    for (int i = 1; i < groups; i++)
        for (int j = i; j > 0 && g[j].n > g[j - 1].n; j--) {
            const struct paint_group t = g[j]; g[j] = g[j - 1]; g[j - 1] = t;
        }
    double top[4][3];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++)
            top[i][j] = i < groups ? g[i].c[j] : (groups ? g[groups - 1].c[j] : 128);
    /* top: base, detail, joint, optional -> stored base, optional, detail, joint */
    const int order[4] = { 0, 3, 1, 2 };
    for (int s = 0; s < 4; s++)
        for (int t = 0; t < 4; t++)
            for (int c = 0; c < 3; c++)
                u->paint[s * 12 + t * 3 + c] =
                    (uint8_t)js_round(top[order[t]][c] * CARD_PMAX / 255.0);
}

int moa_card_decode(const uint8_t *file, size_t bytes, struct moa_card *out)
{
    memset(out, 0, sizeof *out);
    if (!file || bytes < 8 || bytes > 64u * 1024u * 1024u) return 0;
    int w = 0, h = 0, n = 0;
    uint8_t *rgb = stbi_load_from_memory(file, (int)bytes, &w, &h, &n, 3);
    if (!rgb) return 0;
    struct card_image im = { rgb, w, h };
    int ok = 0;
    double c[3];
    if (w < 160 || h < 160) goto done;

    /* Strip width: the white start mark is as wide as the strip. */
    int cols = 0;
    while (cols < CARD_MAXCOLS) {
        card_avg(&im, cols * CARD_CELL + 2, 2, 4, 4, c);
        if (c[0] + c[1] + c[2] < 384) break;
        cols++;
    }
    if (cols < 2) goto done;

    /* Data cells: eight inks, three bits each (R, G, B over 127). */
    uint8_t bits[CARD_ROWS * CARD_MAXCOLS * 3];
    int nbits = 0;
    const int cells_max = CARD_ROWS * cols;
#define CARD_CELL_BITS(k) do { \
        card_avg(&im, ((k) % cols) * CARD_CELL + 2, \
                 CARD_Y_DATA + ((k) / cols) * CARD_CELL + 2, 4, 4, c); \
        bits[nbits++] = c[0] > 127; bits[nbits++] = c[1] > 127; bits[nbits++] = c[2] > 127; \
    } while (0)
    for (int k = 0; k < 3; k++) CARD_CELL_BITS(k);
    int len = 0;
    for (int i = 0; i < 8; i++) len = (len << 1) | bits[i];
    if (!len || len + 1 > card_cap(cols)) goto done;
    const int cells = ((len + 1) * 8 + 2) / 3;
    if (cells > cells_max) goto done;
    for (int k = 3; k < cells; k++) CARD_CELL_BITS(k);
#undef CARD_CELL_BITS
    uint8_t payload[CARD_ROWS * CARD_MAXCOLS * 3 / 8 + 1];
    int pbytes = 0;
    for (int i = 0; i + 8 <= nbits; i += 8) {
        int v = 0;
        for (int j = 0; j < 8; j++) v = (v << 1) | bits[i + j];
        payload[pbytes++] = (uint8_t)v;
    }
    int has_paint = 0;
    if (!card_unpack(payload, pbytes, out, &has_paint)) goto done;

    /* Emblem palette: the sixteen swatches under the start mark. */
    int pal_rgb[16][3];
    for (int i = 0; i < 16; i++) {
        card_avg(&im, 4, CARD_Y_SW + i * CARD_SW + 2, 8, 4, c);
        out->palette[i] = c15(c[0], c[1], c[2]);
        c15_rgb(out->palette[i], pal_rgb[i]);
    }
    /* Emblem: each 8x8 card block (a 4x4 block of the 256x256 art) averaged,
     * then the nearest palette colour. */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            card_avg(&im, CARD_EX + x * 8, CARD_EY + y * 8, 8, 8, c);
            int best = 0;
            double bd = 1e18;
            for (int k = 0; k < 16; k++) {
                const double dr = c[0] - pal_rgb[k][0], dg = c[1] - pal_rgb[k][1],
                             db = c[2] - pal_rgb[k][2];
                const double d = dr * dr + dg * dg + db * db;
                if (d < bd) { bd = d; best = k; }
            }
            out->emblem[y * 64 + x] = (uint8_t)best;
        }
    if (!has_paint) card_paint_from_emblem(out);
    out->pattern = 0;                          /* preset patterns are not offered */
    ok = 1;
done:
    stbi_image_free(rgb);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

void moa_card_record(const struct moa_card *card, uint8_t record[64])
{
    memcpy(record, card->rec, 13);
    record[13] = card->flag;
    record[14] = 0;                            /* custom paint */
    record[15] = card->variant;
    memcpy(record + 16, card->paint, 48);
}

/* ---- AI ------------------------------------------------------------------------
 *
 * The record the arena engine reads (36 bytes at 0x8004AC80) and the way the
 * game's own AI editor (RANKER MK AI, menu 0x8007FB78) writes its sliders into
 * it. Bytes the editor leaves alone keep the default record's values; the
 * advanced page can override any byte.
 */
static const uint8_t moa_ai_default[MOA_AI_RECORD_SIZE] = {
    0, 50, 50, 50, 0, 0, 0, 0, 80, 50, 55, 51, 12, 51, 50, 0,
    0xE8, 0x03, 0x88, 0x13,                          /* 1000, 5000 */
    50, 25, 50, 25, 50, 25, 50, 25, 50, 25,
    50, 50, 50, 50, 50, 50,
};

static const struct { const char *name; uint8_t s[MOA_AI_SLIDERS]; } moa_ai_presets[MOA_AI_PRESETS] = {
    /*                 shoot blade lock avoid fly range */
    { "CUSTOM",    {  50,  50,  50,  50,  50,  50 } },
    { "BALANCED",  {  50,  50,  50,  50,  50,  50 } },
    { "AGGRESSOR", {  90,  85,  95,  85,  80,  30 } },
    { "SNIPER",    {  95,  10,  95,  50,  30,  90 } },
    { "DUELIST",   {  40, 100,  70,  60,  70,   0 } },
    { "EVASIVE",   {  60,  30,  60, 100,  90,  70 } },
};

const char *moa_ai_preset_name(int preset)
{
    return (preset >= 0 && preset < MOA_AI_PRESETS) ? moa_ai_presets[preset].name : "CUSTOM";
}

void moa_ai_preset_sliders(int preset, uint8_t out[MOA_AI_SLIDERS])
{
    if (preset <= 0 || preset >= MOA_AI_PRESETS) preset = MOA_AI_PRESET_BALANCED;
    memcpy(out, moa_ai_presets[preset].s, MOA_AI_SLIDERS);
}

int moa_ai_rebel_step(uint8_t plus)
{
    /* menu 0x8007FAD8 for 0..3; step 4 = the builder's full level 9 */
    return plus == 0 ? 0 : plus < 4 ? 1 : plus < 6 ? 2 : plus < 9 ? 3 : 4;
}

uint8_t moa_ai_rebel_level(int step)
{
    static const uint8_t level[5] = { 0, 2, 5, 7, 9 };       /* menu 0x8007FB78 + full */
    return level[step < 0 ? 0 : step > 4 ? 4 : step];
}

void moa_ai_build_record(uint8_t plus, const uint8_t *sliders, const uint8_t *set,
                         const uint8_t *raw, uint8_t a[MOA_AI_RECORD_SIZE])
{
    uint8_t s[MOA_AI_SLIDERS];
    if (sliders) memcpy(s, sliders, sizeof s);
    else moa_ai_preset_sliders(MOA_AI_PRESET_BALANCED, s);
    for (int i = 0; i < MOA_AI_SLIDERS; i++) if (s[i] > 100) s[i] = 100;
    memcpy(a, moa_ai_default, MOA_AI_RECORD_SIZE);
    const int shoot = s[MOA_AI_SHOOT], blade = s[MOA_AI_BLADE], lock = s[MOA_AI_LOCK];
    const int avoid = s[MOA_AI_AVOID], fly = s[MOA_AI_FLY], range = s[MOA_AI_RANGE];
    /* As the editor writes them (menu 0x8007FBC4..0x8007FCC8). */
    a[13] = (uint8_t)(shoot + 1);
    a[9] = (uint8_t)blade;
    a[10] = (uint8_t)(105 - blade);
    a[11] = (uint8_t)(lock == 100 ? 0 : lock + 1);
    a[12] = (uint8_t)((100 - lock) / 4);
    a[14] = (uint8_t)avoid;
    a[1] = (uint8_t)(100 - fly);
    a[2] = (uint8_t)(100 - fly);
    a[3] = (uint8_t)fly;
    a[20] = (uint8_t)((range + 100 - fly) / 2);
    a[22] = (uint8_t)((fly + 100 - range) / 2);
    a[24] = (uint8_t)range;
    a[28] = (uint8_t)(100 - range);
    if (set && raw)
        for (int i = 1; i < MOA_AI_RECORD_SIZE; i++)
            if (set[i]) a[i] = raw[i];
    a[0] = plus;
}

void moa_card_ai_record(const struct moa_card *c, uint8_t record[MOA_AI_RECORD_SIZE])
{
    if (!c->has_ai) {
        moa_ai_build_record(c->plus, NULL, NULL, NULL, record);
        return;
    }
    moa_ai_build_record(c->plus, c->ai_slider, c->ai_set, c->ai_raw, record);
}

void moa_card_set_preset(struct moa_card *c, int preset)
{
    c->has_ai = 1;
    c->ai_preset = (uint8_t)((preset > 0 && preset < MOA_AI_PRESETS) ? preset
                                                                     : MOA_AI_PRESET_BALANCED);
    moa_ai_preset_sliders(c->ai_preset, c->ai_slider);
    memset(c->ai_set, 0, sizeof c->ai_set);
    memset(c->ai_raw, 0, sizeof c->ai_raw);
}

/* ---- writing cards ------------------------------------------------------------ */

/* stb_image_write is compiled into the launcher (launcher_debug.c). */
int stbi_write_png_to_func(void (*func)(void *, void *, int), void *context,
                           int w, int h, int comp, const void *data, int stride);

#define CARD_INK_ON  200                       /* data inks, as the builder draws them */
#define CARD_INK_OFF 56

/* Scenario save layout (one block; offsets from the block's start, as the
 * game's save writer at menu 0x80071938 lays it out). */
#define SAVE_PARTS    0x1B8                    /* 12 part ids, garage order */
#define SAVE_PATTERN  0x1C4
#define SAVE_VARIANT  0x1C5
#define SAVE_OPTIONS  0x1C6                    /* u16 optional-parts mask */
#define SAVE_ACNAME   0x1CC                    /* 18 bytes: 8 two-byte characters, '>' ends */
#define SAVE_PILOT    0x1DE                    /* the game's name editors: "AC NAME" / "PILOT NAME" */
#define SAVE_PAINT    0x250                    /* 48 colour values */
#define SAVE_EMBLEM   0x480                    /* 64x64, 4 bits per pixel */
#define SAVE_CLUT     0xC80                    /* 16 x 15-bit colours */
#define SAVE_PLUS     0xDEC                    /* Human PLUS state; [0] = level */

/* Names are Shift-JIS, two bytes a character (the name editor offers full-width
 * letters, digits, kana and symbols), eight characters at most, ended by '>'
 * on a character boundary. The card keeps plain ASCII: full-width letters,
 * digits and common punctuation map to their ASCII twins, anything else (kana,
 * Greek, box drawing) becomes '?'. */
static char sjis_to_ascii(uint8_t hi, uint8_t lo)
{
    if (hi >= 0x20 && hi < 0x7F) return (char)hi;          /* already one byte */
    if (hi == 0x82) {
        if (lo >= 0x4F && lo <= 0x58) return (char)('0' + (lo - 0x4F));
        if (lo >= 0x60 && lo <= 0x79) return (char)('A' + (lo - 0x60));
        if (lo >= 0x81 && lo <= 0x9A) return (char)('a' + (lo - 0x81));
    } else if (hi == 0x81) {
        switch (lo) {
        case 0x40: return ' ';  case 0x43: return ',';  case 0x44: return '.';
        case 0x45: return '.';  case 0x46: return ':';  case 0x47: return ';';
        case 0x48: return '?';  case 0x49: return '!';  case 0x5B: return '-';
        case 0x5E: return '/';  case 0x60: return '~';  case 0x62: return '|';
        case 0x66: return '\'';  case 0x68: return '"';  case 0x69: return '(';
        case 0x6A: return ')';  case 0x6D: return '[';  case 0x6E: return ']';
        case 0x7B: return '+';  case 0x7C: return '-';  case 0x81: return '=';
        case 0x83: return '<';  case 0x84: return '>';  case 0x90: return '$';
        case 0x93: return '%';  case 0x94: return '#';  case 0x95: return '&';
        case 0x96: return '*';  case 0x97: return '@';
        default: break;
        }
    }
    return '?';
}

static void save_name(const uint8_t *src, char out[17])
{
    int n = 0;
    for (int i = 0; i < 9 && n < 16; i++) {
        const uint8_t hi = src[2 * i], lo = src[2 * i + 1];
        if (hi == 0 || hi == '>') break;
        out[n++] = sjis_to_ascii(hi, lo);
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

/* The builder's default emblem: an orange ring and a red arrow on dark slate. */
static void default_emblem(struct moa_card *c)
{
    static const uint8_t rgb[4][3] = {
        { 16, 22, 28 }, { 240, 160, 32 }, { 226, 91, 58 }, { 255, 244, 220 } };
    memset(c->palette, 0, sizeof c->palette);
    for (int i = 0; i < 4; i++) c->palette[i] = c15(rgb[i][0], rgb[i][1], rgb[i][2]);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const double dx = x + 0.5 - 32, dy = y + 0.5 - 32, r = sqrt(dx * dx + dy * dy);
            uint8_t v = 0;
            if (r > 20 && r < 26) v = 1;
            /* arrow: apex (32,10), base corners (50,48) and (14,48), notch (32,40) */
            const double fx = x + 0.5, fy = y + 0.5;
            if (fy >= 10 && fy <= 48) {
                const double half = (fy - 10) * 18.0 / 38.0;
                const double notch = fy > 40 ? (fy - 40) * 18.0 / 8.0 : -1;
                if (fabs(fx - 32) <= half && !(notch >= 0 && fabs(fx - 32) < notch)) v = 2;
            }
            if (fx >= 30 && fx <= 34 && fy >= 24 && fy <= 36) v = 3;
            c->emblem[y * 64 + x] = v;
        }
}

int moa_card_from_save(const uint8_t *b, size_t bytes, struct moa_card *c)
{
    memset(c, 0, sizeof *c);
    if (!b || bytes < 0x2000 || b[0] != 'S' || b[1] != 'C') return 0;
    const uint8_t *g = b + SAVE_PARTS;      /* core, booster, head, arms, blade,
                                               hand, back L, back R, generator,
                                               FCS, -, legs */
    if (g[0] == 0xFF || g[2] == 0xFF || g[3] == 0xFF || g[11] == 0xFF) return 0;
    c->format = 3;
    const uint8_t rec[13] = { g[0], g[2], g[3], g[11], g[5], g[4], g[6], g[7], g[9], g[8],
                              b[SAVE_OPTIONS], b[SAVE_OPTIONS + 1], g[1] };
    memcpy(c->rec, rec, 13);
    c->pattern = 0;
    c->variant = b[SAVE_VARIANT] & 3;
    c->plus = b[SAVE_PLUS] > 15 ? 15 : b[SAVE_PLUS];
    for (int i = 0; i < 48; i++) c->paint[i] = b[SAVE_PAINT + i] > 127 ? 127 : b[SAVE_PAINT + i];
    save_name(b + SAVE_PILOT, c->pilot);
    save_name(b + SAVE_ACNAME, c->ac);
    int drawn = 0;
    for (int i = 0; i < 2048; i++) drawn |= b[SAVE_EMBLEM + i];
    if (drawn) {
        for (int i = 0; i < 16; i++)
            c->palette[i] = (uint16_t)((b[SAVE_CLUT + 2 * i] | (b[SAVE_CLUT + 2 * i + 1] << 8)) & 0x7FFF);
        for (int i = 0; i < 2048; i++) {
            c->emblem[2 * i] = b[SAVE_EMBLEM + i] & 15;
            c->emblem[2 * i + 1] = b[SAVE_EMBLEM + i] >> 4;
        }
    } else {
        default_emblem(c);
    }
    return 1;
}

/* ---- format 3 payload (builder: pack) ---- */

struct bit_writer { uint8_t b[160]; int pos; };

static void put_bits(struct bit_writer *w, unsigned v, int n)
{
    for (int i = n - 1; i >= 0; i--, w->pos++)
        if (w->pos < 160 * 8 && ((v >> i) & 1u)) w->b[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7));
}

static int card_pack(const struct moa_card *c, uint8_t out[164])
{
    struct bit_writer w;
    memset(&w, 0, sizeof w);
    put_bits(&w, c->has_ai ? 4u : 3u, 4);
    for (int i = 0; i < 13; i++) put_bits(&w, c->rec[i], 8);
    put_bits(&w, c->flag & 1, 1);
    put_bits(&w, 0, 4);                        /* pattern: custom paint */
    put_bits(&w, c->variant & 3, 2);
    put_bits(&w, c->plus & 15, 4);
    int uni = 1;
    for (int i = 12; i < 48; i++) if (c->paint[i] != c->paint[i % 12]) uni = 0;
    put_bits(&w, (unsigned)uni, 1);
    for (int i = 0; i < (uni ? 12 : 48); i++) put_bits(&w, c->paint[i] & 127, 7);
    const char *names[2] = { c->pilot, c->ac };
    int len[2];
    for (int k = 0; k < 2; k++) { len[k] = (int)strlen(names[k]); if (len[k] > 16) len[k] = 16; }
    for (int k = 0; k < 2; k++) put_bits(&w, (unsigned)len[k], 5);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < len[k]; i++) {
            const unsigned ch = (unsigned char)names[k][i];
            put_bits(&w, (ch >= 32 && ch < 127) ? ch : 63, 7);
        }
    if (c->has_ai) {                           /* format 4 */
        put_bits(&w, 1, 1);
        put_bits(&w, c->ai_preset < MOA_AI_PRESETS ? c->ai_preset : 0u, 3);
        for (int i = 0; i < MOA_AI_SLIDERS; i++)
            put_bits(&w, c->ai_slider[i] > 100 ? 100u : c->ai_slider[i], 7);
        int adv = 0;
        for (int i = 1; i < MOA_AI_RECORD_SIZE; i++) if (c->ai_set[i]) adv = 1;
        put_bits(&w, (unsigned)adv, 1);
        if (adv) {
            for (int i = 0; i < MOA_AI_RECORD_SIZE; i++) put_bits(&w, i && c->ai_set[i], 1);
            for (int i = 1; i < MOA_AI_RECORD_SIZE; i++)
                if (c->ai_set[i]) put_bits(&w, c->ai_raw[i], 8);
        }
    }
    const int body = (w.pos + 7) / 8;
    const uint16_t crc = crc16_ccitt(w.b, body);
    out[0] = (uint8_t)(body + 2);
    memcpy(out + 1, w.b, (size_t)body);
    out[1 + body] = (uint8_t)(crc >> 8);
    out[2 + body] = (uint8_t)crc;
    return body + 3;
}

/* ---- drawing ---- */

struct canvas { uint8_t *px; };

static void fill(struct canvas *cv, int x, int y, int w, int h, int r, int g, int b)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            if (i < 0 || j < 0 || i >= CARD_N || j >= CARD_N) continue;
            uint8_t *p = cv->px + ((size_t)j * CARD_N + i) * 3;
            p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
        }
}

static int text_width(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        const int ch = (unsigned char)*s;
        w += (ch >= 32 && ch < 127) ? moa_font_width[ch - 32] + 1 : 8;
    }
    return w;
}

/* Upper-case text; `y` is the vertical middle, as the builder sets it. */
static void draw_text(struct canvas *cv, int x, int y, const char *s, int r, int g, int b)
{
    const int top = y - MOA_FONT_H / 2;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch >= 127) ch = '?';
        const uint16_t *rows = moa_font_rows[ch - 32];
        for (int j = 0; j < MOA_FONT_H; j++)
            for (int i = 0; i < 16; i++)
                if (rows[j] >> i & 1u) fill(cv, x + i, top + j, 1, 1, r, g, b);
        x += moa_font_width[ch - 32] + 1;
    }
}

static void upper_copy(char *dst, const char *src)
{
    for (; *src; src++, dst++) *dst = (*src >= 'a' && *src <= 'z') ? (char)(*src - 32) : *src;
    *dst = 0;
}

struct png_sink { uint8_t *data; size_t size, cap; int failed; };

static void png_sink_write(void *ctx, void *data, int size)
{
    struct png_sink *s = (struct png_sink *)ctx;
    if (s->failed || size <= 0) return;
    if (s->size + (size_t)size > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 65536;
        while (cap < s->size + (size_t)size) cap *= 2;
        uint8_t *d = (uint8_t *)realloc(s->data, cap);
        if (!d) { s->failed = 1; return; }
        s->data = d; s->cap = cap;
    }
    memcpy(s->data + s->size, data, (size_t)size);
    s->size += (size_t)size;
}

int moa_card_write_png(const struct moa_card *c, const char *path)
{
    uint8_t payload[164];
    const int n = card_pack(c, payload);
    int cols = 2;
    while (cols < CARD_MAXCOLS && n > card_cap(cols)) cols++;
    if (n > card_cap(cols)) return 0;
    struct canvas cv = { (uint8_t *)malloc((size_t)CARD_N * CARD_N * 3) };
    if (!cv.px) return 0;
    fill(&cv, 0, 0, CARD_N, CARD_N, 6, 9, 13);
    /* frame (under the strip: a six-column strip reaches x = 48) */
    fill(&cv, 40, 24, 576, 1, 47, 74, 85); fill(&cv, 40, 615, 576, 1, 47, 74, 85);
    fill(&cv, 40, 24, 1, 592, 47, 74, 85); fill(&cv, 615, 24, 1, 592, 47, 74, 85);
    const int W = cols * CARD_CELL;
    fill(&cv, 0, 0, W, CARD_Y_SW, 255, 255, 255);
    int rgb[16][3];
    for (int i = 0; i < 16; i++) {
        c15_rgb(c->palette[i], rgb[i]);
        fill(&cv, 0, CARD_Y_SW + i * CARD_SW, W, CARD_SW, rgb[i][0], rgb[i][1], rgb[i][2]);
    }
    fill(&cv, 0, CARD_Y_SW + 16 * CARD_SW, W, 4, 255, 255, 255);
    const int bits = n * 8, cells = (bits + 2) / 3;
    for (int k = 0; k < cells; k++) {
        int v = 0;
        for (int j = 0; j < 3; j++) {
            const int bit = k * 3 + j;
            v = (v << 1) | (bit < bits ? (payload[bit >> 3] >> (7 - (bit & 7))) & 1 : 0);
        }
        fill(&cv, (k % cols) * CARD_CELL, CARD_Y_DATA + (k / cols) * CARD_CELL, CARD_CELL, CARD_CELL,
             v & 4 ? CARD_INK_ON : CARD_INK_OFF, v & 2 ? CARD_INK_ON : CARD_INK_OFF,
             v & 1 ? CARD_INK_ON : CARD_INK_OFF);
    }
    /* emblem: 64x64 at 8x */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int *p = rgb[c->emblem[y * 64 + x] & 15];
            fill(&cv, CARD_EX + x * 8, CARD_EY + y * 8, 8, 8, p[0], p[1], p[2]);
        }
    char up[20];
    upper_copy(up, c->ac);
    draw_text(&cv, CARD_EX, 44, up, 185, 212, 207);
    upper_copy(up, c->pilot);
    draw_text(&cv, CARD_EX + 512 - text_width(up), 44, up, 95, 127, 128);
    draw_text(&cv, CARD_EX, 598, "MASTER OF ARENA  /  AI BATTLE ENTRY", 95, 127, 128);

    struct png_sink sink = { 0 };
    const int ok = stbi_write_png_to_func(png_sink_write, &sink, CARD_N, CARD_N, 3, cv.px,
                                          CARD_N * 3);
    free(cv.px);
    int written = 0;
    if (ok && !sink.failed && sink.size) {
        FILE *f = fopen(path, "wb");
        if (f) {
            written = fwrite(sink.data, 1, sink.size, f) == sink.size;
            if (fclose(f) != 0) written = 0;
            if (!written) remove(path);
        }
    }
    free(sink.data);
    return written;
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int moa_card_export_memcard(const char *mcd_path, const char *out_dir)
{
    FILE *f = fopen(mcd_path, "rb");
    if (!f) return -1;
    uint8_t *mc = (uint8_t *)malloc(131072);
    const size_t got = mc ? fread(mc, 1, 131072, f) : 0;
    fclose(f);
    if (got != 131072 || mc[0] != 'M' || mc[1] != 'C') { free(mc); return -1; }
    int written = 0;
    static struct moa_card card;
    for (int i = 1; i < 16; i++) {
        const uint8_t *fr = mc + i * 128;
        const uint32_t state = fr[0] | (fr[1] << 8) | (fr[2] << 16) | ((uint32_t)fr[3] << 24);
        if (state != 0x51) continue;           /* first block of a save */
        if (memcmp(fr + 10, "BASLUS-01030", 12) != 0) continue;
        if (!moa_card_from_save(mc + i * 8192, 8192, &card)) continue;
        char base[40];
        const char *name = card.ac[0] ? card.ac : (card.pilot[0] ? card.pilot : "raven");
        int n = 0;
        for (const char *p = name; *p && n < 32; p++) {
            const char ch = *p;
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) base[n++] = ch;
            else if (ch >= 'A' && ch <= 'Z') base[n++] = (char)(ch - 'A' + 'a');
            else if (n && base[n - 1] != '-') base[n++] = '-';
        }
        while (n && base[n - 1] == '-') n--;
        if (!n) { memcpy(base, "raven", 5); n = 5; }
        base[n] = 0;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s-card.png", out_dir, base);
        for (int k = 2; file_exists(path) && k < 100; k++)
            snprintf(path, sizeof path, "%s/%s-card-%d.png", out_dir, base, k);
        if (file_exists(path)) continue;
        if (moa_card_write_png(&card, path)) written++;
    }
    free(mc);
    return written;
}
