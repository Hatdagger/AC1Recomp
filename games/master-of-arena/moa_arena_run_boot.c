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
 * Since 2026-11-17 the default is the direct boot instead (see "Direct boot"
 * below): no title, no LOAD GAME, no card screens. The run's save goes
 * straight into RAM through the game's own unpack, the hub opens on Arena
 * and opens it, and the player is handed the Arena list. The menu presses
 * above stay as the fallback (and with PSX_MOA_RUN_DIRECT=0).
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
#include <string.h>
#include <stdlib.h>
#include <string.h>

#include "moa_arena_run.h"
#include "moa_ac_view.h"
#include "moa_screen.h"
#include "moa_run_record.h"

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
/* The prize payout, run after the result (the result routine sets state+0x78
 * to it): credits += word[state+0xB8 + 8*opponent]; 0 skips the prize screen. */
#define RUN_PAYOUT           0x800BC420u
#define RUN_SIG_PAYOUT       0x27BDFFD0u  /* addiu sp,sp,-48 */
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
/* L1/R1 page flip (ranked <-> sub arena), arena 0 only: `lbu v0,93(s1)`
 * then this `bne v0,zero,0x800BD474` skips it for every other arena. In a run
 * there is no sub page (all 50 rows are on page 0, and flipping showed rows
 * the run never fills while its hooks forced ranked rules: the hang), so v0 = 1
 * takes the other arenas' way. v0 is set again at 0x800BD474 before any use;
 * the delay slot (lui t0) runs either way. */
#define RUN_A0_FLIP          0x800BD310u
#define RUN_A0_FLIP_I        0x14400058u

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
 * 0x800160C4 at four sites; each is rebuilt from the sources as soon as its
 * read has landed (checked where the game itself checks the CD queue,
 * 0x800589DC; see run_queue_fix).
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

/* ---- Direct boot (default; PSX_MOA_RUN_DIRECT=0 brings back the menu presses)
 *
 * Instead of the title, LOAD GAME and the card screens, the title-open hook
 * does what those screens end up doing, with the game's own routines
 * (docs/mapping/menus-hub.md, mode-launch.md):
 *   1. SCENARIO row (0x800AB58C): 0x8003EA94 = 255, mode 0, five setup calls,
 *      Human PLUS bytes cleared, 0x80057B28(2), menu pictures 0x80057CB0(4, 0/1/4).
 *   2. LOAD's 'N' path (Master of Arena's own save; 0x80073424 after its card
 *      read): the save file's first 0xE80 bytes (session card, block 1),
 *      checksum = sum of inverted bytes 432..3671 (0x80070F1C) == word +384,
 *      byte +3672 == 1, then LOAD's inline unpack (0x80073610..0x80073B90),
 *      replayed here as the 11 copies in k_run_d_unpack, emblem LoadImage
 *      0x8002C75C(0x800CC070, buf + 1152) / (0x800CC078, buf + 3200),
 *      DrawSync, six setup calls, 0x8003EA82 = 1. (0x80070F70 / 0x80072E54
 *      are LOAD's import path for original Armored Core saves, not ours.)
 *   3. Open the hub (0x800552C0, as LOAD's callback 0x800AC9E8 does) on
 *      entry 5 (Arena), and press X once in its input state (0x80055848).
 * The run is handed over at step 2 (the arena hooks need it); the loading
 * screen stays up until the Arena list opens. Any check failing falls back
 * to the menu presses. */
#define RUN_D_SCORE          0x8003EA94u  /* byte: 255 on SCENARIO */
#define RUN_D_MODE           0x8003EA2Au  /* engine mode byte: 0 */
#define RUN_D_PLUS           0x8003EAC8u  /* 8 bytes cleared on SCENARIO */
#define RUN_D_MAIL_SHOWN     0x8003EA82u  /* "You have new mail." already shown */
static const uint32_t k_run_d_scenario[] = {
    0x800631FCu, 0x800BA854u, 0x800C566Cu, 0x800B69F8u, 0x800B6A80u };
#define RUN_D_TASKS_CLEAR    0x80057B28u  /* (2) */
#define RUN_D_PICTURES       0x80057CB0u  /* (4, entry): MENU_TIM.T 0, 1, 4 */
#define RUN_D_SIG_PICTURES   0x27BDFFE8u
#define RUN_D_SAVE_BYTES     3712u        /* 0xE80, what LOAD reads */
#define RUN_D_SUM_AT         384u         /* word: sum of ~byte over 3240 bytes from 432 */
#define RUN_D_SUM_FROM       432u
#define RUN_D_SUM_BYTES      3240u
#define RUN_D_VERSION_AT     3672u        /* byte: 1 */
/* LOAD's 'N' check and first copy, so the copy list below is the right one. */
#define RUN_D_LOAD_N         0x80073480u  /* addiu v0,zero,'N' */
#define RUN_D_SIG_LOAD_N     0x2402004Eu
#define RUN_D_LOAD_COPY      0x80073620u  /* addiu t4,v0,0x6FB0 */
#define RUN_D_SIG_LOAD_COPY  0x244C6FB0u
/* LOAD's inline unpack for 'N' saves (0x80073610..0x80073B90, flag 0),
 * traced statically: every byte it writes comes from the save, unchanged. */
static const struct { uint32_t dst; uint16_t from, bytes; } k_run_d_unpack[] = {
    { 0x80036FB0u,  440,  12 },   /* garage parts */
    { 0x80036FBCu,  592, 560 },   /* paint, owned (+640), shop stock (+896) ... */
    { 0x800371ECu,  460,  36 },   /* AC name, pilot name */
    { 0x8003EA70u,  454,   2 },   /* options */
    { 0x8003EA72u,  452,   2 },
    { 0x8003EA74u,  456,   4 },   /* credits */
    { 0x8003EAC8u, 3564,   8 },   /* Human PLUS */
    { 0x8004AC50u, 3232,  44 },
    { 0x8004B508u,  496,  96 },
    { 0x8004B568u, 3276, 288 },
    { 0x8004B688u, 3572, 100 },
};
#define RUN_D_LOAD_IMAGE     0x8002C75Cu  /* boot exe LoadImage(rect, data) */
#define RUN_D_DRAW_SYNC      0x8002C4B0u
#define RUN_D_EMBLEM_RECT    0x800CC070u
#define RUN_D_CLUT_RECT      0x800CC078u
static const uint32_t k_run_d_after_load[] = {
    0x800B6A80u, 0x80099098u, 0x800941F4u, 0x80093BC4u, 0x800A2318u, 0x800A2020u };
#define RUN_D_HUB_OPEN       0x800552C0u
#define RUN_D_SIG_HUB_OPEN   0x27BDFFB8u  /* addiu sp,sp,-72 */
#define RUN_D_HUB_INPUT      0x80055848u
#define RUN_D_SIG_HUB_INPUT  0x27BDFFC0u  /* addiu sp,sp,-64 */
#define RUN_D_PAD            0x801B1CB8u  /* +0x18 new presses, +0x1C the mask it must lack */

static int run_enabled = -1;
static int run_autopilot;                 /* driving the menus */
static int run_at_hub;                    /* the player has the hub */
static uint32_t run_ticks, run_frames, run_fade;
static struct moa_run_state run_state;
static int run_state_ok;
enum { STAGE_TITLE = 0, STAGE_LOADGAME, STAGE_LOADING, STAGE_BACK, STAGE_DIRECT };
static int run_direct;                     /* this session booted directly */
static int run_direct_press;               /* X still to press at the hub */
static uint32_t run_direct_buf;            /* guest buffer for the save */
/* At the hub: one O press (one menu back), at normal speed so the hub's
 * music starts as it would, then hand over behind the loading screen. */
#define RUN_BACK_FRAMES      90u
#define RUN_PAD_CIRCLE       0x2000u
/* Direct boot: the Arena list loads its names from the disc after it opens;
 * this short hold at normal speed covers that before the hand-over. */
#define RUN_DIRECT_HOLD      45u
static uint32_t run_back_start;
static int run_stage;
static uint32_t run_newgame_seen;          /* run_frames when that menu last ran */
static int run_result_seen;                /* this fight's result is booked */
static uint32_t run_pay;                   /* what the next payout pays: 1 on a new win, else 0 */
static int run_save_pending;               /* a win waits for the autosave */
static unsigned run_win_screen;            /* booked wins waiting for the arena screen (P, restock) */

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
static int run_sent, run_told;
static int run_fixes;
static uint8_t run_arena_at_builds = 0xFF, run_arena_at_ai = 0xFF;

static void run_prize_reset(void);
static void run_reconcile(void);

/* Sessions run in one process, so nothing may carry over from the last one
 * (a stale "at the hub" kept a Retry from booting). The launcher numbers each
 * start in PSX_MOA_RUN_SESSION; a new number clears everything. */
