/* Rogue Arena sound finish (moa_sfx.h). */
#include "moa_sfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const struct moa_sfx_room moa_sfx_hall = { 0.28f, 1.0f, 3800.0f, 1.3f, 8500.0f };

void moa_sfx_lowpass(float *x, int n, float hz)
{
    /* RBJ biquad, Q 0.707 */
    if (hz <= 0.0f || hz >= 0.49f * MOA_SFX_RATE) return;
    const float w = 6.2831853f * hz / MOA_SFX_RATE, c = cosf(w), al = sinf(w) / (2.0f * 0.7071f);
    const float a0 = 1.0f + al;
    const float b0 = (1.0f - c) / 2.0f / a0, b1 = (1.0f - c) / a0, b2 = b0;
    const float a1 = -2.0f * c / a0, a2 = (1.0f - al) / a0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; i++) {
        const float y = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x[i]; y2 = y1; y1 = y;
        x[i] = y;
    }
}

/* Comb + all-pass hall (Schroeder / Freeverb style), darkened in the loop. */
static void hall(const float *in, float *out, int n, const struct moa_sfx_room *r)
{
    static const int k_comb[6] = { 1117, 1188, 1277, 1356, 1422, 1491 };
    static const float k_fb[6] = { 0.84f, 0.83f, 0.82f, 0.81f, 0.80f, 0.79f };
    static const int k_ap[4] = { 225, 556, 441, 341 };
    const float d = expf(-6.2831853f * r->damp / MOA_SFX_RATE);    /* one-pole in the loop */
    memset(out, 0, sizeof(float) * (size_t)n);
    for (int c = 0; c < 6; c++) {
        const int len = (int)((float)k_comb[c] / 2.0f * r->size) + 1;     /* tuned at 44.1k */
        float *buf = (float *)calloc((size_t)len, sizeof *buf);
        if (!buf) continue;
        float lp = 0.0f;
        int p = 0;
        for (int i = 0; i < n; i++) {
            const float y = buf[p];
            lp = y * (1.0f - d) + lp * d;
            buf[p] = in[i] + k_fb[c] * lp;
            out[i] += y;
            if (++p == len) p = 0;
        }
        free(buf);
    }
    for (int a = 0; a < 4; a++) {
        const int len = k_ap[a] / 2 + 1;
        float *buf = (float *)calloc((size_t)len, sizeof *buf);
        if (!buf) continue;
        int p = 0;
        for (int i = 0; i < n; i++) {
            const float b = buf[p];
            const float y = -0.5f * out[i] + b;
            buf[p] = out[i] + 0.5f * b;
            out[i] = y;
            if (++p == len) p = 0;
        }
        free(buf);
    }
    for (int i = 0; i < n; i++) out[i] *= 0.10f;
}

float *moa_sfx_finish(const float *in, int n, float tail_s, const struct moa_sfx_room *r, int *n_out)
{
    const int m = n + (int)(tail_s * MOA_SFX_RATE);
    float *x = (float *)calloc((size_t)m, sizeof *x);
    float *w = (float *)calloc((size_t)m, sizeof *w);
    if (!x || !w) { free(x); free(w); return NULL; }
    float top = 0.0f;
    for (int i = 0; i < n; i++) if (fabsf(in[i]) > top) top = fabsf(in[i]);
    const float k = top > 0.0f ? 1.0f / top : 0.0f;
    for (int i = 0; i < n; i++) {
        float v = in[i] * k;
        if (r->drive > 0.0f) v = tanhf(v * r->drive) / tanhf(r->drive);
        x[i] = v;
    }
    moa_sfx_lowpass(x, n, r->top);
    hall(x, w, m, r);
    for (int i = 0; i < m; i++) x[i] += r->wet * w[i];
    /* no click at the very end */
    const int f = m < 400 ? m : 400;
    for (int i = 0; i < f; i++) x[m - 1 - i] *= (float)i / (float)f;
    free(w);
    *n_out = m;
    return x;
}

/* ---- VS count-in ------------------------------------------------------------- */
float *moa_sfx_count_shot(const float *src, int n, int last, int *n_out)
{
    /* The last shot of the count plays a little lower and duller; the
     * others are the game's sound as it is. */
    const float pitch = last ? 0.94f : 1.0f;
    const int m = (int)((float)n / pitch);
    float *x = (float *)calloc((size_t)m + 1u, sizeof *x);
    if (!x) return NULL;
    for (int i = 0; i < m; i++) {                        /* read slower = lower */
        const float p = (float)i * pitch;
        const int k = (int)p;
        const float f = p - (float)k;
        const float a = src[k < n ? k : n - 1], b = src[k + 1 < n ? k + 1 : n - 1];
        x[i] = a + (b - a) * f;
    }
    const int fo = m < 300 ? m : 300;                   /* no click where the sample stops */
    for (int i = 0; i < fo; i++) x[m - 1 - i] *= (float)i / (float)fo;
    if (last) moa_sfx_lowpass(x, m, 7400.0f);
    float *f = moa_sfx_finish(x, m, 1.0f, &moa_sfx_hall, n_out);
    free(x);
    return f;
}
