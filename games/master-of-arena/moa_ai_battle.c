/* Master of Arena: AI Battle (computer against computer).
 *
 * The arena engine has two AC slots. Slot 1 is flown from the controller;
 * slot 2 by the opponent AI, which writes a button word for it every frame
 * (both ACs then run through the same flying code). The AI is written as
 * "slot 2 fights slot 1" with every address fixed.
 *
 * To let the same AI fly slot 1 as well, each AI entry point is run a second
 * time with the two sides exchanged in memory: the two slot blocks and the
 * two AC objects trade places (pointers between them are rebased), and the
 * AI's own state is exchanged with a second copy kept here. After the call
 * everything is put back, so the button word the AI wrote for "slot 2" is
 * now in slot 1.
 *
 * Offline only (psx_game_register_offline_*), and only when the launcher asked
 * for it (PSX_MOA_AI_BATTLE=1). Every engine of this game loads at the same
 * address, so each hook first checks that the arena engine is in memory.
 *
 * The second AI state is host memory: savestates and rewind do not cover it.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOA_MODE_BYTE     0x8003EA2Au  /* engine the exe launches next */
#define MOA_MODE_ARENA    3u

#define MOA_SLOT1         0x80045198u  /* player block; slot 2 follows it */
#define MOA_SLOT_BYTES    928u
#define MOA_SLOT_PAD      4u           /* button word the AC code reads */
#define MOA_SLOT_PAD_PREV 6u
#define MOA_SLOT_OBJECT   920u         /* pointer to the slot's AC object */
#define MOA_OBJECT_BYTES  376u

#define MOA_AI_RECORD     0x8004AC80u  /* 36 bytes handed over by the menu */
#define MOA_AI_RECORD_BYTES 36u
#define MOA_AI_STATE      0x801E2730u  /* AI working memory and settings */
#define MOA_AI_STATE_END  0x801E29C4u
#define MOA_AI_TABLES     0x80098890u  /* per-opponent tables the setup fills */
#define MOA_AI_TABLES_END 0x80098A70u

/* Arena engine (FDAT entry 210). */
#define MOA_AI_FRAME      0x8008D4ACu  /* opponent AI, once per frame */
#define MOA_AI_INIT       0x8008DC98u  /* clears the AI state at match start */
#define MOA_AI_SETUP      0x80055768u  /* AI record -> working settings */
#define MOA_SIG_AI_FRAME  0x27BDFFB0u
#define MOA_SIG_AI_INIT   0x27BDFFE0u
#define MOA_SIG_AI_SETUP  0x27BDFFB0u

#define MOA_PAD_START     0x0008u

static int moa_ai_enabled = -1;
static int moa_ai_nested;

static uint8_t moa_side_state[MOA_AI_STATE_END - MOA_AI_STATE];
static uint8_t moa_side_tables[MOA_AI_TABLES_END - MOA_AI_TABLES];
static uint8_t moa_side_record[MOA_AI_RECORD_BYTES];
static int moa_side_record_set;

/* The launcher sets PSX_MOA_AI_BATTLE=1 for an AI Battle session. */
static int moa_ai_battle_on(void)
{
    if (moa_ai_enabled < 0) {
        const char *e = getenv("PSX_MOA_AI_BATTLE");
        moa_ai_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return moa_ai_enabled;
}

static int moa_arena_engine(uint32_t address, uint32_t signature)
{
    return moa_ai_battle_on() &&
           psx_mod_read_byte(MOA_MODE_BYTE) == MOA_MODE_ARENA &&
           psx_mod_read_word(address) == signature;
}

static void moa_exchange_host(uint32_t address, uint8_t *side, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++) {
        const uint8_t guest = psx_mod_read_byte(address + i);
        psx_mod_write_byte(address + i, side[i]);
        side[i] = guest;
    }
}

