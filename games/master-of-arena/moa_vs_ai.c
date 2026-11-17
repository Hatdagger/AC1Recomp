/* Master of Arena: VS AI, step 1 (bare bones).
 *
 * Our own AI presses the buttons for one or both sides of the split-screen
 * VS engine (FDAT 209). It runs once per game step, straight after the VS
 * controller sampler (0x8007DA4C), and replaces the button word of each AI
 * side. Everything after the sampler (the AC code, the replay recorder)
 * then sees the AI's buttons as if a person pressed them.
 *
 * Step 1 behaviour: turn to face the enemy, close in, fire the selected
 * weapon when locked, swing the blade up close, change weapon when the
 * current one is empty. Nothing else yet (no strafing, evasion or profile).
 *
 * Switch: PSX_MOA_VS_AI = 1 (player 1), 2 (player 2) or 3 (both).
 * Offline only. Separate from the arena-engine AI Battle (moa_ai_battle.c).
 *
 * Addresses: docs/mapping/battle-system.md and stat-block.md (VS engine).
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* Engine check: every engine loads at the same address. */
#define VS_MODE_BYTE      0x8003EA2Au   /* 2 = split-screen VS */
#define VS_MODE_VS        2u
#define VS_SAMPLER        0x8007DA4Cu   /* VS controller sampler */
#define VS_SIG0           0x27BDFFE8u   /* addiu sp,sp,-24 */
#define VS_SIG1           0xAFBF0010u   /* sw ra,16(sp) */
#define VS_SIG2           0x0C005D9Fu   /* jal 0x8001767C (read both pads) */

/* Match */
#define VS_FLAGS          0x80097EACu   /* bit 0x400: inputs frozen */
#define VS_END_COUNT      0x80097EB0u   /* nonzero: the round is over */
#define VS_STEPS          0x80097EB2u   /* steps elapsed */
#define VS_REPLAY_FLAG    0x8003EA92u

/* Player block (928 bytes each, P2 follows P1) */
#define PB_P1             0x80045198u
#define PB_BYTES          928u
#define PB_RAW            0x000u
#define PB_HELD           0x004u
#define PB_HELD_PREV      0x006u
#define PB_SLOT_SEL       0x010u        /* u8 selected weapon slot */
#define PB_ENERGY         0x016u
#define PB_BOOST_LOCK     0x018u        /* bit 0: booster locked until full */
#define PB_LOCK_TARGET    0x07Cu        /* locked AC object, 0 = none */
#define PB_E_CAP          0x262u
#define PB_E_RED          0x268u
#define PB_M_FWD          0x28Eu        /* button masks (player's layout) */
#define PB_M_BACK         0x290u
#define PB_M_LEFT         0x292u        /* turn left: facing goes up */
#define PB_M_RIGHT        0x294u
#define PB_M_WEAPON       0x296u
#define PB_M_BOOST        0x298u
#define PB_M_FIRE         0x29Au
#define PB_M_BLADE        0x29Cu
#define PB_BLADE_ATK      0x2B8u        /* 0 = no blade */
#define PB_USABLE         0x2D7u        /* u8 usable weapon count */
#define PB_SLOTS          0x2D8u        /* 62 bytes each */
#define PB_SLOT_BYTES     62u
#define SLOT_AMMO         16u
#define PB_OBJECT         0x398u

/* AC object */
#define AC_X              0x08u
#define AC_Y              0x0Au
#define AC_Z              0x0Cu
#define AC_FACING         0x12u         /* 4096 = full turn, 0 = +z, 3072 = +x */
#define AC_TURN_SPEED     0x1Au
#define AC_FLAGS          0x36u         /* bit 0x10: busy (swing, stagger) */
#define AC_STATE          0x45u
#define AC_ALIVE          0x49u         /* 255 while fighting */
#define AC_AP4            0x168u

/* Step 1 tuning (to be replaced by the habit profile). */
#define AI_FAR            4500          /* boost in from beyond this */
#define AI_NEAR           2200          /* stop closing in here */
#define AI_TOO_CLOSE      900           /* back off inside this (gun) */
#define AI_BLADE_REACH    620           /* about 650 measured for 2 hits */
#define AI_FACE_OK        24            /* facing error that needs no turn */
#define AI_FIRE_CONE      220           /* fire without lock inside ±19° */
#define AI_ENERGY_MARGIN  4000          /* boost only this far above red */

static int vs_ai_sides = -1;            /* bit 0 = P1, bit 1 = P2 */
static int vs_ai_nested;

