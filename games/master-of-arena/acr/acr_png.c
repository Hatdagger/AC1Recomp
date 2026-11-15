#include "acr_png.h"

#include <stdlib.h>
#include <string.h>

/* ---- inflate (RFC 1951), in the style of zlib's "puff" ---------------- */

typedef struct bits {
    const uint8_t *in; size_t in_size, pos;
    uint32_t buf; int count;
    uint8_t *out; size_t out_size, out_pos;
    int error;
} bits;

static int need(bits *s, int n) {
    while (s->count < n) {
        if (s->pos >= s->in_size) { s->error = 1; return 0; }
        s->buf |= (uint32_t)s->in[s->pos++] << s->count;
        s->count += 8;
    }
    return 1;
}

static int getbits(bits *s, int n) {
    if (!need(s, n)) return 0;
    const int v = (int)(s->buf & ((1u << n) - 1));
    s->buf >>= n;
    s->count -= n;
    return v;
}

typedef struct huff { short count[16]; short symbol[320]; } huff;

static int build(huff *h, const short *len, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; ++i) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) { left <<= 1; left -= h->count[l]; if (left < 0) return -1; }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; ++i) if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
    return left;
}

static int decode(bits *s, const huff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        code |= getbits(s, 1);
        if (s->error) return -1;
        const int count = h->count[l];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->error = 1;
    return -1;
}

static const short k_lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short k_lext[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short k_dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short k_dext[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int codes(bits *s, const huff *lc, const huff *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_size) return -1;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            const int len = k_lbase[sym] + getbits(s, k_lext[sym]);
            const int d = decode(s, dc);
            if (d < 0 || d >= 30) return -1;
            const size_t dist = (size_t)(k_dbase[d] + getbits(s, k_dext[d]));
            if (s->error || dist > s->out_pos || s->out_pos + (size_t)len > s->out_size) return -1;
            for (int i = 0; i < len; ++i, ++s->out_pos) s->out[s->out_pos] = s->out[s->out_pos - dist];
        }
    }
}

static int fixed_block(bits *s) {
    static huff lc, dc;
    static int ready;
    if (!ready) {
        short len[288];
        int i = 0;
        for (; i < 144; ++i) len[i] = 8;
        for (; i < 256; ++i) len[i] = 9;
        for (; i < 280; ++i) len[i] = 7;
        for (; i < 288; ++i) len[i] = 8;
        build(&lc, len, 288);
        for (i = 0; i < 30; ++i) len[i] = 5;
        build(&dc, len, 30);
        ready = 1;
    }
    return codes(s, &lc, &dc);
}

