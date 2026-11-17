/* Master of Arena: VS AI.
 *
 * Our own AI presses the buttons for one or both sides of the split-screen
 * VS engine (FDAT 209). It runs once per game step, straight after the VS
 * controller sampler (0x8007DA4C), and replaces the button word of each AI
 * side. Everything after the sampler (the AC code, the replay recorder)
 * then sees the AI's buttons as if a person pressed them.
 *
 * Behaviour comes from a habit profile (moa_vs_ai.h): preferred distance,
 * boosting, jumping, energy floor, strafing, weapon choice by distance,
 * trigger discipline, blade use, missile evasion and reaction delay.
 *
 * Switches:
 *   PSX_MOA_VS_AI    = 1 (player 1), 2 (player 2) or 3 (both)
 *   PSX_MOA_VS_AI_P1 / PSX_MOA_VS_AI_P2 = preset name (default balanced):
 *                      balanced, aggressor, sniper, duelist, evasive
 * Offline only. Separate from the arena-engine AI Battle (moa_ai_battle.c).
 *
 * Addresses: docs/mapping/battle-system.md and stat-block.md (VS engine).
 * Inputs checked in the game: ✕ while standing jumps; a direction plus ✕
 * boosts along the ground, but only once the AC is already moving.
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "moa_vs_ai.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
#define VS_STEPS_PER_MIN  1320u         /* about 22 steps a second */

/* Player block (928 bytes each, P2 follows P1) */
#define PB_P1             0x80045198u
#define PB_BYTES          928u
#define PB_RAW            0x000u
#define PB_HELD           0x004u
#define PB_SLOT_SEL       0x010u        /* u8 selected weapon slot */
#define PB_ENERGY         0x016u
#define PB_BOOST_LOCK     0x018u        /* bit 0: booster locked until full */
#define PB_LOCKS_FREE     0x01Au        /* missile locks still free */
#define PB_LOCK_TARGET    0x07Cu        /* lock slot 0: locked AC object, 0 = none */
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
#define PB_M_STRAFE_L     0x29Eu
#define PB_M_STRAFE_R     0x2A2u
#define PB_BLADE_ATK      0x2B8u        /* 0 = no blade */
#define PB_USABLE         0x2D7u        /* u8 usable weapon count */
#define PB_SLOTS          0x2D8u        /* 62 bytes each, up to 3 */
#define PB_SLOT_BYTES     62u
#define SLOT_KIND         0u            /* 0 gun, 1 missile, 2 cannon */
#define SLOT_AMMO         16u
#define SLOT_RANGE        22u
#define SLOT_MAX_LOCKS    34u
#define PB_OBJECT         0x398u

/* AC object */
#define AC_X              0x08u
#define AC_Z              0x0Cu
#define AC_FACING         0x12u         /* 4096 = full turn, 0 = +z, 3072 = +x */
#define AC_FLAGS          0x36u         /* bit 0x10: busy (swing, stagger) */
#define AC_STATE          0x45u
#define AC_CLASS          0x46u         /* 0x10 ground, 0xB0 ground boost, 0xC0 air */
#define AC_VOLLEY         0x47u         /* missile volley steps left */
#define AC_ALIVE          0x49u         /* 255 while fighting */

/* Object pool: shots, missiles, effects. */
#define POOL              0x801D6F6Cu
#define POOL_COUNT        256u
#define POOL_BYTES        228u
#define POOL_FREE         0x06u         /* s16: -1 free */
#define POOL_UPDATE       0x18u
#define POOL_TARGET       0x38u
#define POOL_AGE          0x3Cu
#define POOL_FLAGS        0x42u         /* bit 1: gave up */
#define MISSILE_UPDATE    0x8008907Cu

/* Fixed tuning that is not a habit. */
#define AI_BLADE_REACH    620           /* about 650 measured for 2 hits */
#define AI_FACE_OK        24            /* facing error that needs no turn */
#define AI_FIRE_CONE      220           /* guns fire without lock inside ±19° */
#define AI_HOLD_BAND      300           /* no forward / back inside ± this */
#define AI_NEAR_BAND      1500
#define AI_FAR_BAND       4000
#define AI_HISTORY        32            /* reaction delay up to 31 steps */

const char *const moa_vs_preset_names[VS_PRESETS] = {
    "balanced", "aggressor", "sniper", "duelist", "evasive" };

