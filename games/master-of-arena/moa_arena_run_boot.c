/* Master of Arena: Arena Run, straight to the hub.
 *
 * An Arena Run session goes from boot to the scenario hub by itself, the way
 * an AI Battle session goes to the versus menu (moa_ai_battle.c): fast
 * forward, the controllers locked out and a loading screen over the picture
 * while the menus are driven underneath.
 *
 *   boot -> logos and intro skipped (title opened with a0 = 1, movies
 *   "skipped"), START tapped through the title, the mode row forced to
 *   SCENARIO MODE; on LOAD GAME / NEW GAME the cursor is set to LOAD GAME
 *   and X tapped (that menu ignores START), then X through the load screens
 *   (slot 1, the one save, "Load the saved data?" yes) -> the hub opens,
 *   the overlay fades and the player takes over.
 *
 * Slot 1 holds the run's own card for the session (PSX_MEMCARD1_SESSION,
 * written by the launcher: moa_run_write_card), so the game loads the run's
 * AC, names, P and parts itself, and in-game saves go to that card. The
 * player's own card is not in the machine.
 *
 * If the hub does not come up in time the overlay is dropped and the
 * player is given the menus, so a wrong step can never leave a black screen.
 *
 * Offline only, and only when the launcher asked for it (PSX_MOA_ARENA_RUN=1).
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "moa_arena_run.h"

/* Shared with AI Battle (see moa_ai_battle.c for how each was found). */
#define RUN_MENU_ENTRY       0x800A69C8u  /* menu module entry */
#define RUN_SIG_MENU_ENTRY   0x27BDFEB0u
#define RUN_MENU_INPUT_A     0x800639A4u  /* menu: controller sampling */
#define RUN_SIG_INPUT_A      0x27BDFFE8u
#define RUN_MENU_INPUT_B     0x800AF590u
#define RUN_SIG_INPUT_B      0x27BDFFD0u
#define RUN_MODE_CONFIRM     0x800AB500u  /* title: mode row confirmed */
#define RUN_SIG_MODE_CONFIRM 0x27BDFFC8u
#define RUN_MODE_ROW_Y       80u          /* row widget: 176 scenario, 192 versus */
#define RUN_MODE_ROW_SCENARIO 176u
#define RUN_TITLE_OPEN       0x800AAE94u  /* a0 = 1: no logos or intro */
#define RUN_SIG_TITLE_OPEN   0x27BDFFE0u
#define RUN_MOVIE_PLAY       0x80054B54u  /* returns 1 when skipped */
#define RUN_SIG_MOVIE_PLAY   0x27BDFFD8u
#define RUN_PAD_BUFFER       0x800458D8u  /* port 1: status, id, buttons (low = held) */
#define RUN_PAD_START        0x0008u
#define RUN_PAD_X            0x4000u

/* LOAD GAME / NEW GAME (title module). Its frame routine reads the cursor
 * byte at state + 0xA2 (0 LOAD GAME, 1 NEW GAME; up/down toggle it) and on X
 * goes to 0x800AC820 for NEW GAME or 0x800AC8E8 for LOAD GAME; O goes back.
 * START is not read there. */
#define RUN_NEWGAME_FRAME    0x800AC4B4u
#define RUN_SIG_NEWGAME      0x27BDFFD0u  /* addiu sp,sp,-48 */
#define RUN_NEWGAME_CURSOR   0xA2u

/* The hub. CONFIRM: 0x80056528 is where the hub takes over again when the
 * mission screen closes (mod_builtin_moa_phantasma.c); it is taken here as
 * the sign the hub is up until the hub's own open routine is found. */
#define RUN_HUB_RESUME       0x80056528u
#define RUN_SIG_HUB_RESUME   0x27BDFFE8u


/* Arena result (menu module): runs on the arena screen each frame and, when
 * a fight has just finished (byte 0x8003EA7C), books it and clears that
 * byte with the halfwords below. a0 = screen state; arena number at
 * a0 + 0x80 + 0x5D, beaten flags pointer at a0 + 0x80 + 0x58. Before the
 * booking it waits on 0x800589DC, so the fight can be seen for a few frames
 * first: the hook books it once, on the first sight. */
#define RUN_RESULT           0x800BBEA8u
#define RUN_SIG_RESULT       0x27BDFFE0u  /* addiu sp,sp,-32 */
#define RUN_FIGHT_DONE       0x8003EA7Cu  /* byte: a fight just finished */
#define RUN_FIGHT_WON        0x8003EAB0u  /* half: non-zero on a win */
#define RUN_FIGHT_OPPONENT   0x8003EAD5u  /* byte: the opponent's index */
#define RUN_STATE_ARENA      (0x80u + 0x5Du)
#define RUN_STATE_FLAGS      (0x80u + 0x58u)

/* The game's save (menu module): RUN_SAVE(a0 = card port, a1 = file index
 * in the card list, or -1 for a new file) builds the 0xE80-byte scenario
 * block and writes it in one call (open, write, close); v0 (byte) is 0 on a
 * failure. Its one caller is the SAVE screen (0x8005CA7C). An existing
 * file keeps its name letter from the list: entry (port*0x8A0 + index*0x44)
 * of the table at RUN_SAVE_LIST, letter at +0x460, count at RUN_SAVE_COUNT.
 * The run card holds one save, BASLUS-01030A (launcher), so the autosave
 * writes file 0 on port 0 and only when the list still names that file;
 * index -1 would add a new file each time. */
/* The main arena's lock: the hub's Arena entry opens arena 0 with this byte
 * as the lock (0x800BA6C4 -> 0x800BB93C a1 -> state + 0x80 + 0x5E). A new
 * game holds it set until the story opens the arena ("You may not enter
 * this Arena now." / "Arena license suspended."); a run clears it. */
#define RUN_ARENA_LOCK       0x8004B686u

#define RUN_SAVE             0x8007155Cu
#define RUN_SIG_SAVE         0x27BDFF98u  /* addiu sp,sp,-104 */
#define RUN_SAVE_LIST        0x801F9DB8u
#define RUN_SAVE_COUNT       0x801B1CE4u  /* half */
#define RUN_SAVE_LETTER      0x460u
#define RUN_ARENA            0u           /* the run's opponents replace arena 0 */
#define RUN_CREDITS          0x8003EA74u  /* u32: the save routine copies it to save +456 (0x80071BBC); 0x8003EA94 is the fight-count byte */
#define RUN_LOSS_FRAMES      (60u * 4u)   /* the result is shown, then the launcher */