struct vs_ai_side {
    uint32_t steps;
    uint16_t last_word;
    uint16_t weapon_wait;               /* steps before the next △ */
    uint16_t boosting;                  /* steps the current boost has run */
    uint32_t rng;
};
static struct vs_ai_side vs_side[2];

static int vs_ai_on(void)
{
    if (vs_ai_sides < 0) {
        const char *e = getenv("PSX_MOA_VS_AI");
        vs_ai_sides = (e && e[0] >= '1' && e[0] <= '3') ? (e[0] - '0') : 0;
    }
    return vs_ai_sides;
}

static int vs_engine(uint32_t address)
{
    return psx_mod_read_byte(VS_MODE_BYTE) == VS_MODE_VS &&
           psx_mod_read_byte(VS_REPLAY_FLAG) == 0 &&
           psx_mod_read_word(address) == VS_SIG0 &&
           psx_mod_read_word(address + 4u) == VS_SIG1 &&
           psx_mod_read_word(address + 8u) == VS_SIG2;
}

static uint32_t vs_object(uint32_t pb)
{
    const uint32_t o = psx_mod_read_word(pb + PB_OBJECT);
    if (o < 0x80100000u || o >= 0x80200000u || (o & 3u)) return 0;
    return o;
}

static int16_t rd16s(uint32_t a) { return (int16_t)psx_mod_read_half(a); }

/* Angle of a direction in the game's units: 0 = +z, 3072 = +x. */
static int vs_angle(int dx, int dz)
{
    const double a = atan2(-(double)dx, (double)dz);       /* radians */
    int u = (int)(a * 4096.0 / 6.283185307179586);
    return u & 4095;
}

static int vs_wrap(int a)               /* -2048 … 2047 */
{
    a &= 4095;
    return a >= 2048 ? a - 4096 : a;
}

static uint32_t vs_isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static uint32_t vs_rand(struct vs_ai_side *s)
{
    s->rng = s->rng * 1103515245u + 12345u;
    return s->rng >> 16;
}

/* One step of the bare-bones AI for the side whose block is `me`. */
static uint16_t vs_ai_think(unsigned side, uint32_t me, uint32_t foe)
{
    struct vs_ai_side *s = &vs_side[side];
    const uint32_t my = vs_object(me), en = vs_object(foe);
    if (!my || !en) return 0;
    if (psx_mod_read_byte(my + AC_ALIVE) != 0xFFu) return 0;   /* wrecked */
    s->steps++;

    const uint16_t m_fwd = psx_mod_read_half(me + PB_M_FWD);
    const uint16_t m_back = psx_mod_read_half(me + PB_M_BACK);
    const uint16_t m_left = psx_mod_read_half(me + PB_M_LEFT);
    const uint16_t m_right = psx_mod_read_half(me + PB_M_RIGHT);
    const uint16_t m_weapon = psx_mod_read_half(me + PB_M_WEAPON);
    const uint16_t m_boost = psx_mod_read_half(me + PB_M_BOOST);
    const uint16_t m_fire = psx_mod_read_half(me + PB_M_FIRE);
    const uint16_t m_blade = psx_mod_read_half(me + PB_M_BLADE);

    /* Read */
    const int dx = rd16s(en + AC_X) - rd16s(my + AC_X);
    const int dz = rd16s(en + AC_Z) - rd16s(my + AC_Z);
    const uint32_t dist = vs_isqrt((uint32_t)(dx * dx + dz * dz));
    const int facing = psx_mod_read_half(my + AC_FACING) & 4095;
    const int err = vs_wrap(vs_angle(dx, dz) - facing);       /* + = turn left */
    const int aerr = err < 0 ? -err : err;
    const int locked = psx_mod_read_word(me + PB_LOCK_TARGET) == en;
    const int energy = psx_mod_read_half(me + PB_ENERGY);
    const int red = psx_mod_read_half(me + PB_E_RED);
    const int boost_locked = psx_mod_read_half(me + PB_BOOST_LOCK) & 1;
    const unsigned sel = psx_mod_read_byte(me + PB_SLOT_SEL);
    const unsigned usable = psx_mod_read_byte(me + PB_USABLE);
    const int ammo = (sel < 8u && usable)
        ? psx_mod_read_half(me + PB_SLOTS + PB_SLOT_BYTES * sel + SLOT_AMMO) : 0;
    const int has_blade = psx_mod_read_half(me + PB_BLADE_ATK) != 0;
    const int busy = psx_mod_read_half(my + AC_FLAGS) & 0x10;
    uint16_t w = 0;

    /* Out of ammo everywhere: fight with the blade. */
    int all_empty = 1;
    for (unsigned i = 0; i < usable && i < 8u; i++)
        if (psx_mod_read_half(me + PB_SLOTS + PB_SLOT_BYTES * i + SLOT_AMMO)) all_empty = 0;
    const int blade_only = all_empty && has_blade;

    /* Face */
    if (aerr > AI_FACE_OK) w |= err > 0 ? m_left : m_right;

    /* Range */
    const uint32_t near = blade_only ? (uint32_t)(AI_BLADE_REACH - 150) : AI_NEAR;
    const int want_boost = dist > AI_FAR && !boost_locked &&
                           energy > red + AI_ENERGY_MARGIN && aerr < 400;
    if (dist > near && aerr < 700) {
        w |= m_fwd;
        if (want_boost || (s->boosting && s->boosting < 40 && !boost_locked &&
                           energy > red + 1000)) {
            w |= m_boost;
            s->boosting++;
        } else s->boosting = 0;
    } else {
        s->boosting = 0;
        if (!blade_only && dist < AI_TOO_CLOSE) w |= m_back;
    }

    /* Weapon: change when the selected one is empty (△ is a new press). */
    if (s->weapon_wait) s->weapon_wait--;
    else if (!all_empty && ammo == 0 && usable > 1) {
        if (!(s->last_word & m_weapon)) w |= m_weapon;
        s->weapon_wait = 20;
    }

    /* Fire: held while locked, or with the enemy in the sight. */
    if (ammo > 0 && (locked || aerr < AI_FIRE_CONE) && (vs_rand(s) & 7u) != 0)
        w |= m_fire;

    /* Blade: new press, facing, in reach, not already swinging. */
    if (has_blade && !busy && dist < AI_BLADE_REACH && aerr < 300 &&
        energy > red && !(s->last_word & m_blade))
        w |= m_blade;

    s->last_word = w;
    return w;
}

