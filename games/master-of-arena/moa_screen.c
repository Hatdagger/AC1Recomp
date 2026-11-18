/* AI Battle loading screen (moa_screen.h). */
#include "moa_screen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- game font ------------------------------------------------------------
 * MENU_TIM.T opens with the menu's text sheet: a 4-bit TIM, 256x216, whose
 * top 48 lines are the large font, 8x16 cells, 32 per row, ASCII 32..127.
 * The sheet has ';' where ':' would be and the other way round. */
#define FONT_W 8
#define FONT_H 16
static uint8_t s_font[96][FONT_H];             /* one bit per pixel column */
static int s_font_ok;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

int moa_screen_load_font(const uint8_t *tim, uint32_t bytes)
{
    s_font_ok = 0;
    if (!tim) return 0;
    const uint32_t end = bytes < 0x12800u ? bytes : 0x12800u;
    for (uint32_t o = 0; o + 64u < end; o += 4u) {
        if (rd32(tim + o) != 0x10u || rd32(tim + o + 4u) != 8u) continue;
        const uint32_t clut_len = rd32(tim + o + 8u);
        const uint32_t cw = rd16(tim + o + 16u);
        const uint32_t img = o + 8u + clut_len;
        if (cw < 16u || clut_len < 12u + 32u || img + 12u > end) continue;
        const uint32_t ilen = rd32(tim + img), w = rd16(tim + img + 8u), h = rd16(tim + img + 10u);
        if (w != 64u || h != 216u || ilen != 12u + w * 2u * h || img + ilen > end) continue;
        uint8_t bright[16];
        for (int i = 0; i < 16; i++) {
            const uint32_t c = rd16(tim + o + 20u + 2u * (uint32_t)i);
            const uint32_t r = (c & 31u) * 8u, g = ((c >> 5) & 31u) * 8u, b = ((c >> 10) & 31u) * 8u;
            bright[i] = (uint8_t)((r * 3u + g * 6u + b) / 10u > 128u);
        }
        const uint8_t *px = tim + img + 12u;
        for (int ch = 0; ch < 96; ch++) {
            const int cx = (ch % 32) * FONT_W, cy = (ch / 32) * FONT_H;
            for (int y = 0; y < FONT_H; y++) {
                uint8_t bits = 0;
                for (int x = 0; x < FONT_W; x++) {
                    const int X = cx + x;
                    const uint8_t v = (uint8_t)((px[(cy + y) * 128 + X / 2] >> (4 * (X & 1))) & 15u);
                    if (bright[v]) bits |= (uint8_t)(0x80u >> x);
                }
                s_font[ch][y] = bits;
            }
        }
        s_font_ok = 1;
        return 1;
    }
    return 0;
}

int moa_screen_ready(void) { return s_font_ok; }

static int glyph_index(char c)
{
    if (c == ':') c = ';';
    else if (c == ';') c = ':';
    const int i = (unsigned char)c - 32;
    return i >= 0 && i < 96 ? i : 0;
}

/* ---- canvas ---------------------------------------------------------------- */
#define CANVAS_H 720
#define CONTENT_W 960
#define MAX_W 1920

static uint32_t *s_static, *s_frame;           /* background + fixed content; the output */
static uint8_t *s_glow;                        /* text halo, 0..255 */
static uint8_t *s_core;                        /* glowing text itself, 0/1 */
static int s_w, s_h;
static int s_dirty = 1;
static struct moa_screen_unit s_unit[2];
static char s_status[64];
static int s_glow_x0, s_glow_y0, s_glow_x1, s_glow_y1;

void moa_screen_set_units(const struct moa_screen_unit *a, const struct moa_screen_unit *b)
{
    memset(s_unit, 0, sizeof s_unit);
    if (a) s_unit[0] = *a;
    if (b) s_unit[1] = *b;
    s_dirty = 1;
}

static uint32_t rgb(int r, int g, int b) { return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }

static void fill(uint32_t *buf, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_w) x1 = s_w;
    if (y1 > s_h) y1 = s_h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) buf[y * s_w + x] = c;
}

static void frame_rect(uint32_t *buf, int x0, int y0, int x1, int y1, int t, uint32_t c)
{
    fill(buf, x0, y0, x1, y0 + t, c);
    fill(buf, x0, y1 - t, x1, y1, c);
    fill(buf, x0, y0, x0 + t, y1, c);
    fill(buf, x1 - t, y0, x1, y1, c);
}

static int text_width(const char *s, int scale) { return (int)strlen(s) * FONT_W * scale; }

/* Text centred on (cx, cy): into a colour buffer, or as 1s into a mask. */
static void text(uint32_t *buf, uint8_t *mask, const char *s, int cx, int cy, int scale, uint32_t c)
{
    const int x0 = cx - text_width(s, scale) / 2, y0 = cy - FONT_H * scale / 2;
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = s_font[glyph_index(s[i])];
        for (int y = 0; y < FONT_H * scale; y++) {
            const int Y = y0 + y;
            if (Y < 0 || Y >= s_h) continue;
            const uint8_t bits = g[y / scale];
            for (int x = 0; x < FONT_W * scale; x++) {
                if (!(bits & (0x80u >> (x / scale)))) continue;
                const int X = x0 + i * FONT_W * scale + x;
                if (X < 0 || X >= s_w) continue;
                if (buf) buf[Y * s_w + X] = c;
                if (mask) mask[Y * s_w + X] = 1;
            }
        }
    }
}

/* Dull steel plates, 64x64: seams, a bevel, rivets, an inset square and
 * vent slots. */
static uint32_t tile_px(int x, int y)
{
    const int T = 64, h = T / 2;
    int v = 32;                                             /* plate */
    if (x == 0 || y == 0 || x == T - 1 || y == T - 1) v = 15;  /* seam */
    else if (x == 1 || y == 1) v = 44;                      /* bevel light */
    if ((x == h && y < h - 6) || (y == h && x < h - 6)) v = 19;
    if ((x == h - 6 || x == h + 6) && y >= h - 6 && y <= h + 6) v = 42;
    if ((y == h - 6 || y == h + 6) && x >= h - 6 && x <= h + 6) v = 42;
    for (int k = 0; k < 4; k++)
        if (y == h + 12 + k * 4 && x >= h + 10 && x <= T - 10) v = 18;
    const int rv[4][2] = { { 5, 5 }, { T - 6, 5 }, { 5, T - 6 }, { T - 6, T - 6 } };
    for (int k = 0; k < 4; k++) {
        const int dx = x - rv[k][0], dy = y - rv[k][1];
        if (dx * dx + dy * dy <= 5) v = (dx == -1 && dy == -1) ? 68 : 50;
    }
    return rgb(v, v + 4, v + 7);
}

