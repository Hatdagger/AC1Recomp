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
 * A direct boot (see "Direct boot" below: no title, no LOAD GAME, no card
 * screens; the save goes straight into RAM through the game's own unpack) is
 * the default boot (launcher FAST BOOT). PSX_MOA_RUN_DIRECT=0 (FAST BOOT
 * OFF) makes the menu presses above the boot, the fallback.
 *
 * If the hub does not come up in time the overlay is dropped and the
 * player is given the menus, so a wrong step can never leave a black screen.
 *
 * Offline only, and only when the launcher asked for it (PSX_MOA_ARENA_RUN=1).
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "cdrom.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>

#include "moa_arena_run.h"
#include "moa_hub_rank_bg.h"
#include "moa_ac_view.h"
#include "moa_screen.h"
#include "moa_run_record.h"
#include "moa_boot_trace.h"
#include "moa_card.h"

#include <time.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

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

/* ---- Direct boot (on; PSX_MOA_RUN_DIRECT=0 turns it off, see run_direct_wanted)
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
/* The title flags: 0x8003EA7A "go to the title" (1 in the exe's data) and
 * 0x8003EA7F "no logos". The menu entry already clears 0x8003EA7A right after
 * its title call (0x800A6EA4), even when this filter takes that call, so the
 * direct boot clearing it is harmless but not what fixed the after-fight hang
 * of the first test (boot chat, boot-loads.md section 2). Only the link-return
 * check 0x800A6BB0 sets it to 1 again; if it is 1 after a fight, the trace's
 * RAM snapshots show it. 0x800544A0 is the title's intro movie (logos road
 * only), so the direct boot doesn't replay it. */
#define RUN_D_TITLE_FLAG     0x8003EA7Au
#define RUN_D_NO_LOGOS       0x8003EA7Fu
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
/* What the card screen does after LOAD returns (state 0x8005CE68, the
 * title case: parent task's open routine == 0x800AAE94), mapped 2026-11-17
 * (mode-launch.md, "After LOAD: the card screen's finish"). Skipping it left
 * the AC's part textures out of video memory: no AC in the hub or the first
 * fight, until the arena's return path (0x800A70C0) queued them. */
#define RUN_D_OWNED          0x80036FECu  /* owned parts, 256 bytes ... */
#define RUN_D_OWNED_COPY     0x8003EB20u  /* ... copied here (garage/shop lists) */
static const uint32_t k_run_d_card_finish[] = {
    0x80090FD8u,                          /* part lists from the copy */
    0x80083130u };                        /* empty core/booster/head/arms/legs -> defaults */
#define RUN_D_EMPTY_SLOTS    0x80067244u  /* (1): fallback files for empty slots */
#define RUN_D_SIG_EMPTY_SLOTS 0x27BDFFE0u
#define RUN_D_AC_TEXTURES    0x800822B0u  /* queue the garage AC's part textures */
#define RUN_D_SIG_AC_TEXTURES 0x27BDFFE0u
#define RUN_D_MOVE_IMAGE     0x8002C81Cu  /* boot exe MoveImage(rect, x, y) */
#define RUN_D_WEAPON_RECT    0x800CB8B8u  /* (384,256,64,64) -> (768,448) */
#define RUN_D_WEAPON_CLUT    0x800CB8C0u  /* (320,482,256,1) -> (0,493), (0,494) */
/* The disc queue (boot exe 0x800160C4): a ring of 16 entries of 48 bytes;
 * write pointer +888, read pointer +892 from 0x80043C70. Empty when equal.
 * The card screen waits for it (0x800589DC) before the hub opens; the hub's
 * first state waits for its backdrop task (0x80056DBC), whose two MENU_TIM
 * loads queue behind the AC's seven part textures (0x800822B0). Opening the
 * hub in the same frame as those seven left the hub stalled in two of three
 * tests (trace 2026-11-17: no hub state after the backdrop loads, 90 s
 * timeout). So the hub opens on a later menu frame, once the queue is empty. */
#define RUN_D_CDQ_WRITE      0x80043FE8u
#define RUN_D_CDQ_READ       0x80043FECu
#define RUN_D_HUB_WAIT_MAX   600u         /* frames: open anyway after this */
#define RUN_D_HUB_OPEN       0x800552C0u
#define RUN_D_SIG_HUB_OPEN   0x27BDFFB8u  /* addiu sp,sp,-72 */
#define RUN_D_HUB_INPUT      0x80055848u
#define RUN_D_SIG_HUB_INPUT  0x27BDFFC0u  /* addiu sp,sp,-64 */
/* Hub background by rank (karate belts, moa_hub_rank_bg.c). The hub's
 * background is a 64x64 4-bit tile + 16 colours that both hub opens put into
 * VRAM from these RAM copies (LoadImage at 0x800553AC/BC and 0x800572D8/E8);
 * the screens opened from the hub store VRAM back into them. Writing the
 * copies at each hub open shows our tile. */
#define RUN_HUB_OPEN_B       0x800571D0u  /* the other hub open (called from 0x800AC2CC) */
#define RUN_SIG_HUB_OPEN_B   0x27BDFFC8u  /* addiu sp,sp,-56 */
#define RUN_HUB_TILE         0x8004ACA8u  /* -> VRAM (416,0,16,64) */
#define RUN_HUB_TILE_CLUT    0x8004B6F0u  /* -> VRAM (368,225,16,1) */
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
static int run_direct_failed;              /* wanted, but could not start: menu presses */
static int run_quiet(void);
static int run_direct_hub_due;             /* the hub still to open (queue draining) */
static int run_hub_tile_last = -1;         /* tile last logged */
static uint32_t run_direct_hub_wait;
static uint32_t run_direct_buf;            /* guest buffer for the save */
/* At the hub: one O press (one menu back), at normal speed so the hub's
 * music starts as it would, then hand over behind the loading screen. */
#define RUN_BACK_FRAMES      90u
#define RUN_PAD_CIRCLE       0x2000u
/* Direct boot: the Arena list loads its names from the disc after it opens;
 * this short hold at normal speed covers that before the hand-over. */
#define RUN_DIRECT_HOLD      12u          /* frames at normal speed after the hub opens */
#define RUN_DIRECT_TIMEOUT   (60u * 20u)  /* direct road: the hub opens by frame ~110 */
static uint32_t run_back_start;
static int run_stage;
static uint32_t run_newgame_seen;          /* run_frames when that menu last ran */
static int run_result_seen;                /* this fight's result is booked */
static uint32_t run_pay;                   /* what the next payout pays: 1 on a new win, else 0 */
static int run_prize_paid;                 /* a new win was just paid: its prize may open now */
static int run_save_pending;               /* a win waits for the autosave */
static unsigned run_win_screen;            /* booked wins waiting for the arena screen (P, restock) */
static uint8_t run_win_told = 0xFF;        /* last entry run_book_win logged (booked or skipped) */
/* After a win that restocks the shop, back to the hub (where the restock is
 * shown and the Shop is one press away) instead of staying on the arena
 * screen: once the result is applied and saved, O is pressed for the player
 * (the arena screen's own way out) until the hub is back. */
enum { TOHUB_NONE = 0, TOHUB_ARMED, TOHUB_PRESSING };
static int run_to_hub;
static uint32_t run_to_hub_frames;         /* arena-screen frames spent pressing */
static int run_to_hub_o;                   /* O down this frame */
static int run_to_hub_sampled;             /* the menu's controller hook took the press */
#define RUN_TOHUB_PAD_FROM   120u          /* no hub yet: press through the menu's pad block too */
#define RUN_TOHUB_GIVE_UP    600u

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
    extern uint64_t s_frame_count;
    fprintf(f, "[%llu] ", (unsigned long long)s_frame_count);   /* host frame: shows delays */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static uint32_t run_loss_wait;             /* frames until the loss screen opens */
/* After a loss: a choice over the game (RETRY ARENA, NEW ARENA, QUIT TO
 * LAUNCHER). The pick goes to the launcher in "<run file>.next" ("retry" or
 * "new"; none = just the launcher), which carries it out when it is back. */
static int run_end_stage;                  /* 0 not up, 1 choosing, 2 picked: back to the launcher */
static int run_end_cur, run_end_armed, run_end_overlay;
static uint32_t run_end_frames;
static uint16_t run_end_held;
static int run_overlay_up(void);
static void run_pad_lock(void);
/* Overlay hub (step 1; see "overlay hub" near the end of the file). */
static int run_hx;                         /* 0 off, 1 menu, 2 stage list, 3 next battle under way */
static int run_hx_off;                     /* O: the game's own hub until the player leaves it */
static uint32_t run_hx_hub_seen = 0xFFFFu; /* frames since the hub's input state last ran */
static uint32_t run_hx_list_seen = 0xFFFFu;/* frames since the run's arena list state last ran */
static int run_hx_go_entry = -1;           /* hub entry still to be opened (X pressed by the hub hook) */
static uint32_t run_hx_go_frames;
static int run_hx_next;                    /* next battle: 1 to the list, 2 list X in, 3 detail X in (fight box) */
static uint32_t run_hx_next_frames, run_hx_next_tries, run_hx_next_t;
static int run_hx_hold;                    /* write the stage pick until the fight runs */
static uint8_t run_hx_hold_id;
static void run_hx_fight_setup(void);
static void run_hx_fight_runs(void);
static void run_hx_reset(void);
static void run_hx_fail(const char *why);
static int run_hx_menu_pad(void);
static int run_sent, run_told;
static int run_fixes;

/* Hang watch state (see "hang watch" below). */
#define RUN_WATCH_FRAMES     (60u * 45u)   /* the replay and result screens fit well inside */
#define RUN_WATCH_LOAD       (60u * 15u)   /* a fight runs ~4 s after its AI is applied; players
                                            * closed hung loads after ~25 s, before 45 s */
static int run_watch_phase;               /* 0 off, 1 loading a fight, 2 after a fight (3: idle dump) */
/* Phase 3: at the hub (or a screen opened from it, like the Garage) and
 * neither the menus' controller sampling nor the fight's has run for 30 s:
 * the game is stuck (a Retry session hung opening the Garage, no files). */
#define RUN_WATCH_IDLE       (60u * 30u)
static uint32_t run_idle;                 /* frames since the game last sampled the controller */
static uint32_t run_watch_count;
static unsigned run_watch_dumps;
static char run_watch_stamp[24];          /* this session's start time, for dump names */
static uint32_t run_irq_again;            /* frames until the second interrupt snapshot; 0 = none */

static void run_watch(int phase)
{
    if (run_watch_phase == phase) return;
    run_watch_phase = phase;
    run_watch_count = 0;
}

static uint8_t run_arena_at_builds = 0xFF, run_arena_at_ai = 0xFF;

static void run_prize_reset(void);
static void run_reconcile(void);
static int run_prize_where;               /* defined with the prize picks below */
static int run_replay_state;              /* replay watch: 0 none, 1 running */
static uint32_t run_replay_t;
static void run_replay_over(const char *where);
/* Ghosts beaten in a fight, saved as cards once the arena screen is back. */
static uint8_t run_ghost_beaten[8];
static unsigned run_ghost_beaten_n;

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
    run_hub_tile_last = -1;
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
    run_win_told = 0xFF;
    run_to_hub = TOHUB_NONE;
    run_to_hub_frames = 0;
    run_to_hub_o = run_to_hub_sampled = 0;
    run_replay_state = 0;
    run_replay_t = 0;
    run_loss_wait = 0;
    run_end_stage = run_end_cur = run_end_armed = run_end_overlay = 0;
    run_end_frames = 0;
    run_end_held = 0xFFFFu;
    run_sent = run_told = 0;
    run_fixes = 0;
    run_arena_at_builds = run_arena_at_ai = 0xFF;
    run_direct = run_direct_press = 0;
    run_watch_phase = 0;
    run_watch_count = 0;
    run_idle = 0;
    run_watch_dumps = 0;
    run_watch_stamp[0] = 0;
    run_irq_again = 0;
    run_direct_failed = 0;
    run_direct_hub_due = 0;
    run_direct_hub_wait = 0;
    run_prize_paid = 0;
    run_prize_reset();
    moa_rec_reset();
    run_ghost_beaten_n = 0;
    run_hx_reset();
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
/* Arena 0's progress in RAM, as LOAD unpacks the save (+3276 -> 0x8004B568):
 * beaten flags at save +3386 + the arena's start (0 for arena 0), the
 * player's rank byte at +3553 + arena (docs/mapping/missions.md: 0x8004B67D,
 * main-arena rank). */
#define RUN_A0_BEATEN        0x8004B5D6u
#define RUN_A0_RANK          0x8004B67Du

/* A win booked in the fight but never shown on the arena screen (a hang, or
 * the window closed) is in the run file but not on the run card: the game
 * would offer that opponent again and the win would be skipped. So on load
 * the game's flags and rank are set to match the run file. */
static void run_sync_arena(void)
{
    if (!run_state_ok || run_state.run.over) return;
    unsigned fixed = 0;
    char ids[MOA_RUN_SLOTS * 4 + 1];
    ids[0] = 0;
    for (unsigned who = 0; who + 1u < MOA_RUN_SLOTS; who++) {
        if (!(run_state.run.beaten[who >> 5] & (1u << (who & 31u)))) continue;
        if (psx_mod_read_byte(RUN_A0_BEATEN + who) == 1u) continue;
        psx_mod_write_byte(RUN_A0_BEATEN + who, 1u);
        snprintf(ids + strlen(ids), sizeof ids - strlen(ids), " %u", who);
        fixed++;
    }
    const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
    const uint8_t want = (uint8_t)(MOA_RUN_SLOTS - wins);
    const uint8_t rank = psx_mod_read_byte(RUN_A0_RANK);
    if (rank > want && rank <= MOA_RUN_SLOTS) {
        psx_mod_write_byte(RUN_A0_RANK, want);
        fixed++;
    }
    if (fixed) {
        run_save_pending = 1;                    /* the card catches up at the next arena screen */
        psx_mod_counter_add("moa.run.arena_synced", 1);
    }
    run_log("load: arena %s (card rank %u, run file rank %u%s%s)",
            fixed ? "synced to the run file" : "matches the run file", (unsigned)rank, (unsigned)want,
            ids[0] ? "; beaten flags set:" : "", ids);
}

static void run_apply_state(void)
{
    /* The save carries the run; the run file is read for what comes next
     * (results, P, the arena). */
    const char *path = run_file_path();
    run_state_ok = path && path[0] && moa_run_load(path, &run_state) == 1;
    if (!run_state_ok) psx_mod_counter_add("moa.run.file_missing", 1);
    else { run_reconcile(); run_sync_arena(); }
}

static void run_finish_handover(void)
{
    run_autopilot = 0;
    run_at_hub = 1;
    run_prize_where = 1;                       /* at the hub: a prize due may open (test 2 waited for the Shop) */
    run_apply_state();
    if (run_quiet()) {
        run_fade = 0;
        psx_mod_set_loading_screen(NULL, 0);
    } else run_fade = RUN_FADE_FRAMES;
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

/* On by default (launcher FAST BOOT); PSX_MOA_RUN_DIRECT=0 falls back to the
 * menu presses. (It was off while the first builds missed the card screen's
 * finish and opened the hub before the disc queue was empty.) */
static int run_direct_wanted(void)
{
    const char *e = getenv("PSX_MOA_RUN_DIRECT");
    return !(e && e[0] == '0');
}

/* The direct road is quick, so it shows the game itself, no loading screen.
 * The menu-press road (FAST BOOT off, or a direct boot that could not
 * start) keeps the screen over the menus it drives. */
static int run_quiet(void)
{
    return run_direct_wanted() && !run_direct_failed;
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
           psx_mod_read_word(RUN_D_EMPTY_SLOTS) == RUN_D_SIG_EMPTY_SLOTS &&
           psx_mod_read_word(RUN_D_AC_TEXTURES) == RUN_D_SIG_AC_TEXTURES &&
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
    /* 0. What the title itself would have done (see RUN_D_TITLE_FLAG). */
    psx_mod_write_byte(RUN_D_TITLE_FLAG, 0);
    psx_mod_write_byte(RUN_D_NO_LOGOS, 0);
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
    /* 2b. The card screen's finish after a good LOAD (0x8005CFB0..0x8005D14C,
     * title case). The game then waits for the disc queue (0x800589DC); here
     * the menu loop drains it behind the loading screen. */
    for (uint32_t i = 0; i < 256u; i++)
        psx_mod_write_byte(RUN_D_OWNED_COPY + i, psx_mod_read_byte(RUN_D_OWNED + i));
    for (size_t i = 0; i < sizeof k_run_d_card_finish / sizeof k_run_d_card_finish[0]; i++)
        (void)psx_mod_call_guest(cpu, k_run_d_card_finish[i], ra, 0, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_EMPTY_SLOTS, ra, 1, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_AC_TEXTURES, ra, 0, 0, 0, 0);
    {   /* back weapon ids 52 and up keep a texture block aside; the hub open
         * (0x800554A8) moves it back */
        const uint8_t bl = psx_mod_read_byte(0x80036FB6u), br = psx_mod_read_byte(0x80036FB7u);
        if ((bl != 0xFFu && bl >= 52u) || (br != 0xFFu && br >= 52u)) {
            (void)psx_mod_call_guest(cpu, RUN_D_MOVE_IMAGE, ra, RUN_D_WEAPON_RECT, 768, 448, 0);
            (void)psx_mod_call_guest(cpu, RUN_D_MOVE_IMAGE, ra, RUN_D_WEAPON_CLUT, 0, 493, 0);
            (void)psx_mod_call_guest(cpu, RUN_D_MOVE_IMAGE, ra, RUN_D_WEAPON_CLUT, 0, 494, 0);
            (void)psx_mod_call_guest(cpu, RUN_D_DRAW_SYNC, ra, 0, 0, 0, 0);
        }
    }
    /* The run is in RAM: hand the data over now (the arena hooks wait for
     * it), keep the loading screen until the Arena list is up. */
    run_at_hub = 1;
    run_apply_state();
    run_apply_prices();                          /* shop in P; the hub-resume hook redoes it later */
    run_restock_catch_up("load");
    psx_mod_write_byte(RUN_ARENA_LOCK, 0);
    psx_mod_write_byte(RUN_D_MAIL_SHOWN, 1);     /* no mail box in the way */
    /* 3. The hub, on Arena: on a later menu frame, once the disc queue has
     * taken the AC's textures (run_direct_hub_frame, RUN_D_CDQ_WRITE). */
    run_direct = 1;
    run_direct_hub_due = 1;
    run_direct_hub_wait = 0;
    run_stage = STAGE_DIRECT;
    psx_mod_counter_add("moa.run.direct_boot", 1);
    run_log("direct: title flags cleared, save loaded, card screen finish replayed "
            "(frame %u); the hub opens once the disc queue is empty", (unsigned)run_frames);
    return 1;
}

/* Put the tile for the current rank (50 - wins) into the hub's RAM copies. */
static void run_hub_tile(void)
{
    if (!run_on() || !run_state_ok) return;
    const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
    const int rank = (int)(MOA_RUN_SLOTS - wins);
    const uint8_t *pix;
    const uint16_t *clut;
    const int t = moa_hub_rank_tile(rank, &pix, &clut);
    for (uint32_t i = 0; i < 2048u; i += 4u)
        psx_mod_write_word(RUN_HUB_TILE + i, (uint32_t)pix[i] | (uint32_t)pix[i + 1u] << 8 |
                                             (uint32_t)pix[i + 2u] << 16 | (uint32_t)pix[i + 3u] << 24);
    for (uint32_t i = 0; i < 16u; i += 2u)
        psx_mod_write_word(RUN_HUB_TILE_CLUT + 2u * i, (uint32_t)clut[i] | (uint32_t)clut[i + 1u] << 16);
    if (t != run_hub_tile_last) {
        run_hub_tile_last = t;
        run_log("hub: rank %d, %s belt", rank, moa_hub_rank_belt(rank));
    }
}

static void run_hub_open_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    const uint32_t sig = address == RUN_HUB_OPEN_B ? RUN_SIG_HUB_OPEN_B : RUN_D_SIG_HUB_OPEN;
    if (psx_mod_read_word(address) == sig) run_hub_tile();
}

/* From the menu's controller sampling, every menu frame of the boot. */
static void run_direct_hub_frame(struct CPUState *cpu)
{
    if (!run_direct_hub_due) return;
    const int empty = psx_mod_read_word(RUN_D_CDQ_WRITE) == psx_mod_read_word(RUN_D_CDQ_READ);
    if (!empty && ++run_direct_hub_wait < RUN_D_HUB_WAIT_MAX) return;
    run_direct_hub_due = 0;
    if (psx_mod_read_word(RUN_D_HUB_OPEN) != RUN_D_SIG_HUB_OPEN) {
        run_log("direct: hub open code not in memory, the menus take over");
        return;
    }
    run_hub_tile();                         /* in case a direct call skips the entry hook */
    const uint32_t hub = psx_mod_call_guest(cpu, RUN_D_HUB_OPEN, cpu->gpr[31], 0, 0, 0, 0);
    if (hub >= 0x80000000u && hub < 0x80200000u) psx_mod_write_byte(hub + RUN_HUB_ENTRY, RUN_HUB_ARENA);
    run_log("direct: hub opened on Arena (frame %u, waited %u frames for the disc queue%s)",
            (unsigned)run_frames, (unsigned)run_direct_hub_wait, empty ? "" : ", gave up");
    /* The player takes over right here, on the hub (Arena highlighted). The
     * old X press on Arena often never landed (4 of 5 sessions in the
     * 2026-11-17 test) and the boot then sat out the 90 s hub timeout. */
    run_reach_hub("moa.run.direct_hub");
}

static int run_title_open_filter(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_TITLE_OPEN) return 0;
    if (run_at_hub && !run_autopilot) {       /* the player quit to the title */
        psx_mod_counter_add("moa.run.return_from_title", 1);
        run_log("title: opened while at the hub (quit to the title), back to the launcher");
        psx_mod_request_return_to_launcher();
        return 0;
    }
    if (!run_autopilot) return 0;
    if (!run_direct && run_direct_wanted()) {
        if (run_direct_boot(cpu)) return 1;
        if (!run_direct_failed) {
            run_direct_failed = 1;                /* the menu presses, behind the loading screen */
            run_log("direct: could not start, the menu presses take over");
        }
    }
    cpu->gpr[4] = 1;
    return 0;
}