static void moa_exchange_guest(uint32_t a, uint32_t b, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i += 4) {
        const uint32_t wa = psx_mod_read_word(a + i);
        psx_mod_write_word(a + i, psx_mod_read_word(b + i));
        psx_mod_write_word(b + i, wa);
    }
}

/* A pointer into one side's block now has to mean the other side's. */
static void moa_rebase(uint32_t block, uint32_t bytes, uint32_t object1)
{
    const uint32_t object2 = object1 + MOA_OBJECT_BYTES;
    const uint32_t slot2 = MOA_SLOT1 + MOA_SLOT_BYTES;
    for (uint32_t i = 0; i < bytes; i += 4) {
        const uint32_t v = psx_mod_read_word(block + i);
        uint32_t r = v;
        if (v >= object1 && v < object2) r = v + MOA_OBJECT_BYTES;
        else if (v >= object2 && v < object2 + MOA_OBJECT_BYTES) r = v - MOA_OBJECT_BYTES;
        else if (v >= MOA_SLOT1 && v < slot2) r = v + MOA_SLOT_BYTES;
        else if (v >= slot2 && v < slot2 + MOA_SLOT_BYTES) r = v - MOA_SLOT_BYTES;
        if (r != v) psx_mod_write_word(block + i, r);
    }
}

/* Trade the two sides. Its own inverse. `object1` is slot 1's AC object, or
 * zero before the objects exist (match setup). */
static void moa_trade_sides(uint32_t object1)
{
    const uint32_t slot2 = MOA_SLOT1 + MOA_SLOT_BYTES;
    moa_exchange_guest(MOA_SLOT1, slot2, MOA_SLOT_BYTES);
    if (object1) {
        moa_exchange_guest(object1, object1 + MOA_OBJECT_BYTES, MOA_OBJECT_BYTES);
        moa_rebase(MOA_SLOT1, 2u * MOA_SLOT_BYTES, object1);
        moa_rebase(object1, 2u * MOA_OBJECT_BYTES, object1);
    }
    moa_exchange_host(MOA_AI_STATE, moa_side_state, sizeof moa_side_state);
    moa_exchange_host(MOA_AI_TABLES, moa_side_tables, sizeof moa_side_tables);
    moa_exchange_host(MOA_AI_RECORD, moa_side_record, sizeof moa_side_record);
}

/* The AC objects sit side by side, slot 1's first. Zero until both exist. */
static uint32_t moa_object1(void)
{
    const uint32_t o1 = psx_mod_read_word(MOA_SLOT1 + MOA_SLOT_OBJECT);
    const uint32_t o2 = psx_mod_read_word(MOA_SLOT1 + MOA_SLOT_BYTES + MOA_SLOT_OBJECT);
    if (o1 < 0x80100000u || o1 >= 0x80200000u || (o1 & 3u)) return 0;
    return o2 == o1 + MOA_OBJECT_BYTES ? o1 : 0;
}

/* Run the hooked function for slot 2 as the game does, then once more with
 * the sides traded so it does the same work for slot 1. */
static void moa_run_both(struct CPUState *cpu, uint32_t address)
{
    const uint32_t ra = cpu->gpr[31];
    const uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5];
    const uint32_t a2 = cpu->gpr[6], a3 = cpu->gpr[7];
    moa_ai_nested = 1;
    (void)psx_mod_call_guest(cpu, address, ra, a0, a1, a2, a3);
    const uint32_t object1 = moa_object1();
    moa_trade_sides(object1);
    (void)psx_mod_call_guest(cpu, address, ra, a0, a1, a2, a3);
    moa_trade_sides(object1);
    moa_ai_nested = 0;
}

/* How often the AI holds each button, per slot (mod_counters). */
static const char *const moa_bits1[16] = {
    "moa.ai.u1.select", "moa.ai.u1.b1", "moa.ai.u1.b2", "moa.ai.u1.start",
    "moa.ai.u1.up", "moa.ai.u1.right", "moa.ai.u1.down", "moa.ai.u1.left",
    "moa.ai.u1.l2", "moa.ai.u1.r2", "moa.ai.u1.l1", "moa.ai.u1.r1",
    "moa.ai.u1.triangle", "moa.ai.u1.circle", "moa.ai.u1.cross", "moa.ai.u1.square" };