static void build_static(void)
{
    const int W = s_w, H = s_h, cx = W / 2;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t c = tile_px(x & 63, y & 63);
            /* darker towards the middle so the content reads */
            const float nx = (x - cx) / (CONTENT_W * 0.42f), ny = (y - H / 2) / (H * 0.46f);
            float d = 1.0f - (nx * nx + ny * ny);
            if (d < 0) d = 0;
            const float k = 1.0f - 0.55f * d;
            const int r = (int)(((c >> 16) & 255) * k), g = (int)(((c >> 8) & 255) * k), b = (int)((c & 255) * k);
            s_static[y * W + x] = rgb(r, g, b);
        }
    const uint32_t grey = rgb(150, 156, 168), white = rgb(235, 238, 242);
    text(s_static, NULL, "AI BATTLE", cx, 70, 2, grey);
    fill(s_static, cx - 220, 91, cx + 220, 93, rgb(70, 74, 82));
    const uint32_t side[2] = { rgb(90, 170, 255), rgb(255, 90, 80) };
    for (int u = 0; u < 2; u++) {
        const int ux = cx + (u ? 230 : -230), x0 = ux - 96, y0 = 150;
        fill(s_static, x0 - 6, y0 - 6, x0 + 198, y0 + 198, rgb(0, 0, 0));
        frame_rect(s_static, x0 - 6, y0 - 6, x0 + 198, y0 + 198, 3, side[u]);
        if (!s_unit[u].have) continue;
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 192; x++)
                s_static[(y0 + y) * W + x0 + x] = 0xFF000000u | s_unit[u].emblem[(y / 3) * 64 + x / 3];
        char line[32];
        snprintf(line, sizeof line, "AC: %s", s_unit[u].ac);
        text(s_static, NULL, line, ux, y0 + 244, 3, white);
        snprintf(line, sizeof line, "PILOT: %s", s_unit[u].pilot);
        text(s_static, NULL, line, ux, y0 + 294, 2, grey);
        if (s_unit[u].style[0]) text(s_static, NULL, s_unit[u].style, ux, y0 + 334, 2, side[u]);
    }
}

/* The halo of "VS" and the status line: their pixels, spread and blurred. */
static void build_glow(void)
{
    const int W = s_w, cx = W / 2;
    memset(s_core, 0, (size_t)W * s_h);
    memset(s_glow, 0, (size_t)W * s_h);
    text(NULL, s_core, "VS", cx, 246, 3, 0);
    text(NULL, s_core, s_status, cx, 590, 2, 0);
    const int sw = text_width(s_status, 2) / 2 + 16;
    s_glow_x0 = cx - (sw > 60 ? sw : 60); s_glow_x1 = cx + (sw > 60 ? sw : 60);
    s_glow_y0 = 210; s_glow_y1 = 620;
    if (s_glow_x0 < 0) s_glow_x0 = 0;
    if (s_glow_x1 > W) s_glow_x1 = W;
    /* spread 2 px, then three box blurs of radius 3 */
    const int x0 = s_glow_x0, x1 = s_glow_x1, y0 = s_glow_y0, y1 = s_glow_y1;
    const int bw = x1 - x0, bh = y1 - y0;
    float *a = (float *)calloc((size_t)bw * bh, sizeof(float));
    float *t = (float *)calloc((size_t)bw * bh, sizeof(float));
    if (!a || !t) { free(a); free(t); return; }
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int on = 0;
            for (int dy = -2; dy <= 2 && !on; dy++)
                for (int dx = -2; dx <= 2 && !on; dx++) {
                    const int X = x0 + x + dx, Y = y0 + y + dy;
                    if (X >= 0 && X < W && Y >= 0 && Y < s_h && s_core[Y * W + X]) on = 1;
                }
            a[y * bw + x] = on ? 1.0f : 0.0f;
        }
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                float s = 0; int n = 0;
                for (int k = -3; k <= 3; k++) { const int X = x + k; if (X >= 0 && X < bw) { s += a[y * bw + X]; n++; } }
                t[y * bw + x] = s / (float)n;
            }
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                float s = 0; int n = 0;
                for (int k = -3; k <= 3; k++) { const int Y = y + k; if (Y >= 0 && Y < bh) { s += t[Y * bw + x]; n++; } }
                a[y * bw + x] = s / (float)n;
            }
    }
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            float v = a[y * bw + x] * 1.6f;
            if (v > 1.0f) v = 1.0f;
            s_glow[(y0 + y) * W + x0 + x] = (uint8_t)(v * 255.0f);
        }
    free(a); free(t);
}

static uint32_t add_glow(uint32_t c, float g)
{
    int r = (int)((c >> 16) & 255) + (int)(200.0f * g);
    int gr = (int)((c >> 8) & 255) + (int)(80.0f * g);
    int b = (int)(c & 255);
    if (r > 255) r = 255;
    if (gr > 255) gr = 255;
    return rgb(r, gr, b);
}