static int dynamic_block(bits *s) {
    static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    short len[320];
    const int nlen = getbits(s, 5) + 257, ndist = getbits(s, 5) + 1, ncode = getbits(s, 4) + 4;
    if (s->error || nlen > 286 || ndist > 30) return -1;
    int i;
    for (i = 0; i < ncode; ++i) len[order[i]] = (short)getbits(s, 3);
    for (; i < 19; ++i) len[order[i]] = 0;
    huff lc, dc;
    if (build(&lc, len, 19) != 0) return -1;
    for (i = 0; i < nlen + ndist; ) {
        int sym = decode(s, &lc);
        if (sym < 0) return -1;
        if (sym < 16) { len[i++] = (short)sym; continue; }
        short v = 0;
        int rep;
        if (sym == 16) { if (i == 0) return -1; v = len[i - 1]; rep = 3 + getbits(s, 2); }
        else if (sym == 17) rep = 3 + getbits(s, 3);
        else rep = 11 + getbits(s, 7);
        if (s->error || i + rep > nlen + ndist) return -1;
        while (rep--) len[i++] = v;
    }
    if (len[256] == 0) return -1;
    const int err = build(&lc, len, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -1;
    const int derr = build(&dc, len + nlen, ndist);
    if (derr < 0 || (derr > 0 && ndist - dc.count[0] != 1)) return -1;
    return codes(s, &lc, &dc);
}

long acr_inflate_zlib(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    if (in_size < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20)) return -1;
    bits s;
    memset(&s, 0, sizeof s);
    s.in = in; s.in_size = in_size; s.pos = 2;
    s.out = out; s.out_size = out_size;
    int last;
    do {
        last = getbits(&s, 1);
        const int type = getbits(&s, 2);
        if (s.error) return -1;
        if (type == 0) {
            s.buf = 0; s.count = 0;   /* to a byte boundary */
            if (s.pos + 4 > in_size) return -1;
            const unsigned len = in[s.pos] | in[s.pos + 1] << 8, nlen = in[s.pos + 2] | in[s.pos + 3] << 8;
            s.pos += 4;
            if ((len ^ 0xFFFFu) != nlen || s.pos + len > in_size || s.out_pos + len > out_size) return -1;
            memcpy(out + s.out_pos, in + s.pos, len);
            s.pos += len; s.out_pos += len;
        } else if (type == 1) {
            if (fixed_block(&s)) return -1;
        } else if (type == 2) {
            if (dynamic_block(&s)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return (long)s.out_pos;
}

/* ---- PNG ---------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

void acr_image_free(acr_image *img) {
    free(img->pixels);
    img->pixels = NULL;
}

int acr_png_decode(const uint8_t *d, size_t n, acr_image *out) {
    memset(out, 0, sizeof *out);
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0) return 0;
    uint32_t w = 0, h = 0;
    int depth = 0, type = -1, palette_size = 0;
    uint8_t *z = NULL;
    size_t z_size = 0;
    int ok = 1;
    for (int i = 0; i < 256; ++i) { out->palette[i][0] = out->palette[i][1] = out->palette[i][2] = (uint8_t)i; out->palette[i][3] = 255; }
    for (size_t p = 8; p + 12 <= n; ) {
        const uint32_t len = be32(d + p);
        const uint8_t *tag = d + p + 4, *body = d + p + 8;
        if (len > n - p - 12) { ok = 0; break; }
        if (!memcmp(tag, "IHDR", 4) && len >= 13) {
            w = be32(body); h = be32(body + 4); depth = body[8]; type = body[9];
            if (body[10] || body[11] || body[12]) ok = 0;   /* compression, filter, interlace */
        } else if (!memcmp(tag, "PLTE", 4)) {
            palette_size = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < palette_size; ++i) {
                out->palette[i][0] = body[3 * i]; out->palette[i][1] = body[3 * i + 1]; out->palette[i][2] = body[3 * i + 2];
            }
        } else if (!memcmp(tag, "tRNS", 4) && type == 3) {
            for (uint32_t i = 0; i < len && i < 256; ++i) out->palette[i][3] = body[i];
        } else if (!memcmp(tag, "IDAT", 4)) {
            uint8_t *nz = realloc(z, z_size + len + 1);
            if (!nz) { ok = 0; break; }
            z = nz;
            memcpy(z + z_size, body, len);
            z_size += len;
        } else if (!memcmp(tag, "IEND", 4)) {
            break;
        }
        p += 12 + (size_t)len;
    }
    const int channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    if (!ok || !z || depth != 8 || !channels || w == 0 || h == 0 || w > 8192 || h > 8192) { free(z); return 0; }
    const size_t stride = (size_t)w * channels, raw_size = (stride + 1) * h;
    uint8_t *raw = malloc(raw_size);
    if (!raw || acr_inflate_zlib(z, z_size, raw, raw_size) != (long)raw_size) { free(z); free(raw); return 0; }
    free(z);
    /* Undo the row filters in place (each row keeps its filter byte in front). */
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *row = raw + y * (stride + 1) + 1;
        const uint8_t *up = y ? row - (stride + 1) : NULL;
        const int f = row[-1];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= (size_t)channels ? row[x - channels] : 0, b = up ? up[x] : 0,
                      c = (up && x >= (size_t)channels) ? up[x - channels] : 0;
            int v = row[x];
            if (f == 1) v += a;
            else if (f == 2) v += b;
            else if (f == 3) v += (a + b) / 2;
            else if (f == 4) v += paeth(a, b, c);
            else if (f != 0) { free(raw); return 0; }
            row[x] = (uint8_t)v;
        }
    }
    out->w = (int)w; out->h = (int)h;
    out->indexed = type == 3;
    out->pixels = malloc((size_t)w * h * (out->indexed ? 1 : 4));
    if (!out->pixels) { free(raw); return 0; }
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *row = raw + y * (stride + 1) + 1;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t *s = row + (size_t)x * channels;
            if (out->indexed) { out->pixels[(size_t)y * w + x] = s[0]; continue; }
            uint8_t *o = out->pixels + ((size_t)y * w + x) * 4;
            if (channels <= 2) { o[0] = o[1] = o[2] = s[0]; o[3] = channels == 2 ? s[1] : 255; }
            else { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = channels == 4 ? s[3] : 255; }
        }
    }
    free(raw);
    return 1;
}