/* Hub: X on an entry calls 0x80056414 (a0 = hub state). It reads the
 * entry at a0 + 0xBE (signed byte, 0-7) and jumps through the table at
 * 0x80050ED8; 2 opens Missions (0x800943B4), 5 the Arena and 6 the Ex Arena
 * (0x800BA64C with a0 = 0 / 1). Any other value takes the table's default:
 * set byte 1 of the state at a0 + 0xAC and go on with 0x80057C2C (the hub
 * waiting for input), which is what a blocked entry does.
 * CONFIRM in game: which of 0, 1, 3, 4, 7 are Garage and Shop (the counter
 * moa.run.hub_entry_N shows the entry each pick was). */
#define RUN_HUB_PICK         0x80056414u
#define RUN_SIG_HUB_PICK     0x27BDFFE8u
#define RUN_HUB_ENTRY        0xBEu
#define RUN_HUB_FLAGS        0xACu
#define RUN_HUB_NEXT         0x80057C2Cu
#define RUN_HUB_ARENA        5
/* Entries (test 3 log): 0 Garage, 1 Shop, 2 Missions, 3 System, 4 Mail,
 * 5 Arena, 6 Ex Arena, 7 Arena Maker. A run keeps Arena, Shop and Garage. */
#define RUN_HUB_ALLOWED      ((1u << 0) | (1u << 1) | (1u << 5))

/* Arena engine: the controller sampler runs only while the engine (a fight
 * or its replay) is up, so SELECT, which offers leaving the fight without
 * a result, is taken out there. START (pause) is left alone. */
#define RUN_FIGHT_SAMPLER    0x8007F69Cu
#define RUN_SIG_SAMPLER      0x27BDFFE8u
#define RUN_PAD_SELECT       0x0001u

/* Arena 0 as one 50-rank ladder. Stock arena 0 is two lists in one: ranks
 * 1-15 the ranked arena, 16-50 the sub arena (beaten flags only; the ranked
 * list opens once the rank is 15 or better). In a run there is no sub arena:
 * each site below takes the ranked arena's path for every rank. All guarded
 * by the instruction word and arena 0 (state + 0x80 + 0x5D).
 *   open      0x800BBA98 sb v0,0x60(s2): rows = the arena's count (s2+0x5F,
 *             50), view (s2+0x5C) = ranked
 *   win       0x800BBFB4 bnez a1 (0x8003EAC9, sub-arena fight): a1 = 0
 *             0x800BBFE0 sltiu v0,v0,16 (rank < 16): v0 = 1, so a win over
 *             anyone above moves the rank up one, as in the Ex arenas
 *   result    0x800BC110 beqz v0 (rank < 16): v0 = 1, cursor on the rank
 *   list      0x800BC274 beqz v0 (row < 15): v0 = 0, so beaten opponents
 *             stay beaten (stock clears ranks 1-15 for refights) */
#define RUN_A0_OPEN          0x800BBA98u
#define RUN_A0_OPEN_I        0xA2420060u
#define RUN_A0_SUBFIGHT      0x800BBFB4u
#define RUN_A0_SUBFIGHT_I    0x14A00036u
#define RUN_A0_RANKED        0x800BBFE0u
#define RUN_A0_RANKED_I      0x2C420010u
#define RUN_A0_CURSOR        0x800BC110u
#define RUN_A0_CURSOR_I      0x10400015u
#define RUN_A0_CLEAR         0x800BC274u
#define RUN_A0_CLEAR_I       0x10400005u

/* The run's opponents in arena 0 (GG/MS/MENU_TIM.T, file 4 of the table at
 * 0x8004402C: 18 slots of 12 bytes, word 0 the entry index in RAM, halfword
 * k = entry k's first sector in the file). Arena a's entries start at
 * 140 + 4*start[a] + 4*a (start at 0x800D5A14, count at 0x800D5A08): count
 * descriptions, count emblems (the last, the player's, empty), count
 * portraits, count second descriptions, then the whole arena's builds (64
 * bytes each), names (pilot then AC, 24 bytes each), rewards (8) and AI
 * (36). Every entry ends with its checksum (sum of its words + 0x12345678).
 * Project Phantasma's arena (10), when installed, starts at entry 888.
 *
 * Per-opponent entries are loaded one by one, through 0x800BDE88 (a0 =
 * entry) and 0x800586A0 (a0 = file, a1 = entry): the entry is pointed at the
 * source arena's own. The whole-arena entries are read (asynchronously) by
 * 0x800160C4 at four sites; once the game's CD queue is empty (checked where
 * the game itself checks it, 0x800589DC) they are rebuilt from the sources.
 * Arena 0's entry e is rank e + 1, so the run's slot 49 - e. */
#define RUN_FILE_TABLE       0x8004402Cu
#define RUN_FILE_COUNT       18u
#define RUN_ARENA_COUNTS     0x800D5A08u
#define RUN_ARENA_STARTS     0x800D5A14u
#define RUN_A0_FIRST         140u
#define RUN_PP_FIRST         888u
#define RUN_PP_ARENA         10u
#define RUN_LOAD_ONE         0x800BDE88u  /* a0 = entry */
#define RUN_LOAD_FILE        0x800586A0u  /* a0 = file, a1 = entry */
#define RUN_SIG_LOADER       0x27BDFFD8u
#define RUN_CD_BUSY          0x800589DCu
#define RUN_SIG_CD_BUSY      0x27BDFFE8u
#define RUN_CDQ              0x80043C70u  /* the game's CD queue: write +0x378, read +0x37C */
#define RUN_JAL_READ         0x0C005831u  /* jal 0x800160C4: a2 = bytes, a3 = buffer */
#define RUN_READ_NAMES       0x800BBB3Cu  /* arena screen open (arena in s4) */
#define RUN_READ_REWARDS     0x800BBCFCu
#define RUN_READ_BUILDS      0x800C0250u  /* fight setup */
#define RUN_READ_AI          0x800C04FCu
#define RUN_ARENA_AT_BUILDS  0x800C01FCu  /* lbu a3,0x5D(s1) */
#define RUN_ARENA_AT_BUILDS_I 0x9227005Du
#define RUN_ARENA_AT_AI      0x800C04A8u  /* lbu v1,0x5D(s2) */
#define RUN_ARENA_AT_AI_I    0x9243005Du
enum { KIND_BUILDS = 0, KIND_NAMES, KIND_REWARDS, KIND_AI };