const uint32_t *moa_screen_render(int win_w, int win_h, const char *status,
                                  int step, int permille, int limit_secs, float fade,
                                  uint64_t now_ms, int *out_w, int *out_h)
{
    *out_w = *out_h = 0;
    if (!s_font_ok) return NULL;
    int W = CONTENT_W;
    if (win_w > 0 && win_h > 0) W = (int)((int64_t)CANVAS_H * win_w / win_h);
    if (W < CONTENT_W) W = CONTENT_W;
    if (W > MAX_W) W = MAX_W;
    W &= ~1;
    if (W != s_w || !s_static) {
        free(s_static); free(s_frame); free(s_glow); free(s_core);
        s_w = W; s_h = CANVAS_H;
        s_static = (uint32_t *)malloc((size_t)W * s_h * 4u);
        s_frame = (uint32_t *)malloc((size_t)W * s_h * 4u);
        s_glow = (uint8_t *)malloc((size_t)W * s_h);
        s_core = (uint8_t *)malloc((size_t)W * s_h);
        if (!s_static || !s_frame || !s_glow || !s_core) {
            free(s_static); free(s_frame); free(s_glow); free(s_core);
            s_static = s_frame = NULL; s_glow = s_core = NULL; s_w = 0;
            return NULL;
        }
        s_dirty = 1;
        s_status[0] = '\0';
    }
    if (s_dirty) { build_static(); s_dirty = 0; s_status[0] = '\0'; }
    if (!status) status = "";
    if (strcmp(status, s_status) != 0) {
        snprintf(s_status, sizeof s_status, "%s", status);
        build_glow();
    }
    /* pulse about once a second, never fully off */
    const float ph = (float)(now_ms % 1100u) / 1100.0f;
    const float pulse = 0.45f + 0.55f * (0.5f + 0.5f * sinf(ph * 6.2831853f));
    memcpy(s_frame, s_static, (size_t)W * s_h * 4u);
    const int cx = W / 2;
    for (int y = s_glow_y0; y < s_glow_y1; y++)
        for (int x = s_glow_x0; x < s_glow_x1; x++) {
            const int i = y * W + x;
            if (s_glow[i]) s_frame[i] = add_glow(s_frame[i], s_glow[i] / 255.0f * pulse);
        }
    for (int y = s_glow_y0; y < s_glow_y1; y++)
        for (int x = s_glow_x0; x < s_glow_x1; x++)
            if (s_core[y * W + x]) s_frame[y * W + x] = rgb(255, 246, 232);
    if (permille >= 0) {
        if (permille > 1000) permille = 1000;
        const int bx0 = cx - 260, bx1 = cx + 260, by = 625;
        fill(s_frame, bx0, by, bx1, by + 11, rgb(10, 9, 6));
        frame_rect(s_frame, bx0, by, bx1, by + 11, 1, rgb(90, 60, 20));
        const int fx1 = bx0 + 2 + (bx1 - bx0 - 4) * permille / 1000;
        if (fx1 > bx0 + 2) {
            for (int y = by - 8; y < by + 19; y++)
                for (int x = bx0 - 6; x < fx1 + 8; x++) {
                    if (x < 0 || x >= W) continue;
                    const int dx = x < bx0 + 2 ? bx0 + 2 - x : (x >= fx1 ? x - fx1 + 1 : 0);
                    const int dy = y < by + 2 ? by + 2 - y : (y >= by + 9 ? y - by - 8 : 0);
                    const float d = sqrtf((float)(dx * dx + dy * dy));
                    if (d <= 0.0f || d >= 8.0f) continue;
                    const float g = (1.0f - d / 8.0f);
                    s_frame[y * W + x] = add_glow(s_frame[y * W + x], g * g * pulse);
                }
            fill(s_frame, bx0 + 2, by + 2, fx1, by + 9, rgb(255, 220, 170));
        }
    }
    {   /* the steps: done dim amber, current bright, to come grey */
        static const char *const names[MOA_STEP_COUNT] = {
            "1 SETTING UP", "2 STAGING", "3 BATTLE DATA", "4 COMPLETE" };
        int widths = 0;
        for (int i = 0; i < MOA_STEP_COUNT; i++) widths += text_width(names[i], 1);
        const int gap = 34;
        int x = cx - (widths + gap * (MOA_STEP_COUNT - 1)) / 2;
        for (int i = 0; i < MOA_STEP_COUNT; i++) {
            const int w = text_width(names[i], 1);
            const uint32_t c = i == step ? rgb(255, 210, 140)
                             : i < step  ? rgb(150, 100, 40) : rgb(90, 94, 102);
            text(s_frame, NULL, names[i], x + w / 2, 660, 1, c);
            if (i == step) fill(s_frame, x, 670, x + w, 672, rgb(255, 150, 30));
            if (i + 1 < MOA_STEP_COUNT) text(s_frame, NULL, ">", x + w + gap / 2, 660, 1, rgb(70, 74, 82));
            x += w + gap;
        }
    }
    if (limit_secs > 0) {
        char line[32];
        snprintf(line, sizeof line, "TIME LIMIT %d:%02d", limit_secs / 60, limit_secs % 60);
        text(s_frame, NULL, line, cx, 696, 1, rgb(130, 136, 148));
    }
    if (fade > 0.0f) {
        const float k = fade >= 1.0f ? 0.0f : 1.0f - fade;
        for (int i = 0; i < W * s_h; i++) {
            const uint32_t c = s_frame[i];
            s_frame[i] = rgb((int)(((c >> 16) & 255) * k), (int)(((c >> 8) & 255) * k), (int)((c & 255) * k));
        }
    }
    *out_w = W;
    *out_h = s_h;
    return s_frame;
}

/* ---- part tiles (moa_screen_render_tiles) ---------------------------------- */
/* A tile for the opaque backdrop (moa_screen_set_backdrop), drawn at 3x so a
 * 64x64 tile matches the hub's own pixel size on a 720-line canvas. */
#define BACK_MAX 64
#define BACK_SCALE 3
static uint32_t s_back[BACK_MAX * BACK_MAX];
static int s_back_w, s_back_h;

void moa_screen_set_backdrop(const uint32_t *rgb_tile, int w, int h)
{
    if (!rgb_tile || w <= 0 || h <= 0 || w > BACK_MAX || h > BACK_MAX) { s_back_w = s_back_h = 0; return; }
    for (int i = 0; i < w * h; i++) s_back[i] = 0xFF000000u | (rgb_tile[i] & 0xFFFFFFu);
    s_back_w = w;
    s_back_h = h;
}

/* Opaque: the backdrop tile when one is set, else the steel plate at half
 * brightness. See-through: a dark veil the live game shows through. */
static void backdrop(uint32_t *buf, int W, int see_through)
{
    for (int y = 0; y < s_h; y++)
        for (int x = 0; x < W; x++) {
            if (see_through) { buf[y * W + x] = 0xB4101216u; continue; }
            if (s_back_w) {
                buf[y * W + x] = s_back[((y / BACK_SCALE) % s_back_h) * s_back_w + (x / BACK_SCALE) % s_back_w];
                continue;
            }
            const uint32_t c = tile_px(x & 63, y & 63);
            buf[y * W + x] = rgb((int)((c >> 16) & 255) / 2, (int)((c >> 8) & 255) / 2, (int)(c & 255) / 2);
        }
}

const uint32_t *moa_screen_render_tiles(int win_w, int win_h, const char *title,
                                        const char *line, const struct moa_screen_tile *tile,
                                        int count, const char *footer, int see_through,
                                        int *out_w, int *out_h)
{
    static uint32_t *buf;
    static int bw;
    *out_w = *out_h = 0;
    if (!s_font_ok) return NULL;
    int W = CONTENT_W;
    if (win_w > 0 && win_h > 0) W = (int)((int64_t)CANVAS_H * win_w / win_h);
    if (W < CONTENT_W) W = CONTENT_W;
    if (W > MAX_W) W = MAX_W;
    W &= ~1;
    if (W != bw || !buf) {
        free(buf);
        buf = (uint32_t *)malloc((size_t)W * CANVAS_H * 4u);
        bw = buf ? W : 0;
        if (!buf) return NULL;
    }
    /* fill()/text() draw at s_w x s_h: borrow them, then let the AI Battle
     * screen rebuild its own picture next time. */
    const int keep_w = s_w, keep_h = s_h;
    s_w = W; s_h = CANVAS_H;
    const int cx = W / 2;
    backdrop(buf, W, see_through);
    const uint32_t grey = rgb(150, 156, 168), white = rgb(235, 238, 242);
    const uint32_t gold = rgb(255, 200, 80), blue = rgb(90, 170, 255);
    if (title) text(buf, NULL, title, cx, 56, 3, white);
    fill(buf, cx - 300, 84, cx + 300, 86, rgb(70, 74, 82));
    if (line) text(buf, NULL, line, cx, 110, 2, grey);
    if (count > 8) count = 8;
    const int tw = 210, th = 230, gap = 16;
    for (int i = 0; i < count; i++) {
        const int row = i / 4, col = i % 4;
        const int in_row = count - row * 4 < 4 ? count - row * 4 : 4;
        const int x0 = cx - (in_row * tw + (in_row - 1) * gap) / 2 + col * (tw + gap);
        const int y0 = 150 + row * (th + gap);
        fill(buf, x0, y0, x0 + tw, y0 + th, rgb(0, 0, 0));
        frame_rect(buf, x0, y0, x0 + tw, y0 + th, 2, blue);
        const struct moa_screen_tile *t = &tile[i];
        if (t->category) text(buf, NULL, t->category, x0 + tw / 2, y0 + 18, 1, blue);
        if (t->thumb) {
            const int px = x0 + (tw - 128) / 2, py = y0 + 34;
            for (int y = 0; y < 128; y++)
                for (int x = 0; x < 128; x++) {
                    const uint32_t p = t->thumb[y * 128 + x];
                    if (p >> 24) buf[(py + y) * W + px + x] = 0xFF000000u | p;
                }
        } else {
            fill(buf, x0 + 40, y0 + 60, x0 + tw - 40, y0 + 140, rgb(18, 20, 24));
            text(buf, NULL, "?", x0 + tw / 2, y0 + 100, 3, rgb(60, 64, 72));
        }
        if (t->name) text(buf, NULL, t->name, x0 + tw / 2, y0 + 182, 1, white);
        if (t->label) text(buf, NULL, t->label, x0 + tw / 2, y0 + 208, 2, gold);
    }
    if (footer) text(buf, NULL, footer, cx, CANVAS_H - 40, 2, grey);
    s_w = keep_w; s_h = keep_h;
    s_dirty = 1;
    *out_w = W; *out_h = CANVAS_H;
    return buf;
}