static void run_check_session(void)
{
    static char last[32] = "-";
    const char *e = getenv("PSX_MOA_RUN_SESSION");
    if (!e) e = "";
    if (strncmp(last, e, sizeof last - 1) == 0) return;
    snprintf(last, sizeof last, "%s", e);
    run_enabled = -1;
    run_autopilot = run_at_hub = 0;
    run_ticks = run_frames = run_fade = 0;
    memset(&run_state, 0, sizeof run_state);
    run_state_ok = 0;
    run_back_start = 0;
    run_stage = 0;
    run_newgame_seen = 0;
    run_result_seen = 0;
    run_pay = 0;
    run_save_pending = 0;
    run_win_screen = 0;
    run_loss_wait = 0;
    run_sent = run_told = 0;
    run_fixes = 0;
    run_arena_at_builds = run_arena_at_ai = 0xFF;
    run_direct = run_direct_press = 0;
    run_prize_reset();
    moa_rec_reset();
}
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
    else run_reconcile();
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
    run_log("boot: %s (frame %u)", why, (unsigned)run_frames);
}

/* ---- hooks -------------------------------------------------------------------- */

static void run_menu_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    run_check_session();
    run_enabled = -1;
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_MENU_ENTRY) return;
    if (run_at_hub) return;                   /* already handed over */
    run_autopilot = 1;
    run_ticks = run_frames = 0;
    run_stage = STAGE_TITLE;
    run_set_ff(1);
    psx_mod_counter_add("moa.run.boot", 1);
    run_log("boot: menu reached, autopilot on");
}

static int run_direct_wanted(void)
{
    const char *e = getenv("PSX_MOA_RUN_DIRECT");
    return !(e && e[0] == '0');
}

/* The run's save as LOAD would read it: the session card's block 1, first
 * 0xE80 bytes (moa_memcard_write_blocks puts file A there). */
static int run_direct_read_save(uint8_t out[RUN_D_SAVE_BYTES])
{
    const char *card = getenv("PSX_MEMCARD1_SESSION");
    if (!card || !card[0]) return 0;
    FILE *f = fopen(card, "rb");
    if (!f) return 0;
    const int ok = fseek(f, 8192L, SEEK_SET) == 0 &&
                   fread(out, 1, RUN_D_SAVE_BYTES, f) == RUN_D_SAVE_BYTES;
    fclose(f);
    return ok && out[0] == 'S' && out[1] == 'C';
}

static int run_direct_code_ok(void)
{
    return psx_mod_read_word(RUN_D_PICTURES) == RUN_D_SIG_PICTURES &&
           psx_mod_read_word(RUN_D_LOAD_N) == RUN_D_SIG_LOAD_N &&
           psx_mod_read_word(RUN_D_LOAD_COPY) == RUN_D_SIG_LOAD_COPY &&
           psx_mod_read_word(RUN_D_HUB_OPEN) == RUN_D_SIG_HUB_OPEN &&
           psx_mod_read_word(RUN_D_HUB_INPUT) == RUN_D_SIG_HUB_INPUT;
}

/* Steps 1-3 of the direct boot (top of file). 1 = done, the title is not
 * opened; 0 = nothing changed, carry on with the menu presses. */
static void run_apply_prices(void);
static void run_restock_catch_up(const char *where);

static int run_direct_boot(struct CPUState *cpu)
{
    static uint8_t save[RUN_D_SAVE_BYTES];
    if (!run_direct_code_ok()) { run_log("direct: menu code not as expected"); return 0; }
    if (!run_direct_read_save(save)) { run_log("direct: run card not readable"); return 0; }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < RUN_D_SUM_BYTES; i++) sum += ~(uint32_t)save[RUN_D_SUM_FROM + i];  /* nor on the byte */
    const uint32_t want = (uint32_t)save[RUN_D_SUM_AT] | (uint32_t)save[RUN_D_SUM_AT + 1] << 8 |
                          (uint32_t)save[RUN_D_SUM_AT + 2] << 16 | (uint32_t)save[RUN_D_SUM_AT + 3] << 24;
    if (sum != want || save[RUN_D_VERSION_AT] != 1u) {
        run_log("direct: save check failed (sum %08x, stored %08x, byte 3672 = %u)",
                (unsigned)sum, (unsigned)want, save[RUN_D_VERSION_AT]);
        return 0;
    }
    if (!run_direct_buf) run_direct_buf = psx_mod_alloc_guest_memory(RUN_D_SAVE_BYTES, 4u);
    if (!run_direct_buf) { run_log("direct: no guest memory"); return 0; }
    for (uint32_t i = 0; i < RUN_D_SAVE_BYTES; i += 4u)
        psx_mod_write_word(run_direct_buf + i, (uint32_t)save[i] | (uint32_t)save[i + 1] << 8 |
                                               (uint32_t)save[i + 2] << 16 | (uint32_t)save[i + 3] << 24);
    const uint32_t ra = cpu->gpr[31], buf = run_direct_buf;
    /* 1. SCENARIO MODE */
    psx_mod_write_byte(RUN_D_SCORE, 255);
    psx_mod_write_byte(RUN_D_MODE, 0);
    for (size_t i = 0; i < sizeof k_run_d_scenario / sizeof k_run_d_scenario[0]; i++)
        (void)psx_mod_call_guest(cpu, k_run_d_scenario[i], ra, 0, 0, 0, 0);
    for (uint32_t i = 0; i < 8u; i++) psx_mod_write_byte(RUN_D_PLUS + i, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_TASKS_CLEAR, ra, 2, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_PICTURES, ra, 4, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_PICTURES, ra, 4, 1, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_PICTURES, ra, 4, 4, 0, 0);
    /* 2. LOAD's 'N' path, after the card read */
    for (size_t k = 0; k < sizeof k_run_d_unpack / sizeof k_run_d_unpack[0]; k++)
        for (uint32_t i = 0; i < k_run_d_unpack[k].bytes; i++)
            psx_mod_write_byte(k_run_d_unpack[k].dst + i, save[k_run_d_unpack[k].from + i]);
    (void)psx_mod_call_guest(cpu, RUN_D_LOAD_IMAGE, ra, RUN_D_EMBLEM_RECT, buf + 1152u, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_LOAD_IMAGE, ra, RUN_D_CLUT_RECT, buf + 3200u, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_DRAW_SYNC, ra, 0, 0, 0, 0);
    for (size_t i = 0; i < sizeof k_run_d_after_load / sizeof k_run_d_after_load[0]; i++)
        (void)psx_mod_call_guest(cpu, k_run_d_after_load[i], ra, 0, 0, 0, 0);
    /* The run is in RAM: hand the data over now (the arena hooks wait for
     * it), keep the loading screen until the Arena list is up. */
    run_at_hub = 1;
    run_apply_state();
    run_apply_prices();                          /* shop in P; the hub-resume hook redoes it later */
    run_restock_catch_up("load");
    psx_mod_write_byte(RUN_ARENA_LOCK, 0);
    psx_mod_write_byte(RUN_D_MAIL_SHOWN, 1);     /* no mail box in the way */
    /* 3. The hub, on Arena */
    const uint32_t hub = psx_mod_call_guest(cpu, RUN_D_HUB_OPEN, ra, 0, 0, 0, 0);
    if (hub >= 0x80000000u && hub < 0x80200000u) psx_mod_write_byte(hub + RUN_HUB_ENTRY, RUN_HUB_ARENA);
    run_direct = 1;
    run_direct_press = 1;
    run_stage = STAGE_DIRECT;
    psx_mod_counter_add("moa.run.direct_boot", 1);
    run_log("direct: save loaded, hub opened on Arena (frame %u)", (unsigned)run_frames);
    return 1;
}

static int run_title_open_filter(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_TITLE_OPEN) return 0;
    if (run_at_hub && !run_autopilot) {       /* the player quit to the title */
        psx_mod_counter_add("moa.run.return_from_title", 1);
        psx_mod_request_return_to_launcher();
        return 0;
    }
    if (!run_autopilot) return 0;
    if (!run_direct && run_direct_wanted() && run_direct_boot(cpu)) return 1;
    cpu->gpr[4] = 1;
    return 0;
}

/* Hub input state: X once on Arena (the hub opens it the normal way). */
static void run_hub_input_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_direct_press || !run_on() || psx_mod_read_word(address) != RUN_D_SIG_HUB_INPUT) return;
    run_direct_press = 0;
    const uint32_t hub = cpu->gpr[4];
    if (hub >= 0x80000000u && hub < 0x80200000u) psx_mod_write_byte(hub + RUN_HUB_ENTRY, RUN_HUB_ARENA);
    psx_mod_write_word(RUN_D_PAD + 0x18u, psx_mod_read_word(RUN_D_PAD + 0x18u) | RUN_PAD_X);
    psx_mod_write_word(RUN_D_PAD + 0x1Cu, psx_mod_read_word(RUN_D_PAD + 0x1Cu) & ~(uint32_t)RUN_PAD_X);
    run_log("direct: X at the hub (frame %u)", (unsigned)run_frames);
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
    if (run_stage != STAGE_LOADGAME) {
        psx_mod_counter_add("moa.run.load_game", 1);
        run_log("boot: new game / load game menu (frame %u)", (unsigned)run_frames);
    }
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
    {
        run_stage = STAGE_LOADING;                       /* that menu has closed */
        run_log("boot: loading the run's save (frame %u)", (unsigned)run_frames);
    }
    /* One button held for 3 samples out of every 24: START on the title,
     * then X on LOAD GAME and through the load screens (all defaults: slot 1,
     * the card's one save, yes). */
    const uint32_t t = run_ticks++;
    if (run_direct) {                            /* nothing pressed; the hub hook presses X */
        psx_mod_write_half(RUN_PAD_BUFFER + 2u, 0xFFFFu);
        return;
    }
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

