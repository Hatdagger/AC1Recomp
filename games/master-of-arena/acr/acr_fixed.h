/* 20.12 fixed point: the precision the PS1 used for gameplay math.
 * Integer-only, so results are identical on every compiler and CPU. */
#ifndef ACR_FIXED_H
#define ACR_FIXED_H

#include <stdint.h>

typedef int32_t acr_fx;

#define ACR_FX_SHIFT 12
#define ACR_FX_ONE   ((acr_fx)1 << ACR_FX_SHIFT)

static inline acr_fx acr_fx_from_int(int32_t v) { return (acr_fx)((uint32_t)v << ACR_FX_SHIFT); }

/* Rounds toward negative infinity, like an arithmetic shift. */
static inline int32_t acr_fx_to_int(acr_fx v) {
    return v >= 0 ? v >> ACR_FX_SHIFT : -(((-(int64_t)v) + ACR_FX_ONE - 1) >> ACR_FX_SHIFT);
}

/* Rounds toward negative infinity, like the PS1's arithmetic shifts; written
 * out so it never depends on how a compiler shifts negative numbers. */
static inline acr_fx acr_fx_mul(acr_fx a, acr_fx b) {
    const int64_t p = (int64_t)a * (int64_t)b;
    return (acr_fx)(p >= 0 ? p >> ACR_FX_SHIFT
                           : -((-p + ACR_FX_ONE - 1) >> ACR_FX_SHIFT));
}

/* Division by zero saturates instead of trapping. */
static inline acr_fx acr_fx_div(acr_fx a, acr_fx b) {
    if (b == 0) return a >= 0 ? INT32_MAX : INT32_MIN;
    return (acr_fx)(((int64_t)a * ACR_FX_ONE) / b);
}

#endif