static const char *const moa_bits2[16] = {
    "moa.ai.u2.select", "moa.ai.u2.b1", "moa.ai.u2.b2", "moa.ai.u2.start",
    "moa.ai.u2.up", "moa.ai.u2.right", "moa.ai.u2.down", "moa.ai.u2.left",
    "moa.ai.u2.l2", "moa.ai.u2.r2", "moa.ai.u2.l1", "moa.ai.u2.r1",
    "moa.ai.u2.triangle", "moa.ai.u2.circle", "moa.ai.u2.cross", "moa.ai.u2.square" };

static void moa_count_bits(uint16_t pad, const char *const *names)
{
    for (unsigned i = 0; i < 16; i++)
        if (pad & (1u << i)) psx_mod_counter_add(names[i], 1);
}

static int moa_ai_setup_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_ai_nested || !moa_arena_engine(address, MOA_SIG_AI_SETUP)) return 0;
    if (!moa_side_record_set)
        for (unsigned i = 0; i < MOA_AI_RECORD_BYTES; i++)
            moa_side_record[i] = psx_mod_read_byte(MOA_AI_RECORD + i);
    /* Unit 1's AI starts from the same engine data as unit 2's: the tables
     * hold constants the setup only partly rewrites. */
    for (uint32_t i = 0; i < sizeof moa_side_state; i++)
        moa_side_state[i] = psx_mod_read_byte(MOA_AI_STATE + i);
    for (uint32_t i = 0; i < sizeof moa_side_tables; i++)
        moa_side_tables[i] = psx_mod_read_byte(MOA_AI_TABLES + i);
    moa_run_both(cpu, address);
    psx_mod_counter_add("moa.ai.setup", 1);
    return 1;
}

static int moa_ai_init_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_ai_nested || !moa_arena_engine(address, MOA_SIG_AI_INIT)) return 0;
    moa_run_both(cpu, address);
    psx_mod_counter_add("moa.ai.init", 1);
    return 1;
}

static int moa_ai_frame_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_ai_nested || !moa_arena_engine(address, MOA_SIG_AI_FRAME)) return 0;
    /* The controller sampler has already moved slot 1's word to "previous"
     * and stored the controller in its place. Put last frame's word back so
     * the AI's own previous/current step sees what slot 2's would. */
    const uint16_t human = psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_PAD);
    psx_mod_write_half(MOA_SLOT1 + MOA_SLOT_PAD,
                       psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_PAD_PREV));
    moa_run_both(cpu, address);
    /* Start still reaches the game, for the pause menu. */
    psx_mod_write_half(MOA_SLOT1 + MOA_SLOT_PAD,
        (uint16_t)((psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_PAD) & ~MOA_PAD_START) |
                   (human & MOA_PAD_START)));
    psx_mod_counter_add("moa.ai.frame", 1);
    moa_count_bits(psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_PAD), moa_bits1);
    moa_count_bits(psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_BYTES + MOA_SLOT_PAD), moa_bits2);
    return 1;
}

/* ---- units ------------------------------------------------------------------
 *
 * Test roster: every VS START draws two arena opponents from the player's own
 * disc (MS\MENU_TIM.T holds each arena's AC and AI tables) and enters them as
 * unit 1 and unit 2 of the versus menu, each with its own AI. The versus menu
 * then launches the arena engine in place of the split-screen one.
 */
