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
                                  int permille, int limit_secs, float fade,
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
    if (limit_secs > 0) {
        char line[32];
        snprintf(line, sizeof line, "TIME LIMIT %d:%02d", limit_secs / 60, limit_secs % 60);
        text(s_frame, NULL, line, cx, 670, 2, rgb(150, 156, 168));
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