/* Hub input state (every frame the hub waits for the player): X once on
 * Arena for the boot, or on the entry the overlay hub picked (Shop, Garage,
 * Arena for NEXT BATTLE), pressed again each frame until the pick hook sees
 * it (one press often never landed in the 2026-11-17 boot tests). */
static void run_hub_input_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_D_SIG_HUB_INPUT) return;
    run_hx_hub_seen = 0;
    int entry = -1;
    if (run_direct_press) { run_direct_press = 0; entry = RUN_HUB_ARENA; }
    else if (run_hx_go_entry >= 0) entry = run_hx_go_entry;
    if (entry < 0) return;
    const uint32_t hub = cpu->gpr[4];
    if (hub >= 0x80000000u && hub < 0x80200000u) psx_mod_write_byte(hub + RUN_HUB_ENTRY, (uint8_t)entry);
    psx_mod_write_word(RUN_D_PAD + 0x18u, psx_mod_read_word(RUN_D_PAD + 0x18u) | RUN_PAD_X);
    psx_mod_write_word(RUN_D_PAD + 0x1Cu, psx_mod_read_word(RUN_D_PAD + 0x1Cu) & ~(uint32_t)RUN_PAD_X);
    if (run_hx_go_entry < 0) run_log("direct: X at the hub (frame %u)", (unsigned)run_frames);
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

