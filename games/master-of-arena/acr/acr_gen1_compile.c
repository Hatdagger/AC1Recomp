#include "acr_gen1_compile.h"

/* The game divides with multiply-by-reciprocal tricks; these reproduce the
 * exact instructions so results match for every input, not just typical ones. */
static uint32_t mulhi_u(uint32_t a, uint32_t b) {
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
}
static int32_t mulhi_s(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
}
/* Arithmetic right shift, written out so it never depends on the compiler. */
static int32_t sra(int32_t v, unsigned n) {
    return v >= 0 ? (int32_t)((uint32_t)v >> n) : (int32_t)~(~(uint32_t)v >> n);
}

/* (x << 18) * (2^38 / 1000) >> 50: about x * 64 / 1000 (refill). The shift
 * wraps in 32 bits like the original, which matters when drain > output. */
static uint32_t refill(uint32_t output, uint32_t drain, uint32_t max) {
    const uint32_t x = (uint32_t)(output - drain) << 18;
    uint32_t v = mulhi_u(x, 0x10624DD3u) >> 18;
    if (v == 0) v = 1;
    if (v > max) v = max;
    return v;
}

void acr_gen1_compile(const acr_gen1_build *b, acr_gen1_stats *out) {
    const acr_gen1_part *parts[] = { &b->core, &b->booster, &b->head, &b->arms, &b->blade,
                                     &b->hand, &b->back_l, &b->back_r, &b->fcs, &b->legs };
    /* Carried weight: everything except the generator, capped at what the
     * legs can hold (their own weight + carry limit). */
    uint32_t weight = 0;
    for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; ++i)
        if (parts[i]->present) weight += parts[i]->weight;
    const uint32_t cap = (uint32_t)b->legs.weight + b->legs.carry_limit;
    if (cap < weight) weight = cap;
    out->carried_weight = weight;

    out->energy_capacity = b->generator.stat_2c;
    out->red_zone = b->generator.stat_2e;
    out->blade_power = b->blade.present ? b->blade.stat_2c : 0;

    /* Rise drain: booster's boost drain / 10 (legs' own value without one),
     * at least 1. (x << 12) * 0x66666667 >> 34, then >> 12. */
    {
        const uint32_t src = b->booster.present ? b->booster.stat_2e : b->legs.boost_drain_legs;
        const int32_t x = (int32_t)(src << 12);
        int32_t v = sra(mulhi_s(x, 0x66666667), 2) - sra(x, 31);
        v = sra(v, 12);
        out->rise_drain = (uint16_t)(v > 0 ? v : 1);
    }

    /* Refill per step: generator output minus the frame's drain. Standing
     * uses the legs' standing drain, moving their moving drain. */
    const uint32_t frame_drain = (uint32_t)b->head.energy_drain + b->arms.energy_drain +
                                 b->core.energy_drain + b->fcs.energy_drain;
    out->refill_standing = (uint16_t)refill(b->generator.energy_drain,
                                            frame_drain + b->legs.drain_standing, 384);
    out->refill_moving = (uint16_t)refill(b->generator.energy_drain,
                                          frame_drain + b->legs.energy_drain, 192);

    /* Stability: legs' stability (plus a Balancer's bonus) / 1018 * 4096,
     * times carried weight * 256 / 1000 / 512, / 16384, limited to 32..255.
     * Heavier ACs are steadier. */
    {
        uint32_t stability = b->legs.stability;
        if (b->option_mask & ACR_GEN1_OPTION_BALANCER_RE) stability += 0x640;
        else if (b->option_mask & ACR_GEN1_OPTION_BALANCER) stability += 0x320;
        const int32_t x = (int32_t)(stability << 12);
        const int32_t a = sra((int32_t)((uint32_t)mulhi_s(x, (int32_t)0x80C121B3u) + (uint32_t)x), 9)
                          - sra(x, 31);
        const uint32_t w = mulhi_u(weight << 8, 0x10624DD3u) >> 9;
        int32_t v = sra((int32_t)((uint32_t)a * w), 14);
        if (v < 33) v = 32;
        if (v > 255) v = 255;
        out->stability = (uint8_t)v;
    }

    /* Movement (0x800A8038-0x800A8238). Thrust: booster power (a flat 9800
     * without a booster: the game keys this on caterpillar legs, which carry
     * none) * 4096 / weight, then * 74898 >> 24 (about / 224). */
    {
        const uint32_t power = b->booster.present ? b->booster.stat_2c : 9800u;
        const uint32_t w = weight ? weight : 1;      /* the game never divides by 0 */
        const uint32_t a = (power << 12) / w;
        const uint32_t raw = (a * 74898u) >> 24;   /* 32-bit wrap, as the shifts do */
        uint32_t t = raw ? raw : 1;
        if (t > 64) t = 64;
        out->thrust = (uint8_t)t;

        const int32_t x8 = (int32_t)(raw << 3);
        int32_t c = sra(mulhi_s(x8, 0x66666667), 2) - sra(x8, 31);
        if (c < 1) c = 1;
        if (c > 50) c = 50;
        out->thrust_80 = (uint8_t)c;

        const int32_t x12 = (int32_t)(raw * 12u);
        uint32_t d = ((uint32_t)mulhi_s(x12, 0x66666667) >> 2) & 0xFF;
        if (d == 0) d = 1;
        if (d > 64) d = 64;
        out->thrust_120 = b->legs.fixed_boost ? b->legs.fixed_boost : (uint8_t)d;   /* 0x800AA96C */

        const int32_t e3 = (int32_t)(((uint32_t)b->legs.turn_accel * 3u) << 16);
        const int32_t f = sra(mulhi_s(e3, 0x66666667), 6) - sra(e3, 31);
        uint32_t g = (((uint32_t)f * 3573u) / w) >> 12;
        if ((int32_t)g < 1) g = 1;
        if ((int32_t)g > 64) g = 64;
        out->turn_accel = (uint8_t)g;

        uint32_t h = (((uint32_t)b->legs.turn_speed * 14635008u) / w) >> 12;
        if (h > 64) h = 64;
        out->turn_speed = (uint8_t)h;

        uint32_t fr = mulhi_u(weight << 18, 0x10624DD3u) >> 21;
        if (fr < 32) fr = 32;
        if (fr > 96) fr = 96;
        out->friction = (uint8_t)fr;
    }
}

void acr_gen1_motion_from_stats(const acr_gen1_stats *s, acr_gen1_motion *out) {
    out->rise_accel = (uint16_t)(s->thrust * 2u);
    out->rise_max = (uint16_t)(s->thrust_80 * 200u);
    out->air_accel = (uint16_t)(s->thrust * 14u);
    out->air_max = (uint16_t)(s->thrust_80 * 140u);
    out->boost_accel = (uint16_t)(s->thrust_120 * 14u);
    out->boost_max = (uint16_t)(s->thrust_120 * 140u);
    out->turn_accel = (uint16_t)((s->turn_accel * 5u) >> 4);
    out->turn_max = (uint16_t)((s->turn_speed * 15u) >> 4);
    out->friction = s->friction;
    out->axis_step_max = ACR_GEN1_AXIS_STEP_MAX;
}