/* folder: the part models' folder (models/menu/<folder>/<row>); label: shown
 * on the restock screen. */
static const struct { uint32_t addr, stride, end; uint16_t base; uint8_t back; const char *folder, *label; } k_run_tables[] = {
    { 0x800CE690u, 60, 0x800CEAC8u,   0, 0, "head",      "HEAD" },
    { 0x800CEAC8u, 56, 0x800CED30u,  18, 0, "core",      "CORE" },
    { 0x800CED30u, 72, 0x800CF870u,  53, 0, "legs",      "LEGS" },
    { 0x800CF870u, 52, 0x800CFD50u,  29, 0, "arms",      "ARMS" },
    { 0x800CFD50u, 44, 0x800D01C8u, 217, 0, "hand",      "ARM WEAPON" },
    { 0x800D01C8u, 52, 0x800D0DF8u, 157, 1, "back",      "BACK WEAPON" },   /* stock 2: stays on sale */
    { 0x800D0DF8u, 52, 0x800D0F30u, 243, 0, "blade",     "BLADE" },
    { 0x800D0F30u, 48, 0x800D11D0u, 143, 0, "booster",   "BOOSTER" },
    { 0x800D11D0u, 96, 0x800D17D0u, 108, 0, "fcs",       "FCS" },
    { 0x800D17D0u, 48, 0x800D1AA0u,  93, 0, "generator", "GENERATOR" },
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

/* ---- the restock screen ----------------------------------------------------
 *
 * After a restock, once the hub is back: one picture of the 8 new parts (2x4),
 * each drawn from the player's own disc models with the garage's renderer
 * (PSX_MOA_MODELS_DIR, set by the launcher; no models: name and price only),
 * shown as the loading screen until a button is pressed. The game's pad is
 * held released meanwhile so the press does not reach the hub menu. */
#define RUN_SHOW_MAX   RUN_RESTOCK_PARTS
#define RUN_THUMB      128
struct run_show_part { uint8_t tab, row; char name[19]; char price[8]; };
static struct run_show_part run_show[RUN_SHOW_MAX];
static unsigned run_show_n;
static uint8_t run_show_rank;          /* rank of the restock waiting to be shown; 0 = none */
static int run_show_on;                /* the picture is up */
static uint32_t run_show_frames;
static int run_show_armed;             /* every button seen released since it went up */

static void run_show_add(unsigned t, unsigned row, uint32_t at)
{
    if (run_show_n >= RUN_SHOW_MAX) return;
    struct run_show_part *e = &run_show[run_show_n++];
    e->tab = (uint8_t)t;
    e->row = (uint8_t)row;
    int n = 0;
    for (; n < 18; n++) {
        const uint8_t c = psx_mod_read_byte(at + (uint32_t)n);
        if (c < 32u || c > 126u) break;
        e->name[n] = (char)c;
    }
    while (n && e->name[n - 1] == ' ') n--;
    e->name[n] = 0;
    uint32_t c = psx_mod_read_word(at + RUN_PRICE_AT);
    if (c > 7u) c = run_tier(c);
    snprintf(e->price, sizeof e->price, "%u P", (unsigned)c);
}

static int run_show_font(void)
{
    if (moa_screen_ready()) return 1;
    static int tried;
    if (tried) return 0;
    tried = 1;
    uint32_t size = 0;
    if (!psx_mod_read_disc_file("GG/MS/MENU_TIM.T", NULL, 0, &size) || size < 4096u) return 0;
    uint8_t *b = (uint8_t *)malloc(size);
    uint32_t got = 0;
    int ok = b && psx_mod_read_disc_file("GG/MS/MENU_TIM.T", b, size, &got) &&
             moa_screen_load_font(b, got);
    free(b);
    if (!ok) run_log("restock screen: menu font not found");
    return ok;
}

/* One part drawn into 128x128 0xAARRGGBB, background transparent. */
static int run_part_thumb(const char *dir, unsigned tab, unsigned row, uint32_t *argb)
{
    uint8_t paint[48];
    moa_run_paint(run_state.lineup.seed, run_state.run.attempt, paint);
    moa_acv *v = moa_acv_load_part(dir, k_run_tables[tab].folder, row, paint);
    if (!v) return 0;
    static uint8_t rgba[RUN_THUMB * RUN_THUMB * 4];
    memset(rgba, 0, sizeof rgba);
    moa_acv_render(v, -0.6f, 0.35f, 0.0f, RUN_THUMB, RUN_THUMB, rgba);
    moa_acv_free(v);
    for (int p = 0; p < RUN_THUMB * RUN_THUMB; p++) {
        const uint8_t *q = rgba + 4 * p;
        argb[p] = (uint32_t)q[3] << 24 | (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
    }
    return 1;
}

static int run_show_thumb(const char *dir, unsigned i, uint32_t *argb)
{
    return run_part_thumb(dir, run_show[i].tab, run_show[i].row, argb);
}

static void run_show_open(void)
{
    const unsigned rank = run_show_rank;
    run_show_rank = 0;
    if (!run_show_n || !run_show_font()) return;
    const char *dir = getenv("PSX_MOA_MODELS_DIR");
    const int models = dir && dir[0] && moa_acv_has_part_models(dir);
    static uint32_t thumb[RUN_SHOW_MAX][RUN_THUMB * RUN_THUMB];
    struct moa_screen_tile tile[RUN_SHOW_MAX];
    unsigned drawn = 0;
    for (unsigned i = 0; i < run_show_n; i++) {
        tile[i].category = k_run_tables[run_show[i].tab].label;
        tile[i].name = run_show[i].name;
        tile[i].label = run_show[i].price;
        tile[i].thumb = models && run_show_thumb(dir, i, thumb[i]) ? (drawn++, thumb[i]) : NULL;
    }
    char title[40], line[48];
    snprintf(title, sizeof title, "SHOP RESTOCK  RANK %u", rank);
    snprintf(line, sizeof line, "%u NEW PARTS FOR SALE   YOU HAVE %u P", run_show_n,
             (unsigned)psx_mod_read_word(RUN_CREDITS));
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render_tiles(ww, wh, title, line, tile, (int)run_show_n,
                                                 "PRESS ANY BUTTON", 0, &w, &h);
    if (!px) return;
    psx_mod_set_loading_screen("ROGUE ARENA  Shop restock", -1);
    psx_mod_set_loading_image(px, w, h);
    run_show_on = 1;
    run_show_frames = 0;
    run_show_armed = 0;
    run_log("restock screen: rank %u, %u parts, %u pictures%s", rank, run_show_n, drawn,
            models ? "" : " (no model install)");
}

/* Per frame while the hub is up. */
static void run_show_frame(void)
{
    if (!run_show_on) {
        if (run_show_rank && run_at_hub && !psx_mod_read_byte(RUN_FIGHT_DONE)) run_show_open();
        return;
    }
    psx_mod_write_half(RUN_PAD_BUFFER + 2u, 0xFFFFu);       /* the hub sees nothing pressed */
    psx_mod_set_loading_screen("ROGUE ARENA  Shop restock", -1);
    const uint16_t b = psx_mod_host_pad_buttons(0);          /* active low */
    run_show_frames++;
    if (b == 0xFFFFu) { run_show_armed = 1; return; }
    if (!run_show_armed || run_show_frames < 30u) return;
    run_show_on = 0;
    psx_mod_set_loading_screen(NULL, 0);
    run_log("restock screen: closed");
}

/* Restocks on reaching ranks 45, 40 ... 5 and 1 (wins 5, 10 ... 45, 49):
 * 8 parts each. Each milestone's 8 are fixed by the seed and the attempt,
 * drawn in order from every real part except the starting AC's, none twice,
 * so they never depend on what the shop holds at the time. They are put on
 * sale (and caught up) whenever the hub opens or the Shop is picked: a win
 * booked in a fight, two wins before an arena screen, or a session that ended
 * before the card was saved can't skip one. A part already owned or on sale is
 * left alone, so this can run any number of times. */
#define RUN_RESTOCKS 10u
struct run_pool_part { uint16_t ix; uint8_t tab, back; uint32_t row; };

static unsigned run_restock_wins(unsigned k) { return k + 1u < RUN_RESTOCKS ? 5u * (k + 1u) : MOA_RUN_SLOTS - 1u; }

static void run_restock_lists(struct run_pool_part out[RUN_RESTOCKS][RUN_RESTOCK_PARTS],
                              unsigned n[RUN_RESTOCKS])
{
    static struct run_pool_part pool[256];
    uint8_t start[256];
    memset(start, 0, sizeof start);
    for (int b = 0; b < 13; ++b) {
        const int ix = moa_run_part_index(run_state.run.start_ac, b);
        if (ix >= 0) start[ix] = 1;
    }
    unsigned np = 0;
    for (size_t t = 0; t < sizeof k_run_tables / sizeof k_run_tables[0]; t++) {
        uint32_t row = 0;
        for (uint32_t r = k_run_tables[t].addr; r < k_run_tables[t].end; r += k_run_tables[t].stride, row++) {
            const uint32_t ix = k_run_tables[t].base + row;
            if (ix > 255u || start[ix] || !run_row_real(r) || !psx_mod_read_word(r + RUN_PRICE_AT)) continue;
            pool[np].ix = (uint16_t)ix;
            pool[np].tab = (uint8_t)t;
            pool[np].back = k_run_tables[t].back;
            pool[np].row = r;
            np++;
        }
    }
    for (unsigned k = 0; k < RUN_RESTOCKS; k++) {
        uint64_t rng = run_state.lineup.seed ^ (0x5E570C4Bull * (k + 1u)) ^
                       (0x9E3779B97F4A7C15ull * (uint64_t)(run_state.run.attempt + 1u));
        n[k] = 0;
        while (n[k] < RUN_RESTOCK_PARTS && np) {
            const uint32_t pick = moa_run_below(&rng, np);
            out[k][n[k]++] = pool[pick];
            pool[pick] = pool[--np];
        }
    }
}

static void run_restock_catch_up(const char *where)
{
    if (!run_state_ok || run_state.run.over || run_state.run.wins < 5u) return;
    if (psx_mod_read_word(k_run_tables[0].addr) != 0x302D4448u) {
        run_log("restock (%s): part tables not loaded, later", where);
        return;
    }
    static struct run_pool_part list[RUN_RESTOCKS][RUN_RESTOCK_PARTS];
    unsigned n[RUN_RESTOCKS];
    run_restock_lists(list, n);
    int fresh = -1;
    for (unsigned k = 0; k < RUN_RESTOCKS && run_state.run.wins >= run_restock_wins(k); k++) {
        unsigned seen = 0, added = 0;
        char ids[RUN_RESTOCK_PARTS * 5 + 1];
        ids[0] = 0;
        for (unsigned i = 0; i < n[k]; i++) {
            const uint16_t ix = list[k][i].ix;
            if (psx_mod_read_byte(RUN_OWNED + ix) || psx_mod_read_byte(RUN_STOCK + ix)) { seen++; continue; }
            psx_mod_write_byte(RUN_STOCK + ix, list[k][i].back ? 2u : 1u);
            snprintf(ids + strlen(ids), sizeof ids - strlen(ids), " %u", ix);
            added++;
        }
        if (!added) continue;
        if (!seen) fresh = (int)k;                 /* a new restock, not a sold part coming back */
        psx_mod_counter_add("moa.run.restock", 1);
        run_log("restock (%s): rank %u, %u parts on sale:%s", where,
                (unsigned)(MOA_RUN_SLOTS - run_restock_wins(k)), added, ids);
    }
    if (fresh < 0) return;
    run_show_n = 0;                                 /* the newest restock's picture */
    for (unsigned i = 0; i < n[fresh]; i++)
        run_show_add(list[fresh][i].tab, list[fresh][i].ix - k_run_tables[list[fresh][i].tab].base,
                     list[fresh][i].row);
    run_show_rank = (uint8_t)(MOA_RUN_SLOTS - run_restock_wins((unsigned)fresh));
}

/* ---- P bookkeeping ------------------------------------------------------------
 *
 * The game's credits are the player's P: what is left after shopping. P is
 * only ever added to them (a win, a P prize), never set from the run's total,
 * which once handed back everything spent at the next win. The run file says
 * how much P went into the credits (p_paid) and how much of that the run card
 * holds (p_card, at its last save), and which prizes are on the card. When a
 * session loads the card, whatever never reached it (the session ended before
 * the autosave) is added again: P to the credits, prize parts to the garage. */
static int run_reconciled;
static int run_pay_due;                    /* the payout still has to pay the win's 1 P */
static uint32_t run_pay_wait;              /* result frames the autosave waited on it */

static void run_reconcile(void)
{
    if (run_reconciled || !run_state_ok || run_state.run.over) return;
    run_reconciled = 1;
    struct moa_run_progress *r = &run_state.run;
    const unsigned owed = r->points > r->p_card ? (unsigned)(r->points - r->p_card) : 0u;
    unsigned parts = 0;
    if (owed) psx_mod_write_word(RUN_CREDITS, psx_mod_read_word(RUN_CREDITS) + owed);
    for (unsigned m = 0; m < MOA_RUN_PRIZES; m++) {
        const uint8_t bit = (uint8_t)(1u << m), ix = r->prize_ix[m];
        if (!(r->prizes & bit) || (r->prize_saved & bit) || ix == MOA_RUN_PRIZE_P) continue;
        const uint8_t have = psx_mod_read_byte(RUN_OWNED + ix);
        psx_mod_write_byte(RUN_OWNED + ix, (uint8_t)(have + (ix >= 157u && ix < 209u ? 2u : 1u)));
        parts++;
    }
    r->p_paid = r->points;
    if (owed || parts) {
        run_save_pending = 1;
        (void)moa_run_save(run_file_path(), &run_state);
    }
    run_log("load: P %u, %u P and %u prize part(s) not on the card added", (unsigned)r->points, owed, parts);
}

/* ---- prize picks ---------------------------------------------------------------
 *
 * After wins 1, 3, 6, 9, 12, 18 and 24: four boxes to pick one from, drawn
 * over the live game (dimmed) where the renderer can (psx_mod_set_overlay_image),
 * else as the loading picture. A box is a frame/internal category (HEAD, CORE,
 * ARMS, LEGS, BOOSTER, FCS, GENERATOR) or a weapon type (the part's type text:
 * GRENADE LAUNCHER, SMALL MISSILE, LASERBLADE, weapon arms by theirs), each with
 * at least one part the player can get: a real part, not owned, not on sale and
 * not in any of this attempt's restocks (so a restock stays 8 new parts), three
 * of those and a fourth, last box of P (+2 up to win 9, +3 after). Taking a box
 * gives a random part of it (back weapons: a pair, as the game does), then a
 * picture of the part until a button. The options come from the seed, the
 * attempt and the milestone, so a reopen shows the same four. Shown at the
 * arena screen or the hub, never in a fight; after the restock picture. */
#define RUN_PAD_UP    0x0010u
#define RUN_PAD_DOWN  0x0040u
static const uint8_t k_prize_wins[MOA_RUN_PRIZES] = MOA_RUN_PRIZE_WINS;
enum { G_HEAD, G_CORE, G_ARMS, G_LEGS, G_BOOST, G_FCS, G_GEN, G_WARMS, G_HAND, G_BACK, G_BLADE, G_COUNT };
static const struct { uint8_t tab; int8_t arm_unit; const char *label; } k_prize_group[G_COUNT] = {
    { 0, -1, "HEAD" }, { 1, -1, "CORE" }, { 3, 1, "ARMS" }, { 2, -1, "LEGS" },
    { 7, -1, "BOOSTER" }, { 8, -1, "FCS" }, { 9, -1, "GENERATOR" },
    { 3, 0, NULL }, { 4, -1, NULL }, { 5, -1, NULL }, { 6, -1, NULL },   /* NULL: by type */
};
#define RUN_TYPE_AT 0x12u

struct run_prize_opt { char label[17]; int8_t group; char type[19]; uint8_t p; };
static struct run_prize_opt run_prize_opt[4];
static int run_prize_n;
static int run_prize_stage;               /* 0 none, 1 choosing, 2 the prize shown */
static int run_prize_m;                   /* the milestone up */
static int run_prize_cur;
static int run_prize_where;               /* 1: at the arena screen or the hub (not a fight) */
static int run_prize_overlay;             /* shown over the live game */
static int run_prize_armed;
static uint32_t run_prize_frames;
static uint16_t run_prize_held;

static void run_prize_reset(void)
{
    run_reconciled = 0;
    run_pay_due = 0;
    run_pay_wait = 0;
    run_prize_stage = 0;
    run_prize_where = 0;
    (void)psx_mod_set_overlay_image(NULL, 0, 0);
}

static void run_text_at(uint32_t at, unsigned n, char *out)
{
    unsigned k = 0;
    for (; k < n; k++) {
        const uint8_t c = psx_mod_read_byte(at + k);
        if (c < 32u || c > 126u) break;
        out[k] = (char)c;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* Parts a prize may give, by group (and type). */
struct run_prize_part { uint16_t ix; uint8_t tab, row; char type[19]; int8_t group; };
static unsigned run_prize_pool(struct run_prize_part *out, unsigned max)
{
    static struct run_pool_part list[RUN_RESTOCKS][RUN_RESTOCK_PARTS];
    unsigned nl[RUN_RESTOCKS];
    uint8_t restock[256];
    memset(restock, 0, sizeof restock);
    run_restock_lists(list, nl);
    for (unsigned k = 0; k < RUN_RESTOCKS; k++)
        for (unsigned i = 0; i < nl[k]; i++) restock[list[k][i].ix] = 1;
    unsigned n = 0;
    for (int g = 0; g < G_COUNT; g++) {
        const unsigned t = k_prize_group[g].tab;
        unsigned row = 0;
        for (uint32_t r = k_run_tables[t].addr; r < k_run_tables[t].end && n < max;
             r += k_run_tables[t].stride, row++) {
            const uint32_t ix = k_run_tables[t].base + row;
            if (ix > 255u || restock[ix] || !run_row_real(r) || !psx_mod_read_word(r + RUN_PRICE_AT) ||
                psx_mod_read_byte(RUN_OWNED + ix) || psx_mod_read_byte(RUN_STOCK + ix))
                continue;
            struct run_prize_part *e = &out[n];
            run_text_at(r + RUN_TYPE_AT, 18, e->type);
            if (!e->type[0] || e->type[0] == '-') continue;
            if (k_prize_group[g].arm_unit >= 0 &&
                (strcmp(e->type, "ARM UNIT") == 0) != (k_prize_group[g].arm_unit == 1))
                continue;
            e->ix = (uint16_t)ix; e->tab = (uint8_t)t; e->row = (uint8_t)row; e->group = (int8_t)g;
            n++;
        }
    }
    return n;
}

static int run_prize_matches(const struct run_prize_part *e, const struct run_prize_opt *o)
{
    return e->group == o->group && (k_prize_group[o->group].label || strcmp(e->type, o->type) == 0);
}

static uint64_t run_prize_rng(unsigned m, uint64_t salt)
{
    return run_state.lineup.seed ^ (0x5052495Aull * (m + 1u)) ^ salt ^
           (0x9E3779B97F4A7C15ull * (uint64_t)(run_state.run.attempt + 1u));
}

/* The four boxes of milestone m: three part boxes, then P. */
static void run_prize_roll(unsigned m)
{
    static struct run_prize_part pool[256];
    const unsigned np = run_prize_pool(pool, 256);
    uint64_t rng = run_prize_rng(m, 0);
    int groups[G_COUNT], ng = 0;
    for (int g = 0; g < G_COUNT; g++)
        for (unsigned i = 0; i < np; i++) if (pool[i].group == g) { groups[ng++] = g; break; }
    for (int i = 0; i < ng; i++) {                       /* shuffle */
        const int j = i + (int)moa_run_below(&rng, (uint32_t)(ng - i));
        const int t = groups[i]; groups[i] = groups[j]; groups[j] = t;
    }
    run_prize_n = 0;
    for (int i = 0; i < ng && run_prize_n < 3; i++) {
        struct run_prize_opt *o = &run_prize_opt[run_prize_n];
        memset(o, 0, sizeof *o);
        o->group = (int8_t)groups[i];
        if (k_prize_group[o->group].label) {
            snprintf(o->label, sizeof o->label, "%s", k_prize_group[o->group].label);
        } else {
            /* a type of this group, not a label already up */
            const char *types[64]; unsigned nt = 0;
            for (unsigned k = 0; k < np && nt < 64; k++) {
                if (pool[k].group != o->group) continue;
                int dup = 0;
                for (unsigned t = 0; t < nt; t++) dup |= strcmp(types[t], pool[k].type) == 0;
                for (int b = 0; b < run_prize_n; b++) dup |= strncmp(run_prize_opt[b].label, pool[k].type, 16) == 0;
                if (!dup) types[nt++] = pool[k].type;
            }
            if (!nt) continue;
            snprintf(o->type, sizeof o->type, "%s", types[moa_run_below(&rng, nt)]);
            snprintf(o->label, sizeof o->label, "%s", o->type);
        }
        run_prize_n++;
    }
    /* The P box: always there, always last. */
    const uint8_t pay = k_prize_wins[m] <= 9u ? 2u : 3u;
    struct run_prize_opt *o = &run_prize_opt[run_prize_n++];
    memset(o, 0, sizeof *o);
    o->group = -1;
    o->p = pay;
    snprintf(o->label, sizeof o->label, "+%u P", pay);
}

static void run_prize_hide(void)
{
    (void)psx_mod_set_overlay_image(NULL, 0, 0);
    if (!run_prize_overlay) psx_mod_set_loading_screen(NULL, 0);
}

/* Up on the screen: over the live game if the renderer can, else as the
 * loading picture (drawn opaque). */
static int run_prize_present(int choosing, const char *title, const char *line,
                             const struct moa_screen_tile *tile)
{
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    for (int pass = 0; pass < 2; pass++) {
        const int see = pass == 0;
        const char *labels[4];
        for (int i = 0; i < run_prize_n; i++) labels[i] = run_prize_opt[i].label;
        const uint32_t *px = choosing
            ? moa_screen_render_choice(ww, wh, title, line, labels, run_prize_n, run_prize_cur,
                                       "UP / DOWN  CHOOSE     X  TAKE", see, &w, &h)
            : moa_screen_render_tiles(ww, wh, title, line, tile, tile ? 1 : 0,
                                      "PRESS ANY BUTTON", see, &w, &h);
        if (!px) return 0;
        if (see && psx_mod_set_overlay_image(px, w, h)) { run_prize_overlay = 1; return 1; }
        if (!see) {
            run_prize_overlay = 0;
            psx_mod_set_loading_screen("ROGUE ARENA  Prize", -1);
            psx_mod_set_loading_image(px, w, h);
            return 1;
        }
    }
    return 0;
}

static void run_prize_draw_choice(void)
{
    char title[32], line[48];
    snprintf(title, sizeof title, "PRIZE  RANK %u", (unsigned)(MOA_RUN_SLOTS - k_prize_wins[run_prize_m]));
    snprintf(line, sizeof line, "CHOOSE ONE   YOU HAVE %u P", (unsigned)psx_mod_read_word(RUN_CREDITS));
    if (!run_prize_present(1, title, line, NULL)) { run_prize_stage = 0; run_log("prize: no picture"); }
}

static int run_prize_due(void)
{
    for (unsigned m = 0; m < MOA_RUN_PRIZES; m++)
        if (run_state.run.wins >= k_prize_wins[m] && !(run_state.run.prizes & (1u << m))) return (int)m;
    return -1;
}

static void run_prize_open(int m)
{
    if (!run_show_font()) return;
    run_prize_m = m;
    run_prize_roll((unsigned)m);
    if (!run_prize_n) return;
    run_prize_cur = 0;
    run_prize_stage = 1;
    run_prize_armed = 0;
    run_prize_frames = 0;
    run_prize_held = 0xFFFFu;
    run_prize_draw_choice();
    if (run_prize_stage)
    {
        char all[80] = "";
        for (int i = 0; i < run_prize_n; i++)
            snprintf(all + strlen(all), sizeof all - strlen(all), "%s%s", i ? " / " : "", run_prize_opt[i].label);
        if (run_prize_stage)
            run_log("prize %d (win %u): %s%s", m, k_prize_wins[m], all,
                    run_prize_overlay ? "" : " (loading picture)");
    }
}

/* The box taken: the part or P, booked in the run file at once. */
static void run_prize_take(void)
{
    const struct run_prize_opt *o = &run_prize_opt[run_prize_cur];
    struct moa_run_progress *r = &run_state.run;
    const unsigned m = (unsigned)run_prize_m;
    static struct moa_screen_tile tile;
    static uint32_t thumb[RUN_THUMB * RUN_THUMB];
    static char name[19], cat[19];
    char title[32], line[48];
    memset(&tile, 0, sizeof tile);
    snprintf(title, sizeof title, "PRIZE  RANK %u", (unsigned)(MOA_RUN_SLOTS - k_prize_wins[m]));
    if (o->p) {
        r->points = (uint8_t)(r->points + o->p);
        r->p_paid = (uint8_t)(r->p_paid + o->p);
        psx_mod_write_word(RUN_CREDITS, psx_mod_read_word(RUN_CREDITS) + o->p);
        r->prize_ix[m] = MOA_RUN_PRIZE_P;
        snprintf(line, sizeof line, "+%u P   YOU HAVE %u P", o->p, (unsigned)psx_mod_read_word(RUN_CREDITS));
        run_log("prize %u: +%u P", m, o->p);
    } else {
        static struct run_prize_part pool[256];
        const unsigned np = run_prize_pool(pool, 256);
        unsigned hits = 0;
        for (unsigned i = 0; i < np; i++) hits += run_prize_matches(&pool[i], o);
        if (!hits) { run_log("prize %u: nothing left in %s", m, o->label); return; }
        uint64_t rng = run_prize_rng(m, 0x50415254ull * (uint64_t)(run_prize_cur + 1));
        unsigned pick = moa_run_below(&rng, hits);
        const struct run_prize_part *e = NULL;
        for (unsigned i = 0; i < np; i++)
            if (run_prize_matches(&pool[i], o) && pick-- == 0) { e = &pool[i]; break; }
        const uint32_t row = k_run_tables[e->tab].addr + (uint32_t)e->row * k_run_tables[e->tab].stride;
        const int pair = e->ix >= 157u && e->ix < 209u;
        psx_mod_write_byte(RUN_OWNED + e->ix, (uint8_t)(psx_mod_read_byte(RUN_OWNED + e->ix) + (pair ? 2u : 1u)));
        r->prize_ix[m] = (uint8_t)e->ix;
        run_text_at(row, 18, name);
        snprintf(cat, sizeof cat, "%s", o->type[0] ? o->type : k_run_tables[e->tab].label);
        const char *dir = getenv("PSX_MOA_MODELS_DIR");
        tile.category = cat;
        tile.name = name;
        tile.label = pair ? "NEW PAIR" : "NEW PART";
        tile.thumb = dir && dir[0] && moa_acv_has_part_models(dir) && run_part_thumb(dir, e->tab, e->row, thumb)
                   ? thumb : NULL;
        snprintf(line, sizeof line, "ADDED TO YOUR GARAGE");
        psx_mod_counter_add("moa.run.prize_part", 1);
        run_log("prize %u: %s (index %u)%s", m, name, (unsigned)e->ix, pair ? ", a pair" : "");
    }
    r->prizes |= (uint8_t)(1u << m);
    (void)moa_run_save(run_file_path(), &run_state);
    run_save_pending = 1;
    run_prize_stage = 2;
    run_prize_armed = 0;
    run_prize_frames = 0;
    if (!run_prize_present(0, title, line, o->p ? NULL : &tile)) { run_prize_stage = 0; run_prize_hide(); }
}

/* Per frame. */
static void run_prize_frame(void)
{
    if (!run_prize_stage) {
        if (!run_at_hub || run_autopilot || run_fade || !run_state_ok || run_state.run.over ||
            run_show_on || run_show_rank || !run_prize_where || psx_mod_read_byte(RUN_FIGHT_DONE) ||
            psx_mod_read_word(k_run_tables[0].addr) != 0x302D4448u)
            return;
        const int m = run_prize_due();
        if (m >= 0) run_prize_open(m);
        return;
    }
    psx_mod_write_half(RUN_PAD_BUFFER + 2u, 0xFFFFu);       /* the game sees nothing pressed */
    if (!run_prize_overlay) psx_mod_set_loading_screen("ROGUE ARENA  Prize", -1);
    const uint16_t b = psx_mod_host_pad_buttons(0);          /* active low */
    const uint16_t down = (uint16_t)(run_prize_held & ~b);   /* newly pressed */
    run_prize_held = b;
    run_prize_frames++;
    if (b == 0xFFFFu) run_prize_armed = 1;
    if (!run_prize_armed || run_prize_frames < 20u || !down) return;
    if (run_prize_stage == 2) {
        run_prize_stage = 0;
        run_prize_hide();
        run_log("prize: closed");
        return;
    }
    if (down & (RUN_PAD_UP | RUN_PAD_DOWN)) {
        run_prize_cur = (run_prize_cur + ((down & RUN_PAD_UP) ? run_prize_n - 1 : 1)) % run_prize_n;
        run_prize_draw_choice();
    } else if (down & RUN_PAD_X) {
        run_prize_take();
    }
}

static void run_maybe_return(void);

static void run_hub_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_HUB_RESUME) return;
    if (run_state_ok && run_state.run.over) { run_loss_wait = 0; run_maybe_return(); return; }
    run_reach_hub("moa.run.hub_reached");
    run_prize_where = 1;
    if (run_state_ok) {
        run_apply_prices();
        run_restock_catch_up("hub");
    }
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
    /* The win's 1 P comes with the game's payout: save once it is paid (or
     * after about 5 s of arena screen, should the payout never run). */
    if (run_pay_due && ++run_pay_wait < 300u) return;
    run_pay_due = 0;
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
    if (ok & 0xFFu) {                       /* the card now holds all P paid and every prize */
        run_state.run.p_card = run_state.run.p_paid;
        run_state.run.prize_saved = run_state.run.prizes;
        (void)moa_run_save(run_file_path(), &run_state);
    }
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
    /* The ghost is the build that lost, parts bought included: the garage's
     * AC (0x80036FB0, the save's part order) into the card record's order. */
    {
        uint8_t g[12];
        for (unsigned i = 0; i < 12; i++) g[i] = psx_mod_read_byte(0x80036FB0u + i);
        if (g[0] != 0xFF && g[2] != 0xFF && g[3] != 0xFF && g[11] != 0xFF) {
            const uint8_t rec[13] = { g[0], g[2], g[3], g[11], g[5], g[4], g[6], g[7], g[9], g[8],
                                      psx_mod_read_byte(0x8003EA70u), psx_mod_read_byte(0x8003EA71u), g[1] };
            memcpy(run_state.run.start_ac, rec, sizeof rec);
        }
    }
    moa_run_lose(&run_state);
    (void)moa_run_save(run_file_path(), &run_state);
    run_loss_wait = RUN_LOSS_FRAMES;
    psx_mod_counter_add("moa.run.loss", 1);
    run_log("loss: run over (%s)", where);
}

static void run_maybe_return(void)
{
    if (!run_state.run.over || run_loss_wait) return;
    if (run_sent) return;
    run_sent = 1;
    psx_mod_counter_add("moa.run.over", 1);
    run_log("loss: back to the launcher");
    psx_mod_request_return_to_launcher();
}

/* A new win over arena 0 entry `who`, booked once: into the run file at once
 * (from the fight as soon as it shows the win, so quitting or a crash before
 * the arena screen can't lose it). The 1 P and the restock wait for the arena
 * screen (run_result_entry): the shop's part tables are not in memory during
 * a fight. 1 = newly booked. */
static int run_book_win(uint8_t who, const char *where)
{
    if (!run_state_ok || run_state.run.over || who >= MOA_RUN_SLOTS - 1u) return 0;
    uint32_t *word = &run_state.run.beaten[who >> 5];
    const uint32_t bit = 1u << (who & 31u);
    if (*word & bit) return 0;                           /* already booked */
    if (!moa_run_win(&run_state)) return 0;
    *word |= bit;
    run_win_screen++;
    (void)moa_run_save(run_file_path(), &run_state);
    psx_mod_counter_add("moa.run.win", 1);
    run_log("win: entry %u booked (%s), wins %u, rank %u, P %u", who, where,
            (unsigned)run_state.run.wins, (unsigned)(MOA_RUN_SLOTS - run_state.run.wins),
            (unsigned)run_state.run.points);
    return 1;
}

/* A fight's result in the run's arena: a new win pays 1 P, a loss ends the
 * run (the file keeps it for Retry) and hands back to the launcher. */
static void run_result_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_RESULT) return;
    run_maybe_return();
    if (!run_at_hub || !run_state_ok) {
        if (psx_mod_read_byte(RUN_FIGHT_DONE) && !run_told) {
            run_told = 1;
            run_log("result: not booked (at hub %d, run file loaded %d)", run_at_hub, run_state_ok);
        }
        return;
    }
    if (!psx_mod_read_byte(RUN_FIGHT_DONE)) {
        run_result_seen = 0;
        run_prize_where = 1;                   /* the arena screen: prizes may open */
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
    if (!psx_mod_read_half(RUN_FIGHT_WON)) {
        run_book_loss("arena screen");
        return;
    }
    /* Only a first win over that opponent counts (refights pay nothing). */
    const uint32_t flags = psx_mod_read_word(st + RUN_STATE_FLAGS);
    const uint8_t who = psx_mod_read_byte(RUN_FIGHT_OPPONENT);
    if (flags < 0x80000000u || flags >= 0x80200000u || who >= MOA_RUN_SLOTS ||
        psx_mod_read_byte(flags + who) == 1u) {
        run_pay = 0;
        if (run_win_screen) run_log("win: booked in the fight but the arena calls it a refight");
        run_win_screen = 0;
        psx_mod_counter_add("moa.run.refight", 1);
        run_log("win: refight, nothing paid");
        return;
    }
    (void)run_book_win(who, "arena screen");             /* nothing if the fight booked it */
    if (!run_win_screen) return;
    /* The game's prize screen pays this win's 1 P. Any other P not yet in the
     * credits (a win booked in a fight that never reached this screen) is
     * added here. Added, never set: the credits are what shopping left. */
    run_win_screen = 0;
    struct moa_run_progress *r = &run_state.run;
    const uint32_t owed = r->points > r->p_paid ? (uint32_t)(r->points - r->p_paid) : 1u;
    const uint32_t credits = psx_mod_read_word(RUN_CREDITS);
    psx_mod_write_word(RUN_CREDITS, credits + owed - 1u);
    r->p_paid = r->points;
    (void)moa_run_save(run_file_path(), &run_state);
    run_pay = 1;
    run_pay_due = 1;
    run_pay_wait = 0;
    run_log("win: credits %u + %u (P earned %u)", (unsigned)credits, (unsigned)owed,
            (unsigned)r->points);
    run_save_pending = 1;                                 /* restocks: at the hub (run_restock_catch_up) */
}

/* The payout after a run fight: the game's prize screen shows and pays
 * run_pay (1 P on a new win, nothing on a refight), not arena 0's stock
 * money (which paid 20000). */
static void run_payout_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || !run_at_hub || psx_mod_read_word(address) != RUN_SIG_PAYOUT) return;
    const uint32_t st = cpu->gpr[4];
    if (st < 0x80000000u || st >= 0x80200000u) return;
    if (psx_mod_read_byte(st + RUN_STATE_ARENA) != RUN_ARENA) {
        run_log("payout: not the run's arena (%u)", psx_mod_read_byte(st + RUN_STATE_ARENA));
        return;
    }
    const uint32_t tab = psx_mod_read_word(st + 0xB8u);
    const uint8_t who = psx_mod_read_byte(RUN_FIGHT_OPPONENT);
    if (tab < 0x80000000u || tab >= 0x80200000u || who >= MOA_RUN_SLOTS) {
        run_log("payout: table %08x, opponent %u: left alone", (unsigned)tab, who);
        return;
    }
    const uint32_t was = psx_mod_read_word(tab + 8u * who);
    psx_mod_write_word(tab + 8u * who, run_pay);
    run_pay_due = 0;
    run_log("payout: %u (stock %u), credits %u", (unsigned)run_pay, (unsigned)was,
            (unsigned)psx_mod_read_word(RUN_CREDITS));
    run_pay = 0;
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