#define RUN_HUB_TIMEOUT      (60u * 90u)  /* frames; fast forward makes this short */
#define RUN_FADE_FRAMES      24u

static int run_enabled = -1;
static int run_autopilot;                 /* driving the menus */
static int run_at_hub;                    /* the player has the hub */
static uint32_t run_ticks, run_frames, run_fade;
static struct moa_run_state run_state;
static int run_state_ok;
enum { STAGE_TITLE = 0, STAGE_LOADGAME, STAGE_LOADING, STAGE_BACK };
/* At the hub: one O press (one menu back), at normal speed so the hub's
 * music starts as it would, then hand over behind the loading screen. */
#define RUN_BACK_FRAMES      90u
#define RUN_PAD_CIRCLE       0x2000u
static uint32_t run_back_start;
static int run_stage;
static uint32_t run_newgame_seen;          /* run_frames when that menu last ran */
static int run_result_seen;                /* this fight's result is booked */
static int run_save_pending;               /* a win waits for the autosave */

/* A plain log beside the run file (<run file>.log), for test reports: the
 * mod counters are only visible through the debug server. */
static void run_log(const char *fmt, ...)
{
    const char *path = getenv("PSX_MOA_RUN_FILE");
    if (!path || !path[0]) return;
    char name[1024];
    snprintf(name, sizeof name, "%s.log", path);
    FILE *f = fopen(name, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static uint32_t run_loss_wait;             /* frames until the launcher reopens */
static const char *run_file_path(void) { return getenv("PSX_MOA_RUN_FILE"); }

static int run_on(void)
{
    if (run_enabled < 0) {
        const char *e = getenv("PSX_MOA_ARENA_RUN");
        run_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return run_enabled;
}

static void run_set_ff(int on)
{
    psx_mod_request_fast_forward(on);
    if (!on && !run_fade) psx_mod_set_loading_screen(NULL, 0);
}

/* The run's AC into the garage: the card record's 13 bytes in the versus
 * player's order (as moa_card_parts in moa_ai_battle.c). */
static void run_apply_state(void)
{
    /* The save carries the run; the run file is read for what comes next
     * (results, P, the arena). */
    const char *path = run_file_path();
    run_state_ok = path && path[0] && moa_run_load(path, &run_state) == 1;
    if (!run_state_ok) psx_mod_counter_add("moa.run.file_missing", 1);
}

static void run_finish_handover(void)
{
    run_autopilot = 0;
    run_at_hub = 1;
    run_apply_state();
    run_fade = RUN_FADE_FRAMES;
    psx_mod_counter_add("moa.run.handover", 1);
    run_log("handover: run file loaded %d, P %u", run_state_ok,
            run_state_ok ? (unsigned)run_state.run.points : 0u);
}

static void run_reach_hub(const char *why)
{
    if (!run_autopilot || run_stage == STAGE_BACK) return;
    run_stage = STAGE_BACK;
    run_back_start = run_frames;
    run_ticks = 0;
    psx_mod_request_fast_forward(0);
    psx_mod_counter_add(why, 1);
}

/* ---- hooks -------------------------------------------------------------------- */

static void run_menu_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    run_enabled = -1;
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_MENU_ENTRY) return;
    if (run_at_hub) return;                   /* already handed over */
    run_autopilot = 1;
    run_ticks = run_frames = 0;
    run_stage = STAGE_TITLE;
    run_set_ff(1);
    psx_mod_counter_add("moa.run.boot", 1);
}

static void run_title_open_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_TITLE_OPEN) return;
    if (run_at_hub) {                         /* the player quit to the title */
        psx_mod_counter_add("moa.run.return_from_title", 1);
        psx_mod_request_return_to_launcher();
        return;
    }
    if (run_autopilot) cpu->gpr[4] = 1;
}

static int run_movie_filter(struct CPUState *cpu, uint32_t address)
{
    if (!run_autopilot || !run_on() || psx_mod_read_word(address) != RUN_SIG_MOVIE_PLAY)
        return 0;
    cpu->gpr[2] = 1;
    return 1;
}

static void run_mode_confirm_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_autopilot || !run_on() || psx_mod_read_word(address) != RUN_SIG_MODE_CONFIRM)
        return;
    const uint32_t row = psx_mod_read_word(cpu->gpr[4] + 148u);
    if (row < 0x80000000u || row >= 0x80200000u) return;
    psx_mod_write_word(row + RUN_MODE_ROW_Y, RUN_MODE_ROW_SCENARIO);
    psx_mod_counter_add("moa.run.mode_scenario", 1);
}

static void run_newgame_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_autopilot || !run_on() || psx_mod_read_word(address) != RUN_SIG_NEWGAME) return;
    const uint32_t st = cpu->gpr[4];
    if (st < 0x80000000u || st >= 0x80200000u) return;
    psx_mod_write_byte(st + RUN_NEWGAME_CURSOR, 0);      /* LOAD GAME: the run's card */
    if (run_stage != STAGE_LOADGAME) psx_mod_counter_add("moa.run.load_game", 1);
    run_stage = STAGE_LOADGAME;
    run_newgame_seen = run_frames;
}

static void run_menu_input_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!run_autopilot || !run_on()) return;
    const uint32_t w = psx_mod_read_word(address);
    if (w != RUN_SIG_INPUT_A && w != RUN_SIG_INPUT_B) return;
    if (run_stage == STAGE_LOADGAME && run_frames - run_newgame_seen > 10u)
        run_stage = STAGE_LOADING;                       /* that menu has closed */
    /* One button held for 3 samples out of every 24: START on the title,
     * then X on LOAD GAME and through the load screens (all defaults: slot 1,
     * the card's one save, yes). */
    const uint32_t t = run_ticks++;
    if (run_stage == STAGE_BACK) {               /* one O press, once */
        psx_mod_write_half(RUN_PAD_BUFFER + 2u,
                           (t >= 6u && t < 9u) ? (uint16_t)~RUN_PAD_CIRCLE : (uint16_t)0xFFFFu);
        return;
    }
    const uint16_t b = run_stage == STAGE_TITLE ? RUN_PAD_START : RUN_PAD_X;
    psx_mod_write_half(RUN_PAD_BUFFER + 2u, (t % 24u) < 3u ? (uint16_t)~b : (uint16_t)0xFFFFu);
}