/* ---- choice boxes (moa_screen_render_choice) -------------------------------- */
const uint32_t *moa_screen_render_choice(int win_w, int win_h, const char *title,
                                         const char *line, const char *const *label,
                                         int count, int cursor, const char *footer,
                                         int see_through, int *out_w, int *out_h)
{
    static uint32_t *buf;
    static int bw;
    *out_w = *out_h = 0;
    if (!s_font_ok) return NULL;
    int W = CONTENT_W;
    if (win_w > 0 && win_h > 0) W = (int)((int64_t)CANVAS_H * win_w / win_h);
    if (W < CONTENT_W) W = CONTENT_W;
    if (W > MAX_W) W = MAX_W;
    W &= ~1;
    if (W != bw || !buf) {
        free(buf);
        buf = (uint32_t *)malloc((size_t)W * CANVAS_H * 4u);
        bw = buf ? W : 0;
        if (!buf) return NULL;
    }
    const int keep_w = s_w, keep_h = s_h;            /* borrow fill()/text(), as the tiles do */
    s_w = W; s_h = CANVAS_H;
    const int cx = W / 2;
    backdrop(buf, W, see_through);
    const uint32_t grey = rgb(170, 176, 188), white = rgb(235, 238, 242);
    if (title) text(buf, NULL, title, cx, 56, 3, white);
    fill(buf, cx - 300, 84, cx + 300, 86, rgb(70, 74, 82));
    if (line) text(buf, NULL, line, cx, 110, 2, grey);
    if (count > 4) count = 4;
    /* One width for every box: the longest label at scale 3, plus margins. */
    const int scale = 3;
    int longest = 8;
    for (int i = 0; i < count; i++) {
        const int n = label[i] ? (int)strlen(label[i]) : 0;
        if (n > longest) longest = n > 16 ? 16 : n;
    }
    const int bwid = longest * FONT_W * scale + 2 * 40, bh = 76, gap = 22;
    const int total = count * bh + (count - 1) * gap;
    int y0 = 190 + (CANVAS_H - 190 - 90 - total) / 2;
    for (int i = 0; i < count; i++, y0 += bh + gap) {
        const int x0 = cx - bwid / 2, x1 = cx + bwid / 2, y1 = y0 + bh;
        const int on = i == cursor;
        fill(buf, x0, y0, x1, y1, on ? rgb(22, 40, 70) : rgb(8, 10, 16));
        frame_rect(buf, x0, y0, x1, y1, 2, on ? rgb(130, 200, 255) : rgb(60, 90, 130));
        frame_rect(buf, x0 + 5, y0 + 5, x1 - 5, y1 - 5, 1, on ? rgb(70, 120, 180) : rgb(28, 40, 58));
        if (on) {                                      /* the cursor: a marker each side */
            for (int k = 0; k < 9; k++) {
                fill(buf, x0 - 26 + k, y0 + bh / 2 - (9 - k), x0 - 25 + k, y0 + bh / 2 + (9 - k), rgb(255, 200, 80));
                fill(buf, x1 + 25 - k, y0 + bh / 2 - (9 - k), x1 + 26 - k, y0 + bh / 2 + (9 - k), rgb(255, 200, 80));
            }
        }
        char s[17];
        snprintf(s, sizeof s, "%s", label[i] ? label[i] : "");
        text(buf, NULL, s, cx, y0 + bh / 2, scale, on ? white : rgb(120, 150, 185));
    }
    if (footer) text(buf, NULL, footer, cx, CANVAS_H - 40, 2, grey);
    s_w = keep_w; s_h = keep_h;
    s_dirty = 1;
    *out_w = W; *out_h = CANVAS_H;
    return buf;
}

/* ---- list rows (moa_screen_render_list) ------------------------------------- */
static void text_left(uint32_t *buf, const char *s, int x0, int cy, int scale, uint32_t c)
{
    text(buf, NULL, s, x0 + text_width(s, scale) / 2, cy, scale, c);
}