static void run_direct_hub_frame(struct CPUState *cpu);

static void run_menu_input_entry(struct CPUState *cpu, uint32_t address)
{
    run_idle = 0;
    if (run_overlay_up() && run_on()) {
        const uint32_t w = psx_mod_read_word(address);
        if ((w == RUN_SIG_INPUT_A || w == RUN_SIG_INPUT_B) && !run_hx_menu_pad()) run_pad_lock();
        return;
    }
    if (run_to_hub == TOHUB_PRESSING && run_to_hub_o && !run_autopilot && run_on()) {
        const uint32_t w = psx_mod_read_word(address);
        if (w == RUN_SIG_INPUT_A || w == RUN_SIG_INPUT_B) {
            psx_mod_write_half(RUN_PAD_BUFFER + 2u, (uint16_t)~RUN_PAD_CIRCLE);
            if (!run_to_hub_sampled) {
                run_to_hub_sampled = 1;
                run_log("restock: O pressed through the menu's controller sampling");
            }
        }
        return;
    }
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
        run_direct_hub_frame(cpu);
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
        if (run_show_rank && run_at_hub && (!psx_mod_read_byte(RUN_FIGHT_DONE) || run_prize_paid)) run_show_open();
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
        /* While a prize is due, log what it waits for (once per change), so
         * a late prize screen shows which condition held it back. */
        static const char *waits;
        const char *why =
            !run_state_ok || run_state.run.over ? NULL :
            !run_at_hub ? "not at the hub" : run_autopilot ? "boot autopilot" :
            run_fade ? "loading screen fade" : run_show_on || run_show_rank ? "restock screen" :
            !run_prize_where && !run_prize_paid ? "not at the arena screen or hub" :
            psx_mod_read_byte(RUN_FIGHT_DONE) && !run_prize_paid ? "fight result not applied" :
            psx_mod_read_word(k_run_tables[0].addr) != 0x302D4448u ? "part tables not in memory" : "";
        if (!why || (why[0] && run_prize_due() < 0)) { waits = NULL; return; }
        if (why[0]) {
            if (why != waits) run_log("prize: due, waiting (%s)", why);
            waits = why;
            return;
        }
        waits = NULL;
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
    if (run_watch_phase == 2) run_watch(0);       /* back at the hub */
    if (run_hx_next >= 2) run_hx_fail("next battle: back at the hub, the fight did not start");
    run_replay_over("hub");
    if (run_state_ok && run_state.run.over) { run_loss_wait = 0; run_maybe_return(); return; }
    run_reach_hub("moa.run.hub_reached");
    run_prize_where = 1;
    if (run_to_hub) {
        run_log("restock: back at the hub (%s, %u frames)", run_to_hub == TOHUB_PRESSING ? "O pressed" :
                "the player left first", (unsigned)run_to_hub_frames);
        psx_mod_counter_add("moa.run.restock_to_hub", 1);
        run_to_hub = TOHUB_NONE;
        run_to_hub_o = 0;
    }
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
    if (run_end_stage != 2) return;            /* the loss screen's choice first */
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
    if (*word & bit) {                                   /* already booked */
        if (run_win_told != who) {
            run_win_told = who;
            psx_mod_counter_add("moa.run.win_already_booked", 1);
            run_log("win: entry %u already booked (%s), not counted again; wins %u", who, where,
                    (unsigned)run_state.run.wins);
        }
        return 0;
    }
    if (!moa_run_win(&run_state)) return 0;
    *word |= bit;
    run_win_told = who;                                  /* the fight calls again every frame */
    run_win_screen++;
    {   /* a ghost: its card is saved at the arena screen (run_ghost_save) */
        const struct moa_run_pick p = run_state.lineup.slot[MOA_RUN_SLOTS - 1u - who];
        if (p.arena == MOA_RUN_GHOST && p.rank < MOA_RUN_SLOTS &&
            run_ghost_beaten_n < sizeof run_ghost_beaten)
            run_ghost_beaten[run_ghost_beaten_n++] = who;
    }
    (void)moa_run_save(run_file_path(), &run_state);
    psx_mod_counter_add("moa.run.win", 1);
    run_log("win: entry %u booked (%s), wins %u, rank %u, P %u", who, where,
            (unsigned)run_state.run.wins, (unsigned)(MOA_RUN_SLOTS - run_state.run.wins),
            (unsigned)run_state.run.points);
    return 1;
}

/* Is `wins` one of the restock milestones (5, 10 ... 45, 49)? */
static int run_restock_win(unsigned wins)
{
    for (unsigned k = 0; k < RUN_RESTOCKS; k++) if (run_restock_wins(k) == wins) return 1;
    return 0;
}

/* Each arena-screen frame once the result is applied: when a restock win's
 * save is done and no prize is up or due, press O (3 frames in every 24)
 * until the hub resume hook sees the hub. The press goes into the raw pad
 * buffer from the menu's controller hook; if the hub isn't back after 2 s,
 * also into the menu's own new-press word (as the direct boot's X). */
static void run_to_hub_frame(void)
{
    run_to_hub_o = 0;
    if (run_to_hub == TOHUB_ARMED) {
        if (run_save_pending || run_prize_stage || run_prize_due() >= 0 || run_show_on) return;
        run_to_hub = TOHUB_PRESSING;
        run_to_hub_frames = 0;
        run_log("restock: result saved, leaving the arena screen for the hub");
    }
    if (run_to_hub != TOHUB_PRESSING) return;
    const uint32_t t = run_to_hub_frames++;
    if (t >= RUN_TOHUB_GIVE_UP) {
        run_to_hub = TOHUB_NONE;
        psx_mod_counter_add("moa.run.restock_to_hub_failed", 1);
        run_log("restock: the hub did not come back after %u frames of O; left to the player%s",
                (unsigned)t, run_to_hub_sampled ? "" : " (the menu's controller hook never ran)");
        return;
    }
    run_to_hub_o = (t % 24u) < 3u;
    if (run_to_hub_o && t >= RUN_TOHUB_PAD_FROM) {
        psx_mod_write_word(RUN_D_PAD + 0x18u, psx_mod_read_word(RUN_D_PAD + 0x18u) | RUN_PAD_CIRCLE);
        psx_mod_write_word(RUN_D_PAD + 0x1Cu, psx_mod_read_word(RUN_D_PAD + 0x1Cu) & ~(uint32_t)RUN_PAD_CIRCLE);
    }
}

/* A fight's result in the run's arena: a new win pays 1 P, a loss ends the
 * run (the file keeps it for Retry) and hands back to the launcher. */
static void run_result_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_RESULT) return;
    if (run_watch_phase == 2) run_watch(0);       /* back at the arena screen */
    run_replay_over("arena screen");
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
        run_to_hub_frame();
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
    if (run_restock_win(r->wins)) {
        run_to_hub = TOHUB_ARMED;
        run_to_hub_frames = 0;
        run_to_hub_o = run_to_hub_sampled = 0;
        run_log("restock: win %u restocks the shop, back to the hub once the result is saved", (unsigned)r->wins);
    }
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
    /* A new win is paid: its prize may open now, over the game's own result
     * windows (they wait while the prize screen holds the pad), instead of
     * ~9 s later when the game clears its fight-finished byte. */
    if (run_pay) run_prize_paid = 1;
    /* A restock win: its picture now, over the arena screen (as a prize
     * does), instead of after the game's ~23 s result wait and the hub. */
    if (run_pay && run_state_ok && run_restock_win(run_state.run.wins)) run_restock_catch_up("arena screen");
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
 * table comes from disc once per menu LBA.
 *
 * Project Phantasma: its package puts a longer copy of MENU_TIM.T in the
 * disc's spare area (the stock file plus a 604-sector block) and points the
 * file's directory record at it, but leaves the copy's entry table exactly as
 * shipped (885 entries). Its plug-in (mod_builtin_moa_phantasma.c, part A)
 * builds the longer table in memory: entries 885-887 empty at the stock end
 * (sector 2906), then arena 10's 204 entries from 888 in a fixed layout. So
 * the run reads the file where the effective disc has it now (not the LBA
 * the launcher saw on the unpatched image) and works out entries 885+ the
 * same way the plug-in does. */
#define RUN_MENU_STOCK_ENTRIES 885u
#define RUN_MENU_STOCK_END     2906u    /* stock file length in sectors */
#define RUN_PP_BLOCK_SECTORS   604u
static uint8_t run_hdr_sec[2048];
static uint32_t run_hdr_lba;
static int run_menu_pp;                /* 1 = the live file holds the arena-10 block */

/* MENU_TIM.T's first sector on the disc as the game reads it now. */
static uint32_t run_menu_lba(void)
{
    static uint32_t live;
    static int looked;
    if (!looked) {
        uint32_t lba = 0, size = 0;
        looked = 1;
        if (psx_mod_disc_file_extent("GG/MS/MENU_TIM.T", &lba, &size) && lba) {
            live = lba;
            run_menu_pp = size >= (RUN_MENU_STOCK_END + RUN_PP_BLOCK_SECTORS) * 2048u;
            run_log("menu file: live at %u (%u bytes, Phantasma block %s); run file says %u",
                    (unsigned)lba, (unsigned)size, run_menu_pp ? "present" : "absent",
                    (unsigned)run_state.lineup.menu_lba);
        } else {
            run_log("menu file: live position not found; using the run file's %u",
                    (unsigned)run_state.lineup.menu_lba);
        }
    }
    return live ? live : run_state.lineup.menu_lba;
}