#define MOA_MENU_VS_START   0x800B2D7Cu  /* menu: VS START pressed */
#define MOA_SIG_VS_START    0x27BDFFD0u
#define MOA_MODE_VERSUS     2u
#define MOA_ARENA_ENTRIES   0x800D5A08u  /* menu: opponents per arena */
#define MOA_ARENA_STARTS    0x800D5A14u  /* menu: first opponent per arena */
#define MOA_ARENAS          9u           /* main arena and the eight Ex arenas */
#define MOA_VS_PARTS        0x80045940u  /* 12 part ids per player */
#define MOA_VS_PLUS         0x8004A7D0u  /* 8 bytes of Human PLUS state per player */
#define MOA_VS_PAINT        0x8004B730u  /* 48 colour bytes per player */
#define MOA_PAINT_BYTES      48u
#define MOA_VS_OPTIONS      0x8003EAC0u  /* 16-bit optional-parts mask per player */
#define MOA_RECORD_BYTES    64u
#define MOA_MENU_TIM_PATH   "GG/MS/MENU_TIM.T"

static uint8_t *moa_menu_tim;
static uint32_t moa_menu_tim_bytes;

static const uint8_t *moa_menu_tim_entry(uint32_t index, uint32_t *bytes)
{
    *bytes = 0;
    if (!moa_menu_tim) {
        uint32_t size = 0;
        if (!psx_mod_read_disc_file(MOA_MENU_TIM_PATH, NULL, 0, &size) || size < 4096u) {
            psx_mod_counter_add("moa.ai.disc_file_missing", 1);
            return NULL;
        }
        uint8_t *data = (uint8_t *)malloc(size);
        if (!data) return NULL;
        if (!psx_mod_read_disc_file(MOA_MENU_TIM_PATH, data, size, &moa_menu_tim_bytes)) {
            free(data);
            return NULL;
        }
        moa_menu_tim = data;
    }
    /* Header: two words, then one 16-bit sector offset per entry. */
    if (index + 4u > 1023u) return NULL;
    const uint8_t *t = moa_menu_tim + 4u + 2u * index;
    const uint32_t a = (uint32_t)(t[0] | (t[1] << 8)) * 2048u;
    const uint32_t b = (uint32_t)(t[2] | (t[3] << 8)) * 2048u;
    if (b <= a || b > moa_menu_tim_bytes) {
        psx_mod_counter_add("moa.ai.disc_entry_bad", 1);
        return NULL;
    }
    *bytes = b - a;
    return moa_menu_tim + a;
}

/* One arena opponent: its 64-byte AC record and 36-byte AI record. */
static int moa_arena_unit(uint32_t arena, uint32_t rank,
                          uint8_t *record, uint8_t *ai)
{
    const uint32_t entries = psx_mod_read_byte(MOA_ARENA_ENTRIES + arena);
    const uint32_t start = psx_mod_read_byte(MOA_ARENA_STARTS + arena);
    if (rank >= entries) return 0;
    const uint32_t base = (start + entries) * 4u + arena * 4u + 140u - 1u;
    uint32_t record_bytes = 0, ai_bytes = 0;
    const uint8_t *records = moa_menu_tim_entry(base, &record_bytes);
    const uint8_t *ais = moa_menu_tim_entry(base + 3u, &ai_bytes);
    if (!records || !ais || (rank + 1u) * MOA_RECORD_BYTES > record_bytes ||
        (rank + 1u) * MOA_AI_RECORD_BYTES > ai_bytes)
        return 0;
    memcpy(record, records + rank * MOA_RECORD_BYTES, MOA_RECORD_BYTES);
    memcpy(ai, ais + rank * MOA_AI_RECORD_BYTES, MOA_AI_RECORD_BYTES);
    /* An unused slot has no core. */
    return record[0] != 0xFFu && record[1] != 0xFFu;
}