const uint32_t *moa_screen_render_list(int win_w, int win_h, const struct moa_screen_list *l,
                                       int see_through, int *out_w, int *out_h)
{
    static uint32_t *buf;
    static int bw;
    *out_w = *out_h = 0;
    if (!s_font_ok || !l) return NULL;
    int W = CONTENT_W;
    if (win_w > 0 && win_h > 0) W = (int)((int64_t)CANVAS_H * win_w / win_h);
    if (W < CONTENT_W) W = CONTENT_W;
    if (W > MAX_W) W = MAX_W;
    W &= ~1;
    if (W != bw || !buf) {
        free(buf);
        buf = (uint32_t *)malloc((size_t)W * CANVAS_H * 4u);
        bw = buf ? W : 0;
        if (!buf) return NULL;
    }
    const int keep_w = s_w, keep_h = s_h;            /* borrow fill()/text(), as the tiles do */
    s_w = W; s_h = CANVAS_H;
    const int cx = W / 2;
    backdrop(buf, W, see_through);
    const uint32_t grey = rgb(170, 176, 188), white = rgb(235, 238, 242), gold = rgb(255, 200, 80);
    if (l->title) text(buf, NULL, l->title, cx, 56, 3, white);
    fill(buf, cx - 300, 84, cx + 300, 86, rgb(70, 74, 82));
    int y = 116, lines = 0;
    for (int i = 0; i < 3; i++)
        if (l->info[i] && l->info[i][0]) { text(buf, NULL, l->info[i], cx, y, 2, i == 0 ? gold : grey); y += 30; lines++; }
    int vis = l->visible > 0 ? l->visible : 8;
    if (vis > 8) vis = 8;
    if (vis > l->count) vis = l->count;
    int top = l->top;
    if (top > l->count - vis) top = l->count - vis;
    if (top < 0) top = 0;
    const int rh = 50, gap = 8, rw = 640;
    const int area_y0 = lines ? y + 22 : 150, area_y1 = CANVAS_H - 80;
    const int total = vis * rh + (vis > 0 ? (vis - 1) * gap : 0);
    int y0 = area_y0 + (area_y1 - area_y0 - total) / 2;
    if (y0 < area_y0) y0 = area_y0;
    const int x0 = cx - rw / 2, x1 = cx + rw / 2;
    if (top > 0)                                     /* more above */
        for (int k = 0; k < 10; k++) fill(buf, cx - k, y0 - 22 + k, cx + k + 1, y0 - 21 + k, grey);
    for (int i = 0; i < vis; i++) {
        const int r = top + i, ry = y0 + i * (rh + gap), on = r == l->cursor;
        fill(buf, x0, ry, x1, ry + rh, on ? rgb(22, 40, 70) : rgb(8, 10, 16));
        frame_rect(buf, x0, ry, x1, ry + rh, 2, on ? rgb(130, 200, 255) : rgb(60, 90, 130));
        if (on)
            for (int k = 0; k < 9; k++)
                fill(buf, x0 - 26 + k, ry + rh / 2 - (9 - k), x0 - 25 + k, ry + rh / 2 + (9 - k), gold);
        if (r == l->marked) fill(buf, x0 + 14, ry + rh / 2 - 5, x0 + 24, ry + rh / 2 + 5, gold);
        char s[27];
        snprintf(s, sizeof s, "%s", l->row && l->row[r] ? l->row[r] : "");
        text_left(buf, s, x0 + 36, ry + rh / 2, 2, on ? white : rgb(120, 150, 185));
        if (l->value && l->value[r] && l->value[r][0]) {
            char v[27];
            snprintf(v, sizeof v, "%s", l->value[r]);
            text_left(buf, v, x1 - 24 - text_width(v, 2), ry + rh / 2, 2, gold);
        }
    }
    if (top + vis < l->count) {                      /* more below */
        const int yb = y0 + total + 12;
        for (int k = 0; k < 10; k++) fill(buf, cx - (9 - k), yb + k, cx + (9 - k) + 1, yb + k + 1, grey);
    }
    if (l->footer) text(buf, NULL, l->footer, cx, CANVAS_H - 40, 2, grey);
    s_w = keep_w; s_h = keep_h;
    s_dirty = 1;
    *out_w = W; *out_h = CANVAS_H;
    return buf;
}

/* ---- prize screen (moa_screen_render_prize) --------------------------------- */
static uint32_t dim_px(uint32_t p, int num, int den)
{
    const uint32_t r = ((p >> 16) & 255u) * (uint32_t)num / (uint32_t)den;
    const uint32_t g = ((p >> 8) & 255u) * (uint32_t)num / (uint32_t)den;
    const uint32_t b = (p & 255u) * (uint32_t)num / (uint32_t)den;
    return (p & 0xFF000000u) | r << 16 | g << 8 | b;
}

static void cursor_marks(uint32_t *buf, int x0, int x1, int ymid, uint32_t c)
{
    for (int k = 0; k < 9; k++) {
        fill(buf, x0 - 26 + k, ymid - (9 - k), x0 - 25 + k, ymid + (9 - k), c);
        fill(buf, x1 + 25 - k, ymid - (9 - k), x1 + 26 - k, ymid + (9 - k), c);
    }
}

const uint32_t *moa_screen_render_prize(int win_w, int win_h, const struct moa_screen_prize *p,
                                        int see_through, int *out_w, int *out_h)
{
    static uint32_t *buf;
    static int bw;
    *out_w = *out_h = 0;
    if (!s_font_ok || !p) return NULL;
    int W = CONTENT_W;
    if (win_w > 0 && win_h > 0) W = (int)((int64_t)CANVAS_H * win_w / win_h);
    if (W < CONTENT_W) W = CONTENT_W;
    if (W > MAX_W) W = MAX_W;
    W &= ~1;
    if (W != bw || !buf) {
        free(buf);
        buf = (uint32_t *)malloc((size_t)W * CANVAS_H * 4u);
        bw = buf ? W : 0;
        if (!buf) return NULL;
    }
    const int keep_w = s_w, keep_h = s_h;            /* borrow fill()/text(), as the tiles do */
    s_w = W; s_h = CANVAS_H;
    const int cx = W / 2;
    backdrop(buf, W, see_through);
    const uint32_t grey = rgb(170, 176, 188), white = rgb(235, 238, 242), gold = rgb(255, 200, 80);
    const uint32_t red = rgb(220, 80, 70);
    if (p->title) text(buf, NULL, p->title, cx, 56, 3, white);
    fill(buf, cx - 300, 84, cx + 300, 86, rgb(70, 74, 82));
    if (p->line) text(buf, NULL, p->line, cx, 110, 2, gold);
    int count = p->count > 4 ? 4 : p->count;
    const int rw = 760, rh = 136, ph = 60, gap = 10;
    int total = 0;
    for (int i = 0; i < count; i++) total += (p->row[i].tag ? rh : ph) + (i ? gap : 0);
    const int area_y0 = 138, area_y1 = CANVAS_H - 76;
    int y0 = area_y0 + (area_y1 - area_y0 - total) / 2;
    if (y0 < area_y0) y0 = area_y0;
    const int x0 = cx - rw / 2, x1 = cx + rw / 2;
    for (int i = 0; i < count; i++) {
        const struct moa_screen_prize_row *r = &p->row[i];
        const int offer = r->tag != NULL, h = offer ? rh : ph, on = i == p->cursor;
        const int y1 = y0 + h;
        fill(buf, x0, y0, x1, y1, on ? rgb(22, 40, 70) : rgb(8, 10, 16));
        frame_rect(buf, x0, y0, x1, y1, 2, on ? rgb(130, 200, 255) : rgb(60, 90, 130));
        if (on && !p->ask_n) cursor_marks(buf, x0, x1, y0 + h / 2, gold);
        const uint32_t name_c = r->locked ? rgb(90, 100, 116) : on ? white : rgb(150, 175, 205);
        char s[40];
        if (offer) {
            const int tx = x0 + 6, ty = y0 + (h - 128) / 2;
            fill(buf, tx, ty, tx + 128, ty + 128, rgb(4, 5, 8));
            if (r->thumb) {
                for (int y = 0; y < 128; y++)
                    for (int x = 0; x < 128; x++) {
                        const uint32_t q = r->thumb[y * 128 + x];
                        if (q >> 24) buf[(ty + y) * W + tx + x] = r->locked ? dim_px(0xFF000000u | q, 2, 5) : 0xFF000000u | q;
                    }
            } else {
                text(buf, NULL, "?", tx + 64, ty + 64, 3, rgb(60, 64, 72));
            }
            const int lx = x0 + 156;
            if (r->category) { snprintf(s, sizeof s, "%s", r->category); text_left(buf, s, lx, y0 + 30, 1, r->locked ? rgb(80, 88, 100) : grey); }
            if (r->name) { snprintf(s, sizeof s, "%.18s", r->name); text_left(buf, s, lx, y0 + 66, 3, name_c); }
            const uint32_t tag_c = r->locked ? rgb(80, 88, 100)
                                 : strcmp(r->tag, "EXPENSIVE") == 0 ? gold
                                 : strcmp(r->tag, "MID") == 0 ? white : rgb(140, 190, 255);
            snprintf(s, sizeof s, "%s", r->tag);
            text_left(buf, s, lx, y0 + 104, 2, tag_c);
            if (r->cost) {
                snprintf(s, sizeof s, "%s", r->cost);
                text_left(buf, s, x1 - 28 - text_width(s, 4), y0 + (r->locked ? 56 : h / 2), 4, r->locked ? red : gold);
                if (r->locked) text_left(buf, "NOT ENOUGH P", x1 - 28 - text_width("NOT ENOUGH P", 1), y0 + 96, 1, red);
            }
        } else {
            snprintf(s, sizeof s, "%s", r->name ? r->name : "PASS");
            text_left(buf, s, x0 + 36, y0 + h / 2, 3, name_c);
            if (r->cost) {
                snprintf(s, sizeof s, "%s", r->cost);
                text_left(buf, s, x1 - 28 - text_width(s, 3), y0 + h / 2, 3, gold);
            }
        }
        y0 = y1 + gap;
    }
    if (p->ask_n > 0) {                               /* the question box over the rows */
        for (int y = 128; y < CANVAS_H - 70; y++)
            for (int x = 0; x < W; x++) buf[y * W + x] = dim_px(buf[y * W + x], 2, 5);
        const int n = p->ask_n > 4 ? 4 : p->ask_n;
        const int bwid = 600, oh = 54, og = 10;
        const int bh = 120 + n * oh + (n - 1) * og + 26;
        const int bx0 = cx - bwid / 2, bx1 = cx + bwid / 2;
        const int by0 = 138 + (CANVAS_H - 76 - 138 - bh) / 2, by1 = by0 + bh;
        fill(buf, bx0, by0, bx1, by1, rgb(10, 13, 20));
        frame_rect(buf, bx0, by0, bx1, by1, 2, rgb(130, 200, 255));
        frame_rect(buf, bx0 + 5, by0 + 5, bx1 - 5, by1 - 5, 1, rgb(40, 60, 90));
        if (p->ask_title) text(buf, NULL, p->ask_title, cx, by0 + 40, 2, white);
        if (p->ask_line) text(buf, NULL, p->ask_line, cx, by0 + 80, 1, grey);
        int oy = by0 + 112;
        const int ox0 = cx - 230, ox1 = cx + 230;
        for (int i = 0; i < n; i++, oy += oh + og) {
            const int on = i == p->ask_cursor;
            fill(buf, ox0, oy, ox1, oy + oh, on ? rgb(22, 40, 70) : rgb(8, 10, 16));
            frame_rect(buf, ox0, oy, ox1, oy + oh, 2, on ? rgb(130, 200, 255) : rgb(60, 90, 130));
            if (on) cursor_marks(buf, ox0, ox1, oy + oh / 2, gold);
            char s[27];
            snprintf(s, sizeof s, "%s", p->ask[i] ? p->ask[i] : "");
            text(buf, NULL, s, cx, oy + oh / 2, 2, on ? white : rgb(120, 150, 185));
        }
    }
    if (p->footer) text(buf, NULL, p->footer, cx, CANVAS_H - 40, 2, grey);
    s_w = keep_w; s_h = keep_h;
    s_dirty = 1;
    *out_w = W; *out_h = CANVAS_H;
    return buf;
}