/* ---- step 2: P prices and shop restocks ------------------------------------
 *
 * Part tables in the menu module (moa_parts.c): rows of `stride` bytes, the
 * credit price a u32 at +36 (options too: 44-byte rows). The garage's owned
 * counts (0x80036FEC) and the shop's stock (0x800370EC) are 256-byte tables
 * indexed base + row (the save routine copies them to +640 and +896). */
#define RUN_OWNED            0x80036FECu
#define RUN_STOCK            0x800370ECu
#define RUN_PRICE_AT         36u
#define RUN_OPTIONS          0x800D1AA0u
#define RUN_OPTION_STRIDE    44u
#define RUN_OPTION_COUNT     12u
#define RUN_RESTOCK_PARTS    8

static const struct { uint32_t addr, stride, end; uint16_t base; uint8_t back; } k_run_tables[] = {
    { 0x800CE690u, 60, 0x800CEAC8u,   0, 0 },   /* head */
    { 0x800CEAC8u, 56, 0x800CED30u,  18, 0 },   /* core */
    { 0x800CED30u, 72, 0x800CF870u,  53, 0 },   /* legs */
    { 0x800CF870u, 52, 0x800CFD50u,  29, 0 },   /* arms */
    { 0x800CFD50u, 44, 0x800D01C8u, 217, 0 },   /* hand weapon */
    { 0x800D01C8u, 52, 0x800D0DF8u, 157, 1 },   /* back (stock 2: stays on sale) */
    { 0x800D0DF8u, 52, 0x800D0F30u, 243, 0 },   /* blade */
    { 0x800D0F30u, 48, 0x800D11D0u, 143, 0 },   /* booster */
    { 0x800D11D0u, 96, 0x800D17D0u, 108, 0 },   /* FCS */
    { 0x800D17D0u, 48, 0x800D1AA0u,  93, 0 },   /* generator */
};

/* Credits to P, by where the price sits among all 174 parts (USA disc):
 * bottom 15% 1 P, to 30% 2, to 50% 3, to 70% 4, to 85% 5, to 95% 6, top 7.
 * Average about 3.5 P, so 49 P buys about 14 parts. */
static uint32_t run_tier(uint32_t credits)
{
    static const uint32_t k_at[6] = { 23000u, 32000u, 48000u, 67000u, 89000u, 138000u };
    uint32_t p = 1;
    for (int i = 0; i < 6; i++) if (credits >= k_at[i]) p++;
    return p;
}

static int run_row_real(uint32_t row)
{
    static const char k_dummy[5] = { 'D', 'U', 'M', 'M', 'Y' };
    for (int i = 0; i < 5; i++) if (psx_mod_read_byte(row + (uint32_t)i) != (uint8_t)k_dummy[i]) return 1;
    return 0;
}

/* Rewrite the shop's prices as P. The tables are part of the menu module, so
 * this runs whenever the hub opens (the module may have been reloaded); a
 * price already in P (7 or less, every real price is 10,800 or more) is left. */
static void run_apply_prices(void)
{
    if (psx_mod_read_word(k_run_tables[0].addr) != 0x302D4448u) return;   /* "HD-0" */
    unsigned n = 0;
    for (size_t t = 0; t < sizeof k_run_tables / sizeof k_run_tables[0]; t++)
        for (uint32_t r = k_run_tables[t].addr; r < k_run_tables[t].end; r += k_run_tables[t].stride) {
            const uint32_t c = psx_mod_read_word(r + RUN_PRICE_AT);
            if (c > 7u && run_row_real(r)) { psx_mod_write_word(r + RUN_PRICE_AT, run_tier(c)); n++; }
        }
    for (uint32_t i = 0; i < RUN_OPTION_COUNT; i++) {
        const uint32_t r = RUN_OPTIONS + i * RUN_OPTION_STRIDE;
        const uint32_t c = psx_mod_read_word(r + RUN_PRICE_AT);
        if (c > 7u) { psx_mod_write_word(r + RUN_PRICE_AT, run_tier(c)); n++; }
    }
    if (n) { psx_mod_counter_add("moa.run.prices", n); run_log("shop: %u prices set in P", n); }
}

/* Restocks on reaching ranks 45, 40 ... 5 and 1: 8 random parts the player
 * neither owns nor already sees in the shop. Seeded by the run and the rank,
 * so a seed always restocks the same way. */
static void run_restock(void)
{
    const unsigned w = run_state.run.wins;
    if (!((w % 5u == 0u && w <= 45u) || w == MOA_RUN_SLOTS - 1)) return;
    if (psx_mod_read_word(k_run_tables[0].addr) != 0x302D4448u) {
        run_log("restock: part tables not loaded, skipped");
        return;
    }
    uint16_t pool[256];
    uint8_t back[256];
    unsigned np = 0;
    for (size_t t = 0; t < sizeof k_run_tables / sizeof k_run_tables[0]; t++) {
        uint32_t row = 0;
        for (uint32_t r = k_run_tables[t].addr; r < k_run_tables[t].end; r += k_run_tables[t].stride, row++) {
            const uint32_t ix = k_run_tables[t].base + row;
            if (ix > 255u || !run_row_real(r) || !psx_mod_read_word(r + RUN_PRICE_AT)) continue;
            if (psx_mod_read_byte(RUN_OWNED + ix) || psx_mod_read_byte(RUN_STOCK + ix)) continue;
            back[np] = k_run_tables[t].back;
            pool[np++] = (uint16_t)ix;
        }
    }
    uint64_t rng = run_state.lineup.seed ^ (0x5E570C4Bull * (w + 1u));
    unsigned added = 0;
    char list[8 * 5 + 1];
    list[0] = 0;
    while (added < RUN_RESTOCK_PARTS && np) {
        const uint32_t k = moa_run_below(&rng, np);
        psx_mod_write_byte(RUN_STOCK + pool[k], back[k] ? 2u : 1u);
        snprintf(list + strlen(list), sizeof list - strlen(list), " %u", pool[k]);
        pool[k] = pool[--np];
        back[k] = back[np];
        added++;
    }
    psx_mod_counter_add("moa.run.restock", 1);
    run_log("restock: rank %u, %u parts added:%s", MOA_RUN_SLOTS - w, added, list);
}

