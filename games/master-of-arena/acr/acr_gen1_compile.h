/* Gen 1 AC compile: parts -> the stats the battle code uses, as Master of
 * Arena computes them (menu module routine 0x800A7BB4; formulas and
 * evidence in docs/research/gen1-movement.md). Integer maths copied
 * instruction for instruction, including the game's rounding and limits. */
#ifndef ACR_GEN1_COMPILE_H
#define ACR_GEN1_COMPILE_H

#include <stdint.h>

/* One part's stats, by meaning. Unused fields stay 0. */
typedef struct acr_gen1_part {
    uint8_t  present;          /* 0 = slot empty */
    uint16_t weight;
    uint16_t energy_drain;     /* legs: while moving; generator: output */
    uint16_t stat_2c;          /* generator capacity, blade attack, booster power, AP */
    uint16_t stat_2e;          /* booster boost drain, generator red zone */
    uint16_t carry_limit;      /* legs */
    uint16_t drain_standing;   /* legs */
    uint16_t stability;        /* legs (+0x40) */
    uint16_t boost_drain_legs; /* legs: used when no booster is fitted */
    uint16_t turn_accel;       /* legs +0x38 */
    uint8_t  turn_speed;       /* legs +0x3A */
    uint8_t  fixed_boost;      /* legs: ground boost byte set by the game for some
                                  four-legged and caterpillar legs (0 = computed) */
} acr_gen1_part;

typedef struct acr_gen1_build {
    acr_gen1_part core, booster, head, arms, blade, hand, back_l, back_r, generator, fcs, legs;
    uint16_t option_mask;      /* optional parts fitted, bit n = option n */
} acr_gen1_build;

typedef struct acr_gen1_stats {
    uint16_t energy_capacity;  /* compiled +18 */
    uint16_t rise_drain;       /* +20: energy per step while rising */
    uint16_t refill_standing;  /* +22 */
    uint16_t red_zone;         /* +24 */
    uint16_t refill_moving;    /* +26 */
    uint16_t blade_power;      /* +28 */
    uint8_t  stability;        /* +36: resistance to impact shock */
    uint32_t carried_weight;   /* all parts but the generator, capped by the legs */
    /* Movement bytes +6..+14 (1..64 unless noted). Booster power (9800 without
     * a booster) and the legs' turn stats, each divided by carried weight. */
    uint8_t  thrust;           /* +6 = +8: power * 4096 / weight / 224 */
    uint8_t  thrust_80;        /* +7 = +9: thrust * 8 / 10, 1..50 */
    uint8_t  thrust_120;       /* +10 = +11: thrust * 12 / 10, or the legs' fixed_boost */
    uint8_t  turn_accel;       /* +12 */
    uint8_t  turn_speed;       /* +13, 0..64 */
    uint8_t  friction;         /* +14: weight * 8 / 1000, 32..96 */
} acr_gen1_stats;

/* What the battle engine derives at match start (VS engine 0x8008342C).
 * Velocities are in 1/64 world units per step; turning in 1/4096 turns. */
typedef struct acr_gen1_motion {
    uint16_t rise_accel;       /* +6 x 2, per step while rising */
    uint16_t rise_max;         /* +7 x 200 */
    uint16_t air_accel;        /* +8 x 14, boosting in the air; / 4 slowing down after */
    uint16_t air_max;          /* +9 x 140 */
    uint16_t boost_accel;      /* +10 x 14, boosting on the ground; also the slowdown after */
    uint16_t boost_max;        /* +11 x 140 */
    uint16_t turn_accel;       /* +12 x 5 / 16 */
    uint16_t turn_max;         /* +13 x 15 / 16 */
    uint16_t friction;         /* +14, taken off each ground axis per step */
    uint16_t axis_step_max;    /* world units per step on each ground axis; 0 = no limit */
} acr_gen1_motion;

/* Fixed by the engine, not by parts. */
#define ACR_GEN1_GRAVITY        256   /* added to vertical speed per step */
#define ACR_GEN1_FALL_MAX       9600
#define ACR_GEN1_AXIS_STEP_MAX  75    /* world units per step on each ground axis (measured);
                                         acr_gen1_motion.axis_step_max starts here and a ruleset may raise it */

/* Optional parts (bit n = option n, Master of Arena's option table). */
#define ACR_GEN1_OPTION_RADAR          0x0001u /* SP-MAW */
#define ACR_GEN1_OPTION_JAMMER         0x0002u /* SP-JAM */
#define ACR_GEN1_OPTION_AUTO_LAUNCHER  0x0004u /* SP-M/AUTO */
#define ACR_GEN1_OPTION_BALANCER       0x0008u /* SP-ABS: legs stability +800 */
#define ACR_GEN1_OPTION_ABSORBER       0x0010u /* SP-SAP */
#define ACR_GEN1_OPTION_CHARGE_EXPANDER 0x0020u /* SP-CND-K */
#define ACR_GEN1_OPTION_FCS_ACCEL      0x0040u /* SP-AXL */
#define ACR_GEN1_OPTION_SHELL_SCREEN   0x0080u /* SP-S/SCR */
#define ACR_GEN1_OPTION_ENERGY_SCREEN  0x0100u /* SP-E/SCR */
#define ACR_GEN1_OPTION_RAPID_CHARGE   0x0200u /* SP-EH */
#define ACR_GEN1_OPTION_ENERGY_AMP     0x0400u /* SP-E+ */
#define ACR_GEN1_OPTION_ENERGY_REDUCE  0x0800u /* SP-DEtq */
#define ACR_GEN1_OPTION_BALANCER_RE    0x1000u /* SP-ABS_Re: legs stability +1600 */

void acr_gen1_compile(const acr_gen1_build *b, acr_gen1_stats *out);
void acr_gen1_motion_from_stats(const acr_gen1_stats *s, acr_gen1_motion *out);

#endif