static void moa_enter_unit(unsigned player, const uint8_t *r, const uint8_t *ai)
{
    /* Arena record order -> the garage's: core, booster, head, arms, blade,
     * hand weapon, back left, back right, generator, FCS, (none), legs. */
    const uint8_t parts[12] = { r[0], r[12], r[1], r[2], r[5], r[4],
                                r[6], r[7], r[9], r[8], 0xFFu, r[3] };
    for (unsigned i = 0; i < 12; i++)
        psx_mod_write_byte(MOA_VS_PARTS + 12u * player + i, parts[i]);
    /* Paint: four sections of base, optional, detail and joint colour, as
     * the garage's sliders show them. */
    for (unsigned i = 0; i < MOA_PAINT_BYTES; i++)
        psx_mod_write_byte(MOA_VS_PAINT + MOA_PAINT_BYTES * player + i, r[16u + i]);
    /* Optional parts: one bit per part, in the option table's order. */
    psx_mod_write_half(MOA_VS_OPTIONS + 2u * player, (uint16_t)(r[10] | (r[11] << 8)));
    /* Human PLUS level from the AI record; arena builds may be overweight. */
    const uint8_t plus[8] = { ai[0], 0, 1, 0, 0, 0, 0, 0 };
    for (unsigned i = 0; i < 8; i++)
        psx_mod_write_byte(MOA_VS_PLUS + 8u * player + i, plus[i]);
}

/* Paint is baked into colour tables in video memory, one set per player.
 * Player 1's set is the menu's working set; the game swaps the two sets to
 * work on player 2's. The paint routine recolours the working set from the
 * garage's current colours. */
#define MOA_GARAGE_PAINT    0x80036FBCu  /* current AC: 48 colour bytes */
#define MOA_PAINT_APPLY     0x80082530u  /* garage colours -> working colour tables */
#define MOA_SIG_PAINT_APPLY 0x27BDFFD0u
#define MOA_AC_SWAP         0x800C0E7Cu  /* swap the two players' AC graphics */
#define MOA_SIG_AC_SWAP     0x27BDFFC0u

static void moa_paint_units(struct CPUState *cpu, const uint8_t *paint1,
                            const uint8_t *paint2)
{
    if (psx_mod_read_word(MOA_PAINT_APPLY) != MOA_SIG_PAINT_APPLY ||
        psx_mod_read_word(MOA_AC_SWAP) != MOA_SIG_AC_SWAP)
        return;
    const uint32_t ra = cpu->gpr[31];
    uint8_t keep[MOA_PAINT_BYTES];
    for (unsigned i = 0; i < MOA_PAINT_BYTES; i++)
        keep[i] = psx_mod_read_byte(MOA_GARAGE_PAINT + i);
    for (unsigned i = 0; i < MOA_PAINT_BYTES; i++)
        psx_mod_write_byte(MOA_GARAGE_PAINT + i, paint1[i]);
    (void)psx_mod_call_guest(cpu, MOA_PAINT_APPLY, ra, 0, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, MOA_AC_SWAP, ra, 0, 0, 0, 0);
    for (unsigned i = 0; i < MOA_PAINT_BYTES; i++)
        psx_mod_write_byte(MOA_GARAGE_PAINT + i, paint2[i]);
    (void)psx_mod_call_guest(cpu, MOA_PAINT_APPLY, ra, 0, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, MOA_AC_SWAP, ra, 0, 0, 0, 0);
    for (unsigned i = 0; i < MOA_PAINT_BYTES; i++)
        psx_mod_write_byte(MOA_GARAGE_PAINT + i, keep[i]);
    psx_mod_counter_add("moa.ai.painted", 1);
}

static uint32_t moa_random(uint32_t below)
{
    static uint32_t state;
    if (!state) state = (uint32_t)time(NULL) | 1u;
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return below ? state % below : 0;
}