static uint32_t run_index(void)
{
    const uint32_t lba = run_menu_lba();
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

/* Sectors of arena-10 block entry n (0-203), as the package lays it out:
 * 50 descriptions (1), 49 emblems (2) and an empty player slot, 50 portraits
 * (8), 50 second descriptions (1), builds (2), names (2), rewards (1), AI (1).
 * Same as pp_block_sectors in the plug-in. */
static uint32_t run_pp_block_sectors(uint32_t n)
{
    if (n < 50u) return 1u;
    if (n < 99u) return 2u;
    if (n == 99u) return 0u;
    if (n < 150u) return 8u;
    if (n < 200u) return 1u;
    if (n < 202u) return 2u;
    return 1u;
}

/* Start sector of entry k (relative to the file). */
static uint32_t run_hdr(uint32_t k)
{
    if (k >= RUN_MENU_STOCK_ENTRIES && run_menu_pp) {
        uint32_t at = RUN_MENU_STOCK_END;
        if (k < RUN_PP_FIRST) return at;
        for (uint32_t n = 0; n < k - RUN_PP_FIRST && n < 204u; ++n) at += run_pp_block_sectors(n);
        return at;                                    /* k = 888+204: the block's end */
    }
    if (k > RUN_MENU_STOCK_ENTRIES) return 0;         /* past the stock end word */
    if (2u + 2u * k + 1u >= sizeof run_hdr_sec) return 0;
    return (uint32_t)(run_hdr_sec[2 + 2 * k] | run_hdr_sec[3 + 2 * k] << 8);
}

/* First entry and count of a source arena; 0 = not available. */
static int run_source(uint32_t ix, uint8_t arena, uint32_t *first, uint32_t *count)
{
    (void)ix;
    if (arena == RUN_PP_ARENA) {
        *first = RUN_PP_FIRST; *count = 50u;
        /* Only once the live menu file holds Phantasma's block. */
        (void)run_menu_lba();
        return run_menu_pp && run_hdr(RUN_PP_FIRST + 203u) > run_hdr(RUN_PP_FIRST);
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
    if (got_lba[kind] == run_menu_lba() && got_bytes[kind] == bytes) return ent[kind];
    uint32_t first, count;
    if (!run_source(1u, RUN_ARENA, &first, &count)) return NULL;
    const uint32_t entry = first + 4u * count + (uint32_t)kind;
    const uint32_t a = run_hdr(entry), b = run_hdr(entry + 1u);
    if (b <= a || (b - a) > 2u || bytes > (b - a) * 2048u ||
        !psx_mod_read_disc_sectors(run_menu_lba() + a, b - a, ent[kind]))
        return NULL;
    got_lba[kind] = run_menu_lba();
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
static void run_read_builds(struct CPUState *c, uint32_t a)  { (void)a; run_queue_fix(KIND_BUILDS, run_arena_at_builds, c); run_hx_fight_setup(); }
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
    while (c < used && (key[c] != entry || lba[c] != run_menu_lba())) ++c;
    if (c == used) {
        const uint32_t a = run_hdr(entry), b = run_hdr(entry + 1u);
        if (used == CACHE) used = c = 0;              /* full: start over */
        if (b <= a || (b - a) > 2u ||
            !psx_mod_read_disc_sectors(run_menu_lba() + a, b - a, sec[c]))
            return 0;
        key[c] = entry; lba[c] = run_menu_lba();
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
    if (!ix || !run_menu_lba() || MOA_RUN_SLOTS * stride + 4u > bytes) {
        psx_mod_counter_add("moa.run.data_skipped", 1);
        run_log("data kind %d: skipped (index %08x, menu lba %u, bytes %u)", kind,
                (unsigned)ix, (unsigned)run_menu_lba(), (unsigned)bytes);
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
    if (kind == KIND_AI) run_watch(1);                   /* the fight's data: it loads next */
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
    if (run_hx_go_entry == entry) run_hx_go_entry = -1;      /* the overlay hub's pick landed */
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

/* At each fight's start: both ACs as the fight engine has them (the compiled
 * stat blocks, stat-block.md): parts, then every weapon slot's kind, ammo and
 * HUD name. A slot with 0 ammo is a weapon the AC can't use (the "0/0" seen in
 * a Retry test); the ghost crashes also came at a fight's start. */
static void run_log_fighters(void)
{
    for (uint32_t pl = 0; pl < 2u; pl++) {
        const uint32_t blk = 0x800453E8u + 928u * pl;
        const unsigned n = psx_mod_read_byte(blk + 0x87u);
        run_log("fight: %s core %u legs %u head %u arms %u hand %u back L %u back R %u blade %u, %u weapon(s)",
                pl ? "opponent" : "player",
                psx_mod_read_byte(blk + 0x2Bu), psx_mod_read_byte(blk + 0x2Cu),
                psx_mod_read_byte(blk + 0x2Du), psx_mod_read_byte(blk + 0x2Eu),
                psx_mod_read_byte(blk + 0x30u), psx_mod_read_byte(blk + 0x31u),
                psx_mod_read_byte(blk + 0x32u), psx_mod_read_byte(blk + 0x33u), n);
        for (uint32_t w = 0; w < n && w < 3u; w++) {
            const uint32_t sl = blk + 0x88u + 62u * w;
            char name[17];
            for (uint32_t i = 0; i < 16u; i++) {
                const uint8_t c = psx_mod_read_byte(sl + 42u + i);
                name[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            }
            name[16] = 0;
            run_log("fight:   slot %u kind %u ammo %u/%u '%s'", (unsigned)w,
                    psx_mod_read_byte(sl), psx_mod_read_half(sl + 16u), psx_mod_read_half(sl + 14u), name);
        }
    }
}

/* Replay watch. The arena engine replays the fight after the result (replay
 * flag 0x8003EA92 non-zero; the sampler runs then too, gen1-engine-map.md).
 * Nothing is pressed: the automatic skip (e588009 .. 08200f8) is taken out
 * (user, 2026-11-17: the after-fight freezes may come from cutting the
 * replay short while the disc is busy; a replay skip can come back later as
 * its own mod). The log still says when a replay starts and ends, and a
 * running replay counts as progress for the hang watch (a 69 s one in a
 * test). */
#define RUN_REPLAY_FLAG      0x8003EA92u
static void run_replay_skip(void)
{
    if (!psx_mod_read_byte(RUN_REPLAY_FLAG)) return;
    if (run_replay_state == 0) {
        run_replay_state = 1;
        run_replay_t = 0;
        run_log("replay: started (no skip presses)");
    }
    run_replay_t++;
    run_watch_count = 0;
}

/* Back on a menu screen: how the replay (if any) ended. */
static void run_replay_over(const char *where)
{
    if (!run_replay_state) return;
    run_log("replay: over at the %s, %u samples after it started", where, (unsigned)run_replay_t);
    run_replay_state = 0;
}

static void run_sampler_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    run_idle = 0;
    if (!run_on() || !run_at_hub || psx_mod_read_word(address) != RUN_SIG_SAMPLER) return;
    run_prize_where = 0;                          /* in a fight: no prize screen */
    run_hx_fight_runs();                          /* the stage pick has been read */
    run_prize_paid = 0;
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
            if (done && !won && !lost) run_log_fighters();
        }
        if (!won && !lost) run_result_seen = 0;   /* a new fight: its result is not booked yet */
        run_watch(won || lost ? 2 : 0);           /* running: fine; over: the arena screen is next */
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
    run_replay_skip();
    if (run_overlay_up()) run_pad_lock();         /* an overlay is up: the fight sees nothing pressed */
    const uint16_t b = psx_mod_read_half(RUN_PAD_BUFFER + 2u);
    if (!(b & RUN_PAD_SELECT)) {                  /* held (active low) */
        psx_mod_write_half(RUN_PAD_BUFFER + 2u, (uint16_t)(b | RUN_PAD_SELECT));
        psx_mod_counter_add("moa.run.select_blocked", 1);
    }
}

/* ---- defeated ghosts ----------------------------------------------------------
 *
 * A ghost the player beats is saved as an AI card (the Raven card PNG that AI
 * Battle loads: its build, names, the run's paint and the AI it flew) in
 * "Rogue Arena Ghosts/Defeated" beside the memory card, one card per ghost
 * (seed + the attempt it came from), and every defeat is one line of
 * defeated.csv there: when, by which attempt, at which rank. Kept for a
 * later look back at the ghosts beaten (planned for reaching rank 1). */
static void run_mkdir(const char *dir)
{
#if defined(_WIN32)
    (void)_mkdir(dir);
#else
    (void)mkdir(dir, 0755);
#endif
}

static void run_ghost_save(uint8_t who)
{
    const char *run = run_file_path();
    if (!run || !run[0]) return;
    const struct moa_run_pick p = run_state.lineup.slot[MOA_RUN_SLOTS - 1u - who];
    if (p.arena != MOA_RUN_GHOST || p.rank >= MOA_RUN_SLOTS) return;
    const struct moa_run_ghost *g = &run_state.lineup.ghost[p.rank];
    static struct moa_card c;
    memset(&c, 0, sizeof c);
    c.format = 4;
    memcpy(c.rec, g->rec, sizeof c.rec);
    snprintf(c.pilot, sizeof c.pilot, "%s", g->pilot);
    snprintf(c.ac, sizeof c.ac, "%s", g->ac[0] ? g->ac : "NEW AC");
    moa_run_paint(run_state.lineup.seed, g->attempt, c.paint);
    uint8_t ai[36];
    const uint32_t ix = run_index();
    if (ix && g->ai_from.arena != MOA_RUN_PLAYER && g->ai_from.arena != MOA_RUN_GHOST &&
        run_record(ix, g->ai_from, KIND_AI, sizeof ai, ai)) {
        c.has_ai = 1;
        c.ai_preset = MOA_AI_PRESET_CUSTOM;
        for (int i = 0; i < 36; i++) { c.ai_set[i] = 1; c.ai_raw[i] = ai[i]; }
    }
    /* <folder of the run file>/Rogue Arena Ghosts/Defeated/ */
    char dir[1100], card[1300], csv[1300], name[18], seed[17];
    snprintf(dir, sizeof dir, "%s", run);
    char *cut = NULL;
    for (char *q = dir; *q; q++) if (*q == '/' || *q == '\\') cut = q;
    if (cut) cut[1] = '\0'; else dir[0] = '\0';
    strncat(dir, "Rogue Arena Ghosts", sizeof dir - strlen(dir) - 1);
    run_mkdir(dir);
    strncat(dir, "/Defeated", sizeof dir - strlen(dir) - 1);
    run_mkdir(dir);
    unsigned k = 0;
    for (const char *q = c.pilot; *q && k + 1 < sizeof name; q++)
        name[k++] = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') ? *q : '_';
    name[k] = 0;
    moa_run_seed_text(run_state.lineup.seed, seed);
    snprintf(card, sizeof card, "%s/%s-A%02u-%s.png", dir, seed, (unsigned)g->attempt + 1u, name[0] ? name : "GHOST");
    /* The card: the AC as it looked (the garage renderer, when the 3D model
     * install is there; else the plain emblem), its best rank on a badge and
     * its attempt on the bottom line. */
    char badge[24], footer[64];
    const unsigned best = g->rank_held ? g->rank_held : (unsigned)who + 1u;
    snprintf(badge, sizeof badge, "RANK %u", best);
    snprintf(footer, sizeof footer, "ROGUE ARENA GHOST  /  ATTEMPT %u  /  BEATEN", (unsigned)g->attempt + 1u);
    static uint8_t art[512 * 512 * 4];
    const char *mdir = getenv("PSX_MOA_MODELS_DIR");
    int have_art = 0;
    if (mdir && mdir[0]) {
        moa_acv *v = moa_acv_load(mdir, c.rec, c.paint);
        if (v) {
            memset(art, 0, sizeof art);
            moa_acv_render_ex(v, -0.55f, 0.18f, 0.0f, 1.3f, 0u, NULL, 512, 512, art);
            moa_acv_free(v);
            have_art = 1;
        }
    }
    const int ok = moa_card_write_png_ex(&c, card, have_art ? art : NULL, badge, footer);
    /* The fight it was beaten in (the recorder's rows), beside the card. */
    char fight[1320];
    snprintf(fight, sizeof fight, "%.*s.fight.csv", (int)(strlen(card) - 4), card);
    const int rec_ok = moa_rec_copy_last(fight);
    snprintf(csv, sizeof csv, "%s/defeated.csv", dir);
    FILE *t = fopen(csv, "r");
    const int fresh = !t;
    if (t) fclose(t);
    FILE *f = fopen(csv, "a");
    if (f) {
        if (fresh) fputs("card,seed,ghost_attempt,best_rank,pilot,ac,rank,defeated_by_attempt,date\n", f);
        char when[32] = "";
        const time_t now = time(NULL);
        const struct tm *lt = localtime(&now);
        if (lt) strftime(when, sizeof when, "%Y-%m-%d %H:%M", lt);
        const char *base = card + strlen(dir) + 1;
        char qp[40], qa[40];                    /* names quoted, '"' doubled */
        const char *src[2] = { c.pilot, c.ac };
        char *dst[2] = { qp, qa };
        for (int n = 0; n < 2; n++) {
            unsigned o = 0;
            dst[n][o++] = '"';
            for (const char *q = src[n]; *q && o + 3 < sizeof qp; q++) {
                if (*q == '"') dst[n][o++] = '"';
                dst[n][o++] = *q;
            }
            dst[n][o++] = '"';
            dst[n][o] = 0;
        }
        fprintf(f, "%s,%s,%u,%u,%s,%s,%u,%u,%s\n", base, seed, (unsigned)g->attempt + 1u, best, qp, qa,
                (unsigned)who + 1u, (unsigned)run_state.run.attempt + 1u, when);
        fclose(f);
    }
    psx_mod_counter_add(ok ? "moa.run.ghost_card" : "moa.run.ghost_card_fail", 1);
    run_log("ghost beaten at rank %u: %s %s (attempt %u, best rank %u) -> %s%s%s%s", (unsigned)who + 1u,
            c.pilot, c.ac, (unsigned)g->attempt + 1u, best, card, ok ? "" : " (not written)",
            have_art ? "" : " (no 3D model install: emblem only)", rec_ok ? ", fight saved" : "");
}

/* ---- hang watch ----------------------------------------------------------------
 *
 * Two hangs in the 2026-11-17 test came after a won fight (the arena screen
 * never came back; the game idled in VSync) and one while a fight loaded.
 * The logs could not say what the game waited for, so: from the fight's
 * data being applied until the fight runs (phase 1), and from the fight's
 * result until the hub or arena screen (phase 2), a long wait logs the disc
 * queue, the fight flags, the title flags and the last menu routines called,
 * and writes RAM + VRAM beside the run file (<run>.hang-<phase>.ram/.vram). */
extern const uint16_t *gpu_get_vram(void);

/* The runtime's interrupt counters (read only; psxrecomp interrupts.c and
 * memory.c). Test 2's first hang: the game waited in the exe's VSync loop
 * with VBlank pending (I_STAT 1) for 45 s while guest time ran on, so the
 * interrupt was never taken. Two snapshots ~2 s apart show which delivery
 * check keeps saying no (the counter that grows) without changing the
 * runtime (psxrecomp/CLAUDE.md: runtime fixes need the oracle). */
extern void psx_interrupt_delivery_diag(uint64_t *need_defer, uint64_t *need_irq, uint64_t *skip_none,
                                        uint64_t *skip_sr, uint64_t *skip_cooldown, uint64_t *skip_nested);
extern void psx_interrupt_check_path_diag(uint64_t *entry, uint64_t *fast_sr, uint64_t *fast_none,
                                          uint64_t *mid, uint64_t *eval, uint64_t *irq_deliv);
extern void psx_get_freeze_diag(uint64_t *out_total_checks, uint32_t *out_dispatch_count,
                                int *out_in_exception, int *out_post_exc_cooldown,
                                uint64_t *out_exc_entries, uint64_t *out_exc_reentry_blocks);
extern uint64_t g_nestgate_depth, g_nestgate_rfepend, g_nestgate_escreason, g_nestgate_iec;
extern uint64_t g_vblank_raise_count, g_vblank_deliver_count, g_vblank_ack_count;
extern uint32_t i_stat, i_mask;

#define RUN_IRQ_N 19
static const char *const k_run_irq_name[RUN_IRQ_N] = {
    "need_defer", "need_irq", "skip_none", "skip_sr", "skip_cooldown", "skip_nested",
    "check_entry", "fast_sr", "fast_none", "checks", "eval", "irq_delivered",
    "exc_entries", "exc_reentry_blocks", "vblank_raised", "vblank_delivered", "vblank_acked",
    "nestgate_depth", "nestgate_iec" };
static uint64_t run_irq_first[RUN_IRQ_N];

static void run_irq_snap(uint64_t v[RUN_IRQ_N], int *in_exc, int *cooldown)
{
    uint32_t dispatch;
    psx_interrupt_delivery_diag(&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    psx_interrupt_check_path_diag(&v[6], &v[7], &v[8], &v[9], &v[10], &v[11]);
    psx_get_freeze_diag(NULL, &dispatch, in_exc, cooldown, &v[12], &v[13]);
    v[14] = g_vblank_raise_count; v[15] = g_vblank_deliver_count; v[16] = g_vblank_ack_count;
    v[17] = g_nestgate_depth; v[18] = g_nestgate_iec;
}

/* first = 1: the values; else what changed since the first snapshot. */
static void run_irq_log(int first)
{
    uint64_t v[RUN_IRQ_N];
    int in_exc = 0, cooldown = 0;
    run_irq_snap(v, &in_exc, &cooldown);
    char line[1200];
    int n = snprintf(line, sizeof line, "hang watch: interrupts %s: I_STAT %03x I_MASK %03x, in exception %d, "
                     "cooldown %d cycles;", first ? "now" : "2 s later (change)",
                     (unsigned)(i_stat & 0x7FFu), (unsigned)(i_mask & 0x7FFu), in_exc, cooldown);
    for (int i = 0; i < RUN_IRQ_N && n > 0 && (size_t)n < sizeof line; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s %llu", k_run_irq_name[i],
                      (unsigned long long)(first ? v[i] : v[i] - run_irq_first[i]));
    if (first) memcpy(run_irq_first, v, sizeof v);
    run_log("%s", line);
}

/* The emulated CD drive's own state (read only; psxrecomp cdrom.c debug
 * rings, always recorded). Tests 2 and 4 froze on a disc read that never
 * finished with no CD interrupt raised at all; these lines say whether the
 * drive got the command, is still reading, or holds an answer the game's
 * interrupt-enable mask hides. The log gets a summary; the full rings go to
 * <run>.hang-<tag>.cd.txt. */

static void run_cd_state_line(const char *when)
{
    CDROMDebugState s;
    memset(&s, 0, sizeof s);
    cdrom_debug_snapshot(&s);
    run_log("hang watch: CD drive %s: stat %02x irq_enable %02x irq_flag %02x mode %02x, pending cmd %02x (%d, "
            "%d cycles, phase %d), queued cmd %02x (%u), reading %d cmd %02x at %02d:%02d:%02d, seek %02d:%02d:%02d, "
            "read_delay %d, irq_present_delay %d, speed %d, last sector %d (frame %u), int1 pended %llu lost %llu "
            "now %u, accel waits %llu, ring starved %llu dropped %llu, hold %llu, response %d/%d",
            when, s.stat_reg, s.irq_enable, s.irq_flag, s.mode_reg, s.pending_cmd, s.pending_pending, s.pending_delay,
            s.pending_phase, s.queued_cmd, (unsigned)s.queued_pending, s.reading, s.read_cmd, s.read_min, s.read_sec,
            s.read_sect, s.seek_min, s.seek_sec, s.seek_sect, s.read_delay, s.irq_present_delay, s.speed_divisor,
            s.last_sector_lba, (unsigned)s.last_sector_frame, (unsigned long long)s.int1_pended,
            (unsigned long long)s.int1_lost, (unsigned)s.int1_pending_now, (unsigned long long)s.accel_consumer_waits,
            (unsigned long long)s.ring_starved, (unsigned long long)s.ring_dropped,
            (unsigned long long)s.read_hold_events, s.response_read, s.response_count);
}

static void run_cd_cmd_text(char *out, size_t cap, const CDROMCommandHistoryEntry *e)
{
    int n = snprintf(out, cap, "f%u %c%02x(", (unsigned)e->frame, e->kind ? e->kind : '?', e->cmd);
    for (int i = 0; i < e->param_count && i < 16 && n > 0 && (size_t)n < cap; i++)
        n += snprintf(out + n, cap - (size_t)n, "%s%02x", i ? " " : "", e->params[i]);
    if (n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, ") st %02x en %02x fl %02x md %02x rd %u", e->stat, e->irq_enable,
                 e->irq_flag, e->mode, (unsigned)e->reading);
}

static void run_cd_dump(const char *tag)
{
    const CDROMCommandHistoryEntry *ch = NULL;
    const CDROMSectorHistoryEntry *sh = NULL;
    const CDROMTraceEntry *tr = NULL;
    const uint64_t nc = cdrom_debug_get_command_history(&ch);
    const uint64_t ns = cdrom_debug_get_sector_history(&sh);
    const uint64_t nt = cdrom_debug_get_trace(&tr);
    char line[1500], one[160];
    int n = 0;
    /* Summary: the last 8 commands. */
    line[0] = 0;
    for (uint64_t k = nc > 8u ? nc - 8u : 0u; ch && k < nc && (size_t)n < sizeof line - 2u; k++) {
        run_cd_cmd_text(one, sizeof one, &ch[k % CDROM_COMMAND_HISTORY_CAP]);
        n += snprintf(line + n, sizeof line - (size_t)n, "%s%s", n ? " | " : "", one);
    }
    run_log("hang watch: CD last commands: %s", line[0] ? line : "none");
    /* Summary: the last 4 sectors. */
    n = 0; line[0] = 0;
    for (uint64_t k = ns > 4u ? ns - 4u : 0u; sh && k < ns && (size_t)n < sizeof line - 2u; k++) {
        const CDROMSectorHistoryEntry *e = &sh[k % CDROM_SECTOR_HISTORY_CAP];
        n += snprintf(line + n, sizeof line - (size_t)n, "%sf%u lba %d md %02x xa %u/%u sub %02x data %u audio %u skip %u",
                      n ? " | " : "", (unsigned)e->frame, e->lba, e->mode, e->xa_file, e->xa_channel, e->xa_submode,
                      e->data_delivered, e->xa_audio_delivered, e->skip_reason);
    }
    run_log("hang watch: CD last sectors (of %llu): %s", (unsigned long long)ns, line[0] ? line : "none");
    if (!tag || !run_file_path()) return;
    /* The full rings, for the next chat. */
    char p[1100];
    snprintf(p, sizeof p, "%s.hang-%s.cd.txt", run_file_path(), tag);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "commands (%llu total, last 300)\n", (unsigned long long)nc);
    for (uint64_t k = nc > 300u ? nc - 300u : 0u; ch && k < nc; k++) {
        const CDROMCommandHistoryEntry *e = &ch[k % CDROM_COMMAND_HISTORY_CAP];
        run_cd_cmd_text(one, sizeof one, e);
        fprintf(f, "%llu cyc %llu pc %08x fn %08x %s at %02u:%02u:%02u pend %02x/%u q %02x/%u resp",
                (unsigned long long)e->seq, (unsigned long long)e->cycle, (unsigned)e->pc, (unsigned)e->func, one,
                e->read_min, e->read_sec, e->read_sect, e->pending_cmd, e->pending_pending, e->queued_cmd,
                e->queued_pending);
        for (int i = 0; i < e->response_count && i < 16; i++) fprintf(f, " %02x", e->response[i]);
        fputc('\n', f);
    }
    fprintf(f, "\nsectors (%llu total, last 200)\n", (unsigned long long)ns);
    for (uint64_t k = ns > 200u ? ns - 200u : 0u; sh && k < ns; k++) {
        const CDROMSectorHistoryEntry *e = &sh[k % CDROM_SECTOR_HISTORY_CAP];
        fprintf(f, "%llu f%u lba %d size %d md %02x raw %u/%02x xa %u/%u sub %02x coding %02x data %u audio %u skip %u\n",
                (unsigned long long)e->seq, (unsigned)e->frame, e->lba, e->size, e->mode, e->have_raw, e->raw_mode,
                e->xa_file, e->xa_channel, e->xa_submode, e->xa_coding, e->data_delivered, e->xa_audio_delivered,
                e->skip_reason);
    }
    fprintf(f, "\nregister trace (%llu total, last 600)\n", (unsigned long long)nt);
    for (uint64_t k = nt > 600u ? nt - 600u : 0u; tr && k < nt; k++) {
        const CDROMTraceEntry *e = &tr[k % CDROM_TRACE_CAP];
        fprintf(f, "%llu f%u %c addr %08x val %08x w%u pc %08x fn %08x istat %03x idx %u st %02x en %02x fl %02x md %02x "
                "pend %02x/%u rd %u/%02x delay %d/%d sec %u pos %d/%d\n",
                (unsigned long long)e->seq, (unsigned)e->frame, e->kind ? e->kind : '?', (unsigned)e->addr,
                (unsigned)e->val, e->width, (unsigned)e->pc, (unsigned)e->func, (unsigned)(e->i_stat & 0x7FFu),
                e->index_reg, e->stat_reg, e->irq_enable, e->irq_flag, e->mode_reg, e->pending_cmd,
                e->pending_pending, e->reading, e->read_cmd, e->pending_delay, e->read_delay, e->sector_available,
                e->sector_read_pos, e->sector_size);
    }
    fclose(f);
    run_log("hang watch: CD rings written beside the run file (.hang-%s.cd.txt)", tag);
}

static uint32_t run_watch_limit(void)
{
    return run_watch_phase == 1 ? RUN_WATCH_LOAD : run_watch_phase == 3 ? RUN_WATCH_IDLE : RUN_WATCH_FRAMES;
}

static void run_watch_dump(void)
{
    static const char *const k_phase[4] = { "", "loading a fight", "after a fight",
                                            "no controller sampling at the hub or its screens" };
    char calls[1024];
    moa_btrace_recent(calls, sizeof calls);
    run_log("hang watch: %s for %u s: disc queue write %u read %u, fight done %u won %u lost %u, "
            "title flags %u/%u, prize where %d, data fixes queued %d",
            k_phase[run_watch_phase], (unsigned)(run_watch_limit() / 60u),
            (unsigned)psx_mod_read_word(RUN_D_CDQ_WRITE), (unsigned)psx_mod_read_word(RUN_D_CDQ_READ),
            (unsigned)psx_mod_read_byte(RUN_FIGHT_DONE), (unsigned)psx_mod_read_half(RUN_FIGHT_WON),
            (unsigned)psx_mod_read_half(RUN_FIGHT_WON + 2u),
            (unsigned)psx_mod_read_byte(RUN_D_TITLE_FLAG), (unsigned)psx_mod_read_byte(RUN_D_TITLE_FLAG + 5u),
            run_prize_where, run_fixes);
    run_log("hang watch: last menu calls (frame:routine): %s", calls[0] ? calls : "none");
    run_irq_log(1);
    run_cd_state_line("now");
    run_irq_again = 120u;
    psx_mod_counter_add("moa.run.hang_watch", 1);
    if (run_watch_dumps >= 3u || !run_file_path()) { run_cd_dump(NULL); return; }   /* 2 MB + 1 MB each: a few at most */
    run_watch_dumps++;
    /* One name per hang: the session's start time and the frame, so a second
     * hang (or the next session's) never writes over the first. */
    char tag[64];
    {
        if (!run_watch_stamp[0]) {               /* cleared per session (run_check_session) */
            const time_t now = time(NULL);
            const struct tm *lt = localtime(&now);
            if (!lt || !strftime(run_watch_stamp, sizeof run_watch_stamp, "%Y%m%d-%H%M%S", lt))
                snprintf(run_watch_stamp, sizeof run_watch_stamp, "s%llu", (unsigned long long)now);
        }
        extern uint64_t s_frame_count;
        snprintf(tag, sizeof tag, "%s-f%llu-%d", run_watch_stamp, (unsigned long long)s_frame_count,
                 run_watch_phase);
    }
    char p[1100];
    static uint8_t ram[2u * 1024u * 1024u];
    for (uint32_t i = 0; i < sizeof ram; i += 4u) {
        const uint32_t w = psx_mod_read_word(0x80000000u + i);
        ram[i] = (uint8_t)w; ram[i + 1] = (uint8_t)(w >> 8);
        ram[i + 2] = (uint8_t)(w >> 16); ram[i + 3] = (uint8_t)(w >> 24);
    }
    snprintf(p, sizeof p, "%s.hang-%s.ram", run_file_path(), tag);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(ram, 1, sizeof ram, f); fclose(f); }
    const uint16_t *v = gpu_get_vram();
    if (v) {
        snprintf(p, sizeof p, "%s.hang-%s.vram", run_file_path(), tag);
        f = fopen(p, "wb");
        if (f) { fwrite(v, 2, 1024u * 512u, f); fclose(f); }
    }
    run_log("hang watch: RAM%s written beside the run file (.hang-%s)", v ? " and VRAM" : "", tag);
    run_cd_dump(tag);
}

/* Any Rogue Arena screen over the game (restock, prize, loss): the game
 * sees no buttons. Called from the frame hook and from the game's own
 * controller sampling (menus and fights), so no press slips through between
 * the two. */
static int run_overlay_up(void)
{
    return run_show_on || run_prize_stage || run_end_stage || run_hx;
}

static void run_pad_lock(void)
{
    psx_mod_write_half(RUN_PAD_BUFFER + 2u, 0xFFFFu);            /* raw pad: nothing held */
    if (!run_autopilot) psx_mod_write_word(RUN_D_PAD + 0x18u, 0u); /* menu: no new presses */
}

static const char *const k_run_end_label[3] = { "RETRY ARENA", "NEW ARENA", "QUIT TO LAUNCHER" };

static int run_end_draw(void)
{
    char title[40], line[64];
    const unsigned wins = run_state.run.wins;
    snprintf(title, sizeof title, "RUN OVER  RANK %u", (unsigned)(MOA_RUN_SLOTS - (wins < MOA_RUN_SLOTS ? wins : 0u)));
    snprintf(line, sizeof line, run_end_cur == 0 ? "SAME 49 OPPONENTS, A NEW AC, 0 P" :
             run_end_cur == 1 ? "A NEW SEED: NEW OPPONENTS AND A NEW AC" : "BACK TO THE ROGUE ARENA PAGE");
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    for (int pass = 0; pass < 2; pass++) {
        const int see = pass == 0;
        const uint32_t *px = moa_screen_render_choice(ww, wh, title, line, k_run_end_label, 3, run_end_cur,
                                                      "UP / DOWN  CHOOSE     X  OK", see, &w, &h);
        if (!px) return 0;
        if (see && psx_mod_set_overlay_image(px, w, h)) { run_end_overlay = 1; return 1; }
        if (!see) {
            run_end_overlay = 0;
            psx_mod_set_loading_screen("ROGUE ARENA  Run over", -1);
            psx_mod_set_loading_image(px, w, h);
            return 1;
        }
    }
    return 0;
}

static void run_end_pick(int pick)
{
    run_end_stage = 2;
    char p[1100];
    snprintf(p, sizeof p, "%s.next", run_file_path() ? run_file_path() : "");
    if (pick == 2) remove(p);
    else {
        FILE *f = fopen(p, "wb");
        if (f) { fputs(pick == 0 ? "retry\n" : "new\n", f); fclose(f); }
    }
    (void)psx_mod_set_overlay_image(NULL, 0, 0);
    psx_mod_set_loading_screen("ROGUE ARENA  Back to the launcher", -1);
    psx_mod_counter_add(pick == 0 ? "moa.run.end_retry" : pick == 1 ? "moa.run.end_new" : "moa.run.end_quit", 1);
    run_log("loss screen: %s", k_run_end_label[pick]);
}

/* Per frame once the run is lost. */
static void run_end_frame(void)
{
    if (!run_state_ok || !run_state.run.over || run_loss_wait || run_sent) return;
    if (run_end_stage == 0) {
        if (!run_show_font() || !run_end_draw()) {    /* no screen: straight back, as before */
            run_log("loss screen: could not be drawn, back to the launcher");
            run_end_stage = 2;
            return;
        }
        run_end_stage = 1;
        run_end_cur = 0;
        run_end_armed = 0;
        run_end_frames = 0;
        run_end_held = 0xFFFFu;
        run_log("loss screen: up%s", run_end_overlay ? "" : " (loading picture)");
        return;
    }
    if (run_end_stage != 1) return;
    run_pad_lock();
    if (!run_end_overlay) psx_mod_set_loading_screen("ROGUE ARENA  Run over", -1);
    const uint16_t b = psx_mod_host_pad_buttons(0);          /* active low */
    const uint16_t down = (uint16_t)(run_end_held & ~b);
    run_end_held = b;
    run_end_frames++;
    if (b == 0xFFFFu) run_end_armed = 1;
    if (!run_end_armed || run_end_frames < 20u || !down) return;
    if (down & (RUN_PAD_UP | RUN_PAD_DOWN)) {
        run_end_cur = (run_end_cur + ((down & RUN_PAD_UP) ? 2 : 1)) % 3;
        (void)run_end_draw();
    } else if (down & (RUN_PAD_X | RUN_PAD_START)) {
        run_end_pick(run_end_cur);
    }
}

/* ---- overlay hub (step 1, claude/overlay-hub-plan.md) ---------------------------
 *
 * Whenever the hub waits for the player (its input state 0x80055848 runs),
 * a list opens over it: the balance, the next opponent, then NEXT BATTLE,
 * STAGE (a scrollable list of stages, remembered in <run>.stage), SHOP and
 * GARAGE (the game's own screens for now). O leaves the game's own hub
 * until the player goes into one of its screens. The game sees no buttons
 * while the list is up.
 *
 * It also opens over the run's arena list (where the game lands after each
 * fight) once the result, payout, prize and restock are done (test 6,
 * user: "as much as possible, overlay should be active").
 *
 * NEXT BATTLE drives the game's own menus behind a "starting" picture: the
 * hub hook presses X on Arena (skipped when already on the list); the arena
 * list state (0x800BD044, a0 = screen state, block at +0x80: +0x60 rows,
 * +0x68 cursor in the 6-row window, +0x69 first row shown; picked row =
 * +0x68 + +0x69 = rank - 1) gets the cursor on the next opponent and an X,
 * which opens the opponent's panel; the panel's state (0x800BED64, same
 * block) gets an X, which opens the "fight?" box (0x80065858, answer at
 * +0x67: 2 open, 0 the first choice = fight); that box gets X from the
 * controller until the fight's data loads (0x800C0250). Test 6: hooking only
 * the panel state never fired (the list's X was missing).
 *
 * SHOP / GARAGE from the list: O there (back to the hub), then the hub hook
 * presses X on the entry; a cover picture stays up until the hub opens it.
 *
 * Stage: the arena fight setup writes the opponent's stage into both player
 * blocks +597 (0x800C097C); the pick is written over it every frame from the
 * fight's data load until the fight runs, as AI Battle does. */
#define RUN_ARENA_INPUT      0x800BED64u  /* the opponent's panel (O back, X fight?) */
#define RUN_SIG_ARENA_INPUT  0x27BDFFB0u  /* addiu sp,sp,-80 */
#define RUN_ARENA_LIST       0x800BD044u  /* the opponent list (up/down, X panel, O hub) */
#define RUN_SIG_ARENA_LIST   0x27BDFFC0u  /* addiu sp,sp,-64 */
#define RUN_STAGE_P1         0x800453EDu
#define RUN_STAGE_P2         0x8004578Du
#define RUN_HX_GO_GIVE_UP    900u         /* frames for the hub to open a pick (from the list: O first) */
#define RUN_HX_NEXT_GIVE_UP  900u         /* frames for the fight to start */
enum { HX_OFF = 0, HX_MENU, HX_STAGES, HX_NEXT, HX_BUSY };
enum { HXR_NEXT = 0, HXR_STAGE, HXR_SHOP, HXR_GARAGE, HXR_COUNT };
#define RUN_HX_STAGE_OWN     0u
#define RUN_HX_STAGE_RANDOM  255u
static const struct { uint8_t id; const char *name; } k_hx_stage[] = {
    { RUN_HX_STAGE_OWN, "ARENA'S OWN" }, { RUN_HX_STAGE_RANDOM, "RANDOM" },
    { 29, "SEABED" }, { 27, "GREEN SILO" }, { 39, "OCEAN PLATFORM" }, { 24, "SCAFFOLD YARD" },
    { 25, "GARDEN PLAZA" }, { 20, "LUNAR BASE" }, { 22, "PILLAR HALL" }, { 33, "FOREST" },
    { 36, "NIGHT CITY" },
};
#define RUN_HX_STAGES ((int)(sizeof k_hx_stage / sizeof k_hx_stage[0]))
static int run_hx_cur, run_hx_scur, run_hx_stop, run_hx_armed, run_hx_overlay;
static int run_hx_stage_i = -1;            /* index into k_hx_stage; -1 = not read yet */
static uint32_t run_hx_frames, run_hx_rep;
static uint16_t run_hx_held = 0xFFFFu;
static char run_hx_foe[64];
static const char *run_hx_busy_title = "";
static int run_hx_failed_told;

static void run_hx_reset(void)
{
    if (run_hx) (void)psx_mod_set_overlay_image(NULL, 0, 0);
    run_hx = HX_OFF;
    run_hx_off = 0;
    run_hx_hub_seen = 0xFFFFu;
    run_hx_list_seen = 0xFFFFu;
    run_hx_go_entry = -1;
    run_hx_go_frames = 0;
    run_hx_next = 0;
    run_hx_next_frames = run_hx_next_tries = run_hx_next_t = 0;
    run_hx_hold = 0;
    run_hx_stage_i = -1;
    run_hx_overlay = 0;
    run_hx_failed_told = 0;
}

static void run_hx_stage_load(void)
{
    if (run_hx_stage_i >= 0) return;
    run_hx_stage_i = 0;
    if (!run_file_path()) return;
    char p[1100];
    snprintf(p, sizeof p, "%s.stage", run_file_path());
    FILE *f = fopen(p, "r");
    if (!f) return;
    int id = -1;
    if (fscanf(f, "%d", &id) == 1)
        for (int i = 0; i < RUN_HX_STAGES; i++)
            if (k_hx_stage[i].id == id) run_hx_stage_i = i;
    fclose(f);
}

static void run_hx_stage_save(void)
{
    if (!run_file_path()) return;
    char p[1100];
    snprintf(p, sizeof p, "%s.stage", run_file_path());
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)k_hx_stage[run_hx_stage_i].id);
    fclose(f);
}

/* Printable upper-case text from a name field. */
static void run_hx_name(const uint8_t *src, unsigned n, char *out, size_t cap)
{
    size_t k = 0;
    for (unsigned i = 0; i < n && src[i] && k + 1 < cap; i++) {
        char c = (char)src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c < 32 || c > 126) c = ' ';
        out[k++] = c;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* The next opponent's line: rank, AC and pilot (the lineup slot one rung up). */
static void run_hx_foe_text(void)
{
    const unsigned wins = run_state.run.wins;
    run_hx_foe[0] = 0;
    if (wins + 1u >= MOA_RUN_SLOTS) { snprintf(run_hx_foe, sizeof run_hx_foe, "RANK 1: NO ONE ABOVE YOU"); return; }
    const unsigned rank = MOA_RUN_SLOTS - wins - 1u;
    const struct moa_run_pick p = run_state.lineup.slot[wins + 1u];
    char ac[25] = "", pilot[25] = "";
    if (p.arena == MOA_RUN_GHOST && p.rank < MOA_RUN_SLOTS) {
        const struct moa_run_ghost *g = &run_state.lineup.ghost[p.rank];
        run_hx_name((const uint8_t *)g->ac, 16, ac, sizeof ac);
        run_hx_name((const uint8_t *)g->pilot, 16, pilot, sizeof pilot);
    } else if (p.arena != MOA_RUN_PLAYER) {
        uint8_t rec[48];
        const uint32_t ix = run_index();
        if (ix && run_menu_lba() && run_record(ix, p, KIND_NAMES, 48u, rec)) {
            run_hx_name(rec, 24, pilot, sizeof pilot);
            run_hx_name(rec + 24, 24, ac, sizeof ac);
        }
    }
    if (ac[0] && pilot[0]) snprintf(run_hx_foe, sizeof run_hx_foe, "NEXT  RANK %u  %s / %s", rank, ac, pilot);
    else if (ac[0] || pilot[0]) snprintf(run_hx_foe, sizeof run_hx_foe, "NEXT  RANK %u  %s", rank, ac[0] ? ac : pilot);
    else snprintf(run_hx_foe, sizeof run_hx_foe, "NEXT  RANK %u", rank);
}

static int run_hx_draw(void)
{
    char title[40], bal[40], stage_v[27];
    const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
    struct moa_screen_list l;
    memset(&l, 0, sizeof l);
    l.title = title;
    l.marked = -1;
    static const char *const k_rows[HXR_COUNT] = { "NEXT BATTLE", "STAGE", "SHOP", "GARAGE" };
    const char *values[HXR_COUNT] = { NULL, stage_v, NULL, NULL };
    const char *srows[RUN_HX_STAGES];
    snprintf(stage_v, sizeof stage_v, "%s", k_hx_stage[run_hx_stage_i < 0 ? 0 : run_hx_stage_i].name);
    snprintf(bal, sizeof bal, "BALANCE  %u P", (unsigned)psx_mod_read_word(RUN_CREDITS));
    if (run_hx == HX_STAGES) {
        snprintf(title, sizeof title, "STAGE");
        l.info[0] = "WHERE YOUR NEXT FIGHTS TAKE PLACE";
        l.info[1] = "ARENA'S OWN: THE STAGE EACH OPPONENT USES";
        for (int i = 0; i < RUN_HX_STAGES; i++) srows[i] = k_hx_stage[i].name;
        l.row = srows;
        l.count = RUN_HX_STAGES;
        l.cursor = run_hx_scur;
        l.top = run_hx_stop;
        l.visible = 7;
        l.marked = run_hx_stage_i;
        l.footer = "UP / DOWN  CHOOSE     X  OK     O  BACK";
    } else if (run_hx == HX_BUSY) {
        snprintf(title, sizeof title, "%s", run_hx_busy_title);
        l.info[0] = bal;
        l.info[2] = "OPENING...";
        l.count = 0;
    } else if (run_hx == HX_NEXT) {
        snprintf(title, sizeof title, "NEXT BATTLE");
        l.info[0] = run_hx_foe;
        l.info[1] = values[HXR_STAGE] && run_hx_stage_i > 0 ? stage_v : NULL;
        l.info[2] = "STARTING THE FIGHT...";
        l.count = 0;
    } else {
        snprintf(title, sizeof title, "ROGUE ARENA  RANK %u", (unsigned)(MOA_RUN_SLOTS - wins));
        l.info[0] = bal;
        l.info[1] = run_hx_foe;
        l.row = k_rows;
        l.value = values;
        l.count = HXR_COUNT;
        l.cursor = run_hx_cur;
        l.visible = HXR_COUNT;
        l.footer = "UP / DOWN  CHOOSE     X  OK     O  GAME MENU";
    }
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render_list(ww, wh, &l, 1, &w, &h);
    if (!px || !psx_mod_set_overlay_image(px, w, h)) return 0;
    run_hx_overlay = 1;
    return 1;
}

static void run_hx_close(void)
{
    if (run_hx_overlay) (void)psx_mod_set_overlay_image(NULL, 0, 0);
    run_hx_overlay = 0;
    run_hx = HX_OFF;
}

static void run_hx_fail(const char *why)
{
    run_log("hub overlay: %s", why);
    psx_mod_counter_add("moa.run.hub_overlay_failed", 1);
    run_hx_go_entry = -1;
    run_hx_next = 0;
    run_hx_close();
}

static void run_hx_open_entry(int entry, const char *name, const char *title)
{
    run_hx_go_entry = entry;
    run_hx_go_frames = 0;
    run_hx_busy_title = title;
    run_hx = HX_BUSY;                                  /* a cover until the hub opens it */
    if (!run_hx_draw()) run_hx_close();
    run_log("hub overlay: %s (from the %s)", name, run_hx_hub_seen <= 2u ? "hub" : "arena list");
}

static void run_hx_start_next(void)
{
    run_hx = HX_NEXT;
    run_hx_next = 1;
    run_hx_next_frames = run_hx_next_tries = run_hx_next_t = 0;
    const int at_hub = run_hx_hub_seen <= 2u;
    run_hx_go_entry = at_hub ? RUN_HUB_ARENA : -1;     /* on the list already: no hub step */
    run_hx_go_frames = 0;
    if (!run_hx_draw()) run_hx_overlay = 0;          /* no picture: the menus still run */
    run_log("hub overlay: next battle from the %s (%s, stage %s)", at_hub ? "hub" : "arena list",
            run_hx_foe, k_hx_stage[run_hx_stage_i].name);
}

/* The run's arena block (a0 + 0x80) when a0 is a screen state of arena 0, else 0. */
static uint32_t run_hx_a0_block(struct CPUState *cpu)
{
    const uint32_t st = cpu->gpr[4];
    if (st < 0x80000000u || st >= 0x801FFF00u) return 0;
    const uint32_t b = st + 0x80u;
    return psx_mod_read_byte(b + 0x5Du) == RUN_ARENA ? b : 0u;
}

/* Cursor on the next opponent's row and X; 0 = given up. */
static int run_hx_pick_row(uint32_t b, int step, const char *where)
{
    const int count = psx_mod_read_byte(b + 0x60u);
    const int row = (int)MOA_RUN_SLOTS - (int)run_state.run.wins - 2;   /* the next opponent's rank - 1 */
    if (count < 1 || row < 0 || row >= count) { run_hx_fail("next battle: no opponent row on the arena list"); return 0; }
    if (run_hx_next == step + 1 && ++run_hx_next_t < 30u) return 1;    /* pressed already: give it a moment */
    if (++run_hx_next_tries > 6u) { run_hx_fail("next battle: 6 X presses on the arena screens and no fight"); return 0; }
    const int vis = count < 6 ? count : 6;
    const int top = row > count - vis ? count - vis : row;
    psx_mod_write_byte(b + 0x69u, (uint8_t)top);
    psx_mod_write_byte(b + 0x68u, (uint8_t)(row - top));
    psx_mod_write_word(RUN_D_PAD + 0x18u, psx_mod_read_word(RUN_D_PAD + 0x18u) | RUN_PAD_X);
    psx_mod_write_word(RUN_D_PAD + 0x1Cu, psx_mod_read_word(RUN_D_PAD + 0x1Cu) & ~(uint32_t)RUN_PAD_X);
    run_hx_next = step + 1;
    run_hx_next_t = 0;
    run_log("hub overlay: %s on row %d (rank %d), X (press %u)", where, row, row + 1, (unsigned)run_hx_next_tries);
    return 1;
}

/* The opponent list (every frame it is up). */
static void run_hx_arena_list(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_ARENA_LIST) return;
    const uint32_t b = run_hx_a0_block(cpu);
    if (!b) return;
    run_hx_list_seen = 0;
    if (run_hx_go_entry >= 0 && !run_hx_next) {           /* Shop / Garage: back to the hub first */
        psx_mod_write_word(RUN_D_PAD + 0x18u, psx_mod_read_word(RUN_D_PAD + 0x18u) | RUN_PAD_CIRCLE);
        psx_mod_write_word(RUN_D_PAD + 0x1Cu, psx_mod_read_word(RUN_D_PAD + 0x1Cu) & ~(uint32_t)RUN_PAD_CIRCLE);
        return;
    }
    if (run_hx_next == 1 || run_hx_next == 2) (void)run_hx_pick_row(b, 1, "arena list");
}

/* The opponent's panel: X asks "fight?". */
static void run_hx_arena_input(struct CPUState *cpu, uint32_t address)
{
    if (!run_hx_next || !run_on() || psx_mod_read_word(address) != RUN_SIG_ARENA_INPUT) return;
    const uint32_t b = run_hx_a0_block(cpu);
    if (!b) return;
    if (run_hx_next == 2 || run_hx_next == 3) (void)run_hx_pick_row(b, 2, "opponent panel");
}

/* From the menu's controller sampling while NEXT BATTLE runs: X for the
 * "fight?" box, 3 samples held out of every 24, once the list's X is in. */
static int run_hx_menu_pad(void)
{
    if (run_hx != HX_NEXT) return 0;
    const uint32_t t = run_hx_next_t++;
    psx_mod_write_half(RUN_PAD_BUFFER + 2u,
                       run_hx_next == 3 && (t % 24u) >= 12u && (t % 24u) < 15u ? (uint16_t)~RUN_PAD_X : (uint16_t)0xFFFFu);
    return 1;
}

/* The fight's data loads (0x800C0250): NEXT BATTLE has done its part; the
 * stage pick is held from here until the fight runs. */
static void run_hx_fight_setup(void)
{
    if (run_hx_next) {
        run_log("hub overlay: the fight is loading (%u frames after NEXT BATTLE)", (unsigned)run_hx_next_frames);
        psx_mod_counter_add("moa.run.hub_next_battle", 1);
        run_hx_next = 0;
        run_hx_go_entry = -1;
        run_hx_close();
    }
    run_hx_stage_load();
    const uint8_t id = k_hx_stage[run_hx_stage_i].id;
    if (id == RUN_HX_STAGE_OWN) { run_hx_hold = 0; return; }
    if (id == RUN_HX_STAGE_RANDOM) {
        uint64_t r = run_state.lineup.seed ^ ((uint64_t)run_state.run.wins << 32) ^ 0x5354414745ull;
        run_hx_hold_id = k_hx_stage[2 + (int)moa_run_below(&r, (uint32_t)(RUN_HX_STAGES - 2))].id;
    } else run_hx_hold_id = id;
    if (!run_hx_hold) run_log("stage: %u for this fight (%s)", (unsigned)run_hx_hold_id, k_hx_stage[run_hx_stage_i].name);
    run_hx_hold = 1;
}

static void run_hx_fight_runs(void)
{
    run_hx_hold = 0;
    if (run_hx) run_hx_close();
}

/* Per frame. */
static void run_hx_frame(void)
{
    if (run_hx_hold) {
        psx_mod_write_byte(RUN_STAGE_P1, run_hx_hold_id);
        psx_mod_write_byte(RUN_STAGE_P2, run_hx_hold_id);
    }
    if (run_hx_hub_seen < 0xFFFFu) run_hx_hub_seen++;
    if (run_hx_list_seen < 0xFFFFu) run_hx_list_seen++;
    if (run_hx_hub_seen > 30u && run_hx_list_seen > 30u) run_hx_off = 0;   /* left both: back next time */
    if (run_hx == HX_BUSY && run_hx_go_entry < 0) run_hx_close();          /* the hub opened the pick */
    if (run_hx_go_entry >= 0 && !run_hx_next && ++run_hx_go_frames > RUN_HX_GO_GIVE_UP)
        run_hx_fail("the hub did not open the pick");
    if (run_hx_next && ++run_hx_next_frames > RUN_HX_NEXT_GIVE_UP)
        run_hx_fail("next battle: the fight did not start in 15 s");
    if (run_hx == HX_OFF) {
        if (run_hx_go_entry >= 0 || run_hx_next || run_hx_off) return;
        if (run_hx_hub_seen > 2u && run_hx_list_seen > 2u) return;          /* neither screen is waiting */
        if (run_win_screen || run_pay_due) return;                          /* the result still to pay */
        if (!run_state_ok || run_state.run.over || !run_at_hub || run_autopilot || run_fade) return;
        if (run_show_on || run_show_rank || run_prize_stage || run_prize_due() >= 0 || run_end_stage) return;
        if (run_to_hub || psx_mod_read_byte(RUN_FIGHT_DONE)) return;
        if (!run_show_font()) return;
        run_hx_stage_load();
        run_hx_foe_text();
        run_hx = HX_MENU;
        run_hx_cur = 0;
        run_hx_armed = 0;
        run_hx_frames = 0;
        run_hx_held = 0xFFFFu;
        if (!run_hx_draw()) {
            run_hx = HX_OFF;
            run_hx_off = 1;
            if (!run_hx_failed_told) { run_hx_failed_told = 1; run_log("hub overlay: could not be drawn; the game's hub stays"); }
            return;
        }
        run_log("hub overlay: up over the %s (P %u, %s)", run_hx_hub_seen <= 2u ? "hub" : "arena list",
                (unsigned)psx_mod_read_word(RUN_CREDITS), run_hx_foe);
        return;
    }
    if (run_hx == HX_NEXT || run_hx == HX_BUSY) return;
    const uint16_t b = psx_mod_host_pad_buttons(0);      /* active low */
    const uint16_t held = (uint16_t)~b;
    uint16_t down = (uint16_t)(run_hx_held & ~b);
    run_hx_held = b;
    run_hx_frames++;
    if (b == 0xFFFFu) run_hx_armed = 1;
    if (!run_hx_armed || run_hx_frames < 20u) return;
    /* Up / Down held: repeat after 18 frames, then every 5. */
    if (held & (RUN_PAD_UP | RUN_PAD_DOWN)) {
        if (down) run_hx_rep = 0;
        else if (++run_hx_rep >= 18u && (run_hx_rep - 18u) % 5u == 0u) down |= (uint16_t)(held & (RUN_PAD_UP | RUN_PAD_DOWN));
    }
    if (!down) return;
    const int up = (down & RUN_PAD_UP) != 0, dn = (down & RUN_PAD_DOWN) != 0;
    if (run_hx == HX_STAGES) {
        if (up || dn) {
            run_hx_scur = (run_hx_scur + (up ? RUN_HX_STAGES - 1 : 1)) % RUN_HX_STAGES;
            if (run_hx_scur < run_hx_stop) run_hx_stop = run_hx_scur;
            if (run_hx_scur >= run_hx_stop + 7) run_hx_stop = run_hx_scur - 6;
            (void)run_hx_draw();
        } else if (down & RUN_PAD_X) {
            run_hx_stage_i = run_hx_scur;
            run_hx_stage_save();
            run_log("hub overlay: stage %s (kept for the next fights)", k_hx_stage[run_hx_stage_i].name);
            run_hx = HX_MENU;
            (void)run_hx_draw();
        } else if (down & RUN_PAD_CIRCLE) {
            run_hx = HX_MENU;
            (void)run_hx_draw();
        }
        return;
    }
    if (up || dn) {
        run_hx_cur = (run_hx_cur + (up ? HXR_COUNT - 1 : 1)) % HXR_COUNT;
        (void)run_hx_draw();
    } else if (down & (RUN_PAD_X | RUN_PAD_START)) {
        switch (run_hx_cur) {
        case HXR_NEXT:
            if (run_state.run.wins + 1u >= MOA_RUN_SLOTS) {      /* rank 1: pick a refight in the game's list */
                run_hx_open_entry(RUN_HUB_ARENA, "arena (rank 1: the game's list)", "ARENA");
                break;
            }
            run_hx_start_next();
            break;
        case HXR_STAGE:
            run_hx = HX_STAGES;
            run_hx_scur = run_hx_stage_i < 0 ? 0 : run_hx_stage_i;
            run_hx_stop = run_hx_scur > RUN_HX_STAGES - 7 ? RUN_HX_STAGES - 7 : run_hx_scur;
            (void)run_hx_draw();
            break;
        case HXR_SHOP:   run_hx_open_entry(1, "shop (the game's)", "SHOP"); break;
        case HXR_GARAGE: run_hx_open_entry(0, "garage (the game's)", "GARAGE"); break;
        }
    } else if (down & RUN_PAD_CIRCLE) {
        run_hx_close();
        run_hx_off = 1;
        run_log("hub overlay: closed, the game's hub");
    }
}

static void run_frame_hook(void)
{
    run_check_session();
    if (!run_on()) return;
    run_apply_fixes();
    if (run_watch_phase && run_at_hub && ++run_watch_count == run_watch_limit()) run_watch_dump();
    if (run_irq_again && --run_irq_again == 0) { run_irq_log(0); run_cd_state_line("2 s later"); }
    if (run_at_hub && !run_autopilot && !run_watch_phase && ++run_idle == RUN_WATCH_IDLE) {
        run_watch_phase = 3;                     /* for the dump's text and file name only */
        run_watch_dump();
        run_watch_phase = 0;
    }
    if (run_at_hub && psx_mod_read_byte(RUN_ARENA_LOCK)) {
        psx_mod_write_byte(RUN_ARENA_LOCK, 0);
        psx_mod_counter_add("moa.run.arena_unlocked", 1);
    }
    run_show_frame();
    if (run_ghost_beaten_n && run_prize_where && !psx_mod_read_byte(RUN_FIGHT_DONE))
        run_ghost_save(run_ghost_beaten[--run_ghost_beaten_n]);
    run_prize_frame();
    if (run_loss_wait) run_loss_wait--;      /* the return itself is made in a guest hook */
    run_end_frame();
    run_hx_frame();
    if (run_autopilot && run_stage == STAGE_BACK) {
        if (++run_frames - run_back_start >= (run_direct ? RUN_DIRECT_HOLD : RUN_BACK_FRAMES))
            run_finish_handover();
        else if (!run_quiet()) psx_mod_set_loading_screen("ROGUE ARENA  Preparing your run", 98);
        return;
    }
    if (run_autopilot) {
        if (!psx_mod_fast_forward_requested()) psx_mod_request_fast_forward(1);
        const uint32_t f = run_frames++;
        int permille = (int)(f * 1000u / (RUN_HUB_TIMEOUT / 3u));
        if (permille > 950) permille = 950;
        if (!run_quiet()) psx_mod_set_loading_screen("ROGUE ARENA  Preparing your run", permille / 10);
        if (f >= (run_direct ? RUN_DIRECT_TIMEOUT : RUN_HUB_TIMEOUT)) run_reach_hub("moa.run.hub_timeout");
        return;
    }
    if (run_fade) {                            /* a short hold at 100%, then away */
        if (--run_fade) psx_mod_set_loading_screen("ROGUE ARENA  Ready", 100);
        else psx_mod_set_loading_screen(NULL, 0);
    }
}

PSX_MOD_CONSTRUCTOR(moa_arena_run_boot_register)
{
    moa_btrace_register();                   /* first: see moa_boot_trace.h */
    (void)psx_game_register_offline_function_entry(RUN_MENU_ENTRY, run_menu_entry);
    (void)psx_game_register_offline_function_entry(RUN_MENU_INPUT_A, run_menu_input_entry);
    (void)psx_game_register_offline_function_entry(RUN_MENU_INPUT_B, run_menu_input_entry);
    (void)psx_game_register_offline_function_entry(RUN_MODE_CONFIRM, run_mode_confirm_entry);
    (void)psx_game_register_offline_function_filter(RUN_TITLE_OPEN, run_title_open_filter);
    (void)psx_game_register_offline_function_entry(RUN_D_HUB_INPUT, run_hub_input_entry);
    (void)psx_game_register_offline_function_entry(RUN_ARENA_INPUT, run_hx_arena_input);
    (void)psx_game_register_offline_function_entry(RUN_ARENA_LIST, run_hx_arena_list);
    (void)psx_game_register_offline_function_filter(RUN_MOVIE_PLAY, run_movie_filter);
    (void)psx_game_register_offline_function_entry(RUN_HUB_RESUME, run_hub_entry);
    (void)psx_game_register_offline_function_entry(RUN_D_HUB_OPEN, run_hub_open_entry);
    (void)psx_game_register_offline_function_entry(RUN_HUB_OPEN_B, run_hub_open_entry);
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