/*                     dist spr  gb air jmp floor str sw  weapon[near|mid|far][gun,missile,cannon]          fire lk  bl  ev rc */
const struct moa_vs_profile moa_vs_presets[VS_PRESETS] = {
    /* Balanced  */ { 2500,  600, 40, 30,  4, 40,  50, 12, {{3,1,2},{3,2,2},{1,3,2}},  85,  0, 20, 50,  6 },
    /* Aggressor */ {  900,  300, 70, 20,  3, 25,  30, 20, {{3,1,3},{3,2,3},{2,3,2}}, 100,  1, 60, 25,  4 },
    /* Sniper    */ { 4500,  800, 30, 20,  2, 55,  60,  8, {{3,1,1},{3,2,2},{3,2,2}},  70,  0,  5, 60,  8 },
    /* Duelist   */ {  650,  150, 60, 30,  6, 30,  40, 30, {{3,1,1},{3,1,1},{2,3,1}},  80,  1, 90, 40,  4 },
    /* Evasive   */ { 3000, 1000, 50, 60, 10, 50,  80, 25, {{2,2,1},{2,3,1},{1,3,1}},  60,  2, 10, 85,  5 },
};

static int vs_ai_sides = -1;            /* bit 0 = P1, bit 1 = P2 */
static int vs_ai_nested;

