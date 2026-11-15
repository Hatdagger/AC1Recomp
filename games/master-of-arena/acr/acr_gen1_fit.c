#include "acr_gen1_fit.h"

/* Master of Arena's own parts. Index lists come from the garage code in the
 * menu module (equip handler 0x800865B4, hover check before it, DATA LOAD
 * fix-ups 0x80089514/0x80089750); see docs/research/gen1-fitting-rules.md. */
uint16_t acr_gen1_fit_flags_moa(acr_gen1_slot slot, unsigned index) {
    uint16_t f = 0;
    switch (slot) {
    case ACR_GEN1_SLOT_ARMS:
        if (index >= 12) f |= ACR_GEN1_FIT_ARM_WEAPON;
        if (index == 5) f |= ACR_GEN1_FIT_NO_BACK_WEAPONS;        /* ANKS-1A46J */
        break;
    case ACR_GEN1_SLOT_LEGS:
        if (index >= 27) f |= ACR_GEN1_FIT_CATERPILLAR;
        /* jump table at 0x80052820 (legs 5..32) */
        if (index == 5 || (index >= 21 && index <= 24) || index == 26 || index == 27 ||
            index == 28 || (index >= 30 && index <= 32))
            f |= ACR_GEN1_FIT_NO_HEAVY_HAND;
        break;
    case ACR_GEN1_SLOT_HAND:
        if (index == 15) f |= ACR_GEN1_FIT_HEAVY_HAND;            /* WG-1-KARASAWA */
        break;
    case ACR_GEN1_SLOT_BACK_L:
    case ACR_GEN1_SLOT_BACK_R:
        /* Only these radars are removed by caterpillars; 43, 45 and 47 stay. */
        if (index == 40 || index == 41 || index == 42 || index == 44 || index == 46)
            f |= ACR_GEN1_FIT_RADAR;
        if (index >= 52) f |= ACR_GEN1_FIT_TWO_SLOT;
        if (index < 40 || index == 53) f |= ACR_GEN1_FIT_BACK_WEAPON;
        break;
    default:
        break;
    }
    return f;
}

/* Ground boost bytes the game sets outright for these legs (compile routine,
 * 0x800AA96C), in place of the value worked out from thrust. */
uint8_t acr_gen1_fixed_boost_moa(unsigned legs_index) {
    switch (legs_index) {
    case 21: return 26; case 22: return 23; case 23: return 19; case 24: return 26;
    case 26: return 24; case 27: return 6;  case 28: return 7;  case 30: return 13;
    case 31: return 11; case 32: return 13; default: return 0;
    }
}

static acr_gen1_part *slot_part(acr_gen1_build *b, acr_gen1_slot s) {
    switch (s) {
    case ACR_GEN1_SLOT_CORE: return &b->core;
    case ACR_GEN1_SLOT_BOOSTER: return &b->booster;
    case ACR_GEN1_SLOT_HEAD: return &b->head;
    case ACR_GEN1_SLOT_ARMS: return &b->arms;
    case ACR_GEN1_SLOT_BLADE: return &b->blade;
    case ACR_GEN1_SLOT_HAND: return &b->hand;
    case ACR_GEN1_SLOT_BACK_L: return &b->back_l;
    case ACR_GEN1_SLOT_BACK_R: return &b->back_r;
    case ACR_GEN1_SLOT_GENERATOR: return &b->generator;
    case ACR_GEN1_SLOT_FCS: return &b->fcs;
    default: return &b->legs;
    }
}

static int has(const acr_gen1_fit_build *b, acr_gen1_slot s, uint16_t flag) {
    const acr_gen1_part *p = slot_part((acr_gen1_build *)&b->parts, s);
    return p->present && (b->flags[s] & flag);
}

static void clear(acr_gen1_fit_build *b, acr_gen1_slot s, uint32_t *removed) {
    acr_gen1_part *p = slot_part(&b->parts, s);
    if (!p->present) return;
    *p = (acr_gen1_part){0};
    b->flags[s] = 0;
    *removed |= 1u << s;
}