static void run_a0_flip(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!run_a0(cpu, 17)) return;
    cpu->gpr[2] = 1;                                      /* no page flip (runs every frame) */
}

/* ---- the run's opponents in arena 0 ---------------------------------------- */

/* MENU_TIM.T's entry table, read from the disc's first sector of the file
 * (halfword count, then each entry's start sector). The game keeps no copy
 * we could find in RAM (the old file-table lookup always gave 0), so the
 * table comes from disc once per menu LBA. */
static uint8_t run_hdr_sec[2048];
static uint32_t run_hdr_lba;

static uint32_t run_index(void)
{
    const uint32_t lba = run_state.lineup.menu_lba;
    if (!lba) return 0;
    if (run_hdr_lba != lba) {
        if (!psx_mod_read_disc_sectors(lba, 1u, run_hdr_sec)) return 0;
        run_hdr_lba = lba;
        run_log("menu table: %u entries, entry 0 at %u, entry 1 at %u (expect 1, 37)",
                (unsigned)(run_hdr_sec[0] | run_hdr_sec[1] << 8),
                (unsigned)(run_hdr_sec[2] | run_hdr_sec[3] << 8), (unsigned)(run_hdr_sec[4] | run_hdr_sec[5] << 8));
    }
    return 1u;
}

/* Start sector of entry k (relative to the file). */
static uint32_t run_hdr(uint32_t k)
{
    if (2u + 2u * k + 1u >= sizeof run_hdr_sec) return 0;
    return (uint32_t)(run_hdr_sec[2 + 2 * k] | run_hdr_sec[3 + 2 * k] << 8);
}