static void moa_vs_start_entry(struct CPUState *cpu, uint32_t address)
{
    moa_ai_enabled = -1;                       /* read the option per match */
    if (!moa_ai_battle_on() || psx_mod_read_word(address) != MOA_SIG_VS_START)
        return;
    const uint8_t mode = psx_mod_read_byte(MOA_MODE_BYTE);
    if (mode != MOA_MODE_VERSUS && mode != MOA_MODE_ARENA) return;
    uint8_t record[2][MOA_RECORD_BYTES], ai[2][MOA_AI_RECORD_BYTES];
    uint32_t picked[2] = { 0, 0 };
    for (unsigned unit = 0; unit < 2; unit++) {
        int ok = 0;
        for (unsigned tries = 0; tries < 64 && !ok; tries++) {
            const uint32_t arena = moa_random(MOA_ARENAS);
            const uint32_t entries = psx_mod_read_byte(MOA_ARENA_ENTRIES + arena);
            if (entries < 2u) continue;
            const uint32_t rank = moa_random(entries - 1u);  /* last = player's slot */
            picked[unit] = arena * 256u + rank;
            if (unit == 1 && picked[1] == picked[0]) continue;
            ok = moa_arena_unit(arena, rank, record[unit], ai[unit]);
        }
        if (!ok) {
            psx_mod_counter_add("moa.ai.roster_failed", 1);
            return;                            /* stock versus match instead */
        }
    }
    moa_enter_unit(0, record[0], ai[0]);
    moa_enter_unit(1, record[1], ai[1]);
    moa_paint_units(cpu, record[0] + 16u, record[1] + 16u);
    memcpy(moa_side_record, ai[0], MOA_AI_RECORD_BYTES);
    moa_side_record_set = 1;
    for (unsigned i = 0; i < MOA_AI_RECORD_BYTES; i++)
        psx_mod_write_byte(MOA_AI_RECORD + i, ai[1][i]);
    psx_mod_write_byte(MOA_MODE_BYTE, MOA_MODE_ARENA);
    psx_mod_counter_add("moa.ai.match", 1);
}

/* ---- straight to the versus menu ----------------------------------------------
 *
 * An AI Battle session goes from boot to the versus menu by itself. The menu
 * module's input routines are fed a tapped START until the versus menu opens:
 * that skips the logos and the intro video and passes the title screen. The
 * mode choice that follows ("Scenario Mode / VS Mode") is settled when its
 * confirm handler runs: the highlighted row is made the versus one.
 */
#define MOA_MENU_ENTRY      0x800A69C8u  /* menu module entry */
#define MOA_SIG_MENU_ENTRY  0x27BDFEB0u
#define MOA_MENU_VERSUS     0x800B0CB4u  /* menu: versus setup */
#define MOA_SIG_MENU_VERSUS 0x27BDFFC0u
#define MOA_MENU_INPUT_A    0x800639A4u  /* menu: controller sampling */
#define MOA_SIG_INPUT_A     0x27BDFFE8u
#define MOA_MENU_INPUT_B    0x800AF590u
#define MOA_SIG_INPUT_B     0x27BDFFD0u
#define MOA_MODE_CONFIRM    0x800AB500u  /* title: mode row confirmed */
#define MOA_SIG_MODE_CONFIRM 0x27BDFFC8u
#define MOA_MODE_ROW_Y      80u          /* row widget: 176 scenario, 192 versus */
#define MOA_MODE_ROW_VERSUS 192u
#define MOA_MENU_FLAGS      0x801B1CD0u  /* bit 9: second controller port usable */
#define MOA_PAD_BUFFER      0x800458D8u  /* port 1: status, id, buttons (low = held) */

static int moa_autopilot;
static uint32_t moa_autopilot_ticks;

static void moa_menu_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    moa_ai_enabled = -1;
    if (!moa_ai_battle_on() || psx_mod_read_word(address) != MOA_SIG_MENU_ENTRY)
        return;
    moa_autopilot = 1;
    moa_autopilot_ticks = 0;
    psx_mod_counter_add("moa.ai.menu_entry", 1);
}

static void moa_menu_versus_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (psx_mod_read_word(address) == MOA_SIG_MENU_VERSUS) moa_autopilot = 0;
}