static void run_maybe_return(void);

static void run_hub_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_HUB_RESUME) return;
    if (run_state_ok && run_state.run.over) { run_loss_wait = 0; run_maybe_return(); return; }
    run_reach_hub("moa.run.hub_reached");
    if (run_state_ok) run_apply_prices();
}

static uint8_t run_rank_now(uint32_t st)
{
    const uint32_t r = psx_mod_read_word(st + 0x80u + 0x54u);
    return (r >= 0x80000000u && r < 0x80200000u) ? psx_mod_read_byte(r) : 0xFFu;
}

/* Autosave to the run card with the game's own save, once the result
 * routine has applied a win (rank, flags, credits). */
static void run_autosave(struct CPUState *cpu)
{
    if (!run_save_pending) return;
    run_save_pending = 0;
    if (psx_mod_read_word(RUN_SAVE) != RUN_SIG_SAVE) {
        psx_mod_counter_add("moa.run.save_skip_code", 1);
        run_log("autosave: skipped, save routine not in memory");
        return;
    }
    /* File 0 takes its name letter from the card list; the list may be gone
     * by now (menu memory is reused), so give it the run card's letter for
     * the call and put the old byte back after. */
    const uint32_t letter_at = RUN_SAVE_LIST + RUN_SAVE_LETTER;
    const uint8_t old_letter = psx_mod_read_byte(letter_at);
    const uint16_t count = psx_mod_read_half(RUN_SAVE_COUNT);
    psx_mod_write_byte(letter_at, (uint8_t)'A');
    const uint32_t ok = psx_mod_call_guest(cpu, RUN_SAVE, cpu->gpr[31], 0u, 0u, 0u, 0u);
    psx_mod_write_byte(letter_at, old_letter);
    psx_mod_counter_add((ok & 0xFFu) ? "moa.run.save_ok" : "moa.run.save_fail", 1);
    run_log("autosave: result %u (list letter was 0x%02X, count %u)",
            (unsigned)(ok & 0xFFu), old_letter, (unsigned)count);
}

/* A loss ends the run once: the file keeps it for Retry, and the launcher
 * reopens a moment later. The return is made from a guest hook (sampler,
 * result or hub), where the scheduler can be left safely; test 3 showed the
 * frame hook's request did not end the session. */
static void run_book_loss(const char *where)
{
    if (!run_state_ok || run_state.run.over) return;
    moa_run_lose(&run_state);
    (void)moa_run_save(run_file_path(), &run_state);
    run_loss_wait = RUN_LOSS_FRAMES;
    psx_mod_counter_add("moa.run.loss", 1);
    run_log("loss: run over (%s)", where);
}

static void run_maybe_return(void)
{
    if (!run_state.run.over || run_loss_wait) return;
    static int sent;
    if (sent) return;
    sent = 1;
    psx_mod_counter_add("moa.run.over", 1);
    run_log("loss: back to the launcher");
    psx_mod_request_return_to_launcher();
}

/* A fight's result in the run's arena: a new win pays 1 P, a loss ends the
 * run (the file keeps it for Retry) and hands back to the launcher. */
static void run_result_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_RESULT) return;
    run_maybe_return();
    if (!run_at_hub || !run_state_ok) {
        static int told;
        if (psx_mod_read_byte(RUN_FIGHT_DONE) && !told) {
            told = 1;
            run_log("result: not booked (at hub %d, run file loaded %d)", run_at_hub, run_state_ok);
        }
        return;
    }
    if (!psx_mod_read_byte(RUN_FIGHT_DONE)) {
        run_result_seen = 0;
        run_autosave(cpu);                     /* the result is applied now */
        return;
    }
    if (run_result_seen) return;
    run_result_seen = 1;
    const uint32_t st = cpu->gpr[4];
    if (st < 0x80000000u || st >= 0x80200000u) return;
    const uint8_t arena = psx_mod_read_byte(st + RUN_STATE_ARENA);
    run_log("result: arena %u, opponent %u, won %u, lost %u, rank %u, P %u",
            arena, psx_mod_read_byte(RUN_FIGHT_OPPONENT),
            psx_mod_read_half(RUN_FIGHT_WON), psx_mod_read_half(RUN_FIGHT_WON + 2u),
            (unsigned)run_rank_now(st), (unsigned)run_state.run.points);
    if (arena != RUN_ARENA) return;
    const char *path = run_file_path();
    if (!psx_mod_read_half(RUN_FIGHT_WON)) {
        run_book_loss("arena screen");
        return;
    }
    /* Only a first win over that opponent counts (refights pay nothing). */
    const uint32_t flags = psx_mod_read_word(st + RUN_STATE_FLAGS);
    const uint8_t who = psx_mod_read_byte(RUN_FIGHT_OPPONENT);
    if (flags < 0x80000000u || flags >= 0x80200000u || who >= MOA_RUN_SLOTS ||
        psx_mod_read_byte(flags + who) == 1u) {
        psx_mod_counter_add("moa.run.refight", 1);
        run_log("win: refight, nothing paid");
        return;
    }
    const uint32_t had = run_state.run.points;
    if (!moa_run_win(&run_state)) return;
    /* P is the game's credits. Step 1 has nothing to spend them on, so the
     * credits are the run's P: if not, leave them (and say so). */
    const uint32_t credits = psx_mod_read_word(RUN_CREDITS);
    if (credits != had) psx_mod_counter_add("moa.run.credits_mismatch", 1);
    psx_mod_write_word(RUN_CREDITS, run_state.run.points);
    run_log("win: P %u -> %u (credits were %u)", (unsigned)had,
            (unsigned)run_state.run.points, (unsigned)credits);
    (void)moa_run_save(path, &run_state);
    psx_mod_counter_add("moa.run.win", 1);
    run_restock();
    run_save_pending = 1;
}