/* First entry and count of a source arena; 0 = not available. */
static int run_source(uint32_t ix, uint8_t arena, uint32_t *first, uint32_t *count)
{
    if (arena == RUN_PP_ARENA) {
        *first = RUN_PP_FIRST; *count = 50u;
        /* Phantasma's index covers its block only once installed. */
        return run_hdr((RUN_PP_FIRST + 203u)) >
               run_hdr(RUN_PP_FIRST);
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

/* Whole-arena reads waiting to be rebuilt. A fix is applied the moment its
 * buffer holds arena 0's stock entry exactly as the disc has it (the read
 * has landed: the checksum is the entry's last word, so it lands last), and
 * dropped once the CD queue is empty without that happening (the memory was
 * reused). So a fix can never write into memory that isn't that read any
 * more, and a new read into the same buffer replaces the older fix. If the
 * buffer already held the stock entry when the read was asked for, landing
 * can't be told apart, so that fix waits for the empty queue. */
#define RUN_FIX_MAX 16
static struct { int kind; uint32_t buf, bytes, sum; int early; } run_fix[RUN_FIX_MAX];
static int run_fix_landed(int i);

/* Arena 0's stock whole-arena entry `kind` (builds, names, rewards, AI) as
 * on the disc, read once per menu file. NULL = not available. */
static const uint8_t *run_stock_entry(int kind, uint32_t bytes)
{
    static uint8_t ent[4][4096];
    static uint32_t got_lba[4], got_bytes[4];
    if (kind < 0 || kind > 3 || bytes > sizeof ent[0] || !run_index()) return NULL;
    if (got_lba[kind] == run_state.lineup.menu_lba && got_bytes[kind] == bytes) return ent[kind];
    uint32_t first, count;
    if (!run_source(1u, RUN_ARENA, &first, &count)) return NULL;
    const uint32_t entry = first + 4u * count + (uint32_t)kind;
    const uint32_t a = run_hdr(entry), b = run_hdr(entry + 1u);
    if (b <= a || (b - a) > 2u || bytes > (b - a) * 2048u ||
        !psx_mod_read_disc_sectors(run_state.lineup.menu_lba + a, b - a, ent[kind]))
        return NULL;
    got_lba[kind] = run_state.lineup.menu_lba;
    got_bytes[kind] = bytes;
    return ent[kind];
}

static uint32_t run_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void run_queue_fix(int kind, uint8_t arena, struct CPUState *cpu)
{
    if (!run_on() || !run_at_hub) return;
    static const char *const k_kind[4] = { "builds", "names", "rewards", "AI" };
    if (!run_state_ok || arena != RUN_ARENA) {
        if (arena == RUN_ARENA) run_log("data %s: not queued (run file loaded %d)", k_kind[kind], run_state_ok);
        return;
    }
    const uint32_t bytes = cpu->gpr[6], buf = cpu->gpr[7];
    const uint8_t *stock = NULL;
    if (bytes < 8u || bytes > 4096u || (bytes & 3u) || buf < 0x80000000u ||
        buf + bytes > 0x80200000u || !(stock = run_stock_entry(kind, bytes))) {
        run_log("data %s: not queued (bytes %u, buffer %08x, stock entry %s)", k_kind[kind],
                (unsigned)bytes, (unsigned)buf, stock ? "read" : "not read");
        return;
    }
    int at = 0;
    while (at < run_fixes && run_fix[at].buf != buf) ++at;    /* same buffer: replace */
    if (at == RUN_FIX_MAX) {                                  /* full: the oldest goes */
        memmove(run_fix, run_fix + 1, sizeof run_fix[0] * (RUN_FIX_MAX - 1));
        at = RUN_FIX_MAX - 1;
        run_log("data: queue full, oldest fix dropped");
    }
    run_fix[at].kind = kind;
    run_fix[at].buf = buf;
    run_fix[at].bytes = bytes;
    run_fix[at].sum = run_le32(stock + bytes - 4u);
    run_fix[at].early = 0;
    if (at == run_fixes) run_fixes++;
    run_fix[at].early = run_fix_landed(at);
    run_log("data %s: queued (bytes %u, buffer %08x)", k_kind[kind], (unsigned)bytes, (unsigned)buf);
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
    /* Each source entry is read from disc once and kept: a lineup draws from
     * many arenas, and re-reading per opponent stalled the arena screen. */
    enum { CACHE = 64 };
    static uint8_t sec[CACHE][4096];
    static uint32_t key[CACHE], lba[CACHE];
    static int used;
    uint32_t first, count;
    if (!run_source(ix, p.arena, &first, &count) || p.rank >= count) return 0;
    const uint32_t entry = first + 4u * count + (uint32_t)kind;
    int c = 0;
    while (c < used && (key[c] != entry || lba[c] != run_state.lineup.menu_lba)) ++c;
    if (c == used) {
        const uint32_t a = run_hdr(entry), b = run_hdr(entry + 1u);
        if (used == CACHE) used = c = 0;              /* full: start over */
        if (b <= a || (b - a) > 2u ||
            !psx_mod_read_disc_sectors(run_state.lineup.menu_lba + a, b - a, sec[c]))
            return 0;
        key[c] = entry; lba[c] = run_state.lineup.menu_lba;
        used = c + 1;
    }
    if ((p.rank + 1u) * stride > 4096u) return 0;
    memcpy(out, sec[c] + p.rank * stride, stride);
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
        run_log("data kind %d: skipped (index %08x, menu lba %u, bytes %u)", kind,
                (unsigned)ix, (unsigned)run_state.lineup.menu_lba, (unsigned)bytes);
        return;
    }
    int missing = 0;
    for (uint32_t i = 0; i < bytes; ++i) out[i] = psx_mod_read_byte(buf + i);
    for (uint32_t e = 0; e + 1u < MOA_RUN_SLOTS; ++e) {   /* entry 49: the player */
        uint8_t *rec = out + e * stride;
        const struct moa_run_pick p = run_state.lineup.slot[MOA_RUN_SLOTS - 1u - e];
        if (kind == KIND_REWARDS) {                       /* every win pays 1 P (the game's own prize screen) */
            memset(rec, 0, 4);
            rec[0] = 1;
            memset(rec + 4, 0xFF, 4);
            continue;
        }
        if (p.arena == MOA_RUN_PLAYER) continue;
        if (p.arena == MOA_RUN_GHOST) {
            if (p.rank >= MOA_RUN_SLOTS) continue;
            const struct moa_run_ghost *g = &run_state.lineup.ghost[p.rank];
            if (kind == KIND_BUILDS) {
                char was[64], now[64];
                for (int i = 0; i < 16; ++i) snprintf(was + 3 * i, 4, "%02x ", rec[i]);
                moa_run_ghost_record(g, run_state.lineup.seed, rec);
                for (int i = 0; i < 16; ++i) snprintf(now + 3 * i, 4, "%02x ", rec[i]);
                run_log("ghost build at entry %u: stock %s", (unsigned)e, was);
                run_log("ghost build at entry %u: ghost %s", (unsigned)e, now);
            }
            else if (kind == KIND_NAMES) {
                memset(rec, 0, 48);
                memcpy(rec, g->pilot, strnlen(g->pilot, 16));
                memcpy(rec + 24, g->ac, strnlen(g->ac, 16));
            } else if (g->ai_from.arena != MOA_RUN_PLAYER && g->ai_from.arena != MOA_RUN_GHOST)
                (void)run_record(ix, g->ai_from, kind, stride, rec);
            continue;
        }
        if (!run_record(ix, p, kind, stride, rec)) { psx_mod_counter_add("moa.run.data_missing", 1); missing++; }
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
    run_log("data kind %d: applied (%d records missing, %u ghosts)", kind, missing,
            (unsigned)run_state.lineup.ghosts);
}

/* 1 = the buffer holds the stock entry exactly (its read has landed). */
static int run_fix_landed(int i)
{
    const uint32_t buf = run_fix[i].buf, bytes = run_fix[i].bytes;
    if (psx_mod_read_word(buf + bytes - 4u) != run_fix[i].sum) return 0;
    const uint8_t *stock = run_stock_entry(run_fix[i].kind, bytes);
    if (!stock) return 0;
    for (uint32_t k = 0; k + 4u < bytes; k += 4u)
        if (psx_mod_read_word(buf + k) != run_le32(stock + k)) return 0;
    return 1;
}

/* Each queued read, as soon as it is in; leftovers once the queue is empty. */
static void run_apply_fixes(void)
{
    if (!run_fixes) return;
    const uint32_t rd = psx_mod_read_word(RUN_CDQ + 0x37Cu);
    const int idle = rd == psx_mod_read_word(RUN_CDQ + 0x378u) && psx_mod_read_byte(rd) == 0u;
    int n = 0;
    for (int i = 0; i < run_fixes; ++i) {
        if ((idle || !run_fix[i].early) && run_fix_landed(i)) {
            run_apply_fix(run_fix[i].kind, run_fix[i].buf, run_fix[i].bytes);
            continue;
        }
        if (idle) {
            psx_mod_counter_add("moa.run.data_stale", 1);
            run_log("data kind %d: dropped (buffer %08x no longer holds the read)",
                    run_fix[i].kind, (unsigned)run_fix[i].buf);
            continue;
        }
        run_fix[n++] = run_fix[i];
    }
    run_fixes = n;
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
    if (run_autopilot && run_stage == STAGE_DIRECT && entry == RUN_HUB_ARENA)
        run_reach_hub("moa.run.direct_arena");   /* short wait at normal speed, then hand over */
    if (entry == 1 && run_state_ok) run_restock_catch_up("shop");   /* before the shop lists its stock */
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
    run_prize_where = 0;                          /* in a fight: no prize screen */
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
        if (!won && !lost) run_result_seen = 0;   /* a new fight: its result is not booked yet */
        if (done && !won && lost) run_book_loss("in fight");
        if (done && won && !lost) (void)run_book_win(psx_mod_read_byte(RUN_FIGHT_OPPONENT), "in fight");
        /* The fight recorder (moa_run_record.c): samples while it runs, the
         * summary once the result is in. */
        if (run_state_ok && !won && !lost)
            moa_rec_sample(run_file_path(), run_state.run.attempt, run_state.run.wins,
                           psx_mod_read_byte(RUN_FIGHT_OPPONENT));
        else if (won || lost) {
            const char *sum = moa_rec_end(won != 0);
            if (sum) run_log("record: %s", sum);
        }
    }
    const uint16_t b = psx_mod_read_half(RUN_PAD_BUFFER + 2u);
    if (!(b & RUN_PAD_SELECT)) {                  /* held (active low) */
        psx_mod_write_half(RUN_PAD_BUFFER + 2u, (uint16_t)(b | RUN_PAD_SELECT));
        psx_mod_counter_add("moa.run.select_blocked", 1);
    }
}

static void run_frame_hook(void)
{
    run_check_session();
    if (!run_on()) return;
    run_apply_fixes();
    if (run_at_hub && psx_mod_read_byte(RUN_ARENA_LOCK)) {
        psx_mod_write_byte(RUN_ARENA_LOCK, 0);
        psx_mod_counter_add("moa.run.arena_unlocked", 1);
    }
    run_show_frame();
    run_prize_frame();
    if (run_loss_wait) run_loss_wait--;      /* the return itself is made in a guest hook */
    if (run_autopilot && run_stage == STAGE_BACK) {
        if (++run_frames - run_back_start >= (run_direct ? RUN_DIRECT_HOLD : RUN_BACK_FRAMES))
            run_finish_handover();
        else psx_mod_set_loading_screen("ROGUE ARENA  Preparing your run", 98);
        return;
    }
    if (run_autopilot) {
        if (!psx_mod_fast_forward_requested()) psx_mod_request_fast_forward(1);
        const uint32_t f = run_frames++;
        int permille = (int)(f * 1000u / (RUN_HUB_TIMEOUT / 3u));
        if (permille > 950) permille = 950;
        psx_mod_set_loading_screen("ROGUE ARENA  Preparing your run", permille / 10);
        if (f >= RUN_HUB_TIMEOUT) run_reach_hub("moa.run.hub_timeout");
        return;
    }
    if (run_fade) {                            /* a short hold at 100%, then away */
        if (--run_fade) psx_mod_set_loading_screen("ROGUE ARENA  Ready", 100);
        else psx_mod_set_loading_screen(NULL, 0);
    }
}

PSX_MOD_CONSTRUCTOR(moa_arena_run_boot_register)
{
    (void)psx_game_register_offline_function_entry(RUN_MENU_ENTRY, run_menu_entry);
    (void)psx_game_register_offline_function_entry(RUN_MENU_INPUT_A, run_menu_input_entry);
    (void)psx_game_register_offline_function_entry(RUN_MENU_INPUT_B, run_menu_input_entry);
    (void)psx_game_register_offline_function_entry(RUN_MODE_CONFIRM, run_mode_confirm_entry);
    (void)psx_game_register_offline_function_filter(RUN_TITLE_OPEN, run_title_open_filter);
    (void)psx_game_register_offline_function_entry(RUN_D_HUB_INPUT, run_hub_input_entry);
    (void)psx_game_register_offline_function_filter(RUN_MOVIE_PLAY, run_movie_filter);
    (void)psx_game_register_offline_function_entry(RUN_HUB_RESUME, run_hub_entry);
    (void)psx_game_register_offline_function_entry(RUN_NEWGAME_FRAME, run_newgame_entry);
    (void)psx_game_register_offline_function_entry(RUN_RESULT, run_result_entry);
    (void)psx_game_register_offline_function_entry(RUN_PAYOUT, run_payout_entry);
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
    (void)psx_game_register_offline_instruction(RUN_A0_FLIP, RUN_A0_FLIP_I, run_a0_flip);
    mod_register_frame_hook(run_frame_hook);
}