struct vs_ai_side {
    struct moa_vs_profile p;
    uint32_t rng;
    uint32_t steps;
    uint16_t last_word;
    int16_t  hist_x[AI_HISTORY], hist_z[AI_HISTORY];   /* enemy positions seen */
    int      want_dist;                 /* current distance target */
    uint16_t dist_timer;                /* steps until want_dist is re-rolled */
    int      strafe_dir;                /* -1 left, +1 right */
    uint8_t  strafe_on;
    uint16_t strafe_timer;
    uint8_t  boosting;                  /* holding the booster (hysteresis) */
    uint8_t  jump;                      /* jump steps left (0 = none) */
    uint8_t  jump_rise;                 /* of which rising on the booster */
    uint8_t  evade;                     /* dodge steps left */
    int      evade_dir;
    uint16_t weapon_wait;               /* steps before the next △ */
    uint16_t band_steps;                /* steps the distance band has held */
    uint8_t  band;
    uint8_t  firing;                    /* missile trigger held steps left */
    uint32_t seen_missiles[8];          /* pool records already judged (bitset) */
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

static void vs_load_profile(unsigned side)
{
    const char *e = getenv(side ? "PSX_MOA_VS_AI_P2" : "PSX_MOA_VS_AI_P1");
    unsigned pick = VS_PRESET_BALANCED;
    for (unsigned i = 0; e && i < VS_PRESETS; i++)
        if (strcasecmp(e, moa_vs_preset_names[i]) == 0) pick = i;
    vs_side[side].p = moa_vs_presets[pick];
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

/* Angle of a direction in the game's units: 0 = +z, 3072 = +x (checked). */
static int vs_angle(int dx, int dz)
{
    const double a = atan2(-(double)dx, (double)dz);
    return (int)(a * 4096.0 / 6.283185307179586) & 4095;
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
    return (s->rng >> 8) & 0xFFFFu;
}

/* True with probability pct %. */
static int vs_pct(struct vs_ai_side *s, unsigned pct)
{
    return (vs_rand(s) % 100u) < pct;
}

/* True with a chance of `per_min` times a minute (per step). */
static int vs_per_min(struct vs_ai_side *s, unsigned per_min)
{
    return (vs_rand(s) % VS_STEPS_PER_MIN) < per_min;
}

/* Missiles in flight at `obj` that haven't given up and are at least
 * `react` steps old. Returns the number of new ones (not judged before). */
static unsigned vs_new_missiles(struct vs_ai_side *s, uint32_t obj, unsigned react)
{
    unsigned fresh = 0;
    uint32_t live[8] = {0};
    for (uint32_t i = 0; i < POOL_COUNT; i++) {
        const uint32_t r = POOL + i * POOL_BYTES;
        if (rd16s(r + POOL_FREE) != 0) continue;
        if (psx_mod_read_word(r + POOL_UPDATE) != MISSILE_UPDATE) continue;
        if (psx_mod_read_word(r + POOL_TARGET) != obj) continue;
        if (psx_mod_read_half(r + POOL_FLAGS) & 2u) continue;
        if (psx_mod_read_half(r + POOL_AGE) < react) continue;
        live[i >> 5] |= 1u << (i & 31);
        if (!(s->seen_missiles[i >> 5] & (1u << (i & 31)))) fresh++;
    }
    memcpy(s->seen_missiles, live, sizeof live);   /* forget the gone ones */
    return fresh;
}

static unsigned vs_band(uint32_t dist)
{
    return dist < AI_NEAR_BAND ? VS_BAND_NEAR : dist < AI_FAR_BAND ? VS_BAND_MID : VS_BAND_FAR;
}

/* The slot the profile likes best for this band (with ammo, in range). */
static int vs_best_slot(const struct vs_ai_side *s, uint32_t me, unsigned band, uint32_t dist)
{
    const unsigned usable = psx_mod_read_byte(me + PB_USABLE);
    int best = -1, best_score = -1;
    for (unsigned i = 0; i < usable && i < 3u; i++) {
        const uint32_t slot = me + PB_SLOTS + PB_SLOT_BYTES * i;
        if (!psx_mod_read_half(slot + SLOT_AMMO)) continue;
        const unsigned kind = psx_mod_read_byte(slot + SLOT_KIND);
        int score = kind < VS_KINDS ? s->p.weapon[band][kind] * 4 : 0;
        const uint32_t range = psx_mod_read_half(slot + SLOT_RANGE);
        if (range && dist <= range) score += 2;          /* reaches from here */
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    return best;
}

/* One step of the AI for the side whose block is `me`. */
static uint16_t vs_ai_think(unsigned side, uint32_t me, uint32_t foe)
{
    struct vs_ai_side *s = &vs_side[side];
    const struct moa_vs_profile *p = &s->p;
    const uint32_t my = vs_object(me), en = vs_object(foe);
    if (!my || !en) return 0;
    if (psx_mod_read_byte(my + AC_ALIVE) != 0xFFu) return 0;   /* wrecked */

    const uint16_t m_fwd = psx_mod_read_half(me + PB_M_FWD);
    const uint16_t m_back = psx_mod_read_half(me + PB_M_BACK);
    const uint16_t m_left = psx_mod_read_half(me + PB_M_LEFT);
    const uint16_t m_right = psx_mod_read_half(me + PB_M_RIGHT);
    const uint16_t m_weapon = psx_mod_read_half(me + PB_M_WEAPON);
    const uint16_t m_boost = psx_mod_read_half(me + PB_M_BOOST);
    const uint16_t m_fire = psx_mod_read_half(me + PB_M_FIRE);
    const uint16_t m_blade = psx_mod_read_half(me + PB_M_BLADE);
    const uint16_t m_sl = psx_mod_read_half(me + PB_M_STRAFE_L);
    const uint16_t m_sr = psx_mod_read_half(me + PB_M_STRAFE_R);
    const uint16_t m_move = m_fwd | m_back | m_sl | m_sr;

    /* Read: the enemy as seen `react` steps ago. */
    const unsigned react = p->react < AI_HISTORY ? p->react : AI_HISTORY - 1;
    const unsigned now = s->steps % AI_HISTORY;
    s->hist_x[now] = rd16s(en + AC_X);
    s->hist_z[now] = rd16s(en + AC_Z);
    const unsigned then = s->steps >= react ? (s->steps - react) % AI_HISTORY : 0;
    s->steps++;
    const int dx = s->hist_x[then] - rd16s(my + AC_X);
    const int dz = s->hist_z[then] - rd16s(my + AC_Z);
    const uint32_t dist = vs_isqrt((uint32_t)(dx * dx + dz * dz));
    const int facing = psx_mod_read_half(my + AC_FACING) & 4095;
    const int err = vs_wrap(vs_angle(dx, dz) - facing);       /* + = turn left */
    const int aerr = err < 0 ? -err : err;
    const int locked = psx_mod_read_word(me + PB_LOCK_TARGET) == en;
    const int energy = psx_mod_read_half(me + PB_ENERGY);
    const int cap = psx_mod_read_half(me + PB_E_CAP);
    const int red = psx_mod_read_half(me + PB_E_RED);
    const int boost_locked = psx_mod_read_half(me + PB_BOOST_LOCK) & 1;
    const unsigned cls = psx_mod_read_byte(my + AC_CLASS);
    const int airborne = cls == 0xC0u;
    const int moving = (s->last_word & m_move) != 0;
    const unsigned sel = psx_mod_read_byte(me + PB_SLOT_SEL);
    const unsigned usable = psx_mod_read_byte(me + PB_USABLE);
    const uint32_t sel_slot = me + PB_SLOTS + PB_SLOT_BYTES * (sel < 3u ? sel : 0u);
    const unsigned kind = psx_mod_read_byte(sel_slot + SLOT_KIND);
    const int ammo = usable ? psx_mod_read_half(sel_slot + SLOT_AMMO) : 0;
    const int has_blade = psx_mod_read_half(me + PB_BLADE_ATK) != 0;
    const int busy = psx_mod_read_half(my + AC_FLAGS) & 0x10;
    uint16_t w = 0;

    int all_empty = 1;
    for (unsigned i = 0; i < usable && i < 3u; i++)
        if (psx_mod_read_half(me + PB_SLOTS + PB_SLOT_BYTES * i + SLOT_AMMO)) all_empty = 0;
    const int blade_only = all_empty && has_blade;

    /* Energy: boost down to the floor, then wait until it is 10% higher. */
    const int floor_e = cap * p->efloor / 100 > red ? cap * p->efloor / 100 : red;
    const int may_boost = !boost_locked &&
        energy > (s->boosting ? floor_e : floor_e + cap / 10);

    /* Distance target wanders inside the profile's spread. */
    if (s->dist_timer) s->dist_timer--;
    else {
        const int spread = p->spread ? (int)(vs_rand(s) % (2u * p->spread + 1u)) - p->spread : 0;
        s->want_dist = blade_only ? AI_BLADE_REACH - 150 : p->dist + spread;
        if (s->want_dist < 300) s->want_dist = 300;
        s->dist_timer = (uint16_t)(66 + vs_rand(s) % 110u);      /* 3 - 8 s */
    }
    if (blade_only) s->want_dist = AI_BLADE_REACH - 150;

    /* Face */
    if (aerr > AI_FACE_OK) w |= err > 0 ? m_left : m_right;

    /* Evade: a missile launched at us, judged once each. */
    const unsigned fresh = vs_new_missiles(s, my, react);
    if (fresh) {
        psx_mod_counter_add(side ? "moa.vsai.p2.missile_seen" : "moa.vsai.p1.missile_seen", fresh);
        if (!s->evade && vs_pct(s, p->evade)) {
            s->evade = 22;                                        /* about 1 s */
            s->evade_dir = (vs_rand(s) & 1u) ? 1 : -1;
            psx_mod_counter_add(side ? "moa.vsai.p2.evade" : "moa.vsai.p1.evade", 1);
        }
    }

    /* Range and movement */
    const int off = (int)dist - s->want_dist;
    const int in_band = off > -AI_HOLD_BAND && off < AI_HOLD_BAND;
    uint16_t move = 0;
    int boost = 0;
    if (s->evade) {
        s->evade--;
        move = s->evade_dir > 0 ? m_sr : m_sl;
        boost = may_boost;                       /* sideways dash */
    } else {
        if (off > AI_HOLD_BAND && aerr < 700) move |= m_fwd;
        else if (off < -AI_HOLD_BAND && !blade_only) move |= m_back;

        /* Strafe runs while near the wanted distance. */
        if (s->strafe_timer) s->strafe_timer--;
        else {
            s->strafe_on = (uint8_t)vs_pct(s, p->strafe);
            s->strafe_timer = (uint16_t)(33 + vs_rand(s) % 44u);  /* 1.5 - 3.5 s */
        }
        if (!s->strafe_dir) s->strafe_dir = 1;
        if (vs_per_min(s, p->switches)) s->strafe_dir = -s->strafe_dir;
        if (s->strafe_on && (in_band || off < 0)) move |= s->strafe_dir > 0 ? m_sr : m_sl;

        /* Ground boost: far from the wanted distance, or by habit. */
        if (move && may_boost) {
            if (off > 1500 || off < -1200) boost = 1;
            else if (s->boosting) boost = vs_pct(s, 96);          /* keep a burst going */
            else boost = vs_pct(s, p->gboost / 4u);
        }

        /* Jumps */
        if (s->jump) {
            s->jump--;
            move = 0;
            boost = s->jump >= s->jump_rise ? 0 : (may_boost || s->jump + 2 > s->jump_rise);
        } else if (!airborne && !busy && vs_per_min(s, p->jumps) && energy > floor_e) {
            s->jump_rise = (uint8_t)(4 + p->air / 4u);           /* rise steps */
            s->jump = (uint8_t)(s->jump_rise + 1);                /* 1 still step first */
            move = 0;
            psx_mod_counter_add(side ? "moa.vsai.p2.jump" : "moa.vsai.p1.jump", 1);
        }
    }
    /* ✕ with no direction while standing jumps; a boost needs a step of moving first. */
    if (boost && move && !moving && !airborne) boost = 0;
    if (boost && !move && !s->jump && !s->evade && !airborne) boost = 0;
    w |= move;
    if (boost) w |= m_boost;
    s->boosting = (uint8_t)(boost != 0);

    /* Weapon: the liked kind for this distance band, held steady a moment. */
    const unsigned band = vs_band(dist);
    if (band == s->band) { if (s->band_steps < 0xFFFFu) s->band_steps++; }
    else { s->band = (uint8_t)band; s->band_steps = 0; }
    if (s->weapon_wait) s->weapon_wait--;
    else if (usable > 1 && !s->firing && !psx_mod_read_byte(my + AC_VOLLEY)) {
        const int best = vs_best_slot(s, me, band, dist);
        if (best >= 0 && (unsigned)best != sel && (s->band_steps > 15 || ammo == 0) &&
            !(s->last_word & m_weapon)) {
            w |= m_weapon;
            s->weapon_wait = 12;
            psx_mod_counter_add(side ? "moa.vsai.p2.weapon" : "moa.vsai.p1.weapon", 1);
        }
    }

    /* Fire */
    if (ammo > 0 && !(w & m_weapon)) {
        if (kind == VS_KIND_MISSILE) {
            const unsigned max_locks = psx_mod_read_byte(sel_slot + SLOT_MAX_LOCKS);
            const unsigned free_locks = psx_mod_read_half(me + PB_LOCKS_FREE);
            const unsigned held = max_locks > free_locks ? max_locks - free_locks : 0;
            unsigned want = p->locks ? p->locks : max_locks;
            if (want > max_locks) want = max_locks;
            if (want > (unsigned)ammo) want = (unsigned)ammo;
            if (want == 0) want = 1;
            if (s->firing) s->firing--;
            else if (held >= want || (held && !locked)) {
                s->firing = 3;
                psx_mod_counter_add(side ? "moa.vsai.p2.volley" : "moa.vsai.p1.volley", 1);
            }
            if (s->firing) w |= m_fire;
        } else if ((locked || aerr < AI_FIRE_CONE) && vs_pct(s, p->fire)) {
            w |= m_fire;
        }
    }

    /* Blade: new press, facing, in reach, not already swinging. */
    if (has_blade && !busy && dist < AI_BLADE_REACH && aerr < 300 && energy > red &&
        !(s->last_word & m_blade) &&
        (blade_only || vs_per_min(s, p->blade))) {
        w |= m_blade;
        psx_mod_counter_add(side ? "moa.vsai.p2.blade" : "moa.vsai.p1.blade", 1);
    }

    s->last_word = w;
    return w;
}

static void vs_reset(void)
{
    for (unsigned i = 0; i < 2; i++) {
        const uint32_t rng = vs_side[i].rng;
        memset(&vs_side[i], 0, sizeof vs_side[i]);
        vs_load_profile(i);
        vs_side[i].rng = rng ? rng : 0x9E3779B9u ^ (i * 0x85EBCA6Bu);
    }
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
    static uint16_t last_steps = 0xFFFFu;
    const uint16_t steps = psx_mod_read_half(VS_STEPS);
    if (steps < last_steps) {
        vs_reset();
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
        const uint16_t w = vs_ai_think(side, me, foe);
        psx_mod_write_half(me + PB_RAW, w);
        psx_mod_write_half(me + PB_HELD, frozen ? 0 : w);
        psx_mod_counter_add(side ? "moa.vsai.p2.steps" : "moa.vsai.p1.steps", 1);
    }
    return 1;
}

PSX_MOD_CONSTRUCTOR(moa_vs_ai_register)
{
    (void)psx_game_register_offline_function_filter(VS_SAMPLER, vs_sampler_filter);
}