/* Arena 0 sites: s2 (open) or s0 (the rest) = state + 0x80. */
static int run_a0(struct CPUState *cpu, int reg)
{
    if (!run_on() || !run_at_hub) return 0;
    const uint32_t b = cpu->gpr[reg];
    return b >= 0x80000000u && b < 0x80200000u && psx_mod_read_byte(b + 0x5Du) == RUN_ARENA;
}

static void run_a0_open(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!run_a0(cpu, 18)) return;
    const uint32_t b = cpu->gpr[18];
    cpu->gpr[2] = psx_mod_read_byte(b + 0x5Fu);           /* the arena's count */
    psx_mod_write_byte(b + 0x5Cu, 0);                     /* the ranked list */
    psx_mod_counter_add("moa.run.a0_open", 1);
}

static void run_a0_subfight(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (run_a0(cpu, 16)) cpu->gpr[5] = 0;
}

static void run_a0_ranked(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (run_a0(cpu, 16)) cpu->gpr[2] = 1;                 /* sltiu 1,16 -> 1 */
}

static void run_a0_cursor(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (run_a0(cpu, 16)) cpu->gpr[2] = 1;
}

static void run_a0_clear(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (run_a0(cpu, 16)) cpu->gpr[2] = 0;
}

/* ---- the run's opponents in arena 0 ---------------------------------------- */

static uint32_t run_index(void)
{
    for (uint32_t f = 0; f < RUN_FILE_COUNT; ++f) {
        const uint32_t ix = psx_mod_read_word(RUN_FILE_TABLE + f * 12u);
        if ((ix >> 24) == 0x80u && psx_mod_read_half(ix) == 1u && psx_mod_read_half(ix + 2u) == 37u)
            return ix;                                   /* MENU_TIM.T (stock or Phantasma's) */
    }
    return 0;
}

/* First entry and count of a source arena; 0 = not available. */
static int run_source(uint32_t ix, uint8_t arena, uint32_t *first, uint32_t *count)
{
    if (arena == RUN_PP_ARENA) {
        *first = RUN_PP_FIRST; *count = 50u;
        /* Phantasma's index covers its block only once installed. */
        return psx_mod_read_half(ix + 2u * (RUN_PP_FIRST + 203u)) >
               psx_mod_read_half(ix + 2u * RUN_PP_FIRST);
    }
    if (arena > 8u) return 0;
    *count = psx_mod_read_byte(RUN_ARENA_COUNTS + arena);
    *first = RUN_A0_FIRST + 4u * psx_mod_read_byte(RUN_ARENA_STARTS + arena) + 4u * arena;
    return *count != 0;
}

/* Arena 0's per-opponent entry -> the run's source entry (unchanged if not one). */
static uint32_t run_remap(uint32_t entry)
{
    if (!run_on() || !run_at_hub || !run_state_ok) return entry;
    if (entry < RUN_A0_FIRST || entry >= RUN_A0_FIRST + 4u * MOA_RUN_SLOTS) return entry;
    const uint32_t kind = (entry - RUN_A0_FIRST) / MOA_RUN_SLOTS;
    const uint32_t e = (entry - RUN_A0_FIRST) % MOA_RUN_SLOTS;
    const struct moa_run_pick p = run_state.lineup.slot[MOA_RUN_SLOTS - 1u - e];
    if (p.arena == MOA_RUN_PLAYER || p.arena == MOA_RUN_GHOST) return entry;
    uint32_t first, count;
    const uint32_t ix = run_index();
    if (!ix || !run_source(ix, p.arena, &first, &count) || p.rank + 1u >= count) return entry;
    return first + kind * count + p.rank;
}

static void run_load_one_entry(struct CPUState *cpu, uint32_t address)
{
    if (psx_mod_read_word(address) != RUN_SIG_LOADER) return;
    cpu->gpr[4] = run_remap(cpu->gpr[4] & 0xFFFFu);
}

static void run_load_file_entry(struct CPUState *cpu, uint32_t address)
{
    if (psx_mod_read_word(address) != RUN_SIG_LOADER || (cpu->gpr[4] & 0xFFFFu) != 4u) return;
    cpu->gpr[5] = run_remap(cpu->gpr[5] & 0xFFFFu);
}

static struct { int kind; uint32_t buf, bytes; } run_fix[4];
static int run_fixes;
static uint8_t run_arena_at_builds = 0xFF, run_arena_at_ai = 0xFF;

static void run_queue_fix(int kind, uint8_t arena, struct CPUState *cpu)
{
    if (!run_on() || !run_at_hub || !run_state_ok || arena != RUN_ARENA) return;
    const uint32_t bytes = cpu->gpr[6], buf = cpu->gpr[7];
    if (bytes < 8u || bytes > 4096u || (bytes & 3u) || buf < 0x80000000u ||
        buf + bytes > 0x80200000u || run_fixes >= 4)
        return;
    run_fix[run_fixes].kind = kind;
    run_fix[run_fixes].buf = buf;
    run_fix[run_fixes].bytes = bytes;
    run_fixes++;
}

static void run_read_names(struct CPUState *c, uint32_t a)   { (void)a; run_queue_fix(KIND_NAMES, (uint8_t)c->gpr[20], c); }
static void run_read_rewards(struct CPUState *c, uint32_t a) { (void)a; run_queue_fix(KIND_REWARDS, (uint8_t)c->gpr[20], c); }
static void run_read_builds(struct CPUState *c, uint32_t a)  { (void)a; run_queue_fix(KIND_BUILDS, run_arena_at_builds, c); }
static void run_read_ai(struct CPUState *c, uint32_t a)      { (void)a; run_queue_fix(KIND_AI, run_arena_at_ai, c); }
static void run_arena_builds(struct CPUState *c, uint32_t a) { (void)a; run_arena_at_builds = psx_mod_read_byte(c->gpr[17] + 0x5Du); }
static void run_arena_ai(struct CPUState *c, uint32_t a)     { (void)a; run_arena_at_ai = psx_mod_read_byte(c->gpr[18] + 0x5Du); }