static void moa_menu_input_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!moa_autopilot || !moa_ai_battle_on()) return;
    const uint32_t word = psx_mod_read_word(address);
    if (word != MOA_SIG_INPUT_A && word != MOA_SIG_INPUT_B) return;
    /* START held for 3 samples out of every 24. */
    const int held = (moa_autopilot_ticks++ % 24u) < 3u;
    psx_mod_write_half(MOA_PAD_BUFFER + 2u,
                       held ? (uint16_t)~MOA_PAD_START : (uint16_t)0xFFFFu);
}

/* The movie player polls the controller inside its own loop and returns 1
 * when the player skips. During the autopilot every movie is "skipped"
 * before it starts. */
#define MOA_MOVIE_PLAY      0x80054B54u
#define MOA_SIG_MOVIE_PLAY  0x27BDFFD8u

static int moa_movie_filter(struct CPUState *cpu, uint32_t address)
{
    if (!moa_autopilot || !moa_ai_battle_on() ||
        psx_mod_read_word(address) != MOA_SIG_MOVIE_PLAY)
        return 0;
    cpu->gpr[2] = 1;
    psx_mod_counter_add("moa.ai.movie_skipped", 1);
    return 1;
}

/* The title sequence takes one argument: 0 plays the publisher logos and the
 * intro video first, 1 opens the title screen at once (the game's own path
 * back from the versus menu). */
#define MOA_TITLE_OPEN      0x800AAE94u
#define MOA_SIG_TITLE_OPEN  0x27BDFFE0u

static void moa_title_open_entry(struct CPUState *cpu, uint32_t address)
{
    if (!moa_autopilot || !moa_ai_battle_on() ||
        psx_mod_read_word(address) != MOA_SIG_TITLE_OPEN)
        return;
    cpu->gpr[4] = 1;
    psx_mod_counter_add("moa.ai.logos_skipped", 1);
}

static void moa_mode_confirm_entry(struct CPUState *cpu, uint32_t address)
{
    if (!moa_autopilot || !moa_ai_battle_on() ||
        psx_mod_read_word(address) != MOA_SIG_MODE_CONFIRM)
        return;
    const uint32_t row = psx_mod_read_word(cpu->gpr[4] + 148u);
    if (row < 0x80000000u || row >= 0x80200000u) return;
    psx_mod_write_word(row + MOA_MODE_ROW_Y, MOA_MODE_ROW_VERSUS);
    psx_mod_write_word(MOA_MENU_FLAGS, psx_mod_read_word(MOA_MENU_FLAGS) | 0x200u);
    psx_mod_counter_add("moa.ai.mode_forced", 1);
}

PSX_MOD_CONSTRUCTOR(moa_ai_battle_register)
{
    (void)psx_game_register_offline_function_entry(MOA_MENU_ENTRY, moa_menu_entry);
    (void)psx_game_register_offline_function_entry(MOA_MENU_VERSUS, moa_menu_versus_entry);
    (void)psx_game_register_offline_function_entry(MOA_MENU_INPUT_A, moa_menu_input_entry);
    (void)psx_game_register_offline_function_entry(MOA_MENU_INPUT_B, moa_menu_input_entry);
    (void)psx_game_register_offline_function_entry(MOA_MODE_CONFIRM, moa_mode_confirm_entry);
    (void)psx_game_register_offline_function_entry(MOA_TITLE_OPEN, moa_title_open_entry);
    (void)psx_game_register_offline_function_filter(MOA_MOVIE_PLAY, moa_movie_filter);
    (void)psx_game_register_offline_function_entry(MOA_MENU_VS_START, moa_vs_start_entry);
    (void)psx_game_register_offline_function_filter(MOA_AI_SETUP, moa_ai_setup_filter);
    (void)psx_game_register_offline_function_filter(MOA_AI_INIT, moa_ai_init_filter);
    (void)psx_game_register_offline_function_filter(MOA_AI_FRAME, moa_ai_frame_filter);
}
