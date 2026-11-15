/* Gen 1 fitting rules: which part combinations Master of Arena allows.
 *
 * Two layers, as in the game:
 *  - acr_gen1_fit_repair() removes parts that cannot be fitted together, the
 *    way the garage and DATA LOAD do (caterpillars lose their booster and
 *    radar, and so on).
 *  - acr_gen1_fit_check() is the VS START check: a build that fails it
 *    cannot be fielded.
 *
 * Rules are expressed as part flags rather than game indices so new parts can
 * opt in; acr_gen1_fit_flags_moa() gives the flags of Master of Arena's own
 * parts. Evidence: docs/research/gen1-fitting-rules.md.
 *
 * Not modelled yet: Human PLUS, which lets the energy, arm load and weight
 * checks pass (flag byte 0x8003EACA in the game). */
#ifndef ACR_GEN1_FIT_H
#define ACR_GEN1_FIT_H

#include <stdint.h>

#include "acr_gen1_compile.h"

/* Part flags. */
#define ACR_GEN1_FIT_ARM_WEAPON      0x0001u /* arms: weapon arms, no hand weapon or blade */
#define ACR_GEN1_FIT_CATERPILLAR     0x0002u /* legs: no booster, no radar */
#define ACR_GEN1_FIT_RADAR           0x0004u /* back: removed by caterpillars */
#define ACR_GEN1_FIT_TWO_SLOT        0x0008u /* back: uses both back slots */
#define ACR_GEN1_FIT_HEAVY_HAND      0x0010u /* hand: not with ACR_GEN1_FIT_NO_HEAVY_HAND legs */
#define ACR_GEN1_FIT_NO_HEAVY_HAND   0x0020u /* legs */
#define ACR_GEN1_FIT_NO_BACK_WEAPONS 0x0040u /* arms: no ACR_GEN1_FIT_BACK_WEAPON units */
#define ACR_GEN1_FIT_BACK_WEAPON     0x0080u /* back */

typedef enum acr_gen1_slot {
    ACR_GEN1_SLOT_CORE, ACR_GEN1_SLOT_BOOSTER, ACR_GEN1_SLOT_HEAD, ACR_GEN1_SLOT_ARMS,
    ACR_GEN1_SLOT_BLADE, ACR_GEN1_SLOT_HAND, ACR_GEN1_SLOT_BACK_L, ACR_GEN1_SLOT_BACK_R,
    ACR_GEN1_SLOT_GENERATOR, ACR_GEN1_SLOT_FCS, ACR_GEN1_SLOT_LEGS,
    ACR_GEN1_SLOT_COUNT
} acr_gen1_slot;

/* A build plus what the fitting rules need beyond the compile stats. */
typedef struct acr_gen1_fit_build {
    acr_gen1_build parts;               /* core.carry_limit = arm load limit (+0x32) */
    uint16_t flags[ACR_GEN1_SLOT_COUNT];
    uint8_t  core_option_slots;         /* core +0x36 */
    uint8_t  option_cost[16];           /* slots used by option n */
} acr_gen1_fit_build;

/* Flags of Master of Arena part `index` in `slot`'s table
 * (ACR_GEN1_SLOT_BACK_R uses the back table too). */
uint16_t acr_gen1_fit_flags_moa(acr_gen1_slot slot, unsigned index);

/* Master of Arena legs whose ground boost the game fixes (see acr_gen1_part.fixed_boost); 0 = none. */
uint8_t acr_gen1_fixed_boost_moa(unsigned legs_index);

/* Removes parts that cannot stay, like the game. `fitted_to` is the slot that
 * was just changed (it wins a conflict), or ACR_GEN1_SLOT_COUNT for a loaded
 * build. Returns a mask of removed slots (bit = acr_gen1_slot). */
uint32_t acr_gen1_fit_repair(acr_gen1_fit_build *b, acr_gen1_slot fitted_to);

/* VS START verdict: 0 when legal, else a mask of the failures below. */
#define ACR_GEN1_FAIL_MISSING_PART  0x01u /* head, core, arms, generator, FCS or legs */
#define ACR_GEN1_FAIL_NO_BOOSTER    0x02u /* legs other than caterpillars need one */
#define ACR_GEN1_FAIL_OPTION_SLOTS  0x04u
#define ACR_GEN1_FAIL_ENERGY        0x08u /* drain above generator output */
#define ACR_GEN1_FAIL_ARM_LOAD      0x10u
#define ACR_GEN1_FAIL_WEIGHT        0x20u /* above the legs' carry limit */

typedef struct acr_gen1_fit_numbers {
    int32_t  energy_surplus;   /* generator output - all drains (legs while moving) */
    uint32_t arm_load;         /* arms + hand + blade; weapon arms alone */
    uint32_t carried_weight;   /* everything but the legs */
    uint32_t option_slots_used;
} acr_gen1_fit_numbers;

unsigned acr_gen1_fit_check(const acr_gen1_fit_build *b, acr_gen1_fit_numbers *out);

#endif