uint32_t acr_gen1_fit_repair(acr_gen1_fit_build *b, acr_gen1_slot fitted_to) {
    uint32_t removed = 0;
    /* Hand weapon and blade need regular arms. */
    if (!b->parts.arms.present || has(b, ACR_GEN1_SLOT_ARMS, ACR_GEN1_FIT_ARM_WEAPON)) {
        clear(b, ACR_GEN1_SLOT_HAND, &removed);
        clear(b, ACR_GEN1_SLOT_BLADE, &removed);
    }
    /* Caterpillars: no booster, no (listed) radar. */
    if (has(b, ACR_GEN1_SLOT_LEGS, ACR_GEN1_FIT_CATERPILLAR)) {
        clear(b, ACR_GEN1_SLOT_BOOSTER, &removed);
        if (has(b, ACR_GEN1_SLOT_BACK_L, ACR_GEN1_FIT_RADAR)) clear(b, ACR_GEN1_SLOT_BACK_L, &removed);
        if (has(b, ACR_GEN1_SLOT_BACK_R, ACR_GEN1_FIT_RADAR)) clear(b, ACR_GEN1_SLOT_BACK_R, &removed);
    }
    /* Heavy hand weapon vs legs that can't hold it. */
    if (has(b, ACR_GEN1_SLOT_HAND, ACR_GEN1_FIT_HEAVY_HAND) &&
        has(b, ACR_GEN1_SLOT_LEGS, ACR_GEN1_FIT_NO_HEAVY_HAND))
        clear(b, ACR_GEN1_SLOT_HAND, &removed);
    /* Arms that can't carry back weapons. */
    if (has(b, ACR_GEN1_SLOT_ARMS, ACR_GEN1_FIT_NO_BACK_WEAPONS)) {
        if (has(b, ACR_GEN1_SLOT_BACK_L, ACR_GEN1_FIT_BACK_WEAPON)) clear(b, ACR_GEN1_SLOT_BACK_L, &removed);
        if (has(b, ACR_GEN1_SLOT_BACK_R, ACR_GEN1_FIT_BACK_WEAPON)) clear(b, ACR_GEN1_SLOT_BACK_R, &removed);
    }
    /* Two-slot units: fitting one empties the other back slot. The game does
     * nothing about it on DATA LOAD (a card can carry both), so neither do we. */
    if (fitted_to == ACR_GEN1_SLOT_BACK_L || fitted_to == ACR_GEN1_SLOT_BACK_R) {
        acr_gen1_slot other = fitted_to == ACR_GEN1_SLOT_BACK_L ? ACR_GEN1_SLOT_BACK_R : ACR_GEN1_SLOT_BACK_L;
        if (has(b, fitted_to, ACR_GEN1_FIT_TWO_SLOT) || has(b, other, ACR_GEN1_FIT_TWO_SLOT))
            clear(b, other, &removed);
    }
    return removed;
}

unsigned acr_gen1_fit_check(const acr_gen1_fit_build *b, acr_gen1_fit_numbers *out) {
    const acr_gen1_build *p = &b->parts;
    unsigned fail = 0;
    acr_gen1_fit_numbers n = {0};

    if (!p->head.present || !p->core.present || !p->arms.present || !p->generator.present ||
        !p->fcs.present || !p->legs.present)
        fail |= ACR_GEN1_FAIL_MISSING_PART;
    if (p->legs.present && !(b->flags[ACR_GEN1_SLOT_LEGS] & ACR_GEN1_FIT_CATERPILLAR) && !p->booster.present)
        fail |= ACR_GEN1_FAIL_NO_BOOSTER;

    for (unsigned i = 0; i < 16; ++i)
        if (p->option_mask & (1u << i)) n.option_slots_used += b->option_cost[i];
    if (n.option_slots_used > b->core_option_slots) fail |= ACR_GEN1_FAIL_OPTION_SLOTS;

    const acr_gen1_part *others[] = { &p->core, &p->booster, &p->head, &p->arms, &p->blade, &p->hand,
                                      &p->back_l, &p->back_r, &p->fcs };
    int32_t drain = p->legs.present ? p->legs.energy_drain : 0;
    uint32_t weight = p->generator.present ? p->generator.weight : 0;
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; ++i) {
        if (!others[i]->present) continue;
        drain += others[i]->energy_drain;
        weight += others[i]->weight;
    }
    n.energy_surplus = (p->generator.present ? (int32_t)p->generator.energy_drain : 0) - drain;
    n.carried_weight = weight;
    n.arm_load = p->arms.present ? p->arms.weight : 0;
    if (!(b->flags[ACR_GEN1_SLOT_ARMS] & ACR_GEN1_FIT_ARM_WEAPON)) {
        if (p->hand.present) n.arm_load += p->hand.weight;
        if (p->blade.present) n.arm_load += p->blade.weight;
    }
    if (n.energy_surplus < 0) fail |= ACR_GEN1_FAIL_ENERGY;
    if (p->core.present && n.arm_load > p->core.carry_limit) fail |= ACR_GEN1_FAIL_ARM_LOAD;
    if (p->legs.present && n.carried_weight > p->legs.carry_limit) fail |= ACR_GEN1_FAIL_WEIGHT;

    if (out) *out = n;
    return fail;
}