/* ---- victory screen (moa_screen_render_victory) ---------------------------- */
/* Fighting-game style: solid black bars, a band in the belt's colour with the
 * belt pattern in it, the beaten AC on its back on the right (dark, smoking),
 * the winner large on the left rising out of the band, the bottom bar in front
 * of both, the lines on black. Animated by v->t (seconds): the beaten AC
 * slides in from the left (0-0.40 s), the winner from the right
 * (0.30-0.72 s), YOU WIN stamps in with a white flash at 0.72 s (the chime's
 * big chord), the lines are typed once the chime is over (VIC_TYPE). The
 * layers are built once per `key`. */
#define VIC_DOWN    0.40f     /* the beaten AC has landed */
#define VIC_WIN_AT  0.30f     /* the winner starts sliding in */
#define VIC_WIN_LEN 0.42f
#define VIC_HIT     0.72f     /* YOU WIN + flash (the chime's hit) */
#define VIC_POP     0.16f     /* YOU WIN big before it settles */
#define VIC_FLASH   0.48f
#define VIC_TYPE    1.95f     /* typing starts: the chime (1.85 s) is over */
#define VIC_WORD    0.18f     /* a word every */
static uint32_t blend_px(uint32_t d, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    const uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    return 0xFF000000u | ((dr * (255 - a) + r * a) / 255) << 16 |
           ((dg * (255 - a) + g * a) / 255) << 8 | ((db * (255 - a) + b * a) / 255);
}

static int alpha_box(const uint8_t *p, int w, int h, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = w; *y0 = h; *x1 = -1; *y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (p[(y * w + x) * 4 + 3] > 8) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
    return *x1 >= *x0;
}

/* A sprite (RGBA) from a crop of the source, scaled (ang = 0) or scaled and
 * turned about the crop's centre (nearest pixel). */
static uint8_t *sprite_make(const uint8_t *src, int sw, int sh, int bx0, int by0, int bx1, int by1,
                            float scale, float ang, int *ow, int *oh)
{
    const float c = cosf(ang), s = sinf(ang);
    const float hw = (bx1 - bx0 + 1) * 0.5f * scale, hh = (by1 - by0 + 1) * 0.5f * scale;
    const int w = (int)(fabsf(hw * c) + fabsf(hh * s)) * 2 + 2, h = (int)(fabsf(hw * s) + fabsf(hh * c)) * 2 + 2;
    uint8_t *o = (uint8_t *)calloc((size_t)w * h, 4);
    if (!o) return NULL;
    const float scx = (bx0 + bx1 + 1) * 0.5f, scy = (by0 + by1 + 1) * 0.5f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float dx = x - w * 0.5f, dy = y - h * 0.5f;
            const float ux = (dx * c + dy * s) / scale, uy = (-dx * s + dy * c) / scale;
            const int sx = (int)floorf(scx + ux), sy = (int)floorf(scy + uy);
            if (sx < bx0 || sy < by0 || sx > bx1 || sy > by1 || sx >= sw || sy >= sh) continue;
            memcpy(o + (y * w + x) * 4, src + (sy * sw + sx) * 4, 4);
        }
    *ow = w; *oh = h;
    return o;
}

