/* Master of Arena: AI Battle card reader. See moa_card.h. */
#include "moa_card.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
#define CARD_MAXCOLS 4
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
    if (fmt != 2 && fmt != 3) return 0;
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

uint32_t moa_card_seed(const struct moa_card *card)
{
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < 13; i++) {
        c ^= card->rec[i];
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

/* Legs that few in-game opponents use, and the nearest similar legs (same
 * type, closest weight and armour) whose opponents join the draw. Indexed by
 * leg part id; computed from the part tables (AI Battle matching notes). */
static const struct { uint8_t legs; uint8_t near[2]; uint8_t count; } moa_leg_near[] = {
    { 0, { 6 }, 1 }, { 2, { 13 }, 1 }, { 3, { 0, 4 }, 2 }, { 9, { 13 }, 1 },
    { 10, { 0 }, 1 }, { 14, { 18 }, 1 }, { 20, { 16 }, 1 }, { 24, { 23 }, 1 },
    { 27, { 28 }, 1 }, { 32, { 30 }, 1 },
};

static int moa_leg_candidate(uint8_t want, uint8_t legs)
{
    if (legs == want) return 1;
    for (size_t i = 0; i < sizeof moa_leg_near / sizeof moa_leg_near[0]; i++)
        if (moa_leg_near[i].legs == want)
            for (int k = 0; k < moa_leg_near[i].count; k++)
                if (moa_leg_near[i].near[k] == legs) return 1;
    return 0;
}

int moa_card_pick_ai(const struct moa_card *card, const uint8_t (*pool)[3],
                     int pool_count)
{
    const uint8_t head = card->rec[1], arms = card->rec[2], legs = card->rec[3];
    /* Nobody on these or similar legs (a damaged card): everyone draws. */
    int everyone = 1;
    for (int i = 0; i < pool_count; i++)
        if (moa_leg_candidate(legs, pool[i][2])) { everyone = 0; break; }
    uint32_t total = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t r = pass ? moa_card_seed(card) % total : 0;
        for (int i = 0; i < pool_count; i++) {
            if (!everyone && !moa_leg_candidate(legs, pool[i][2])) continue;
            const uint32_t w = 1u + 3u * (pool[i][2] == legs) + 3u * (pool[i][1] == arms) +
                               3u * (pool[i][0] == head);
            if (!pass) { total += w; continue; }
            if (r < w) return i;
            r -= w;
        }
        if (!total) return -1;
    }
    return -1;
}