/* One record of a source arena's whole-arena entry. */
static int run_record(uint32_t ix, struct moa_run_pick p, int kind, uint32_t stride, uint8_t *out)
{
    static uint8_t sec[4096];
    static uint32_t have = 0xFFFFFFFFu;               /* the entry in sec */
    uint32_t first, count;
    if (!run_source(ix, p.arena, &first, &count) || p.rank >= count) return 0;
    const uint32_t entry = first + 4u * count + (uint32_t)kind;
    if (entry != have) {
        const uint32_t a = psx_mod_read_half(ix + 2u * entry), b = psx_mod_read_half(ix + 2u * entry + 2u);
        if (b <= a || (b - a) > 2u ||
            !psx_mod_read_disc_sectors(run_state.lineup.menu_lba + a, b - a, sec)) {
            have = 0xFFFFFFFFu;
            return 0;
        }
        have = entry;
    }
    memcpy(out, sec + p.rank * stride, stride);
    return 1;
}

/* Rebuild one whole-arena entry for the run, in place. */
static void run_apply_fix(int kind, uint32_t buf, uint32_t bytes)
{
    static const uint32_t k_stride[4] = { 64u, 48u, 8u, 36u };
    static uint8_t out[4096];
    const uint32_t stride = k_stride[kind];
    const uint32_t ix = run_index();
    if (!ix || !run_state.lineup.menu_lba || MOA_RUN_SLOTS * stride + 4u > bytes) {
        psx_mod_counter_add("moa.run.data_skipped", 1);
        return;
    }
    for (uint32_t i = 0; i < bytes; ++i) out[i] = psx_mod_read_byte(buf + i);
    for (uint32_t e = 0; e + 1u < MOA_RUN_SLOTS; ++e) {   /* entry 49: the player */
        uint8_t *rec = out + e * stride;
        const struct moa_run_pick p = run_state.lineup.slot[MOA_RUN_SLOTS - 1u - e];
        if (kind == KIND_REWARDS) {                       /* P comes from the run */
            memset(rec, 0, 4);
            memset(rec + 4, 0xFF, 4);
            continue;
        }
        if (p.arena == MOA_RUN_PLAYER) continue;
        if (p.arena == MOA_RUN_GHOST) {
            if (p.rank >= MOA_RUN_SLOTS) continue;
            const struct moa_run_ghost *g = &run_state.lineup.ghost[p.rank];
            if (kind == KIND_BUILDS) moa_run_ghost_record(g, rec);
            else if (kind == KIND_NAMES) {
                memset(rec, 0, 48);
                memcpy(rec, g->pilot, strnlen(g->pilot, 16));
                memcpy(rec + 24, g->ac, strnlen(g->ac, 16));
            } else if (g->ai_from.arena != MOA_RUN_PLAYER && g->ai_from.arena != MOA_RUN_GHOST)
                (void)run_record(ix, g->ai_from, kind, stride, rec);
            continue;
        }
        if (!run_record(ix, p, kind, stride, rec)) psx_mod_counter_add("moa.run.data_missing", 1);
    }
    uint32_t sum = 0x12345678u;                           /* the entry's checksum */
    for (uint32_t i = 0; i + 4u < bytes; i += 4u)
        sum += (uint32_t)out[i] | (uint32_t)out[i + 1] << 8 | (uint32_t)out[i + 2] << 16 |
               (uint32_t)out[i + 3] << 24;
    out[bytes - 4] = (uint8_t)sum; out[bytes - 3] = (uint8_t)(sum >> 8);
    out[bytes - 2] = (uint8_t)(sum >> 16); out[bytes - 1] = (uint8_t)(sum >> 24);
    for (uint32_t i = 0; i < bytes; i += 4u)
        psx_mod_write_word(buf + i, (uint32_t)out[i] | (uint32_t)out[i + 1] << 8 |
                                    (uint32_t)out[i + 2] << 16 | (uint32_t)out[i + 3] << 24);
    psx_mod_counter_add("moa.run.data_applied", 1);
}

/* The queued entries, once every read is in. */
static void run_apply_fixes(void)
{
    if (!run_fixes) return;
    const uint32_t rd = psx_mod_read_word(RUN_CDQ + 0x37Cu);
    if (rd != psx_mod_read_word(RUN_CDQ + 0x378u) || psx_mod_read_byte(rd) != 0u) return;
    for (int i = 0; i < run_fixes; ++i) run_apply_fix(run_fix[i].kind, run_fix[i].buf, run_fix[i].bytes);
    run_fixes = 0;
}

static void run_cd_busy_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (run_fixes && psx_mod_read_word(address) == RUN_SIG_CD_BUSY) run_apply_fixes();
}

/* A blocked hub entry does what an unknown entry does: nothing. */
static int run_hub_pick_filter(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || !run_at_hub || psx_mod_read_word(address) != RUN_SIG_HUB_PICK) return 0;
    const uint32_t st = cpu->gpr[4];
    if (st < 0x80000000u || st >= 0x80200000u) return 0;
    const int8_t entry = (int8_t)psx_mod_read_byte(st + RUN_HUB_ENTRY);
    static const char *const k_names[8] = {
        "moa.run.hub_entry_0", "moa.run.hub_entry_1", "moa.run.hub_entry_2",
        "moa.run.hub_entry_3", "moa.run.hub_entry_4", "moa.run.hub_entry_5",
        "moa.run.hub_entry_6", "moa.run.hub_entry_7" };
    if (entry < 0 || entry > 7) return 0;
    psx_mod_counter_add(k_names[entry], 1);
    run_log("hub: entry %d picked", entry);
    if (RUN_HUB_ALLOWED & (1u << entry)) return 0;
    const uint32_t flags = psx_mod_read_word(st + RUN_HUB_FLAGS);
    if (flags >= 0x80000000u && flags < 0x80200000u) psx_mod_write_byte(flags + 1u, 1);
    /* Back to the hub the way a closing submenu returns: the game's own
     * resume (re-opens the hub's menu). The old way set the hub's next
     * state to RUN_HUB_NEXT, which left it waiting on a submenu (hang). */
    if (psx_mod_read_word(RUN_HUB_RESUME) == RUN_SIG_HUB_RESUME)
        (void)psx_mod_call_guest(cpu, RUN_HUB_RESUME, cpu->gpr[31], 0u, 0u, 0u, 0u);
    else
        psx_mod_write_word(st + 0x78u, RUN_HUB_NEXT);
    psx_mod_counter_add("moa.run.hub_blocked", 1);
    run_log("hub: entry %d blocked", entry);
    return 1;
}