/* A black edge r pixels wide around the sprite's shape: 0xAARRGGBB result. */
static uint32_t *sprite_outline(const uint8_t *sp, int w, int h, int r)
{
    uint8_t *t = (uint8_t *)calloc((size_t)w * h, 1);
    uint32_t *o = (uint32_t *)calloc((size_t)w * h, 4);
    if (!t || !o) { free(t); free(o); return NULL; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t v = 0;
            for (int k = -r; k <= r && !v; k++) {
                const int X = x + k;
                if (X >= 0 && X < w && sp[(y * w + X) * 4 + 3] > 8) v = 1;
            }
            t[y * w + x] = v;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *q = sp + (y * w + x) * 4;
            if (q[3] > 8) { o[y * w + x] = (uint32_t)q[3] << 24 | (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2]; continue; }
            for (int k = -r; k <= r; k++) {
                const int Y = y + k;
                if (Y >= 0 && Y < h && t[Y * w + x]) { o[y * w + x] = 0xFF000000u; break; }
            }
        }
    free(t);
    return o;
}

static void blit(uint32_t *buf, int W, int H, const uint32_t *sp, int w, int h, int px, int py)
{
    for (int y = 0; y < h; y++) {
        const int Y = py + y;
        if (Y < 0 || Y >= H) continue;
        for (int x = 0; x < w; x++) {
            const int X = px + x;
            if (X < 0 || X >= W) continue;
            const uint32_t q = sp[y * w + x], a = q >> 24;
            if (a > 8) buf[Y * W + X] = blend_px(buf[Y * W + X], (q >> 16) & 255, (q >> 8) & 255, q & 255, a);
        }
    }
}

static void soft_disc(uint32_t *buf, int W, int H, int cx, int cy, int r, uint32_t c, int a, int y_clip)
{
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= H || y >= y_clip) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= W) continue;
            const float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy))) / (float)r;
            if (d >= 1.0f) continue;
            const int al = (int)(a * (1.0f - d) * (1.0f - d));
            buf[y * W + x] = blend_px(buf[y * W + x], (c >> 16) & 255, (c >> 8) & 255, c & 255, (uint32_t)al);
        }
    }
}

static void text_edged(uint32_t *buf, const char *s, int x0, int cy, int scale, uint32_t c)
{
    const int cx = x0 + text_width(s, scale) / 2;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) text(buf, NULL, s, cx + dx * scale, cy + dy * scale, scale, rgb(0, 0, 0));
    text(buf, NULL, s, cx, cy, scale, c);
}

static float ease_out(float u) { if (u <= 0.0f) return 0.0f; if (u >= 1.0f) return 1.0f; u = 1.0f - u; return 1.0f - u * u * u; }

int moa_screen_victory_words(const struct moa_screen_victory *v);