static void vs_counts(unsigned side, uint16_t w)
{
    static const char *const n[2] = { "moa.vsai.p1.steps", "moa.vsai.p2.steps" };
    psx_mod_counter_add(n[side], 1);
    if (w) psx_mod_counter_add(side ? "moa.vsai.p2.pressed" : "moa.vsai.p1.pressed", 1);
}

/* VS controller sampler: let the game read the pads, then put the AI's
 * buttons in for each AI side. */
static int vs_sampler_filter(struct CPUState *cpu, uint32_t address)
{
    if (vs_ai_nested || !vs_ai_on() || !vs_engine(address)) return 0;
    vs_ai_nested = 1;
    (void)psx_mod_call_guest(cpu, address, cpu->gpr[31], cpu->gpr[4], cpu->gpr[5],
                             cpu->gpr[6], cpu->gpr[7]);
    vs_ai_nested = 0;

    /* New match (steps elapsed restarted): fresh AI state. */
    static uint16_t last_steps;
    const uint16_t steps = psx_mod_read_half(VS_STEPS);
    if (steps < last_steps) {
        for (unsigned i = 0; i < 2; i++) {
            vs_side[i].steps = 0;
            vs_side[i].last_word = 0;
            vs_side[i].weapon_wait = 0;
            vs_side[i].boosting = 0;
        }
        psx_mod_counter_add("moa.vsai.match", 1);
    }
    last_steps = steps;

    /* The round is over: the people answer CONTINUE? with their own pads. */
    if (psx_mod_read_half(VS_END_COUNT) != 0) return 1;

    const int frozen = (psx_mod_read_word(VS_FLAGS) & 0x400u) != 0;
    for (unsigned side = 0; side < 2; side++) {
        if (!(vs_ai_sides & (1 << side))) continue;
        const uint32_t me = PB_P1 + side * PB_BYTES;
        const uint32_t foe = PB_P1 + (1u - side) * PB_BYTES;
        if (!vs_side[side].rng) vs_side[side].rng = 0x9E3779B9u ^ (side * 0x85EBCA6Bu);
        const uint16_t w = vs_ai_think(side, me, foe);
        psx_mod_write_half(me + PB_RAW, w);
        psx_mod_write_half(me + PB_HELD, frozen ? 0 : w);
        vs_counts(side, w);
    }
    return 1;
}

PSX_MOD_CONSTRUCTOR(moa_vs_ai_register)
{
    (void)psx_game_register_offline_function_filter(VS_SAMPLER, vs_sampler_filter);
}