static void run_sampler_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!run_on() || !run_at_hub || psx_mod_read_word(address) != RUN_SIG_SAMPLER) return;
    run_maybe_return();
    /* The fight's own result, before the retry/exit choice (a retry never
     * reaches the arena screen's result routine). Logged on each change so a
     * test shows when the game fills it in. */
    {
        static uint32_t last = 0xFFFFFFFFu;
        const uint8_t done = psx_mod_read_byte(RUN_FIGHT_DONE);
        const uint16_t won = psx_mod_read_half(RUN_FIGHT_WON);
        const uint16_t lost = psx_mod_read_half(RUN_FIGHT_WON + 2u);
        const uint32_t now = ((uint32_t)done << 24) ^ ((uint32_t)won << 12) ^ lost;
        if (now != last) {
            last = now;
            run_log("fight: done %u, won %u, lost %u", done, won, lost);
        }
        if (done && !won && lost) run_book_loss("in fight");
    }
    const uint16_t b = psx_mod_read_half(RUN_PAD_BUFFER + 2u);
    if (!(b & RUN_PAD_SELECT)) {                  /* held (active low) */
        psx_mod_write_half(RUN_PAD_BUFFER + 2u, (uint16_t)(b | RUN_PAD_SELECT));
        psx_mod_counter_add("moa.run.select_blocked", 1);
    }
}

static void run_frame_hook(void)
{
    if (!run_on()) return;
    run_apply_fixes();
    if (run_at_hub && psx_mod_read_byte(RUN_ARENA_LOCK)) {
        psx_mod_write_byte(RUN_ARENA_LOCK, 0);
        psx_mod_counter_add("moa.run.arena_unlocked", 1);
    }
    if (run_loss_wait) run_loss_wait--;      /* the return itself is made in a guest hook */
    if (run_autopilot && run_stage == STAGE_BACK) {
        if (++run_frames - run_back_start >= RUN_BACK_FRAMES) run_finish_handover();
        else psx_mod_set_loading_screen("ARENA RUN  Preparing your run", 98);
        return;
    }
    if (run_autopilot) {
        if (!psx_mod_fast_forward_requested()) psx_mod_request_fast_forward(1);
        const uint32_t f = run_frames++;
        int permille = (int)(f * 1000u / (RUN_HUB_TIMEOUT / 3u));
        if (permille > 950) permille = 950;
        psx_mod_set_loading_screen("ARENA RUN  Preparing your run", permille / 10);
        if (f >= RUN_HUB_TIMEOUT) run_reach_hub("moa.run.hub_timeout");
        return;
    }
    if (run_fade) {                            /* a short hold at 100%, then away */
        if (--run_fade) psx_mod_set_loading_screen("ARENA RUN  Ready", 100);
        else psx_mod_set_loading_screen(NULL, 0);
    }
}

PSX_MOD_CONSTRUCTOR(moa_arena_run_boot_register)
{
    (void)psx_game_register_offline_function_entry(RUN_MENU_ENTRY, run_menu_entry);
    (void)psx_game_register_offline_function_entry(RUN_MENU_INPUT_A, run_menu_input_entry);
    (void)psx_game_register_offline_function_entry(RUN_MENU_INPUT_B, run_menu_input_entry);
    (void)psx_game_register_offline_function_entry(RUN_MODE_CONFIRM, run_mode_confirm_entry);
    (void)psx_game_register_offline_function_entry(RUN_TITLE_OPEN, run_title_open_entry);
    (void)psx_game_register_offline_function_filter(RUN_MOVIE_PLAY, run_movie_filter);
    (void)psx_game_register_offline_function_entry(RUN_HUB_RESUME, run_hub_entry);
    (void)psx_game_register_offline_function_entry(RUN_NEWGAME_FRAME, run_newgame_entry);
    (void)psx_game_register_offline_function_entry(RUN_RESULT, run_result_entry);
    (void)psx_game_register_offline_function_filter(RUN_HUB_PICK, run_hub_pick_filter);
    (void)psx_game_register_offline_function_entry(RUN_FIGHT_SAMPLER, run_sampler_entry);
    (void)psx_game_register_offline_instruction(RUN_A0_OPEN, RUN_A0_OPEN_I, run_a0_open);
    (void)psx_game_register_offline_function_entry(RUN_LOAD_ONE, run_load_one_entry);
    (void)psx_game_register_offline_function_entry(RUN_LOAD_FILE, run_load_file_entry);
    (void)psx_game_register_offline_function_entry(RUN_CD_BUSY, run_cd_busy_entry);
    (void)psx_game_register_offline_instruction(RUN_READ_NAMES, RUN_JAL_READ, run_read_names);
    (void)psx_game_register_offline_instruction(RUN_READ_REWARDS, RUN_JAL_READ, run_read_rewards);
    (void)psx_game_register_offline_instruction(RUN_READ_BUILDS, RUN_JAL_READ, run_read_builds);
    (void)psx_game_register_offline_instruction(RUN_READ_AI, RUN_JAL_READ, run_read_ai);
    (void)psx_game_register_offline_instruction(RUN_ARENA_AT_BUILDS, RUN_ARENA_AT_BUILDS_I, run_arena_builds);
    (void)psx_game_register_offline_instruction(RUN_ARENA_AT_AI, RUN_ARENA_AT_AI_I, run_arena_ai);
    (void)psx_game_register_offline_instruction(RUN_A0_SUBFIGHT, RUN_A0_SUBFIGHT_I, run_a0_subfight);
    (void)psx_game_register_offline_instruction(RUN_A0_RANKED, RUN_A0_RANKED_I, run_a0_ranked);
    (void)psx_game_register_offline_instruction(RUN_A0_CURSOR, RUN_A0_CURSOR_I, run_a0_cursor);
    (void)psx_game_register_offline_instruction(RUN_A0_CLEAR, RUN_A0_CLEAR_I, run_a0_clear);
    mod_register_frame_hook(run_frame_hook);
}