const uint32_t *moa_screen_render_victory(int win_w, int win_h, const struct moa_screen_victory *v,
                                          int *out_w, int *out_h)
{
    static uint32_t *bg, *bg_landed, *buf, *spr_down, *spr_win;
    static int bw, dw, dh, dpx, dpy, ww_, wh_, wpx, wpy, sx[8], sy[8], nsparks;
    static int smx[14], smy[14], nsmoke;
    static unsigned key_done = 0xFFFFFFFFu;
    *out_w = *out_h = 0;
    if (!s_font_ok || !v) return NULL;
    int W = CONTENT_W;
    if (win_w > 0 && win_h > 0) W = (int)((int64_t)CANVAS_H * win_w / win_h);
    if (W < CONTENT_W) W = CONTENT_W;
    if (W > MAX_W) W = MAX_W;
    W &= ~1;
    const int H = CANVAS_H;
    if (W != bw || !bg || !bg_landed || !buf) {
        free(bg); free(bg_landed); free(buf);
        bg = (uint32_t *)malloc((size_t)W * H * 4u);
        bg_landed = (uint32_t *)malloc((size_t)W * H * 4u);
        buf = (uint32_t *)malloc((size_t)W * H * 4u);
        bw = bg && bg_landed && buf ? W : 0;
        key_done = 0xFFFFFFFFu;
        if (!bw) return NULL;
    }
    const int keep_w = s_w, keep_h = s_h;
    s_w = W; s_h = H;
    const int by0 = 128, by1 = 452;
    const int br = (int)((v->band >> 16) & 255), bgc = (int)((v->band >> 8) & 255), bb = (int)(v->band & 255);
    if (key_done != v->key) {
        /* the background: black, the band with the belt pattern pressed in */
        for (int i = 0; i < W * H; i++) bg[i] = 0xFF000000u;
        for (int y = by0; y < by1; y++)
            for (int x = 0; x < W; x++) {
                float k = 1.0f;
                if (s_back_w) {
                    const uint32_t c = s_back[((y / BACK_SCALE) % s_back_h) * s_back_w + (x / BACK_SCALE) % s_back_w];
                    int m = (int)((c >> 16) & 255);
                    if ((int)((c >> 8) & 255) > m) m = (int)((c >> 8) & 255);
                    if ((int)(c & 255) > m) m = (int)(c & 255);
                    k = 1.0f - 0.22f * (m > 40 ? 1.0f : (float)m / 40.0f);
                }
                bg[y * W + x] = rgb((int)(br * k), (int)(bgc * k), (int)(bb * k));
            }
        fill(bg, 0, by0 - 6, W, by0 - 3, rgb(br, bgc, bb));
        memcpy(bg_landed, bg, (size_t)W * H * 4u);
        free(spr_down); free(spr_win);
        spr_down = spr_win = NULL;
        nsparks = nsmoke = 0;
        int x0, y0, x1, y1;
        /* the beaten AC: on its back, dark; its shadow and smoke once landed */
        if (v->loser && alpha_box(v->loser, v->loser_w, v->loser_h, &x0, &y0, &x1, &y1)) {
            int w = 0, h = 0;
            uint8_t *sp = sprite_make(v->loser, v->loser_w, v->loser_h, x0, y0, x1, y1,
                                      400.0f / (float)(y1 - y0 + 1), 1.675f, &w, &h);
            int ax0, ay0, ax1, ay1;
            if (sp && alpha_box(sp, w, h, &ax0, &ay0, &ax1, &ay1)) {
                for (int i = 0; i < w * h; i++) {
                    uint8_t *q = sp + i * 4;
                    q[0] = (uint8_t)(q[0] * 0.41f + 10.0f);
                    q[1] = (uint8_t)(q[1] * 0.41f + 7.5f);
                    q[2] = (uint8_t)(q[2] * 0.41f + 7.5f);
                }
                spr_down = sprite_outline(sp, w, h, 3);
                dw = w; dh = h;
                dpx = W - 40 - ax1; dpy = by1 + 22 - ay1;
                for (int k = 0; k < 6; k++)
                    soft_disc(bg_landed, W, H, dpx + ax0 + (ax1 - ax0) * k / 5, by1 - 12, 50, 0x3C1E00u, 70, by1);
                uint32_t r = 0x2545F491u ^ v->key;
                for (int i = 0; i < 14; i++) {                  /* smoke: drawn per frame, rising */
                    r = r * 1664525u + 1013904223u;
                    smx[i] = dpx + ax0 + 60 + (int)((r >> 16) % 260u);
                    r = r * 1664525u + 1013904223u;
                    smy[i] = dpy + ay0 + 20 + (int)((r >> 16) % 32u) - 16;
                }
                nsmoke = 14;
                for (int i = 0; i < 8; i++) {
                    r = r * 1664525u + 1013904223u;
                    sx[i] = dpx + ax0 + 60 + (int)((r >> 16) % (unsigned)((ax1 - ax0) > 120 ? (ax1 - ax0) - 120 : 1));
                    r = r * 1664525u + 1013904223u;
                    sy[i] = dpy + ay0 + 10 + (int)((r >> 16) % (unsigned)((ay1 - ay0) > 30 ? (ay1 - ay0) - 30 : 1));
                }
                nsparks = 8;
            }
            free(sp);
        }
        /* the winner: from the head down, rising out of the band */
        if (v->winner && alpha_box(v->winner, v->winner_w, v->winner_h, &x0, &y0, &x1, &y1)) {
            const int ah = y1 - y0 + 1;
            const int cy0 = y0 + ah / 10, cy1 = y0 + ah * 86 / 100;
            int w = 0, h = 0;
            uint8_t *sp = sprite_make(v->winner, v->winner_w, v->winner_h, x0 - 40 < 0 ? 0 : x0 - 40, cy0,
                                      x1 + 40 >= v->winner_w ? v->winner_w - 1 : x1 + 40, cy1,
                                      640.0f / (float)(cy1 - cy0), 0.0f, &w, &h);
            if (sp) {
                spr_win = sprite_outline(sp, w, h, 5);
                ww_ = w; wh_ = h;
                wpx = W * 60 / 1280; if (wpx < 30) wpx = 30;
                wpy = 20;
            }
            free(sp);
        }
        key_done = v->key;
    }
    const float t = v->t;
    memcpy(buf, t >= VIC_DOWN ? bg_landed : bg, (size_t)W * H * 4u);
    if (spr_down && t >= VIC_DOWN) {                       /* smoke rising and thinning, looped */
        const float k = t < VIC_DOWN + 0.8f ? (t - VIC_DOWN) / 0.8f : 1.0f;
        for (int i = 0; i < nsmoke; i++) {
            const float u = fmodf(t * 0.35f + (float)i / (float)nsmoke, 1.0f);   /* 0 = at the AC, 1 = gone */
            const int r = 22 + (int)(u * 42.0f);
            soft_disc(buf, W, H, smx[i] + (int)(u * 30.0f), smy[i] - (int)(u * 150.0f), r, 0x3C3A38u,
                      (int)(150.0f * k * (1.0f - u)), by1);
        }
    }
    if (spr_down) {
        const int x = (int)(-dw + (dpx + dw) * ease_out(t / VIC_DOWN));
        blit(buf, W, H, spr_down, dw, dh, x, dpy);
        if (t >= VIC_DOWN) {                               /* sparks, twinkling */
            const int f = (int)(t * 25.0f);
            for (int i = 0; i < nsparks; i++) {
                if ((f + i * 3) % 7 >= 3) continue;
                soft_disc(buf, W, H, sx[i], sy[i], 9, 0xFFC860u, 160, by1);
                fill(buf, sx[i] - 6, sy[i], sx[i] + 7, sy[i] + 2, rgb(255, 240, 180));
                fill(buf, sx[i], sy[i] - 6, sx[i] + 2, sy[i] + 7, rgb(255, 240, 180));
            }
        }
    }
    if (spr_win && t >= VIC_WIN_AT) {
        const int x = (int)(W + (wpx - W) * ease_out((t - VIC_WIN_AT) / VIC_WIN_LEN));
        blit(buf, W, H, spr_win, ww_, wh_, x, wpy);
    }
    /* the bottom bar is in front of both */
    fill(buf, 0, by1 + 6, W, H, rgb(0, 0, 0));
    fill(buf, 0, by1 + 2, W, by1 + 5, rgb(br, bgc, bb));
    const uint32_t white = rgb(240, 240, 240), gold = rgb(255, 200, 72), grey = rgb(170, 176, 190);
    text_left(buf, "ROGUE ARENA", 36, 42, 2, grey);
    if (t >= VIC_HIT) {
        const int sc = t < VIC_HIT + VIC_POP ? 8 : 6;
        text_edged(buf, "YOU WIN", W * 600 / 1280 - (sc - 6) * 30, 196, sc, white);
        if (v->rank) text_left(buf, v->rank, W - 36 - text_width(v->rank, 4), 52, 4, white);
        if (v->pay) text_left(buf, v->pay, W - 36 - text_width(v->pay, 3), 100, 3, gold);
    }
    if (t >= VIC_HIT + VIC_FLASH && v->beaten) text_left(buf, v->beaten, W - 40 - text_width(v->beaten, 2), by1 + 32, 2, grey);
    {   /* the lines, a word at a time once the flash is over */
        int left = moa_screen_victory_words(v);
        for (int i = 0; i < 2 && left > 0; i++) {
            if (!v->line[i]) continue;
            char part[64];
            int k = 0, words = 0;
            const char *q = v->line[i];
            while (q[k] && k < 63) {
                if (q[k] != ' ' && (k == 0 || q[k - 1] == ' ') && ++words > left) break;
                part[k] = q[k];
                k++;
            }
            while (k > 0 && part[k - 1] == ' ') k--;
            part[k] = 0;
            left -= words > left ? left : words;
            const int sc = text_width(v->line[i], 3) <= W - 140 ? 3 : 2;
            text_left(buf, part, 100, 546 + i * 56, sc, white);
        }
    }
    if (t >= VIC_HIT + VIC_FLASH && v->status) text_left(buf, v->status, W - 36 - text_width(v->status, 2), H - 30, 2, grey);
    if (t >= VIC_HIT && t < VIC_HIT + VIC_FLASH) {         /* the flash */
        const uint32_t a = (uint32_t)(230.0f * (1.0f - (t - VIC_HIT) / VIC_FLASH));
        for (int i = 0; i < W * H; i++) buf[i] = blend_px(buf[i], 255, 255, 255, a);
    }
    s_w = keep_w; s_h = keep_h;
    s_dirty = 1;
    *out_w = W; *out_h = H;
    return buf;
}

static int victory_word_total(const struct moa_screen_victory *v)
{
    int total = 0;
    for (int i = 0; i < 2; i++)
        for (const char *q = v->line[i]; q && *q; q++)
            if (*q != ' ' && (q == v->line[i] || q[-1] == ' ')) total++;
    return total;
}

/* Words of the two lines shown at v->t: none before VIC_TYPE, then one every
 * VIC_WORD. */
int moa_screen_victory_words(const struct moa_screen_victory *v)
{
    if (!v || v->t < VIC_TYPE) return 0;
    const int total = victory_word_total(v);
    const int n = 1 + (int)((v->t - VIC_TYPE) / VIC_WORD);
    return n < total ? n : total;
}

/* 1 = every word is out (the prompt may show). */
int moa_screen_victory_typed(const struct moa_screen_victory *v)
{
    return v && moa_screen_victory_words(v) >= victory_word_total(v) && v->t >= VIC_TYPE;
}
