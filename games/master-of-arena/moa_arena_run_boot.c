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
#include "moa_chime.h"
#include "moa_vs_sound.h"
#include "moa_render_queue.h"
#include "moa_music.h"
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
static uint32_t run_pay;                   /* 1: the next payout follows a new win (it pays 0 now) */
static int run_prize_paid;                 /* a new win was just paid: its prize may open now */
static int run_save_pending;               /* a win waits for the autosave */
static unsigned run_win_screen;            /* booked wins waiting for the arena screen (P) */
static uint8_t run_win_told = 0xFF;        /* last entry run_book_win logged (booked or skipped) */

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
static int run_end_choice = -1;            /* the pick: 0 retry, 1 new, 2 quit */
static int run_prize_stage;                /* defined with the prize screen */
static uint32_t run_end_frames;
static uint16_t run_end_held;
static int run_overlay_up(void);
static void run_pad_lock(void);
/* Boot black screen: from the menu's start until the player has control
 * (the hub overlay is up), so the hub never shows half-built during the
 * boot. Locks the controls once the run has handed over. */
static int run_boot_black;                 /* 0 off, 1 overlay picture, 2 loading-screen fallback */
/* Music (moa_music.cpp): a fight is loading or running (the game's music). */
static int run_music_fight;
static void run_boot_black_up(void);
/* The direct boot started the menu song: no fast forward, so it is heard. */
static int run_boot_music;
/* The arena screen's result routine ran this frame with the result applied
 * (it saves). After a win the game soon leaves it for the opponent list,
 * where it never runs: then the menu's controller hook saves. */
static int run_result_ran;
static void run_autosave(struct CPUState *cpu);
static void run_log(const char *fmt, ...);
static uint32_t run_boot_black_frames, run_boot_black_after;
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

/* Fast loading off from the hub on (test 8): MoA runs its drive in the
 * bounded "instant" mode (mod_builtin_moa_loading.c), and the after-win stall
 * and the after-win freezes were reads that mode never finished. Booting to
 * the hub is fine with it, so once the player has the hub and the disc is
 * idle, the run turns the drive back to its real speed for the rest of the
 * session. PSX_MOA_RUN_FAST_LOAD=1 keeps fast loading (A/B). The waits below
 * that assume fast loads get longer while it is off. */
static int run_disc_slow;                 /* 0 not yet, 1 real speed, -1 kept fast */

/* The cover: an opaque picture over the game from the menu's restart after a
 * fight (the hub reloading) through the arena screen's results, until the hub
 * overlay takes over. Phase 1 loading, 2 results. The controls are locked
 * while it is up; the screens it hides finish on their own timers. */
static int run_cover;
static int run_cover_drawn;
/* The victory screen: the cover's first picture after a win. */
static int run_victory_who = -1;          /* arena 0 entry beaten last; -1 none */
static int run_victory_ready;             /* the renders are made for this cover */
static unsigned run_victory_key;
/* It stays up, animating, until the game behind it is ready (the results
 * done: the hub overlay could open) and the player presses X; the hub
 * overlay waits for that (run_hx_frame). */
static int run_victory_hold;
static int run_vs;                         /* the VS screen is up (NEXT BATTLE until the fight) */
static void run_vs_open(void);
static void run_vs_close(const char *why);
static void run_vs_fight_runs(void);
static uint32_t run_victory_frames;
static int run_victory_words_shown, run_victory_armed;
static uint16_t run_victory_held = 0xFFFFu;
static uint32_t run_cover_frames, run_cover_idle;
static void run_cover_end(const char *why);
static void run_backdrop_frame(void);

/* Sounds. The game's menu sound effects go through the exe's SsUtKeyOn
 * (0x800227D4: vab, prog, tone, note, fine, vol L, vol R); the menu's bank
 * id is the halfword at 0x801B1A80. The arena list uses tone 1 (cursor),
 * 2 (X) and 0 (O). The overlay plays those for its own menus (queued from
 * the frame hook, played from the menu's controller sampling, where a guest
 * call is safe); the game's own effects are muted while the run drives its
 * menus behind a picture (NEXT BATTLE, the cover, the payout). */
#define RUN_SS_KEYON         0x800227D4u
#define RUN_MENU_VAB         0x801B1A80u
#define RUN_LOADING_SCREEN   0x80012348u  /* exe: the loading picture before an engine */
#define RUN_MUSIC_START      0x80064F24u  /* menu: the menu song's start (moa_music.cpp) */
enum { RUN_SE_BACK = 0, RUN_SE_CURSOR = 1, RUN_SE_OK = 2 };
static int run_se_queue = -1;
static uint32_t run_se_age;               /* frames queued; dropped after a few (no menu running) */
static int run_se_own;
static void run_se(int tone) { run_se_queue = tone; run_se_age = 0; }
#define RUN_SLOW(n) (run_disc_slow == 1 ? (n) * 4u : (n))

static void run_watch(int phase)
{
    if (run_watch_phase == phase) return;
    run_watch_phase = phase;
    run_watch_count = 0;
}

static uint8_t run_arena_at_builds = 0xFF, run_arena_at_ai = 0xFF;

static void run_prize_reset(void);
static void run_reconcile(int refresh);
static int run_ac_refresh;                /* defined with the garage helpers below */
static void run_ac_refresh_now(struct CPUState *cpu);
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
    run_replay_state = 0;
    run_replay_t = 0;
    run_loss_wait = 0;
    run_end_stage = run_end_cur = run_end_armed = run_end_overlay = 0;
    run_end_choice = -1;
    run_disc_slow = 0;
    run_cover = run_cover_drawn = 0;
    run_se_queue = -1;
    run_victory_who = -1;
    run_victory_ready = 0;
    run_victory_hold = 0;
    run_end_frames = 0;
    run_boot_black = 0;
    run_boot_black_frames = run_boot_black_after = 0;
    run_music_fight = run_boot_music = run_result_ran = 0;
    moa_music_reset(run_log);
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

/* refresh: 1 once the game has queued the AC's textures itself (the menu
 * boot), so a garage put back from the run file queues them again. */
static void run_apply_state(int refresh)
{
    /* The save carries the run; the run file is read for what comes next
     * (results, P, the arena). */
    const char *path = run_file_path();
    run_state_ok = path && path[0] && moa_run_load(path, &run_state) == 1;
    if (!run_state_ok) psx_mod_counter_add("moa.run.file_missing", 1);
    else { run_reconcile(refresh); run_sync_arena(); }
}

static void run_finish_handover(void)
{
    run_autopilot = 0;
    run_at_hub = 1;
    run_prize_where = 1;                       /* at the hub: a prize due may open (test 2 waited for the Shop) */
    run_apply_state(1);
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
    run_music_fight = 0;
    moa_music_menu_entry();                   /* the menu clears its song's flag here */
    if (run_at_hub) {                         /* already handed over: a fight is over */
        if (run_state_ok && !run_state.run.over && !run_cover) {
            run_cover = 1;
            run_cover_drawn = 0;
            run_cover_frames = run_cover_idle = 0;
            run_victory_ready = 0;
            run_victory_hold = run_victory_who >= 0;
            run_victory_frames = 0;
            run_victory_words_shown = run_victory_armed = 0;
            run_victory_held = 0xFFFFu;
            run_log("cover: up while the menu reloads%s", run_victory_who >= 0 ? " (victory screen)" : "");
        }
        return;
    }
    run_autopilot = 1;
    run_ticks = run_frames = 0;
    run_stage = STAGE_TITLE;
    run_set_ff(1);
    run_boot_black_up();
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
    /* The menu song now, so it plays through the rest of the boot (the hub
     * open's own start is then skipped, moa_music_start_filter). Without fast
     * forward from here: under it the sound is muted. */
    if (moa_music_boot_start(cpu, ra)) {
        run_boot_music = 1;
        psx_mod_request_fast_forward(0);
    }
    (void)psx_mod_call_guest(cpu, RUN_D_PICTURES, ra, 4, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_PICTURES, ra, 4, 1, 0, 0);
    (void)psx_mod_call_guest(cpu, RUN_D_PICTURES, ra, 4, 4, 0, 0);
    /* 2. LOAD's 'N' path, after the card read */
    for (size_t k = 0; k < sizeof k_run_d_unpack / sizeof k_run_d_unpack[0]; k++)
        for (uint32_t i = 0; i < k_run_d_unpack[k].bytes; i++)
            psx_mod_write_byte(k_run_d_unpack[k].dst + i, save[k_run_d_unpack[k].from + i]);
    /* The run file now (was after the card finish): a garage the card never
     * got (the session ended before the autosave) goes in before the owned
     * copy, the part lists and the AC's textures below are made from it. */
    run_apply_state(0);
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
    run_apply_prices();                          /* shop in P; the hub-resume hook redoes it later */
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

/* A queued overlay sound, through the game's SsUtKeyOn with the menu's
 * bank (only here: the menu is live and a guest call is safe). The three
 * stack arguments go below the caller's frame; GPRs are restored after. */
static void run_se_flush(struct CPUState *cpu)
{
    if (run_se_queue < 0) return;
    const int tone = run_se_queue;
    run_se_queue = -1;
    const int16_t vab = (int16_t)psx_mod_read_half(RUN_MENU_VAB);
    if (vab < 0 || vab > 15) return;
    const uint32_t sp0 = cpu->gpr[29], sp = sp0 - 64u;
    psx_mod_write_word(sp + 16u, 0u);       /* fine */
    psx_mod_write_word(sp + 20u, 127u);     /* volume L */
    psx_mod_write_word(sp + 24u, 127u);     /* volume R */
    cpu->gpr[29] = sp;
    run_se_own = 1;
    (void)psx_mod_call_guest(cpu, RUN_SS_KEYON, cpu->gpr[31], (uint32_t)(int32_t)vab, 0u, (uint32_t)tone, 60u);
    run_se_own = 0;
    cpu->gpr[29] = sp0;
}

/* The game's own sound effects, muted while the run drives the menus behind
 * a picture. Never in a fight (the cover and NEXT BATTLE end before one). */
static int run_keyon_filter(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (run_se_own || !run_on()) return 0;
    if (!(run_autopilot || (run_at_hub && (run_hx || run_cover)))) return 0;
    cpu->gpr[2] = 0xFFFFFFFFu;               /* SsUtKeyOn: no voice */
    psx_mod_counter_add("moa.run.game_sound_muted", 1);
    return 1;
}

static void run_menu_input_entry(struct CPUState *cpu, uint32_t address)
{
    run_idle = 0;
    if (run_on()) {
        const uint32_t w = psx_mod_read_word(address);
        if (w == RUN_SIG_INPUT_A || w == RUN_SIG_INPUT_B) {
            moa_music_menu_frame(cpu);
            /* Test 9 hang (1ad4e38): the save waited for the arena screen's
             * result routine, which had stopped running (the game was on the
             * opponent list under the cover). Save from here too. */
            if (run_save_pending && run_at_hub && !run_autopilot && !psx_mod_read_byte(RUN_FIGHT_DONE))
                run_autosave(cpu);
            /* A prize equipped (or the garage put back on load): its textures. */
            if (run_ac_refresh && run_at_hub && !run_autopilot && !psx_mod_read_byte(RUN_FIGHT_DONE))
                run_ac_refresh_now(cpu);
        }
    }
    if (run_overlay_up() && run_on()) {
        const uint32_t w = psx_mod_read_word(address);
        if (w != RUN_SIG_INPUT_A && w != RUN_SIG_INPUT_B) return;
        run_se_flush(cpu);
        if (run_hx_menu_pad()) return;
        run_pad_lock();
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


/* ---- step 2: P prices -----------------------------------------------------------
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

/* In moa_part_cat order (moa_parts.h), so a table index is a part category.
 * folder: the part models' folder (models/menu/<folder>/<row>); label: shown
 * on the prize screen. */
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

/* ---- part pictures and the menu font --------------------------------------------- */
#define RUN_THUMB      128

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
    if (!ok) run_log("run screens: menu font not found");
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

/* ---- the garage AC ------------------------------------------------------------------
 *
 * 0x80036FB0 holds the AC in the save's part order: core, booster, head,
 * arms, blade, hand, back L, back R, generator, FCS, -, legs; the options are
 * the two bytes at 0x8003EA70. The card record (moa_parts.h) orders them core,
 * head, arms, legs, hand, blade, back L, back R, FCS, generator, options x2,
 * booster. */
#define RUN_GARAGE           0x80036FB0u
#define RUN_GARAGE_OPTS      0x8003EA70u

static void run_garage_rec(uint8_t rec[13])
{
    uint8_t g[12];
    for (unsigned i = 0; i < 12; i++) g[i] = psx_mod_read_byte(RUN_GARAGE + i);
    const uint8_t r[13] = { g[0], g[2], g[3], g[11], g[5], g[4], g[6], g[7], g[9], g[8],
                            psx_mod_read_byte(RUN_GARAGE_OPTS), psx_mod_read_byte(RUN_GARAGE_OPTS + 1u), g[1] };
    memcpy(rec, r, sizeof r);
}

static void run_garage_set(const uint8_t r[13])
{
    static const uint8_t k_at[13] = { 0, 2, 3, 11, 5, 4, 6, 7, 9, 8, 0xFF, 0xFF, 1 };   /* record byte -> garage byte */
    for (unsigned b = 0; b < 13; b++)
        if (k_at[b] != 0xFF) psx_mod_write_byte(RUN_GARAGE + k_at[b], r[b]);
}

/* The garage after a prize change, kept in the run file until the run card
 * has it (run_autosave), and put back on load when it never got there. */
static void run_snap_take(void)
{
    struct moa_run_progress *r = &run_state.run;
    for (unsigned i = 0; i < 12; i++) r->snap_parts[i] = psx_mod_read_byte(RUN_GARAGE + i);
    for (unsigned i = 0; i < 2; i++) r->snap_opts[i] = psx_mod_read_byte(RUN_GARAGE_OPTS + i);
    for (unsigned i = 0; i < 256; i++) r->snap_owned[i] = psx_mod_read_byte(RUN_OWNED + i);
    r->snap_pending = 1;
}

static void run_snap_put(void)
{
    const struct moa_run_progress *r = &run_state.run;
    for (unsigned i = 0; i < 12; i++) psx_mod_write_byte(RUN_GARAGE + i, r->snap_parts[i]);
    for (unsigned i = 0; i < 2; i++) psx_mod_write_byte(RUN_GARAGE_OPTS + i, r->snap_opts[i]);
    for (unsigned i = 0; i < 256; i++) {
        psx_mod_write_byte(RUN_OWNED + i, r->snap_owned[i]);
        psx_mod_write_byte(RUN_D_OWNED_COPY + i, r->snap_owned[i]);
    }
}

/* The garage AC changed outside the garage (a prize equipped, or the
 * snapshot put back after the boot): queue its part textures again, as the
 * arena's return path does after every fight (0x800822B0; the hub never
 * loads them, docs/mapping/boot-loads.md). The garage rebuilds its own part
 * lists from the owned table when it opens (menus-hub.md). From the menu's
 * controller sampling, where a guest call is safe. */
static int run_ac_refresh;                /* (tentative definition, declared at the top) */

static void run_ac_refresh_now(struct CPUState *cpu)
{
    run_ac_refresh = 0;
    for (unsigned i = 0; i < 256; i++)
        psx_mod_write_byte(RUN_D_OWNED_COPY + i, psx_mod_read_byte(RUN_OWNED + i));
    if (psx_mod_read_word(RUN_D_AC_TEXTURES) != RUN_D_SIG_AC_TEXTURES) {
        run_log("garage: AC textures not queued (routine not in memory)");
        return;
    }
    (void)psx_mod_call_guest(cpu, RUN_D_AC_TEXTURES, cpu->gpr[31], 0, 0, 0, 0);
    psx_mod_counter_add("moa.run.ac_refresh", 1);
    run_log("garage: AC part textures queued again");
}

/* ---- P bookkeeping ------------------------------------------------------------
 *
 * The game's credits are the player's P: what is left after prizes. P is
 * only ever added to them (a win, PASS) or taken (a prize bought), never set
 * from the run's totals. The run file says how much P went into the credits
 * (p_paid) and how much of that the run card holds (p_card, at its last save),
 * and the same for P spent (p_spent, p_spent_card). When a session loads the
 * card, whatever never reached it (the session ended before the autosave) is
 * done again: P earned added, P spent taken, the garage snapshot put back. */
static int run_reconciled;
static int run_pay_due;                    /* the payout still has to pay the win's 1 P */
static uint32_t run_pay_wait;              /* result frames the autosave waited on it */

static void run_reconcile(int refresh)
{
    if (run_reconciled || !run_state_ok || run_state.run.over) return;
    run_reconciled = 1;
    struct moa_run_progress *r = &run_state.run;
    int snap = 0;
    if (r->snap_pending) {
        run_snap_put();
        snap = 1;
        if (refresh) run_ac_refresh = 1;
    }
    const int delta = ((int)r->points - (int)r->p_card) - ((int)r->p_spent - (int)r->p_spent_card);
    if (delta) {
        int64_t c = (int64_t)psx_mod_read_word(RUN_CREDITS) + delta;
        psx_mod_write_word(RUN_CREDITS, c < 0 ? 0u : (uint32_t)c);
    }
    unsigned parts = 0;                        /* version 6 milestone prizes never saved */
    for (unsigned m = 0; m < MOA_RUN_PRIZES; m++) {
        const uint8_t bit = (uint8_t)(1u << m), ix = r->prize_ix[m];
        if (!(r->prizes & bit) || (r->prize_saved & bit) || ix == MOA_RUN_PRIZE_P) continue;
        const uint8_t have = psx_mod_read_byte(RUN_OWNED + ix);
        psx_mod_write_byte(RUN_OWNED + ix, (uint8_t)(have + (ix >= 157u && ix < 209u ? 2u : 1u)));
        parts++;
    }
    r->p_paid = r->points;
    if (delta || parts || snap) {
        run_save_pending = 1;
        (void)moa_run_save(run_file_path(), &run_state);
    }
    run_log("load: P %u (spent %u), %+d P not on the card applied, %u old prize part(s) added%s",
            (unsigned)r->points, (unsigned)r->p_spent, delta, parts,
            snap ? ", garage put back from the run file" : "");
}

/* ---- the prize screen ------------------------------------------------------------
 *
 * After every win (design: docs/rogue-arena-v2-design.md, "Prize screen"):
 * up to three part offers, one cheap (1-2 P), one mid (3-4 P), one expensive
 * (5-7 P), by run_tier, then PASS (+1 P). An offer is a real part the player
 * does not own whose swap into the current AC passes the game's fitting
 * rules and checks (moa_build_fit, moa_build_check) without knocking off
 * anything but the part it replaces (weapon arms also take the hand weapon
 * and blade; caterpillar legs the booster; a two-slot back unit both back
 * slots). Offers the player can't afford are shown dimmed. Taking one costs
 * its P, then EQUIP NOW (the old part is gone for good) or SAVE FOR LATER
 * (the run's saved list; owned, so the game's garage has it too). The offers
 * come from the seed, the attempt, the win and the AC, so a reopen shows the
 * same rows. Shown over the arena screen or the hub, never in a fight. */
#define RUN_PAD_UP    0x0010u
#define RUN_PAD_RIGHT 0x0020u
#define RUN_PAD_DOWN  0x0040u
#define RUN_PAD_LEFT  0x0080u
#define RUN_BACK_DETAIL 0x800D21D4u            /* back weapon details, 24 bytes by back id */
#define RUN_TYPE_AT   0x12u
#define RUN_MENU_FROM 0x800CE690u              /* the part tables ... */
#define RUN_MENU_TO   0x800D2774u              /* ... to the end of the weapon details */

enum { PZ_OFF = 0, PZ_CHOOSE, PZ_ASK };
enum { ASK_EQUIP0 = 0, ASK_EQUIP1, ASK_SAVE, ASK_BACK };
static const char *const k_tier_tag[3] = { "CHEAP", "MID", "EXPENSIVE" };

struct run_offer {
    uint8_t cat, id, cost, band, nput;
    uint8_t income;                            /* not a part: the +1 P per win upgrade */
    uint8_t xn, xcat[2], xid[2];               /* a set: parts that come with it (arms set) */
    uint8_t put_slot[2];                       /* record byte it goes into */
    uint8_t put_rec[2][13];                    /* the AC with it equipped there */
    char put_lost[2][48];                      /* what that equip throws away ("" none) */
    char name[19], category[56], cost_s[8];
    char xname[2][19], xcategory[2][56];       /* a set's other parts, as its box shows them */
    uint32_t w;                                /* draw weight (1000 = normal) */
};
static struct run_offer run_offer[3];
static int run_offer_n;
static uint32_t run_offer_thumb[3][RUN_THUMB * RUN_THUMB];
static int run_offer_has_thumb[3];
static uint32_t run_offer_xthumb[3][2][RUN_THUMB * RUN_THUMB];   /* a set's other parts */
static int run_offer_has_xthumb[3][2];
/* A set's picture: each part held RUN_SET_HOLD frames, then the next wiped in
 * from the top over RUN_SET_WIPE frames (a scan line, like the garage's monitor). */
#define RUN_SET_HOLD 96u
#define RUN_SET_WIPE 16u
static struct moa_parts run_parts;

static int run_prize_stage;               /* PZ_* */
static int run_prize_cur, run_prize_ask_cur, run_prize_ask_n;
static int run_prize_alt;                 /* on the bottom row: 1 = RE-ROLL, 0 = PASS */
static unsigned run_prize_win;            /* the win whose screen is up (0 = the first) */
static int run_prize_ask_kind[4];
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
    run_prize_stage = PZ_OFF;
    run_prize_where = 0;
    run_ac_refresh = 0;
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

/* The part tables from the menu module in RAM (prices already in P). */
static int run_parts_read(void)
{
    if (psx_mod_read_word(k_run_tables[0].addr) != 0x302D4448u) return 0;   /* "HD-0" */
    static uint8_t mem[RUN_MENU_TO - RUN_MENU_FROM];
    for (uint32_t i = 0; i < sizeof mem; i++) mem[i] = psx_mod_read_byte(RUN_MENU_FROM + i);
    return moa_parts_from_menu(mem, RUN_MENU_FROM, (uint32_t)sizeof mem, &run_parts);
}

static uint32_t run_part_p(const struct moa_part *q)
{
    return q->price > 7u ? run_tier(q->price) : q->price;
}

/* The owned-table index of record byte b's part, -1 for none. */
static int run_rec_ix(const uint8_t rec[13], int b)
{
    const int cat = moa_record_cat(b);
    if (cat < 0 || rec[b] == 255u || (cat == MOA_CAT_HAND && rec[b] == 0u)) return -1;
    const int ix = (int)k_run_tables[cat].base + rec[b];
    return ix < 256 ? ix : -1;
}

/* Where part (cat, id) may go into `now`, and what each place costs. 1 = fits. */
static int run_offer_fit(int cat, uint8_t id, const uint8_t now[13], int plus, struct run_offer *o)
{
    static const int8_t k_slot[MOA_CAT__COUNT] = { 1, 0, 3, 2, 4, 6, 5, 12, 8, 9 };
    int slots[2] = { k_slot[cat], -1 };
    const int two_on = (now[6] != 255u && now[6] >= 52u) || (now[7] != 255u && now[7] >= 52u);
    if (cat == MOA_CAT_BACK && id < 52u && !two_on) slots[1] = 7;   /* over a two-slot unit: one way only */
    o->nput = 0;
    int free_at = -1;
    for (int k = 0; k < 2 && slots[k] >= 0; k++) {
        const int s = slots[k];
        uint8_t rec[13];
        memcpy(rec, now, 13);
        rec[s] = id;
        uint32_t allowed = 1u << s;
        if (cat == MOA_CAT_BACK) {
            const int other = s == 6 ? 7 : 6;
            if (id >= 52u || (rec[other] != 255u && rec[other] >= 52u)) {   /* a two-slot unit either way */
                rec[other] = 255u;
                allowed |= 1u << other;
            }
        }
        if (cat == MOA_CAT_ARMS && id >= 12u) allowed |= 1u << 4 | 1u << 5;     /* weapon arms: no hands */
        if (cat == MOA_CAT_LEGS && id >= 27u) allowed |= 1u << 12;              /* caterpillar: no booster */
        moa_build_fit(rec);
        if (rec[s] != id) continue;
        char lost[48] = "";
        int bad = 0;
        for (int b = 0; b < 13 && !bad; b++) {
            if (b == 10 || b == 11 || rec[b] == now[b]) continue;
            const int was = run_rec_ix(now, b);
            if (was < 0) continue;                                              /* that slot was empty */
            if (!(allowed & (1u << b))) { bad = 1; break; }
            const struct moa_part *q = moa_parts_find(&run_parts, moa_record_cat(b), now[b]);
            if (q && strlen(lost) + strlen(q->name) + 3 < sizeof lost)
                snprintf(lost + strlen(lost), sizeof lost - strlen(lost), "%s%s", lost[0] ? ", " : "", q->name);
        }
        if (bad) continue;
        struct moa_build_check c;
        moa_build_check(&run_parts, rec, plus, &c);
        if (c.blocked) continue;
        const int n = o->nput++;
        o->put_slot[n] = (uint8_t)s;
        memcpy(o->put_rec[n], rec, 13);
        snprintf(o->put_lost[n], sizeof o->put_lost[n], "%s", lost);
        if (!lost[0] && free_at < 0) free_at = n;
    }
    if (free_at > 0) {                          /* an empty back slot: no need to ask */
        o->put_slot[0] = o->put_slot[free_at];
        memcpy(o->put_rec[0], o->put_rec[free_at], 13);
        o->put_lost[0][0] = 0;
    }
    if (free_at >= 0) o->nput = 1;
    return o->nput > 0;
}

static void run_offer_label(struct run_offer *o, const struct moa_part *q)
{
    snprintf(o->name, sizeof o->name, "%s", q->name);
    char type[19] = "";
    if (o->cat == MOA_CAT_HAND || o->cat == MOA_CAT_BACK || o->cat == MOA_CAT_BLADE) {
        run_text_at(k_run_tables[o->cat].addr + (uint32_t)o->id * k_run_tables[o->cat].stride + RUN_TYPE_AT, 18, type);
        if (type[0] == '-') type[0] = 0;
    } else if (o->cat == MOA_CAT_ARMS || o->cat == MOA_CAT_LEGS) {
        snprintf(type, sizeof type, "%s", moa_part_group(o->cat, q->name));
    }
    snprintf(o->category, sizeof o->category, "%s%s%s", k_run_tables[o->cat].label, type[0] ? "  " : "", type);
    snprintf(o->cost_s, sizeof o->cost_s, "%u P", (unsigned)o->cost);
}

static uint64_t run_prize_rng(unsigned win, uint64_t salt)
{
    return run_state.lineup.seed ^ (0x5052495A32ull * (win + 1u)) ^ salt ^
           (0x9E3779B97F4A7C15ull * (uint64_t)(run_state.run.attempt + 1u));
}

static int run_plus_on(void)
{
    if (run_state.run.plus) return 1;
    for (uint32_t i = 0; i < 8u; i++) if (psx_mod_read_byte(RUN_D_PLUS + i)) return 1;
    return 0;
}

/* ---- what the randomizer prefers (user, 2026-11-18) -------------------------------
 * - Legs: each screen picks one leg type, the four equally likely (25%
 *   each), then a random part of that type (legs keep their usual share).
 * - Radars: only when the AC has a strong back weapon (5 P or more) and an
 *   arm weapon (hand weapon or weapon arms), and then rarely.
 * - Back weapons that make two-legged ACs (humanoid, reverse joint) crouch
 *   to fire: less likely on those legs, and rarely a second one. Two-legged
 *   legs are rarely offered to an AC that already carries two.
 * - Weapon arms equipped: regular arms come only as the expensive offer, an
 *   ARMS SET of regular arms + hand weapon + blade (each 4 P or less) for 7 P. */
#define RUN_W_NORMAL      1000u
#define RUN_W_RADAR        250u
#define RUN_W_STANCE       400u                /* a crouch weapon, two-legged AC */
#define RUN_W_STANCE_TWO    80u                /* ... that would make two */
#define RUN_SET_COST         7u
#define RUN_INCOME_COST      6u                /* +1 P PER WIN: its price, */
#define RUN_INCOME_ODDS      4u                /* offered on 1 screen in this many, */
#define RUN_INCOME_MAX       3u                /* bought this often at most */
#define RUN_SET_PART_MAX     4u

/* Back weapon `id` makes two-legged ACs crouch to fire (cannons; energy
 * cannons with the guide sight): the stance rule in acr_parts_moa.c. */
static int run_back_stance(uint8_t id)
{
    if (id == 255u || id > 57u) return 0;
    const uint32_t w = RUN_BACK_DETAIL + 24u * id;
    const uint8_t kind = psx_mod_read_byte(w), sight = psx_mod_read_byte(w + 5u), fl = psx_mod_read_byte(w + 0x12u);
    return kind == 2u || ((fl & 0x20u) && sight == 3u);
}

static int run_stance_n(const uint8_t rec[13])
{
    return run_back_stance(rec[6]) + run_back_stance(rec[7]);
}

/* Leg type by id: 0 humanoid (0-13), 1 reverse joint (14-20), 2 four legs
 * (21-26), 3 caterpillar (27-32), as moa_part_group names them. */
static int run_leg_type(uint8_t id)
{
    return id < 14u ? 0 : id < 21u ? 1 : id < 27u ? 2 : 3;
}

static int run_two_legged(uint8_t legs)
{
    return legs != 255u && run_leg_type(legs) <= 1;
}

static int run_is_radar(uint8_t back) { return back >= 40u && back <= 47u; }

static int run_radar_ok(const uint8_t now[13])
{
    const int arm = (now[2] != 255u && now[2] >= 12u) || (now[4] != 255u && now[4] != 0u);
    int strong = 0;
    for (int s = 6; s <= 7; s++) {
        const uint8_t id = now[s];
        if (id == 255u || run_is_radar(id) || id == 57u) continue;   /* 57: P77-ST, stealth */
        const struct moa_part *q = moa_parts_find(&run_parts, MOA_CAT_BACK, id);
        if (q && run_part_p(q) >= 5u) strong = 1;
    }
    return arm && strong;
}

static int run_part_real(int cat, uint8_t id)
{
    char type[19];
    run_text_at(k_run_tables[cat].addr + (uint32_t)id * k_run_tables[cat].stride + RUN_TYPE_AT, 18, type);
    return type[0] && type[0] != '-';
}

static int run_part_owned(int cat, uint8_t id)
{
    const uint32_t ix = k_run_tables[cat].base + id;
    return ix > 255u || psx_mod_read_byte(RUN_OWNED + ix) != 0u;
}

/* The ARMS SET for an AC on weapon arms: regular arms + hand weapon + blade,
 * none owned, each 4 P or less, the three fitting together. 1 = made. */
static int run_set_roll(const uint8_t now[13], int plus, uint64_t *rng, struct run_offer *o)
{
    uint8_t ids[3][MOA_PARTS_MAX];
    unsigned n[3] = { 0, 0, 0 };
    static const int k_cat[3] = { MOA_CAT_ARMS, MOA_CAT_HAND, MOA_CAT_BLADE };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < run_parts.count[k_cat[k]]; i++) {
            const struct moa_part *q = &run_parts.part[k_cat[k]][i];
            const uint32_t p = run_part_p(q);
            if (!p || p > RUN_SET_PART_MAX) continue;
            if (k_cat[k] == MOA_CAT_ARMS && q->id >= 12u) continue;
            if (k_cat[k] == MOA_CAT_HAND && q->id == 0u) continue;
            if (run_part_owned(k_cat[k], q->id) || !run_part_real(k_cat[k], q->id)) continue;
            ids[k][n[k]++] = q->id;
        }
    if (!n[0] || !n[1] || !n[2]) return 0;
    for (int t = 0; t < 64; t++) {
        const uint8_t a = ids[0][moa_run_below(rng, n[0])], h = ids[1][moa_run_below(rng, n[1])],
                      bl = ids[2][moa_run_below(rng, n[2])];
        uint8_t rec[13];
        memcpy(rec, now, 13);
        rec[2] = a; rec[4] = h; rec[5] = bl;
        moa_build_fit(rec);
        if (rec[2] != a || rec[4] != h || rec[5] != bl) continue;
        int bad = 0;
        for (int b = 0; b < 13 && !bad; b++) {
            if (b == 2 || b == 4 || b == 5 || b == 10 || b == 11 || rec[b] == now[b]) continue;
            if (run_rec_ix(now, b) >= 0) bad = 1;               /* something else would come off */
        }
        if (bad) continue;
        struct moa_build_check c;
        moa_build_check(&run_parts, rec, plus, &c);
        if (c.blocked) continue;
        const struct moa_part *qa = moa_parts_find(&run_parts, MOA_CAT_ARMS, a);
        const struct moa_part *qh = moa_parts_find(&run_parts, MOA_CAT_HAND, h);
        const struct moa_part *qb = moa_parts_find(&run_parts, MOA_CAT_BLADE, bl);
        const struct moa_part *qw = moa_parts_find(&run_parts, MOA_CAT_ARMS, now[2]);
        if (!qa || !qh || !qb) continue;
        memset(o, 0, sizeof *o);
        o->cat = MOA_CAT_ARMS; o->id = a; o->cost = RUN_SET_COST; o->band = 2;
        o->xn = 2; o->xcat[0] = MOA_CAT_HAND; o->xid[0] = h; o->xcat[1] = MOA_CAT_BLADE; o->xid[1] = bl;
        o->nput = 1; o->put_slot[0] = 2;
        memcpy(o->put_rec[0], rec, 13);
        snprintf(o->put_lost[0], sizeof o->put_lost[0], "%s", qw ? qw->name : "");
        snprintf(o->name, sizeof o->name, "%s", qa->name);
        snprintf(o->category, sizeof o->category, "ARMS SET  ARMS");
        char type[19];
        snprintf(o->xname[0], sizeof o->xname[0], "%s", qh->name);
        run_text_at(k_run_tables[MOA_CAT_HAND].addr + (uint32_t)h * k_run_tables[MOA_CAT_HAND].stride + RUN_TYPE_AT, 18, type);
        snprintf(o->xcategory[0], sizeof o->xcategory[0], "ARMS SET  ARM WEAPON  %s", type);
        snprintf(o->xname[1], sizeof o->xname[1], "%s", qb->name);
        snprintf(o->xcategory[1], sizeof o->xcategory[1], "ARMS SET  BLADE");
        snprintf(o->cost_s, sizeof o->cost_s, "%u P", RUN_SET_COST);
        o->w = RUN_W_NORMAL;
        return 1;
    }
    return 0;
}

/* One draw of the offers. `excl` (n pairs cat,id): parts left out (a
 * re-roll leaves out what the first draw showed). */
static void run_prize_draw_offers(unsigned win, uint64_t salt, const uint8_t (*excl)[2], int nexcl)
{
    run_offer_n = 0;
    uint8_t now[13];
    run_garage_rec(now);
    const int plus = run_plus_on();
    const int weapon_arms = now[2] != 255u && now[2] >= 12u;
    const int two_legged = run_two_legged(now[3]);
    const int stance_now = run_stance_n(now);
    const int radar_ok = run_radar_ok(now);
    static struct run_offer cand[3][96];
    unsigned nc[3] = { 0, 0, 0 };
    for (int cat = 0; cat < MOA_CAT__COUNT; cat++)
        for (int i = 0; i < run_parts.count[cat]; i++) {
            const struct moa_part *q = &run_parts.part[cat][i];
            const uint32_t p = run_part_p(q);
            if (!p || p > 7u) continue;
            if (run_part_owned(cat, q->id) || !run_part_real(cat, q->id)) continue;
            if (cat == MOA_CAT_BACK && run_is_radar(q->id) && !radar_ok) continue;
            if (cat == MOA_CAT_ARMS && q->id < 12u && weapon_arms) continue;   /* the ARMS SET instead */
            int skip = 0;
            for (int e = 0; e < nexcl && !skip; e++) skip = excl[e][0] == cat && excl[e][1] == q->id;
            if (skip) continue;
            const unsigned band = p <= 2u ? 0u : p <= 4u ? 1u : 2u;
            if (nc[band] >= 96u) continue;
            struct run_offer *o = &cand[band][nc[band]];
            memset(o, 0, sizeof *o);
            o->cat = (uint8_t)cat; o->id = q->id; o->cost = (uint8_t)p; o->band = (uint8_t)band;
            if (!run_offer_fit(cat, q->id, now, plus, o)) continue;
            run_offer_label(o, q);
            o->w = RUN_W_NORMAL;
            if (cat == MOA_CAT_BACK && run_is_radar(q->id)) o->w = RUN_W_RADAR;
            if (cat == MOA_CAT_BACK && two_legged && run_back_stance(q->id)) {
                int two = 0;
                for (int k = 0; k < o->nput; k++) two |= run_stance_n(o->put_rec[k]) >= 2;
                o->w = two ? RUN_W_STANCE_TWO : RUN_W_STANCE;
            }
            if (cat == MOA_CAT_LEGS && run_two_legged(q->id) && stance_now >= 2) o->w /= 4u;
            nc[band]++;
        }
    /* Legs: one leg type for this screen, each type as likely (of those with
     * a part that fits), then its parts take all the legs' share. */
    uint64_t rng = run_prize_rng(win, salt);
    unsigned have = 0;
    for (unsigned band = 0; band < 3u; band++)
        for (unsigned i = 0; i < nc[band]; i++)
            if (cand[band][i].cat == MOA_CAT_LEGS) have |= 1u << run_leg_type(cand[band][i].id);
    int leg_t = -1;
    if (have) {
        unsigned k = moa_run_below(&rng, (uint32_t)__builtin_popcount(have));
        for (int t = 0; t < 4; t++) if ((have >> t) & 1u) { if (!k--) { leg_t = t; break; } }
    }
    unsigned nl = 0, nk = 0;                             /* over all bands: a type found only */
    for (unsigned band = 0; band < 3u; band++)            /* in some bands keeps legs' whole share */
        for (unsigned i = 0; i < nc[band]; i++)
            if (cand[band][i].cat == MOA_CAT_LEGS) { nl++; nk += run_leg_type(cand[band][i].id) == leg_t; }
    for (unsigned band = 0; band < 3u; band++) {
        for (unsigned i = 0; i < nc[band]; i++) {
            struct run_offer *o = &cand[band][i];
            if (o->cat != MOA_CAT_LEGS) continue;
            if (run_leg_type(o->id) != leg_t) { o->w = 0; continue; }
            o->w = (uint32_t)((uint64_t)o->w * nl / nk);
            if (!o->w) o->w = 1;
        }
    }
    unsigned used = 0;                                    /* categories already up */
    struct run_offer set;
    const int has_set = weapon_arms && run_set_roll(now, plus, &rng, &set);
    if (has_set) used |= 1u << MOA_CAT_ARMS | 1u << MOA_CAT_HAND | 1u << MOA_CAT_BLADE;
    for (unsigned band = 0; band < 3u; band++) {
        if (band == 2u && has_set) { run_offer[run_offer_n++] = set; break; }
        if (!nc[band]) continue;
        uint64_t fresh = 0, all = 0;
        for (unsigned i = 0; i < nc[band]; i++) {
            all += cand[band][i].w;
            if (!(used & (1u << cand[band][i].cat))) fresh += cand[band][i].w;
        }
        if (!all) continue;
        const int only_fresh = fresh != 0u;
        uint32_t pick = moa_run_below(&rng, (uint32_t)(only_fresh ? fresh : all));
        for (unsigned i = 0; i < nc[band]; i++) {
            if (only_fresh && (used & (1u << cand[band][i].cat))) continue;
            if (pick < cand[band][i].w) { run_offer[run_offer_n++] = cand[band][i]; used |= 1u << cand[band][i].cat; break; }
            pick -= cand[band][i].w;
        }
    }
    /* +1 P PER WIN: now and then as the expensive offer (not with an ARMS SET,
     * not in the last fights, where it could not pay its price back). */
    if (!has_set && run_state.run.income < RUN_INCOME_MAX && win + 1u + RUN_INCOME_COST < MOA_RUN_SLOTS - 1u &&
        moa_run_below(&rng, RUN_INCOME_ODDS) == 0u) {
        struct run_offer *o = NULL;
        for (int i = 0; i < run_offer_n; i++) if (run_offer[i].band == 2u) o = &run_offer[i];
        if (!o && run_offer_n < 3) o = &run_offer[run_offer_n++];
        if (o) {
            memset(o, 0, sizeof *o);
            o->income = 1; o->cat = 0xFFu; o->cost = RUN_INCOME_COST; o->band = 2; o->w = RUN_W_NORMAL;
            snprintf(o->name, sizeof o->name, "+1 P PER WIN");
            snprintf(o->category, sizeof o->category, "UPGRADE  EVERY WIN PAYS 1 P MORE");
            snprintf(o->cost_s, sizeof o->cost_s, "%u P", RUN_INCOME_COST);
        }
    }
}

/* The offers for the win's prize screen (after its re-roll, if used). */
static void run_prize_roll(unsigned win)
{
    run_offer_n = 0;
    if (!run_parts_read()) { run_log("prize: part tables could not be read; PASS only"); return; }
    run_prize_draw_offers(win, 0, NULL, 0);
    if (run_state.run.reroll_win == win + 1u) {
        uint8_t excl[3][2];
        const int n = run_offer_n;
        for (int i = 0; i < n; i++) { excl[i][0] = run_offer[i].cat; excl[i][1] = run_offer[i].id; }
        run_prize_draw_offers(win, 0x5245524F4C4Cull, excl, n);      /* "REROLL" */
        if (!run_offer_n) run_prize_draw_offers(win, 0x5245524F4C4Cull, NULL, 0);   /* nothing else fits */
    }
    const char *dir = getenv("PSX_MOA_MODELS_DIR");
    const int models = dir && dir[0] && moa_acv_has_part_models(dir);
    for (int i = 0; i < run_offer_n; i++) {
        run_offer_has_thumb[i] = models && !run_offer[i].income &&
                                 run_part_thumb(dir, run_offer[i].cat, run_offer[i].id, run_offer_thumb[i]);
        for (int k = 0; k < 2; k++)
            run_offer_has_xthumb[i][k] = models && k < run_offer[i].xn &&
                run_part_thumb(dir, run_offer[i].xcat[k], run_offer[i].xid[k], run_offer_xthumb[i][k]);
    }
    run_log("prize: %d offer(s)%s%s", run_offer_n, run_state.run.reroll_win == win + 1u ? " (re-rolled)" : "",
            models ? "" : " (no model install: no pictures)");
}

/* Part k of offer i's set (0 the arms, 1 the gun, 2 the blade) as its box
 * shows it. The number of parts (1 for a lone part). */
static int run_set_item(int i, int k, const char **category, const char **name, const uint32_t **thumb)
{
    const struct run_offer *o = &run_offer[i];
    if (k <= 0 || k > o->xn) {
        *category = o->category; *name = o->name;
        *thumb = run_offer_has_thumb[i] ? run_offer_thumb[i] : NULL;
    } else {
        *category = o->xcategory[k - 1]; *name = o->xname[k - 1];
        *thumb = run_offer_has_xthumb[i][k - 1] ? run_offer_xthumb[i][k - 1] : NULL;
    }
    return 1 + o->xn;
}

/* Where a cycling set picture is at frame t: showing part *k, the next
 * wiped in over `*wipe` rows (0 = none). 1 = mid-wipe or just finished one. */
static int run_set_anim(uint32_t t, int n, int *k, int *wipe)
{
    const uint32_t per = RUN_SET_HOLD + RUN_SET_WIPE, u = t % per;
    *k = (int)((t / per) % (uint32_t)n);
    *wipe = u < RUN_SET_HOLD ? 0 : (int)((u - RUN_SET_HOLD + 1u) * (uint32_t)RUN_THUMB / RUN_SET_WIPE);
    return u >= RUN_SET_HOLD || (u == 0u && t > 0u);
}

static int run_reroll_can(void)
{
    return run_state.run.reroll_win != run_prize_win + 1u && psx_mod_read_word(RUN_CREDITS) >= 1u;
}

static void run_prize_hide(void)
{
    (void)psx_mod_set_overlay_image(NULL, 0, 0);
    if (!run_prize_overlay) psx_mod_set_loading_screen(NULL, 0);
}

/* The screen, over the live game if the renderer can, else as the loading
 * picture. 0 = could not be drawn. */
static int run_prize_draw(void)
{
    const uint32_t credits = psx_mod_read_word(RUN_CREDITS);
    struct moa_screen_prize_row row[4];
    memset(row, 0, sizeof row);
    for (int i = 0; i < run_offer_n; i++) {
        const struct run_offer *o = &run_offer[i];
        row[i].tag = k_tier_tag[o->band];
        row[i].category = o->category;
        row[i].name = o->name;
        row[i].cost = o->cost_s;
        row[i].thumb = run_offer_has_thumb[i] ? run_offer_thumb[i] : NULL;
        row[i].locked = o->cost > credits;
        if (o->income) row[i].icon = "+1P";
        if (o->xn) {                                   /* a set: the box cycles through its parts */
            const int np = 1 + o->xn;
            int k, wipe;
            (void)run_set_anim(run_prize_frames, np, &k, &wipe);
            (void)run_set_item(i, k, &row[i].category, &row[i].name, &row[i].thumb);
            if (wipe) (void)run_set_item(i, (k + 1) % np, &row[i].category_next, &row[i].name_next, &row[i].thumb_next);
            row[i].wipe = wipe;
            row[i].dots = np;
            row[i].dot_on = wipe > RUN_THUMB / 2 ? (k + 1) % np : k;
            row[i].special = 1;
        }
    }
    row[run_offer_n].name = "PASS";
    row[run_offer_n].cost = "+1 P";
    const int rerolled = run_state.run.reroll_win == run_prize_win + 1u;
    char title[32], line[40], ask_title[48], ask_line[112];
    const unsigned win = run_prize_win + 1u;
    snprintf(title, sizeof title, "PRIZE  RANK %u", (unsigned)(MOA_RUN_SLOTS - (win < MOA_RUN_SLOTS ? win : MOA_RUN_SLOTS - 1u)));
    snprintf(line, sizeof line, "YOU HAVE %u P", (unsigned)credits);
    struct moa_screen_prize p;
    memset(&p, 0, sizeof p);
    p.title = title;
    p.line = line;
    p.row = row;
    p.count = run_offer_n + 1;
    p.cursor = run_prize_cur;
    p.footer = "ARROWS  CHOOSE     X  TAKE";
    p.alt_name = rerolled ? "RE-ROLLED" : "RE-ROLL";
    p.alt_cost = rerolled ? NULL : "1 P";
    p.alt_locked = !run_reroll_can();
    p.alt_on = run_prize_cur == run_offer_n && run_prize_alt;
    const char *ask[4];
    if (run_prize_stage == PZ_ASK && run_prize_cur == run_offer_n) {    /* the re-roll question */
        static const char *const k_reroll_ask[2] = { "RE-ROLL", "BACK" };
        p.ask = k_reroll_ask;
        p.ask_n = 2;
        p.ask_cursor = run_prize_ask_cur;
        p.ask_title = "RE-ROLL FOR 1 P";
        p.ask_line = "3 NEW PRIZES IN PLACE OF THESE.  ONE RE-ROLL PER WIN";
        p.footer = "UP / DOWN  CHOOSE     X  OK     O  BACK";
    } else if (run_prize_stage == PZ_ASK && run_prize_cur < run_offer_n) {
        const struct run_offer *o = &run_offer[run_prize_cur];
        for (int i = 0; i < run_prize_ask_n; i++)
            ask[i] = run_prize_ask_kind[i] == ASK_SAVE ? "SAVE FOR LATER" : run_prize_ask_kind[i] == ASK_BACK ? "BACK"
                   : o->income ? "BUY" : o->nput < 2 ? "EQUIP NOW" : o->put_slot[run_prize_ask_kind[i]] == 6 ? "EQUIP LEFT" : "EQUIP RIGHT";
        snprintf(ask_title, sizeof ask_title, "%s  FOR %s", o->xn ? "ARMS SET" : o->name, o->cost_s);
        if (o->income)
            snprintf(ask_line, sizeof ask_line, "EVERY WIN PAYS %u P FROM NOW ON (NOW %u P)",
                     2u + run_state.run.income, 1u + run_state.run.income);
        else if (o->xn)
            snprintf(ask_line, sizeof ask_line, "%s + %s + %s.  REPLACES %s (GONE FOR GOOD)", o->name, o->xname[0],
                     o->xname[1], o->put_lost[0]);
        else if (o->nput >= 2)
            snprintf(ask_line, sizeof ask_line, "LEFT REPLACES %s   RIGHT REPLACES %s", o->put_lost[0], o->put_lost[1]);
        else if (o->put_lost[0][0])
            snprintf(ask_line, sizeof ask_line, "EQUIP REPLACES  %s  (GONE FOR GOOD)", o->put_lost[0]);
        else
            snprintf(ask_line, sizeof ask_line, "EQUIP GOES INTO AN EMPTY SLOT");
        p.ask = ask;
        p.ask_n = run_prize_ask_n;
        p.ask_cursor = run_prize_ask_cur;
        p.ask_title = ask_title;
        p.ask_line = ask_line;
        p.footer = "UP / DOWN  CHOOSE     X  OK     O  BACK";
    }
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render_prize(ww, wh, &p, 0, &w, &h);
    if (!px) return 0;
    if (psx_mod_set_overlay_image(px, w, h)) { run_prize_overlay = 1; return 1; }
    run_prize_overlay = 0;
    psx_mod_set_loading_screen("ROGUE ARENA  Prize", -1);
    psx_mod_set_loading_image(px, w, h);
    return 1;
}

/* The win whose prize screen is owed, -1 for none. */
static int run_prize_due(void)
{
    if (!run_state_ok || run_state.run.over) return -1;
    return run_state.run.prize_wins < run_state.run.wins ? (int)run_state.run.prize_wins : -1;
}

static void run_prize_open(int win)
{
    if (!run_show_font()) return;
    run_prize_win = (unsigned)win;
    run_prize_roll((unsigned)win);
    run_prize_cur = 0;
    run_prize_alt = 0;
    while (run_prize_cur < run_offer_n && run_offer[run_prize_cur].cost > psx_mod_read_word(RUN_CREDITS))
        run_prize_cur++;                                  /* on the first offer that can be taken (else PASS) */
    run_prize_stage = PZ_CHOOSE;
    run_prize_armed = 0;
    run_prize_frames = 0;
    run_prize_held = 0xFFFFu;
    if (!run_prize_draw()) { run_prize_stage = PZ_OFF; run_log("prize: no picture"); return; }
    char all[160] = "";
    for (int i = 0; i < run_offer_n; i++)
        snprintf(all + strlen(all), sizeof all - strlen(all), "%s%s%s %s", i ? " / " : "", run_offer[i].name,
                 run_offer[i].xn ? " set" : "", run_offer[i].cost_s);
    run_log("prize (win %d): %s%sPASS, credits %u%s", win + 1, all, run_offer_n ? " / " : "",
            (unsigned)psx_mod_read_word(RUN_CREDITS), run_prize_overlay ? "" : " (loading picture)");
}

/* The win's prize is settled: booked in the run file at once, the card
 * catches up at the autosave. */
static void run_prize_done(const char *what)
{
    struct moa_run_progress *r = &run_state.run;
    r->prize_wins++;
    run_snap_take();
    (void)moa_run_save(run_file_path(), &run_state);
    run_save_pending = 1;
    run_prize_stage = PZ_OFF;
    run_prize_hide();
    run_log("prize (win %u): %s; credits %u, P earned %u, spent %u, saved parts %u", (unsigned)r->prize_wins, what,
            (unsigned)psx_mod_read_word(RUN_CREDITS), (unsigned)r->points, (unsigned)r->p_spent, (unsigned)r->saved_n);
}

static void run_prize_pass(void)
{
    struct moa_run_progress *r = &run_state.run;
    r->points = (uint8_t)(r->points + 1u);
    r->p_paid = (uint8_t)(r->p_paid + 1u);
    psx_mod_write_word(RUN_CREDITS, psx_mod_read_word(RUN_CREDITS) + 1u);
    psx_mod_counter_add("moa.run.prize_pass", 1);
    run_prize_done("PASS, +1 P");
}

/* Take offer `run_prize_cur`: equipped at place `put` (0/1), or saved (-1). */
static void run_prize_take(int put)
{
    const struct run_offer *o = &run_offer[run_prize_cur];
    struct moa_run_progress *r = &run_state.run;
    const uint32_t credits = psx_mod_read_word(RUN_CREDITS);
    if (o->cost > credits) return;
    psx_mod_write_word(RUN_CREDITS, credits - o->cost);
    r->p_spent = (uint16_t)(r->p_spent + o->cost);
    if (o->income) {
        r->income = (uint8_t)(r->income + 1u);
        psx_mod_counter_add("moa.run.prize_income", 1);
        char what[64];
        snprintf(what, sizeof what, "+1 P PER WIN for %u P (each win now pays %u P)", (unsigned)o->cost,
                 1u + r->income);
        run_prize_done(what);
        return;
    }
    const uint32_t ix = k_run_tables[o->cat].base + o->id;
    uint32_t xix[2] = { 0, 0 };
    for (int k = 0; k < o->xn; k++) xix[k] = k_run_tables[o->xcat[k]].base + o->xid[k];
    psx_mod_write_byte(RUN_OWNED + ix, (uint8_t)(psx_mod_read_byte(RUN_OWNED + ix) + 1u));
    for (int k = 0; k < o->xn; k++)
        psx_mod_write_byte(RUN_OWNED + xix[k], (uint8_t)(psx_mod_read_byte(RUN_OWNED + xix[k]) + 1u));
    char what[128];
    if (put >= 0) {
        uint8_t now[13];
        run_garage_rec(now);
        const uint8_t *rec = o->put_rec[put];
        for (int b = 0; b < 13; b++) {                 /* what came off is gone for good */
            if (b == 10 || b == 11 || rec[b] == now[b]) continue;
            const int was = run_rec_ix(now, b);
            if (was < 0) continue;
            const uint8_t have = psx_mod_read_byte(RUN_OWNED + (uint32_t)was);
            if (have) psx_mod_write_byte(RUN_OWNED + (uint32_t)was, (uint8_t)(have - 1u));
        }
        run_garage_set(rec);
        run_ac_refresh = 1;
        psx_mod_counter_add("moa.run.prize_equip", 1);
        snprintf(what, sizeof what, "%s%s equipped for %u P%s%s", o->name, o->xn ? " set" : "", (unsigned)o->cost,
                 o->put_lost[put][0] ? ", gone: " : "", o->put_lost[put]);
    } else {
        if (r->saved_n < MOA_RUN_SAVED_MAX) r->saved[r->saved_n++] = (uint8_t)ix;
        for (int k = 0; k < o->xn; k++)
            if (r->saved_n < MOA_RUN_SAVED_MAX) r->saved[r->saved_n++] = (uint8_t)xix[k];
        psx_mod_counter_add("moa.run.prize_save", 1);
        snprintf(what, sizeof what, "%s%s saved for later for %u P", o->name, o->xn ? " set" : "", (unsigned)o->cost);
    }
    run_prize_done(what);
}

/* RE-ROLL: 1 P, once per win's screen, booked in the run file at once (a
 * reopen shows the new offers and no second re-roll). */
static void run_prize_reroll(void)
{
    struct moa_run_progress *r = &run_state.run;
    const uint32_t credits = psx_mod_read_word(RUN_CREDITS);
    if (!run_reroll_can()) return;
    psx_mod_write_word(RUN_CREDITS, credits - 1u);
    r->p_spent = (uint16_t)(r->p_spent + 1u);
    r->reroll_win = (uint8_t)(run_prize_win + 1u);
    (void)moa_run_save(run_file_path(), &run_state);
    run_save_pending = 1;
    psx_mod_counter_add("moa.run.prize_reroll", 1);
    run_prize_roll(run_prize_win);
    run_prize_cur = 0;
    run_prize_alt = 0;
    while (run_prize_cur < run_offer_n && run_offer[run_prize_cur].cost > psx_mod_read_word(RUN_CREDITS))
        run_prize_cur++;
    run_prize_stage = PZ_CHOOSE;
    char all[160] = "";
    for (int i = 0; i < run_offer_n; i++)
        snprintf(all + strlen(all), sizeof all - strlen(all), "%s%s%s %s", i ? " / " : "", run_offer[i].name,
                 run_offer[i].xn ? " set" : "", run_offer[i].cost_s);
    run_log("prize (win %u): RE-ROLL for 1 P: %s; credits %u", run_prize_win + 1u, all,
            (unsigned)psx_mod_read_word(RUN_CREDITS));
}

static void run_prize_ask_open(void)
{
    const struct run_offer *o = &run_offer[run_prize_cur];
    run_prize_ask_n = 0;
    if (o->income) {                                   /* BUY / BACK */
        run_prize_ask_kind[run_prize_ask_n++] = ASK_EQUIP0;
        run_prize_ask_kind[run_prize_ask_n++] = ASK_BACK;
        run_prize_ask_cur = 0;
        run_prize_stage = PZ_ASK;
        return;
    }
    for (int i = 0; i < o->nput && i < 2; i++) run_prize_ask_kind[run_prize_ask_n++] = ASK_EQUIP0 + i;
    if (run_state.run.saved_n + 1u + o->xn <= MOA_RUN_SAVED_MAX) run_prize_ask_kind[run_prize_ask_n++] = ASK_SAVE;
    run_prize_ask_kind[run_prize_ask_n++] = ASK_BACK;
    run_prize_ask_cur = 0;
    run_prize_stage = PZ_ASK;
}

/* Per frame. */
static void run_prize_frame(void)
{
    if (run_prize_stage == PZ_OFF) {
        /* While a prize is due, log what it waits for (once per change), so
         * a late prize screen shows which condition held it back. */
        static const char *waits;
        const char *why =
            !run_state_ok || run_state.run.over ? NULL :
            !run_at_hub ? "not at the hub" : run_autopilot ? "boot autopilot" :
            run_fade ? "loading screen fade" : run_victory_hold ? "victory screen" :
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
    if (run_prize_stage == PZ_CHOOSE)                        /* a set's picture cycling */
        for (int i = 0; i < run_offer_n; i++) {
            int k, wipe;
            if (run_offer[i].xn && run_set_anim(run_prize_frames, 1 + run_offer[i].xn, &k, &wipe)) {
                (void)run_prize_draw();
                break;
            }
        }
    if (b == 0xFFFFu) run_prize_armed = 1;
    if (!run_prize_armed || run_prize_frames < 20u || !down) return;
    const int up = (down & RUN_PAD_UP) != 0, dn = (down & RUN_PAD_DOWN) != 0;
    if (run_prize_stage == PZ_ASK) {
        if (up || dn) {
            run_se(RUN_SE_CURSOR);
            run_prize_ask_cur = (run_prize_ask_cur + (up ? run_prize_ask_n - 1 : 1)) % run_prize_ask_n;
            (void)run_prize_draw();
        } else if (down & RUN_PAD_CIRCLE) {
            run_se(RUN_SE_BACK);
            run_prize_stage = PZ_CHOOSE;
            (void)run_prize_draw();
        } else if ((down & (RUN_PAD_X | RUN_PAD_START)) && run_prize_cur == run_offer_n) {
            if (run_prize_ask_cur == 0) { run_se(RUN_SE_OK); run_prize_reroll(); }
            else { run_se(RUN_SE_BACK); run_prize_stage = PZ_CHOOSE; }
            (void)run_prize_draw();
        } else if (down & (RUN_PAD_X | RUN_PAD_START)) {
            const int k = run_prize_ask_kind[run_prize_ask_cur];
            if (k == ASK_BACK) {
                run_se(RUN_SE_BACK);
                run_prize_stage = PZ_CHOOSE;
                (void)run_prize_draw();
                return;
            }
            run_se(RUN_SE_OK);
            run_prize_take(k == ASK_SAVE ? -1 : k - ASK_EQUIP0);
        }
        return;
    }
    const int rows = run_offer_n + 1;
    const int lr = (down & (RUN_PAD_LEFT | RUN_PAD_RIGHT)) != 0;
    if (up || dn) {
        run_se(RUN_SE_CURSOR);
        run_prize_cur = (run_prize_cur + (up ? rows - 1 : 1)) % rows;
        (void)run_prize_draw();
    } else if (lr && run_prize_cur == run_offer_n) {        /* the bottom row: RE-ROLL | PASS */
        run_se(RUN_SE_CURSOR);
        run_prize_alt = (down & RUN_PAD_LEFT) != 0;
        (void)run_prize_draw();
    } else if (down & (RUN_PAD_X | RUN_PAD_START)) {
        if (run_prize_cur == run_offer_n && run_prize_alt) {
            if (!run_reroll_can()) { run_se(RUN_SE_BACK); return; }   /* used, or no P */
            run_se(RUN_SE_OK);
            run_prize_ask_n = 2;
            run_prize_ask_cur = 0;
            run_prize_stage = PZ_ASK;
            (void)run_prize_draw();
        } else if (run_prize_cur == run_offer_n) {
            run_se(RUN_SE_OK);
            run_prize_pass();
        } else if (run_offer[run_prize_cur].cost > psx_mod_read_word(RUN_CREDITS)) {
            run_se(RUN_SE_BACK);                           /* not enough P */
        } else {
            run_se(RUN_SE_OK);
            run_prize_ask_open();
            (void)run_prize_draw();
        }
    }
}

static void run_maybe_return(void);

static void run_hub_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_HUB_RESUME) return;
    if (run_watch_phase == 2) run_watch(0);       /* back at the hub */
    if (run_cover == 1) { run_cover = 2; run_cover_drawn = 0; run_cover_idle = 0; }
    if (run_hx_next >= 2) run_hx_fail("next battle: back at the hub, the fight did not start");
    run_replay_over("hub");
    if (run_state_ok && run_state.run.over) { run_loss_wait = 0; run_maybe_return(); return; }
    run_reach_hub("moa.run.hub_reached");
    run_prize_where = 1;
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
    /* Save once the game's payout state has run (it now pays 0; the win's
     * 1 P is already in the credits), or after about 5 s of arena screen. */
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
    if (ok & 0xFFu) {                       /* the card now holds all P paid and spent, and the garage */
        run_state.run.p_card = run_state.run.p_paid;
        run_state.run.p_spent_card = run_state.run.p_spent;
        run_state.run.snap_pending = 0;
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
    /* RETRY ARENA / NEW ARENA: the launcher only carries out the pick (the
     * ".next" file) and starts the game again, so it runs hidden and the game
     * window stays open; QUIT TO LAUNCHER shows it as before. */
    if (run_end_choice == 0 || run_end_choice == 1) {
        run_log("loss: next run, same window");
        psx_mod_request_return_to_launcher_quiet();
    } else {
        run_log("loss: back to the launcher");
        psx_mod_request_return_to_launcher();
    }
}

/* A new win over arena 0 entry `who`, booked once: into the run file at once
 * (from the fight as soon as it shows the win, so quitting or a crash before
 * the arena screen can't lose it). The 1 P waits for the arena screen
 * (run_result_entry), the prize screen too: the part tables are not in
 * memory during a fight. 1 = newly booked. */
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
    run_victory_who = who;                               /* the victory screen after the fight */
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

/* A fight's result in the run's arena: a new win pays 1 P, a loss ends the
 * run (the file keeps it for Retry) and hands back to the launcher. */
static void run_result_entry(struct CPUState *cpu, uint32_t address)
{
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_RESULT) return;
    if (run_watch_phase == 2) run_watch(0);       /* back at the arena screen */
    if (run_cover == 1) { run_cover = 2; run_cover_drawn = 0; run_cover_idle = 0; }
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
        run_result_ran = 1;
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
    /* This win's 1 P, and any other P not yet in the credits (a win booked in
     * a fight that never reached this screen), go straight into the credits:
     * the game's payout pays 0, so its money window never shows. Added, never
     * set: the credits are what shopping left. */
    run_win_screen = 0;
    struct moa_run_progress *r = &run_state.run;
    const uint32_t owed = r->points > r->p_paid ? (uint32_t)(r->points - r->p_paid) : 1u;
    const uint32_t credits = psx_mod_read_word(RUN_CREDITS);
    psx_mod_write_word(RUN_CREDITS, credits + owed);
    r->p_paid = r->points;
    (void)moa_run_save(run_file_path(), &run_state);
    run_pay = 1;
    run_pay_due = 1;
    run_pay_wait = 0;
    run_log("win: credits %u + %u (P earned %u)", (unsigned)credits, (unsigned)owed,
            (unsigned)r->points);
    run_save_pending = 1;                                 /* the prize screen follows (run_prize_frame) */
}

/* The payout after a run fight. The win's 1 P is already in the credits
 * (the result hook), so the game's payout pays 0: money 0 skips its "C"
 * window (docs/mapping/arena-prizes.md), and the run's records carry no part
 * prize, so nothing shows. run_pay still says whether this was a new win. */
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
    psx_mod_write_word(tab + 8u * who, 0u);
    run_pay_due = 0;
    run_log("payout: window skipped, new win %u (stock %u), credits %u", (unsigned)run_pay, (unsigned)was,
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
    run_vs_fight_runs();
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
    return run_watch_phase == 1 ? RUN_SLOW(RUN_WATCH_LOAD) : run_watch_phase == 3 ? RUN_WATCH_IDLE
                                                                : RUN_SLOW(RUN_WATCH_FRAMES);
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

/* Any Rogue Arena screen over the game (prize, loss, hub overlay): the game
 * sees no buttons. Called from the frame hook and from the game's own
 * controller sampling (menus and fights), so no press slips through between
 * the two. */
static int run_overlay_up(void)
{
    /* A booked loss locks the controls at once: the RUN OVER screen comes
     * RUN_LOSS_FRAMES later, and the game must not take presses meanwhile. */
    return run_prize_stage || run_end_stage || run_hx || run_cover ||
           (run_boot_black && !run_autopilot) || (run_state_ok && run_state.run.over);
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
                                                      "UP / DOWN  CHOOSE     X  OK", 0, &w, &h);
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
    run_end_choice = pick;
    (void)psx_mod_set_overlay_image(NULL, 0, 0);
    psx_mod_set_loading_screen(pick == 2 ? "ROGUE ARENA  Back to the launcher"
                                         : "ROGUE ARENA  Starting the next run", -1);
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
        run_se(RUN_SE_CURSOR);
        run_end_cur = (run_end_cur + ((down & RUN_PAD_UP) ? 2 : 1)) % 3;
        (void)run_end_draw();
    } else if (down & (RUN_PAD_X | RUN_PAD_START)) {
        run_se(RUN_SE_OK);
        run_end_pick(run_end_cur);
    }
}

/* ---- overlay hub (step 1, claude/overlay-hub-plan.md) ---------------------------
 *
 * Whenever the hub waits for the player (its input state 0x80055848 runs),
 * a list opens over it: the balance, the next opponent, then NEXT BATTLE,
 * STAGE (a scrollable list of stages, remembered in <run>.stage) and GARAGE
 * (the game's own screen for now; the Shop row went with the restocks). O leaves the game's own hub
 * until the player goes into one of its screens. The game sees no buttons
 * while the list is up.
 *
 * It also opens over the run's arena list (where the game lands after each
 * fight) once the result, payout and prize are done (test 6,
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
 * GARAGE from the list: O there (back to the hub), then the hub hook
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
enum { HX_OFF = 0, HX_MENU, HX_STAGES, HX_NEXT, HX_BUSY, HX_SWAP, HX_SWAP_ASK };
enum { HXR_NEXT = 0, HXR_SWAP, HXR_STAGE, HXR_GARAGE, HXR_COUNT };   /* SHOP gone: parts come from the prize screen */
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

static int run_hx_draw(void);
/* ---- SWAP PARTS (hub overlay; design "Saved parts and swapping") ---------------
 *
 * The parts saved for later on the prize screen (run file saved[], owned-
 * table indices), three at a time in the prize screen's look, then BACK.
 * A part that fits the AC as it is now (run_offer_fit: the game's fitting
 * rules and checks, nothing knocked off but the part it replaces) can be
 * equipped: the part it replaces is gone for good (owned count down), the
 * saved one leaves the list. One that does not fit is dimmed, with why. */
#define RUN_SW_ROWS 3
static int run_sw_n, run_sw_cur, run_sw_top, run_sw_ask_cur, run_sw_ask_n;
static int run_sw_ask_put[3];                  /* a place (0/1), -1 BACK */
static struct run_offer run_sw_part[MOA_RUN_SAVED_MAX];
static int run_sw_fits[MOA_RUN_SAVED_MAX];
static char run_sw_why[MOA_RUN_SAVED_MAX][40];
static uint32_t run_sw_thumb[RUN_SW_ROWS][RUN_THUMB * RUN_THUMB];
static int run_sw_thumb_of[RUN_SW_ROWS] = { -1, -1, -1 };   /* owned index drawn there, -1 none */
static int run_sw_thumb_ok[RUN_SW_ROWS];

/* An owned-table index to its part (category, id). 1 = found. */
static int run_ix_part(int ix, int *cat, uint8_t *id)
{
    int best = -1;
    for (int c = 0; c < MOA_CAT__COUNT; c++)
        if ((int)k_run_tables[c].base <= ix && (best < 0 || k_run_tables[c].base > k_run_tables[best].base)) best = c;
    if (best < 0 || ix - (int)k_run_tables[best].base > 255) return 0;
    *cat = best;
    *id = (uint8_t)(ix - (int)k_run_tables[best].base);
    return 1;
}

/* Why (cat, id) can't go on `now`, in a few words. */
static void run_sw_reason(int cat, uint8_t id, const uint8_t now[13], int plus, char *out, size_t cap)
{
    static const int8_t k_slot[MOA_CAT__COUNT] = { 1, 0, 3, 2, 4, 6, 5, 12, 8, 9 };
    if ((cat == MOA_CAT_HAND || cat == MOA_CAT_BLADE) && now[2] != 255u && now[2] >= 12u) {
        snprintf(out, cap, "NEEDS REGULAR ARMS");
        return;
    }
    if (cat == MOA_CAT_BOOST && now[3] != 255u && now[3] >= 27u) {
        snprintf(out, cap, "CATERPILLARS TAKE NO BOOSTER");
        return;
    }
    uint8_t rec[13];
    memcpy(rec, now, 13);
    rec[k_slot[cat]] = id;
    moa_build_fit(rec);
    struct moa_build_check c;
    moa_build_check(&run_parts, rec, plus, &c);
    snprintf(out, cap, "%s", c.leg_over ? "TOO HEAVY FOR THE LEGS" : c.arm_over ? "TOO HEAVY FOR THE ARMS"
                            : c.energy_over ? "NOT ENOUGH ENERGY" : c.slots_over ? "NO OPTION SLOTS LEFT"
                            : c.missing_booster ? "NEEDS A BOOSTER" : "WON'T FIT THIS AC");
}

/* The list against the AC as it is now. 0 = the part tables are not in memory. */
static int run_sw_prepare(void)
{
    if (!run_parts_read()) return 0;
    uint8_t now[13];
    run_garage_rec(now);
    const int plus = run_plus_on();
    const struct moa_run_progress *r = &run_state.run;
    run_sw_n = 0;
    for (unsigned i = 0; i < r->saved_n && i < MOA_RUN_SAVED_MAX; i++) {
        struct run_offer *o = &run_sw_part[run_sw_n];
        int cat;
        uint8_t id;
        memset(o, 0, sizeof *o);
        if (!run_ix_part(r->saved[i], &cat, &id)) continue;
        const struct moa_part *q = moa_parts_find(&run_parts, cat, id);
        if (!q) continue;
        o->cat = (uint8_t)cat;
        o->id = id;
        run_offer_label(o, q);
        run_sw_fits[run_sw_n] = run_offer_fit(cat, id, now, plus, o);
        run_sw_why[run_sw_n][0] = 0;
        if (!run_sw_fits[run_sw_n]) run_sw_reason(cat, id, now, plus, run_sw_why[run_sw_n], sizeof run_sw_why[0]);
        run_sw_n++;
    }
    if (run_sw_cur > run_sw_n) run_sw_cur = run_sw_n;
    if (run_sw_top > run_sw_cur) run_sw_top = run_sw_cur;
    if (run_sw_cur < run_sw_n && run_sw_cur >= run_sw_top + RUN_SW_ROWS) run_sw_top = run_sw_cur - RUN_SW_ROWS + 1;
    if (run_sw_top > run_sw_n - RUN_SW_ROWS) run_sw_top = run_sw_n > RUN_SW_ROWS ? run_sw_n - RUN_SW_ROWS : 0;
    return 1;
}

static int run_sw_draw(void)
{
    const char *mdir = getenv("PSX_MOA_MODELS_DIR");
    const int models = mdir && mdir[0] && moa_acv_has_part_models(mdir);
    struct moa_screen_prize_row row[RUN_SW_ROWS + 1];
    memset(row, 0, sizeof row);
    char tag[RUN_SW_ROWS][40];
    int k = 0;
    for (; k < RUN_SW_ROWS && run_sw_top + k < run_sw_n; k++) {
        const int i = run_sw_top + k;
        const struct run_offer *o = &run_sw_part[i];
        const int ix = (int)k_run_tables[o->cat].base + o->id;
        if (run_sw_thumb_of[k] != ix) {            /* the window moved: draw that part */
            run_sw_thumb_ok[k] = models && run_part_thumb(mdir, o->cat, o->id, run_sw_thumb[k]);
            run_sw_thumb_of[k] = ix;
        }
        /* Under the name: what it replaces (the first part, +n more). */
        if (!run_sw_fits[i]) tag[k][0] = 0;
        else if (o->nput >= 2) snprintf(tag[k], sizeof tag[k], "LEFT OR RIGHT SLOT");
        else if (!o->put_lost[0][0]) snprintf(tag[k], sizeof tag[k], "INTO AN EMPTY SLOT");
        else {
            int more = 0;
            const char *c = strchr(o->put_lost[0], ',');
            for (const char *q = o->put_lost[0]; *q; q++) more += *q == ',';
            snprintf(tag[k], sizeof tag[k], "REPLACES %.*s", c ? (int)(c - o->put_lost[0]) : 18, o->put_lost[0]);
            if (more) snprintf(tag[k] + strlen(tag[k]), sizeof tag[k] - strlen(tag[k]), " +%d", more);
        }
        row[k].tag = tag[k];
        row[k].category = o->category;
        row[k].name = o->name;
        row[k].cost = run_sw_fits[i] ? "EQUIP" : "NO FIT";
        row[k].thumb = run_sw_thumb_ok[k] ? run_sw_thumb[k] : NULL;
        row[k].locked = !run_sw_fits[i];
        row[k].locked_why = run_sw_fits[i] ? NULL : run_sw_why[i];
    }
    row[k].name = "BACK";
    char line[64], ask_title[48], ask_line[112];
    const char *ask[3];
    if (!run_sw_n) snprintf(line, sizeof line, "NO SAVED PARTS");
    else snprintf(line, sizeof line, "SAVED PARTS  %d OF %d", run_sw_cur < run_sw_n ? run_sw_cur + 1 : run_sw_n, run_sw_n);
    struct moa_screen_prize p;
    memset(&p, 0, sizeof p);
    p.title = "SWAP PARTS";
    p.line = line;
    p.row = row;
    p.count = k + 1;
    p.cursor = run_sw_cur < run_sw_n ? run_sw_cur - run_sw_top : k;
    p.footer = run_sw_n > RUN_SW_ROWS ? "UP / DOWN  CHOOSE (MORE BELOW)     X  EQUIP     O  BACK"
                                      : "UP / DOWN  CHOOSE     X  EQUIP     O  BACK";
    if (run_hx == HX_SWAP_ASK && run_sw_cur < run_sw_n) {
        const struct run_offer *o = &run_sw_part[run_sw_cur];
        for (int i = 0; i < run_sw_ask_n; i++)
            ask[i] = run_sw_ask_put[i] < 0 ? "BACK" : o->nput < 2 ? "EQUIP NOW"
                   : o->put_slot[run_sw_ask_put[i]] == 6 ? "EQUIP LEFT" : "EQUIP RIGHT";
        snprintf(ask_title, sizeof ask_title, "EQUIP %s", o->name);
        if (o->nput >= 2)
            snprintf(ask_line, sizeof ask_line, "LEFT REPLACES %s   RIGHT REPLACES %s", o->put_lost[0], o->put_lost[1]);
        else if (o->put_lost[0][0])
            snprintf(ask_line, sizeof ask_line, "REPLACES  %s  (GONE FOR GOOD)", o->put_lost[0]);
        else
            snprintf(ask_line, sizeof ask_line, "GOES INTO AN EMPTY SLOT");
        p.ask = ask;
        p.ask_n = run_sw_ask_n;
        p.ask_cursor = run_sw_ask_cur;
        p.ask_title = ask_title;
        p.ask_line = ask_line;
        p.footer = "UP / DOWN  CHOOSE     X  OK     O  BACK";
    }
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render_prize(ww, wh, &p, 0, &w, &h);
    return px && psx_mod_set_overlay_image(px, w, h);
}

/* Equip saved part `run_sw_cur` at place `put`. */
static void run_sw_equip(int put)
{
    struct moa_run_progress *r = &run_state.run;
    const struct run_offer *o = &run_sw_part[run_sw_cur];
    if (put < 0 || put >= o->nput) return;
    const int ix = (int)k_run_tables[o->cat].base + o->id;
    uint8_t now[13];
    run_garage_rec(now);
    const uint8_t *rec = o->put_rec[put];
    for (int b = 0; b < 13; b++) {                     /* what came off is gone for good */
        if (b == 10 || b == 11 || rec[b] == now[b]) continue;
        const int was = run_rec_ix(now, b);
        if (was < 0) continue;
        const uint8_t have = psx_mod_read_byte(RUN_OWNED + (uint32_t)was);
        if (have) psx_mod_write_byte(RUN_OWNED + (uint32_t)was, (uint8_t)(have - 1u));
    }
    run_garage_set(rec);
    for (unsigned i = 0; i < r->saved_n; i++)          /* off the saved list (the first copy) */
        if (r->saved[i] == (uint8_t)ix) {
            for (unsigned j = i + 1u; j < r->saved_n; j++) r->saved[j - 1u] = r->saved[j];
            r->saved_n--;
            break;
        }
    run_ac_refresh = 1;
    run_snap_take();
    (void)moa_run_save(run_file_path(), &run_state);
    run_save_pending = 1;
    psx_mod_counter_add("moa.run.swap_equip", 1);
    run_log("swap parts: %s equipped%s%s; %u saved left", o->name, o->put_lost[put][0] ? ", gone: " : "",
            o->put_lost[put], (unsigned)r->saved_n);
}

static void run_sw_open(void)
{
    if (!run_sw_prepare()) { run_se(RUN_SE_BACK); run_log("swap parts: the part tables are not in memory"); return; }
    run_sw_cur = run_sw_top = 0;
    while (run_sw_cur < run_sw_n && !run_sw_fits[run_sw_cur]) run_sw_cur++;   /* the first that fits (else BACK) */
    if (run_sw_cur >= RUN_SW_ROWS) run_sw_top = run_sw_cur - RUN_SW_ROWS + 1;
    if (run_sw_cur == run_sw_n) run_sw_top = 0;
    for (int k = 0; k < RUN_SW_ROWS; k++) run_sw_thumb_of[k] = -1;
    run_hx = HX_SWAP;
    run_log("swap parts: open, %d saved", run_sw_n);
}

/* Keys on the SWAP PARTS screens. */
static void run_sw_input(uint16_t down)
{
    const int up = (down & RUN_PAD_UP) != 0, dn = (down & RUN_PAD_DOWN) != 0;
    const int ok = (down & (RUN_PAD_X | RUN_PAD_START)) != 0, back = (down & RUN_PAD_CIRCLE) != 0;
    if (run_hx == HX_SWAP_ASK) {
        if (up || dn) {
            run_se(RUN_SE_CURSOR);
            run_sw_ask_cur = (run_sw_ask_cur + (up ? run_sw_ask_n - 1 : 1)) % run_sw_ask_n;
        } else if (back || (ok && run_sw_ask_put[run_sw_ask_cur] < 0)) {
            run_se(RUN_SE_BACK);
            run_hx = HX_SWAP;
        } else if (ok) {
            run_se(RUN_SE_OK);
            run_sw_equip(run_sw_ask_put[run_sw_ask_cur]);
            run_hx = HX_SWAP;
            (void)run_sw_prepare();                    /* the AC changed: what fits now */
            for (int k = 0; k < RUN_SW_ROWS; k++) run_sw_thumb_of[k] = -1;
        } else return;
        (void)run_hx_draw();
        return;
    }
    const int rows = run_sw_n + 1;                     /* the parts, then BACK */
    if (up || dn) {
        run_se(RUN_SE_CURSOR);
        run_sw_cur = (run_sw_cur + (up ? rows - 1 : 1)) % rows;
        if (run_sw_cur < run_sw_n) {
            if (run_sw_cur < run_sw_top) run_sw_top = run_sw_cur;
            if (run_sw_cur >= run_sw_top + RUN_SW_ROWS) run_sw_top = run_sw_cur - RUN_SW_ROWS + 1;
        }
    } else if (back || (ok && run_sw_cur == run_sw_n)) {
        run_se(RUN_SE_BACK);
        run_hx = HX_MENU;
        run_log("swap parts: closed");
    } else if (ok && !run_sw_fits[run_sw_cur]) {
        run_se(RUN_SE_BACK);                           /* doesn't fit */
        return;
    } else if (ok) {
        run_se(RUN_SE_OK);
        const struct run_offer *o = &run_sw_part[run_sw_cur];
        run_sw_ask_n = 0;
        for (int i = 0; i < o->nput && i < 2; i++) run_sw_ask_put[run_sw_ask_n++] = i;
        run_sw_ask_put[run_sw_ask_n++] = -1;
        run_sw_ask_cur = 0;
        run_hx = HX_SWAP_ASK;
    } else return;
    (void)run_hx_draw();
}

static int run_hx_draw(void)
{
    if (run_vs) return 1;                              /* the VS screen has the picture */
    if (run_hx == HX_SWAP || run_hx == HX_SWAP_ASK) {
        if (!run_sw_draw()) return 0;
        run_hx_overlay = 1;
        return 1;
    }
    char title[40], bal[40], stage_v[27], swap_v[16];
    const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
    struct moa_screen_list l;
    memset(&l, 0, sizeof l);
    l.title = title;
    l.marked = -1;
    static const char *const k_rows[HXR_COUNT] = { "NEXT BATTLE", "SWAP PARTS", "STAGE", "GARAGE" };
    const char *values[HXR_COUNT] = { NULL, swap_v, stage_v, NULL };
    if (run_state.run.saved_n) snprintf(swap_v, sizeof swap_v, "%u SAVED", (unsigned)run_state.run.saved_n);
    else snprintf(swap_v, sizeof swap_v, "NONE");
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
        l.info[2] = run_hx_next ? "STARTING THE FIGHT..." : "LOADING THE FIGHT...";
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
    const uint32_t *px = moa_screen_render_list(ww, wh, &l, 0, &w, &h);
    if (!px || !psx_mod_set_overlay_image(px, w, h)) return 0;
    run_hx_overlay = 1;
    if (run_cover) {                                   /* the hub overlay takes over */
        run_cover = run_cover_drawn = 0;
        run_log("cover: the hub overlay takes over (%u frames)", (unsigned)run_cover_frames);
    }
    return 1;
}

static void run_hx_close(void)
{
    if (run_hx_overlay && !run_vs) (void)psx_mod_set_overlay_image(NULL, 0, 0);   /* the VS screen keeps it */
    run_hx_overlay = 0;
    run_hx = HX_OFF;
}

static void run_hx_fail(const char *why)
{
    run_log("hub overlay: %s", why);
    psx_mod_counter_add("moa.run.hub_overlay_failed", 1);
    run_hx_go_entry = -1;
    run_hx_next = 0;
    run_vs_close("next battle failed");
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
    run_vs_open();                                    /* the VS screen over all of it */
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
    if (++run_hx_next_tries > RUN_SLOW(6u)) { run_hx_fail("next battle: too many X presses on the arena screens and no fight"); return 0; }
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
        /* The picture stays up until the game's own loading screen
         * (run_loading_screen_entry), or the fight itself runs. */
        if (run_hx == HX_NEXT) (void)run_hx_draw();
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
    if (run_hx_go_entry >= 0 && !run_hx_next && ++run_hx_go_frames > RUN_SLOW(RUN_HX_GO_GIVE_UP))
        run_hx_fail("the hub did not open the pick");
    if (run_hx_next && ++run_hx_next_frames > RUN_SLOW(RUN_HX_NEXT_GIVE_UP))
        run_hx_fail(run_disc_slow == 1 ? "next battle: the fight did not start in 60 s"
                                       : "next battle: the fight did not start in 15 s");
    if (run_hx == HX_OFF) {
        if (run_victory_hold) return;                                       /* until X on the victory screen */
        if (run_hx_go_entry >= 0 || run_hx_next || run_hx_off) return;
        if (run_hx_hub_seen > 2u && run_hx_list_seen > 2u) return;          /* neither screen is waiting */
        if (run_win_screen || run_pay_due) return;                          /* the result still to pay */
        if (!run_state_ok || run_state.run.over || !run_at_hub || run_autopilot || run_fade) return;
        if (run_prize_stage || run_prize_due() >= 0 || run_end_stage) return;
        if (psx_mod_read_byte(RUN_FIGHT_DONE)) return;
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
    if (run_hx == HX_SWAP || run_hx == HX_SWAP_ASK) { run_sw_input(down); return; }
    const int up = (down & RUN_PAD_UP) != 0, dn = (down & RUN_PAD_DOWN) != 0;
    if (run_hx == HX_STAGES) {
        if (up || dn) {
            run_se(RUN_SE_CURSOR);
            run_hx_scur = (run_hx_scur + (up ? RUN_HX_STAGES - 1 : 1)) % RUN_HX_STAGES;
            if (run_hx_scur < run_hx_stop) run_hx_stop = run_hx_scur;
            if (run_hx_scur >= run_hx_stop + 7) run_hx_stop = run_hx_scur - 6;
            (void)run_hx_draw();
        } else if (down & RUN_PAD_X) {
            run_se(RUN_SE_OK);
            run_hx_stage_i = run_hx_scur;
            run_hx_stage_save();
            run_log("hub overlay: stage %s (kept for the next fights)", k_hx_stage[run_hx_stage_i].name);
            run_hx = HX_MENU;
            (void)run_hx_draw();
        } else if (down & RUN_PAD_CIRCLE) {
            run_se(RUN_SE_BACK);
            run_hx = HX_MENU;
            (void)run_hx_draw();
        }
        return;
    }
    if (up || dn) {
        run_se(RUN_SE_CURSOR);
        run_hx_cur = (run_hx_cur + (up ? HXR_COUNT - 1 : 1)) % HXR_COUNT;
        (void)run_hx_draw();
    } else if (down & (RUN_PAD_X | RUN_PAD_START)) {
        run_se(RUN_SE_OK);
        switch (run_hx_cur) {
        case HXR_NEXT:
            if (run_state.run.wins + 1u >= MOA_RUN_SLOTS) {      /* rank 1: pick a refight in the game's list */
                run_hx_open_entry(RUN_HUB_ARENA, "arena (rank 1: the game's list)", "ARENA");
                break;
            }
            run_hx_start_next();
            break;
        case HXR_SWAP:
            if (!run_state.run.saved_n) { run_se(RUN_SE_BACK); break; }   /* nothing saved */
            run_sw_open();
            (void)run_hx_draw();
            break;
        case HXR_STAGE:
            run_hx = HX_STAGES;
            run_hx_scur = run_hx_stage_i < 0 ? 0 : run_hx_stage_i;
            run_hx_stop = run_hx_scur > RUN_HX_STAGES - 7 ? RUN_HX_STAGES - 7 : run_hx_scur;
            (void)run_hx_draw();
            break;
        case HXR_GARAGE: run_hx_open_entry(0, "garage (the game's)", "GARAGE"); break;
        }
    } else if (down & RUN_PAD_CIRCLE) {
        run_se(RUN_SE_BACK);
        run_hx_close();
        run_hx_off = 1;
        run_log("hub overlay: closed, the game's hub");
    }
}

/* ---- victory screen (the cover after a win) ----------------------------------- */
/* Band colours by belt (moa_hub_rank_tile_index order). */
static const uint32_t k_belt_band[11] = {
    0xE1E1DCu, 0xECA824u, 0xEC701Cu, 0x40A848u, 0x3070D0u, 0x8048B8u,
    0xCC2828u, 0x8C542Cu, 0x36363Cu, 0x8C1C1Cu, 0xB08C28u,
};
/* Victory lines, grouped by belt (the same bands as the hub backgrounds,
 * moa_hub_rank_tile_index: 0 white 50-46 ... 8 black 10-6, 9 black-red 5-2,
 * 10 black-gold 1), so the words grow with the climb. '#' is replaced by the
 * ranks left to the top. Within a belt a run never repeats a line. */
#define VL_N 8
static const char *const k_victory_belt[11][VL_N] = {
    {   /* white: the bottom */
        "EVERYONE STARTS AT THE BOTTOM.",
        "NOT BAD FOR A NEWCOMER.",
        "THE CROWD BARELY NOTICED. YET.",
        "A FIRST STEP. THERE ARE MANY MORE.",
        "THE BOOKIES JUST CHANGED YOUR ODDS.",
        "THE LOW RANKS ARE CROWDED. MAKE ROOM.",
        "YOUR AC HAS A FEW SCRATCHES NOW. GOOD.",
        "THE ARENA KEEPS A RECORD OF EVERYTHING.",
    }, {   /* yellow */
        "THE ARENA IS STARTING TO KNOW YOUR NAME.",
        "SOMEONE IN THE STANDS IS CHEERING.",
        "THE ROOKIES ARE WATCHING YOU NOW.",
        "A FEW SPONSORS ARE TAKING NOTES.",
        "YOU ARE NOT A FLUKE.",
        "THAT ONE WILL NEED A NEW AC.",
        "ONE STEP CLOSER TO THE TOP.",
        "THEY NEVER SAW IT COMING.",
    }, {   /* orange */
        "YOUR SPONSORS ARE WATCHING.",
        "THE CROWD WANTS MORE.",
        "THE BETTING BOARD HAS YOUR NAME ON IT.",
        "THE BOTTOM OF THE BOARD IS BEHIND YOU.",
        "CLEAN WORK, RAVEN.",
        "THE MECHANICS ARE TALKING ABOUT YOUR AC.",
        "YOUR NAME IS ON THE BIG SCREEN NOW.",
        "THEY WILL STUDY THIS FIGHT.",
    }, {   /* green */
        "THE ARENA HAS STOPPED LAUGHING.",
        "PEOPLE ARE BETTING ON YOU NOW.",
        "A CONTENDER, NOT A NEWCOMER.",
        "THE UPPER RANKS JUST FELT A CHILL.",
        "YOUR REPLAYS ARE GETTING VIEWS.",
        "THAT WAS NOT LUCK. EVERYONE SAW IT.",
        "THE GARAGE CREWS KNOW YOUR AC BY SOUND.",
        "THEY WILL REMEMBER THIS ONE.",
    }, {   /* blue */
        "THE CROWD CHANTS YOUR NAME.",
        "THE TOP 25 IS IN SIGHT.",
        "VETERANS ARE CHECKING YOUR RECORD.",
        "A REAL RAVEN NOW.",
        "THE PRESS WANTS AN INTERVIEW.",
        "YOU FIGHT LIKE YOU WERE BORN IN AN AC.",
        "THE RANKERS ABOVE YOU ARE NERVOUS.",
        "ANOTHER NAME CROSSED OFF THE BOARD.",
    }, {   /* purple: the top half */
        "HALFWAY UP. THE AIR IS GETTING THIN.",
        "THE TOP HALF BELONGS TO YOU NOW.",
        "ONLY THE SERIOUS RAVENS ARE LEFT.",
        "SPONSORS ARE FIGHTING OVER YOU.",
        "KIDS PAINT THEIR TOY ACS LIKE YOURS.",
        "THAT WAS A VETERAN. NOW IT IS SCRAP.",
        "THE TOP 20 HAS BEEN WARNED.",
        "THE BOARD MOVED YOUR NAME UP AGAIN.",
    }, {   /* red: the top 20 */
        "THE TOP 20. FEW EVER GET THIS FAR.",
        "EVERY FIGHT FROM HERE IS A HEADLINE.",
        "THE ELITE ARE WATCHING YOUR REPLAYS.",
        "NO ONE CALLS YOU A NEWCOMER NOW.",
        "THE CROWD IS ON ITS FEET.",
        "THAT AC WAS BUILT TO BEAT YOU. IT FAILED.",
        "THE RANKS ABOVE ARE RUNNING OUT.",
        "A NAME THE ARENA WILL NOT FORGET.",
    }, {   /* brown: the top 15 */
        "THE TOP 15. LEGENDS LIVE UP HERE.",
        "THE BOOKIES STOPPED TAKING BETS ON THEM.",
        "EVEN THE CHAMPIONS ARE WATCHING NOW.",
        "EVERY RAVEN HERE HAS A STORY. NOW YOU DO.",
        "THE ARENA HAS NEVER SEEN A CLIMB LIKE IT.",
        "THE TOP 10 IS GETTING CLOSE.",
        "THAT WAS A LEGEND. WAS.",
        "THE CROWD KNOWS EVERY PART OF YOUR AC.",
    }, {   /* black: the top 10 */
        "THE TOP 10. THE ARENA HOLDS ITS BREATH.",
        "# MORE TO THE TOP.",
        "ONLY CHAMPIONS ARE LEFT.",
        "EVERY SPONSOR WANTS YOU NOW.",
        "EVERY STEP NOW IS ARENA HISTORY.",
        "THE TOP 5 CAN HEAR YOU COMING.",
        "THERE ARE NO EASY FIGHTS UP HERE.",
        "# LEFT. THEY ARE ALL WATCHING.",
    }, {   /* black-red: the top 5 */
        "# MORE TO THE TOP.",
        "THE TOP 5. THE LAST CLIMB.",
        "THE CHAMPION IS WATCHING THIS ONE.",
        "ONE OF THE GREATS. ALMOST THE GREATEST.",
        "THE WHOLE ARENA CAME TO SEE THIS.",
        "NOTHING LEFT ABOVE YOU BUT GIANTS.",
        "THE TOP IS IN SIGHT.",
        "THE CROWD HAS GONE SILENT.",
    }, {   /* black-gold: rank 1 */
        "NO ONE IS LEFT ABOVE YOU.",
        "THE ARENA BELONGS TO YOU.",
        "THEY WILL TELL STORIES ABOUT THIS RUN.",
        "THE TOP OF THE ARENA. YOURS.",
        "NO ONE IS LEFT ABOVE YOU.",
        "THE ARENA BELONGS TO YOU.",
        "THEY WILL TELL STORIES ABOUT THIS RUN.",
        "THE TOP OF THE ARENA. YOURS.",
    },
};
/* The first line: the rank taken (# = the rank). */
static const char *const k_victory_rank_line[] = {
    "RANK # IS YOURS.", "RANK # TAKEN.", "YOU NOW HOLD RANK #.", "UP TO RANK #.", "RANK #. KEEP CLIMBING.",
};
/* Entering a new belt (the background changes): the first line. */
static const char *const k_victory_belt_name[11] = {
    "WHITE", "YELLOW", "ORANGE", "GREEN", "BLUE", "PURPLE", "RED", "BROWN", "BLACK", "BLACK AND RED", "BLACK AND GOLD",
};
/* Beating a ghost (an AC the player lost with earlier). */
static const char *const k_victory_ghost[] = {
    "YOUR OLD AC IS SCRAP NOW.",
    "YOU BEAT YOUR OWN PAST.",
    "THE LAST RUN IS BURIED.",
    "THAT AC LOST TO YOU TWICE NOW.",
};

/* `tpl` into `out` with '#' replaced by n. */
static void run_victory_fill(char *out, size_t cap, const char *tpl, unsigned n)
{
    size_t k = 0;
    for (const char *q = tpl; *q && k + 1 < cap; q++) {
        if (*q != '#') { out[k++] = *q; continue; }
        char num[12];
        snprintf(num, sizeof num, "%u", n);
        for (const char *d = num; *d && k + 1 < cap; d++) out[k++] = *d;
    }
    out[k] = 0;
}

static uint8_t *run_victory_win, *run_victory_lose;
static char run_victory_beaten[64], run_victory_l0[48], run_victory_l1[48], run_victory_rank[16];

/* AC and pilot names of a lineup pick (upper case). */
static void run_pick_names(struct moa_run_pick p, char *ac, char *pilot, size_t cap)
{
    ac[0] = pilot[0] = 0;
    if (p.arena == MOA_RUN_GHOST && p.rank < MOA_RUN_SLOTS) {
        const struct moa_run_ghost *g = &run_state.lineup.ghost[p.rank];
        run_hx_name((const uint8_t *)g->ac, 16, ac, cap);
        run_hx_name((const uint8_t *)g->pilot, 16, pilot, cap);
    } else if (p.arena != MOA_RUN_PLAYER) {
        uint8_t rec[48];
        const uint32_t ix = run_index();
        if (ix && run_menu_lba() && run_record(ix, p, KIND_NAMES, 48u, rec)) {
            run_hx_name(rec, 24, pilot, cap);
            run_hx_name(rec + 24, 24, ac, cap);
        }
    }
}

/* Render one AC (13-byte record + 48 paint bytes) into a new RGBA buffer. */
static uint8_t *run_victory_render(const uint8_t rec[13], const uint8_t paint[48], int size,
                                   float yaw, float pitch, float zoom)
{
    const char *mdir = getenv("PSX_MOA_MODELS_DIR");
    if (!mdir || !mdir[0]) return NULL;
    moa_acv *v = moa_acv_load(mdir, rec, paint);
    if (!v) return NULL;
    uint8_t *px = (uint8_t *)calloc((size_t)size * (size_t)size, 4u);
    if (px) moa_acv_render_ex(v, yaw, pitch, 0.0f, zoom, 0u, NULL, size, size, px);
    moa_acv_free(v);
    return px;
}

/* Once per win: the two renders and the lines. */
static void run_victory_prepare(void)
{
    free(run_victory_win); free(run_victory_lose);
    run_victory_win = run_victory_lose = NULL;
    /* The winner: the garage's AC (the save's part order -> card record). */
    uint8_t g[12], paint[48];
    for (unsigned i = 0; i < 12; i++) g[i] = psx_mod_read_byte(0x80036FB0u + i);
    const uint8_t rec[13] = { g[0], g[2], g[3], g[11], g[5], g[4], g[6], g[7], g[9], g[8],
                              psx_mod_read_byte(0x8003EA70u), psx_mod_read_byte(0x8003EA71u), g[1] };
    moa_run_paint(run_state.lineup.seed, run_state.run.attempt, paint);
    run_victory_win = run_victory_render(rec, paint, 900, -0.45f, 0.10f, 1.15f);
    /* The beaten one: its arena record (parts, then paint at +16). */
    const unsigned who = (unsigned)run_victory_who;
    char ac[25] = "", pilot[25] = "";
    if (who < MOA_RUN_SLOTS) {
        const struct moa_run_pick p = run_state.lineup.slot[MOA_RUN_SLOTS - 1u - who];
        uint8_t r64[64];
        int ok = 0;
        if (p.arena == MOA_RUN_GHOST && p.rank < MOA_RUN_SLOTS) {
            moa_run_ghost_record(&run_state.lineup.ghost[p.rank], run_state.lineup.seed, r64);
            ok = 1;
        } else if (p.arena != MOA_RUN_PLAYER) {
            const uint32_t ix = run_index();
            ok = ix && run_menu_lba() && run_record(ix, p, KIND_BUILDS, 64u, r64);
        }
        if (ok) run_victory_lose = run_victory_render(r64, r64 + 16, 640, 0.6f, 0.25f, 1.0f);
        run_pick_names(p, ac, pilot, sizeof ac);
    }
    if (ac[0] && pilot[0]) snprintf(run_victory_beaten, sizeof run_victory_beaten, "%s / %s  DOWN", ac, pilot);
    else snprintf(run_victory_beaten, sizeof run_victory_beaten, "%s  DOWN", ac[0] ? ac : pilot[0] ? pilot : "OPPONENT");
    const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
    const unsigned rank = MOA_RUN_SLOTS - wins;
    snprintf(run_victory_rank, sizeof run_victory_rank, "RANK %u", rank);
    /* The lines, by belt; a run never repeats a line within a belt. */
    const int belt = moa_hub_rank_tile_index((int)rank);
    const int belt_top = belt >= 10 ? 1 : belt == 9 ? 5 : 50 - 5 * belt;    /* first rank of the belt */
    const unsigned step = (unsigned)(belt_top - (int)rank);                 /* 0 = just promoted */
    uint64_t q = run_state.lineup.seed ^ ((uint64_t)(belt + 1) * 0x9E3779B97F4A7C15ull) ^
                 ((uint64_t)run_state.run.attempt << 32);
    const unsigned base = moa_run_below(&q, VL_N);
    const unsigned nr = (unsigned)(sizeof k_victory_rank_line / sizeof k_victory_rank_line[0]);
    if (rank == 1) snprintf(run_victory_l0, sizeof run_victory_l0, "RANK 1. THE CHAMPION.");
    else if (wins == 1) snprintf(run_victory_l0, sizeof run_victory_l0, "FIRST WIN. RANK %u IS YOURS.", rank);
    else if (step == 0 && belt > 0)
        snprintf(run_victory_l0, sizeof run_victory_l0, "%s BELT. RANK %u.", k_victory_belt_name[belt], rank);
    else run_victory_fill(run_victory_l0, sizeof run_victory_l0, k_victory_rank_line[(base + wins) % nr], rank);
    const int ghost = who < MOA_RUN_SLOTS &&
                      run_state.lineup.slot[MOA_RUN_SLOTS - 1u - who].arena == MOA_RUN_GHOST;
    if (ghost && rank > 1) {
        const unsigned ng = (unsigned)(sizeof k_victory_ghost / sizeof k_victory_ghost[0]);
        snprintf(run_victory_l1, sizeof run_victory_l1, "%s", k_victory_ghost[(base + wins) % ng]);
    } else if (rank == 2) snprintf(run_victory_l1, sizeof run_victory_l1, "ONE FIGHT LEFT. THE TOP AWAITS.");
    else run_victory_fill(run_victory_l1, sizeof run_victory_l1, k_victory_belt[belt][(base + step) % VL_N], rank - 1u);
    run_victory_key++;
    run_victory_ready = 1;
    run_log("victory: %s %s, %s%s", run_victory_l0, run_victory_l1, run_victory_beaten,
            run_victory_win ? "" : " (no 3D model install: text only)");
}

/* One frame of it; `ready`: the prompt instead of the loading line. */
static int run_victory_draw(int ready)
{
    static const char *const k_dots[4] = { "", ".", "..", "..." };
    char status[40];
    const float t = (float)run_victory_frames / 60.0f;
    if (ready) snprintf(status, sizeof status, "%s", ((run_victory_frames / 30u) & 1u) ? "" : "X  CONTINUE");
    else snprintf(status, sizeof status, "LOADING%s", k_dots[(run_victory_frames / 20u) % 4u]);
    const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
    struct moa_screen_victory v;
    memset(&v, 0, sizeof v);
    v.winner = run_victory_win; v.winner_w = v.winner_h = run_victory_win ? 900 : 0;
    v.loser = run_victory_lose; v.loser_w = v.loser_h = run_victory_lose ? 640 : 0;
    v.band = k_belt_band[moa_hub_rank_tile_index((int)(MOA_RUN_SLOTS - wins))];
    v.rank = run_victory_rank;
    char pay[12];
    snprintf(pay, sizeof pay, "+%u P", 1u + run_state.run.income);
    v.pay = pay;
    v.beaten = run_victory_beaten;
    v.line[0] = run_victory_l0;
    v.line[1] = run_victory_l1;
    v.status = status[0] ? status : NULL;
    v.t = t;
    v.key = run_victory_key;
    const int words = moa_screen_victory_words(&v);
    if (words > run_victory_words_shown) { run_victory_words_shown = words; (void)moa_chime_tick(); }
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render_victory(ww, wh, &v, &w, &h);
    return px && psx_mod_set_overlay_image(px, w, h);
}

/* ---- VS screen (design: docs/rogue-arena-v2-design.md, "VS screen") ----------
 *
 * From NEXT BATTLE until the fight runs, over the menus the run drives and
 * the game's own loading: the player's AC and the next opponent, FOR RANK n,
 * VS, the belt's strip, "UNITS DEPLOY IN 5..1" at a second a step; on counts
 * 3, 2, 1 a rifle shot, crossing out the last three ACs beaten in the belt
 * (older ones are crossed out already). Then the hangar shutters close and
 * it fades to black; black holds until the fight runs. A fight that runs
 * before the count is over cuts it: the shutters close at once and hand
 * over as soon as they meet (user 2026-11-18 12:29). Silent but for the
 * shots and the shutters. Renders from the 3D model install; without it,
 * the boxes are empty. */
#define VS_C0        0.6f          /* the count starts */
#define VS_STEP      1.0f          /* a second per number */
#define VS_SHUT      (VS_C0 + 5.0f * VS_STEP)
#define VS_FADE_AT   0.7f          /* after the shutters start: fade to black ... */
#define VS_FADE_LEN  0.4f
#define VS_MAX_MINI  8
static uint32_t run_vs_frames;
static int32_t run_vs_fight_at = -1;      /* frame the fight ran, -1 not yet */
static int32_t run_vs_shut_at = -1;       /* frame the shutters started, -1 not yet */
static int run_vs_overlay;                /* drawn as the overlay (else the loading picture) */
static unsigned run_vs_shots, run_vs_key;
static int run_vs_door;
static uint8_t *run_vs_you_px, *run_vs_foe_px, *run_vs_mini_px[VS_MAX_MINI];
static uint32_t run_vs_emblem[64 * 64];
static uint32_t run_vs_mini_emblem[VS_MAX_MINI][64 * 64];   /* boxes without a render */
static int run_vs_has_emblem;
static struct moa_screen_vs_mini run_vs_mini[VS_MAX_MINI];
static int run_vs_mini_n;
static char run_vs_pilot[25], run_vs_ac[25], run_vs_my_pilot[17], run_vs_my_ac[17], run_vs_belt[40];
static int run_vs_foe_rank, run_vs_my_rank;
static float run_vs_foe_frac, run_vs_my_frac;
static uint32_t run_vs_band;

/* A lineup pick's build (64-byte arena record: parts, paint at +16). */
static int run_vs_pick_record(struct moa_run_pick p, uint8_t r64[64])
{
    if (p.arena == MOA_RUN_GHOST && p.rank < MOA_RUN_SLOTS) {
        moa_run_ghost_record(&run_state.lineup.ghost[p.rank], run_state.lineup.seed, r64);
        return 1;
    }
    if (p.arena == MOA_RUN_PLAYER) return 0;
    const uint32_t ix = run_index();
    return ix && run_menu_lba() && run_record(ix, p, KIND_BUILDS, 64u, r64);
}

/* A pick's emblem from its arena's emblem entries (a ghost: the emblem of
 * the AC it pushed off the ladder, the old rank 49; user 12:33). 1 = got one. */
static int run_vs_pick_emblem(struct moa_run_pick p, uint32_t out[64 * 64])
{
    if (p.arena == MOA_RUN_GHOST && p.rank < MOA_RUN_SLOTS) p = run_state.ghost_emblem[p.rank];
    if (p.arena == MOA_RUN_GHOST || p.arena == MOA_RUN_PLAYER) return 0;
    uint32_t first, count;
    const uint32_t ix = run_index();
    if (!ix || !run_menu_lba() || !run_source(ix, p.arena, &first, &count) || p.rank >= count) return 0;
    const uint32_t e = first + count + p.rank, a = run_hdr(e), b = run_hdr(e + 1u);
    static uint8_t sec[2 * 2048];
    if (b <= a || b - a > 2u || !psx_mod_read_disc_sectors(run_menu_lba() + a, b - a, sec)) return 0;
    return moa_screen_emblem_from_tim(sec, (b - a) * 2048u, out);
}

/* The VS sounds from the disc's shared fight bank (FDAT entry 205), once. */
static void run_vs_sounds(void)
{
    static int tried;
    if (tried || moa_vs_sound_ready()) return;
    tried = 1;
    uint32_t lba = 0, size = 0;
    static uint8_t head[2048];
    if (!psx_mod_disc_file_extent("GG/COM/FDAT.T", &lba, &size) || !lba ||
        !psx_mod_read_disc_sectors(lba, 1u, head)) { run_log("vs: FDAT.T not found, no sounds"); return; }
    const uint32_t k = MOA_VS_FDAT_ENTRY - 1u;
    const uint32_t a = (uint32_t)head[4 + 2 * k] | (uint32_t)head[5 + 2 * k] << 8;
    const uint32_t b = (uint32_t)head[6 + 2 * k] | (uint32_t)head[7 + 2 * k] << 8;
    uint8_t *e = b > a && b - a < 512u ? (uint8_t *)malloc((size_t)(b - a) * 2048u) : NULL;
    const int ok = e && psx_mod_read_disc_sectors(lba + a, b - a, e) && moa_vs_sound_build(e, (b - a) * 2048u);
    free(e);
    run_log("vs: sounds %s", ok ? "ready" : "not made (no sounds)");
}

/* ---- the renders, made ahead (user 2026-11-18 13:20) --------------------------
 * The portraits are rendered on moa_render_queue's worker between fights
 * (from the victory screen on: prize screen, hub overlay), not at NEXT
 * BATTLE. The opponents once per win; the player's AC whenever its parts
 * change (EQUIP NOW, swaps), so only that one is ever redone. At NEXT
 * BATTLE an opponent render that is not done is dropped: its box shows the
 * emblem alone. The player's render is waited for: it pops in when done,
 * until the shutters close. */
#define VS_PRE_MAX 10
struct run_vs_pre_box {
    int rank, big;                     /* big: the 1000 px portrait, else the strip's 400 px */
    moa_rq_job job;
    uint8_t *px;
};
static struct run_vs_pre_box run_vs_pre[VS_PRE_MAX];
static int run_vs_pre_n;
static uint64_t run_vs_pre_seed;
static uint32_t run_vs_pre_attempt, run_vs_pre_wins = 0xFFFFFFFFu;
static uint8_t run_vs_pre_me[13 + 48];         /* the player's record + paint the renders are for */
static int run_vs_pre_me_set;
static moa_rq_job run_vs_pre_me_job[2];        /* [0] big, [1] strip */
static uint8_t *run_vs_pre_me_px[2];

static const char *run_vs_models(void)
{
    const char *mdir = getenv("PSX_MOA_MODELS_DIR");
    return mdir && mdir[0] ? mdir : NULL;
}

/* The player's AC: the garage's loadout (the save's part order -> card record) + the run's paint. */
static void run_vs_me(uint8_t me[13], uint8_t paint[48])
{
    uint8_t g[12];
    for (unsigned i = 0; i < 12; i++) g[i] = psx_mod_read_byte(0x80036FB0u + i);
    const uint8_t r[13] = { g[0], g[2], g[3], g[11], g[5], g[4], g[6], g[7], g[9], g[8],
                            psx_mod_read_byte(0x8003EA70u), psx_mod_read_byte(0x8003EA71u), g[1] };
    memcpy(me, r, 13);
    moa_run_paint(run_state.lineup.seed, run_state.run.attempt, paint);
}

/* The next opponent's belt: ranks hi (first) .. lo (its last AC). */
static int run_vs_belt_of(int foe_rank, int *hi, int *lo)
{
    const int belt = moa_hub_rank_tile_index(foe_rank);
    *hi = belt >= 10 ? 1 : belt == 9 ? 5 : 50 - 5 * belt;
    *lo = belt >= 10 ? 1 : belt == 9 ? 2 : *hi - 4;
    return belt;
}

static void run_vs_pre_drop_me(void)
{
    for (int k = 0; k < 2; k++) {
        moa_rq_drop(run_vs_pre_me_job[k]);
        run_vs_pre_me_job[k] = 0;
        free(run_vs_pre_me_px[k]);
        run_vs_pre_me_px[k] = NULL;
    }
    run_vs_pre_me_set = 0;
}

static void run_vs_pre_drop_all(void)
{
    for (int i = 0; i < run_vs_pre_n; i++) {
        moa_rq_drop(run_vs_pre[i].job);
        free(run_vs_pre[i].px);
    }
    memset(run_vs_pre, 0, sizeof run_vs_pre);
    run_vs_pre_n = 0;
    run_vs_pre_wins = 0xFFFFFFFFu;
    run_vs_pre_drop_me();
}

/* Collect finished renders (frees the worker's slots). */
static void run_vs_pre_collect(void)
{
    uint8_t *px;
    for (int i = 0; i < run_vs_pre_n; i++) {
        struct run_vs_pre_box *b = &run_vs_pre[i];
        if (!b->job) continue;
        const int r = moa_rq_take(b->job, &px);
        if (r == 0) continue;
        b->job = 0;
        if (r > 0) b->px = px;
    }
    for (int k = 0; k < 2; k++) {
        if (!run_vs_pre_me_job[k]) continue;
        const int r = moa_rq_take(run_vs_pre_me_job[k], &px);
        if (r == 0) continue;
        run_vs_pre_me_job[k] = 0;
        if (r > 0) run_vs_pre_me_px[k] = px;
    }
}

/* The player's renders for the loadout as it is now (redone when it changes). */
static void run_vs_pre_player(const char *mdir)
{
    uint8_t me[13 + 48];
    run_vs_me(me, me + 13);
    if (run_vs_pre_me_set && !memcmp(me, run_vs_pre_me, sizeof me)) return;
    const int again = run_vs_pre_me_set;
    run_vs_pre_drop_me();
    memcpy(run_vs_pre_me, me, sizeof me);
    run_vs_pre_me_set = 1;
    run_vs_pre_me_job[0] = moa_rq_submit(mdir, me, me + 13, 1000, 0.55f, 0.06f, 1.3f);
    run_vs_pre_me_job[1] = moa_rq_submit(mdir, me, me + 13, 400, 0.55f, 0.06f, 1.3f);
    if (again) run_log("vs: the player's parts changed, their portrait is redone");
}

/* Per frame between fights: start what the next VS screen needs. */
static void run_vs_pre_frame(void)
{
    run_vs_pre_collect();
    if (run_vs_pre_n || run_vs_pre_me_set) {
        if (!run_state_ok || run_state.run.over) { run_vs_pre_drop_all(); return; }   /* nothing to make it for */
    }
    if (!run_state_ok || run_state.run.over || !run_at_hub || run_autopilot || run_vs || run_music_fight) return;
    const char *mdir = run_vs_models();
    if (!mdir) return;
    const unsigned wins = run_state.run.wins;
    if (wins >= MOA_RUN_SLOTS - 1u) return;
    if (wins != run_vs_pre_wins || run_state.lineup.seed != run_vs_pre_seed ||
        run_state.run.attempt != run_vs_pre_attempt) {
        run_vs_pre_drop_all();
        run_vs_pre_wins = wins;
        run_vs_pre_seed = run_state.lineup.seed;
        run_vs_pre_attempt = run_state.run.attempt;
        const int my_rank = (int)(MOA_RUN_SLOTS - wins), foe_rank = my_rank - 1;
        int hi, lo;
        (void)run_vs_belt_of(foe_rank, &hi, &lo);
        uint8_t r64[64];
        /* The order the worker takes them: the opponent's portrait, the player's, the strip. */
        const struct moa_run_pick foe = run_state.lineup.slot[MOA_RUN_SLOTS - (unsigned)foe_rank];
        if (run_vs_pick_record(foe, r64)) {
            struct run_vs_pre_box *b = &run_vs_pre[run_vs_pre_n++];
            b->rank = foe_rank; b->big = 1;
            b->job = moa_rq_submit(mdir, r64, r64 + 16, 1000, -0.55f, 0.06f, 1.3f);
        }
        run_vs_pre_player(mdir);
        for (int r = hi; r >= lo && run_vs_pre_n < VS_PRE_MAX; r--) {
            if (r > (int)MOA_RUN_SLOTS - 1) continue;                      /* rank 50: the player's start */
            const struct moa_run_pick p = run_state.lineup.slot[MOA_RUN_SLOTS - (unsigned)r];
            if (!run_vs_pick_record(p, r64)) continue;
            struct run_vs_pre_box *b = &run_vs_pre[run_vs_pre_n++];
            b->rank = r; b->big = 0;
            b->job = moa_rq_submit(mdir, r64, r64 + 16, 400, -0.55f, 0.06f, 1.3f);
        }
        run_log("vs: renders started for rank %d's fight (%d opponent renders, the player's 2)", foe_rank, run_vs_pre_n);
        return;
    }
    run_vs_pre_player(mdir);
}

/* At NEXT BATTLE: an opponent's render, or NULL (not done: dropped). */
static uint8_t *run_vs_pre_get(int rank, int big, int *dropped)
{
    for (int i = 0; i < run_vs_pre_n; i++) {
        struct run_vs_pre_box *b = &run_vs_pre[i];
        if (b->rank != rank || b->big != big) continue;
        if (b->px) return b->px;
        if (b->job) { moa_rq_drop(b->job); b->job = 0; (*dropped)++; }
        return NULL;
    }
    return NULL;
}

/* While the VS screen is up: the player's renders as they land. 1 = changed. */
static int run_vs_take_me(void)
{
    run_vs_pre_collect();
    int changed = 0;
    if (!run_vs_you_px && run_vs_pre_me_px[0]) { run_vs_you_px = run_vs_pre_me_px[0]; changed = 1; }
    for (int i = 0; i < run_vs_mini_n; i++)
        if (run_vs_mini[i].kind == MOA_VS_YOU && !run_vs_mini[i].rgba && run_vs_pre_me_px[1]) {
            run_vs_mini[i].rgba = run_vs_pre_me_px[1];
            changed = 1;
        }
    return changed;
}

/* The renders belong to the run_vs_pre_* store: the screen only points at them. */
static void run_vs_free(void)
{
    run_vs_you_px = run_vs_foe_px = NULL;
    run_vs_mini_n = 0;
}

/* Wide legs (four legs, caterpillars) read better with more of the AC in the box. */
static float run_vs_frac(const uint8_t rec[13]) { return rec[3] != 255u && rec[3] >= 21u ? 0.72f : 0.55f; }

static void run_vs_open(void)
{
    if (run_vs || !run_state_ok || run_state.run.over || !run_show_font()) return;
    const unsigned wins = run_state.run.wins;
    if (wins >= MOA_RUN_SLOTS - 1u) return;
    run_vs_free();
    run_backdrop_frame();
    run_vs_sounds();
    const char *mdir = run_vs_models();
    if (mdir) {
        /* Nothing made ahead for this fight (should not happen): start it now;
         * the opponents' renders then miss this screen, the player's still lands. */
        if (wins != run_vs_pre_wins || run_state.lineup.seed != run_vs_pre_seed ||
            run_state.run.attempt != run_vs_pre_attempt) {
            run_log("vs: no renders made ahead for this fight");
            run_vs_pre_drop_all();
        }
        run_vs_pre_player(mdir);
    }
    run_vs_pre_collect();
    int dropped = 0;
    const int my_rank = (int)(MOA_RUN_SLOTS - wins), foe_rank = my_rank - 1;
    run_vs_my_rank = wins ? my_rank : 0;
    run_vs_foe_rank = foe_rank;
    /* The player. */
    uint8_t me[13], paint[48];
    run_vs_me(me, paint);
    run_vs_you_px = run_vs_pre_me_px[0];
    run_vs_my_frac = run_vs_frac(me);
    run_hx_name((const uint8_t *)run_state.run.pilot, 16, run_vs_my_pilot, sizeof run_vs_my_pilot);
    run_hx_name((const uint8_t *)run_state.run.ac, 16, run_vs_my_ac, sizeof run_vs_my_ac);
    if (!run_vs_my_ac[0]) snprintf(run_vs_my_ac, sizeof run_vs_my_ac, "NEW AC");
    /* The opponent: the lineup's next rank. */
    const struct moa_run_pick foe = run_state.lineup.slot[MOA_RUN_SLOTS - (unsigned)foe_rank];
    uint8_t r64[64];
    run_vs_foe_frac = run_vs_pick_record(foe, r64) ? run_vs_frac(r64) : 0.55f;
    run_vs_foe_px = run_vs_pre_get(foe_rank, 1, &dropped);
    run_vs_has_emblem = run_vs_pick_emblem(foe, run_vs_emblem);
    run_pick_names(foe, run_vs_ac, run_vs_pilot, sizeof run_vs_ac);
    /* The strip: the next opponent's belt, beaten first, then YOU, the next, the rest. */
    int hi, lo;
    const int belt = run_vs_belt_of(foe_rank, &hi, &lo);
    run_vs_band = k_belt_band[belt < 0 ? 0 : belt > 10 ? 10 : belt];
    if (hi == lo) snprintf(run_vs_belt, sizeof run_vs_belt, "%s BELT  %d", k_victory_belt_name[belt], hi);
    else snprintf(run_vs_belt, sizeof run_vs_belt, "%s BELT  %d-%d", k_victory_belt_name[belt], hi, lo);
    int beaten = 0, emblems = 0;
    for (int pass = 0; pass < 3; pass++) {
        /* pass 0: the beaten (hi .. my rank), 1: YOU, 2: the next and the rest */
        const int from = pass == 0 ? hi : foe_rank, to = pass == 0 ? my_rank : lo;
        for (int r = from; r >= to && run_vs_mini_n < VS_MAX_MINI; r--) {
            if (pass == 0 && r > (int)MOA_RUN_SLOTS - 1) continue;     /* rank 50: the player's start */
            struct moa_screen_vs_mini *m = &run_vs_mini[run_vs_mini_n];
            memset(m, 0, sizeof *m);
            m->size = 400;
            if (pass == 1) {
                m->rgba = run_vs_pre_me_px[1]; m->kind = MOA_VS_YOU; m->rank = my_rank;
                run_vs_mini_n++;
                break;
            }
            m->rank = r;
            m->rgba = run_vs_pre_get(r, 0, &dropped);
            if (!m->rgba) {
                const struct moa_run_pick p = run_state.lineup.slot[MOA_RUN_SLOTS - (unsigned)r];
                if (run_vs_pick_emblem(p, run_vs_mini_emblem[run_vs_mini_n])) {
                    m->emblem = run_vs_mini_emblem[run_vs_mini_n];
                    emblems++;
                }
            }
            if (pass == 0) { m->kind = MOA_VS_DOWN; beaten++; }
            else { m->kind = r == foe_rank ? MOA_VS_NEXT : MOA_VS_AHEAD; m->top = r == lo; }
            run_vs_mini_n++;
        }
    }
    for (int i = 0; i < beaten; i++) {                    /* the last three on counts 3, 2, 1 */
        const int from_end = beaten - 1 - i;
        run_vs_mini[i].slash_at = from_end >= 3 ? -9.0f : VS_C0 + (float)(4 - from_end) * VS_STEP;
    }
    run_vs = 1;
    run_vs_frames = 0;
    run_vs_fight_at = run_vs_shut_at = -1;
    run_vs_shots = 0;
    run_vs_door = 0;
    run_vs_key++;
    psx_mod_counter_add("moa.run.vs_screen", 1);
    if (dropped) psx_mod_counter_add("moa.run.vs_render_dropped", dropped);
    run_log("vs: up, rank %d vs %s / %s (rank %d), %s, %d beaten in the belt%s", my_rank, run_vs_pilot, run_vs_ac,
            foe_rank, run_vs_belt, beaten, mdir ? "" : " (no 3D model install: empty boxes)");
    if (mdir)
        run_log("vs: renders ready: opponent %s, player %s; %d dropped (not done), %d boxes show the emblem alone",
                run_vs_foe_px ? "yes" : "no", run_vs_you_px ? "yes" : "not yet (waits)", dropped, emblems);
}

static void run_vs_close(const char *why)
{
    if (!run_vs) return;
    run_vs = 0;
    if (run_vs_overlay) (void)psx_mod_set_overlay_image(NULL, 0, 0);
    else psx_mod_set_loading_screen(NULL, 0);
    run_vs_overlay = 0;
    run_vs_free();
    run_log("vs: down (%s, %u frames, fight at %d, shutters at %d)", why, (unsigned)run_vs_frames,
            (int)run_vs_fight_at, (int)run_vs_shut_at);
}

static void run_vs_fight_runs(void)
{
    if (run_vs && run_vs_fight_at < 0) {
        run_vs_fight_at = (int32_t)run_vs_frames;
        run_log("vs: the fight runs at %.2f s", run_vs_frames / 60.0);
    }
}

/* Per frame while it is up. */
static void run_vs_frame(void)
{
    if (!run_vs) return;
    if (!run_state_ok || run_state.run.over) { run_vs_close("the run is over"); return; }
    const float t = (float)run_vs_frames / 60.0f;
    run_vs_frames++;
    if (!run_hx_next) psx_mod_write_half(RUN_PAD_BUFFER + 2u, 0xFFFFu);   /* controls locked (not while the run drives the menus) */
    /* The shutters: at the end of the count, or at once when the fight runs early. */
    if (run_vs_shut_at < 0 && (t >= VS_SHUT || run_vs_fight_at >= 0)) {
        run_vs_shut_at = (int32_t)run_vs_frames - 1;
        if (run_vs_fight_at >= 0 && t < VS_SHUT) run_log("vs: the fight is ready at %.2f s: count cut, shutters", t);
    }
    const float sh = run_vs_shut_at >= 0 ? (float)(run_vs_frames - 1u - (uint32_t)run_vs_shut_at) / 60.0f : -1.0f;
    /* Hand over: the fight runs and the shutters are shut (early: as soon as
     * they have met and settled; else the black is up). */
    if (run_vs_fight_at >= 0 && sh >= 0.0f) {
        const int early = run_vs_fight_at < run_vs_shut_at + 1;
        if (sh >= (early ? MOA_VS_SHUTTER_MEET + 0.10f : VS_FADE_AT + VS_FADE_LEN)) { run_vs_close("the fight runs"); return; }
    }
    if (run_vs_frames > RUN_SLOW(60u * 60u)) { run_vs_close("timed out"); return; }
    /* Sounds: shots on counts 3, 2, 1; the shutters' slam on the meet. */
    if (sh < 0.0f) {
        while (run_vs_shots < 3u && t >= VS_C0 + (2.0f + (float)run_vs_shots) * VS_STEP) {
            if (moa_vs_sound_shot(run_vs_shots == 2u)) psx_mod_counter_add("moa.run.vs_shot", 1);
            run_vs_shots++;
        }
    } else if (!run_vs_door && sh >= MOA_VS_SHUTTER_MEET - MOA_VS_DOOR_PEAK) {
        run_vs_door = 1;
        (void)moa_vs_sound_door();
    }
    /* The player's render, if it was still being made: in it comes (the still
     * layer is redrawn once), until the shutters start. */
    if (sh < 0.0f && run_vs_take_me()) {
        run_vs_key++;
        run_log("vs: the player's portrait landed at %.2f s", t);
    }
    struct moa_screen_vs v;
    memset(&v, 0, sizeof v);
    v.key = run_vs_key;
    v.you.rgba = run_vs_you_px; v.you.size = 1000; v.you.pilot = run_vs_my_pilot; v.you.ac = run_vs_my_ac;
    v.you.rank = run_vs_my_rank; v.you.frac = run_vs_my_frac;
    v.foe.rgba = run_vs_foe_px; v.foe.size = 1000; v.foe.emblem = run_vs_has_emblem ? run_vs_emblem : NULL;
    v.foe.pilot = run_vs_pilot; v.foe.ac = run_vs_ac; v.foe.rank = run_vs_foe_rank; v.foe.frac = run_vs_foe_frac;
    v.mini = run_vs_mini; v.mini_n = run_vs_mini_n;
    v.belt = run_vs_belt; v.band = run_vs_band;
    v.t = t;
    v.count = (sh < 0.0f && t >= VS_C0) ? 5 - (int)((t - VS_C0) / VS_STEP) : 0;
    if (v.count < 1) v.count = 0;
    v.shutter = sh;
    v.black = sh < 0.0f ? 0.0f : (sh - VS_FADE_AT) / VS_FADE_LEN;
    if (v.black < 0.0f) v.black = 0.0f;
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render_vs(ww, wh, &v, &w, &h);
    if (!px) { run_vs_close("could not be drawn"); return; }
    if (psx_mod_set_overlay_image(px, w, h)) { run_vs_overlay = 1; return; }
    run_vs_overlay = 0;
    psx_mod_set_loading_screen("ROGUE ARENA  Next battle", -1);
    psx_mod_set_loading_image(px, w, h);
}

/* ---- the cover (see run_cover) --------------------------------------------- */
static void run_cover_end(const char *why)
{
    if (!run_cover) return;
    if (run_cover_drawn && !run_hx && !run_prize_stage && !run_end_stage && !run_vs)
        (void)psx_mod_set_overlay_image(NULL, 0, 0);
    run_cover = run_cover_drawn = 0;
    run_log("cover: down (%s, %u frames)", why, (unsigned)run_cover_frames);
}

static int run_cover_draw(void)
{
    static const char *const k_dots[4] = { "", ".", "..", "..." };
    char title[40], bal[40], what[40];
    const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
    snprintf(title, sizeof title, "ROGUE ARENA  RANK %u", (unsigned)(MOA_RUN_SLOTS - wins));
    snprintf(bal, sizeof bal, "BALANCE  %u P", (unsigned)psx_mod_read_word(RUN_CREDITS));
    snprintf(what, sizeof what, "%s%s", run_cover == 1 ? "BACK TO THE ARENA" : "RESULTS",
             k_dots[(run_cover_frames / 20u) % 4u]);
    if (run_cover_frames == 0 || !run_hx_foe[0]) run_hx_foe_text();
    struct moa_screen_list l;
    memset(&l, 0, sizeof l);
    l.title = title;
    l.info[0] = bal;
    l.info[1] = run_hx_foe;
    l.info[2] = what;
    l.marked = -1;
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render_list(ww, wh, &l, 0, &w, &h);
    return px && psx_mod_set_overlay_image(px, w, h);
}

/* Per frame while the cover is up. */
static void run_cover_frame(void)
{
    if (!run_cover) return;
    run_cover_frames++;
    if (run_hx) { run_cover = run_cover_drawn = 0; return; }       /* the hub overlay has it */
    if (!run_state_ok || run_state.run.over) { run_cover_end("the run is over"); return; }
    if (!run_victory_hold && run_cover_frames > RUN_SLOW(60u * 30u)) { run_cover_end("timed out"); return; }
    if (run_prize_stage || run_end_stage) run_victory_hold = 0;   /* they take over */
    if (run_prize_stage || run_end_stage) { run_cover_drawn = 0; return; }   /* their own picture */
    /* Results done (the victory screen may close), and nothing left to show:
     * the hub overlay could open. The prize screen comes after the victory
     * screen: it waits for it (run_prize_frame), so the victory screen must
     * not wait for the prize. */
    const int results_in = run_cover == 2 && !psx_mod_read_byte(RUN_FIGHT_DONE) && !run_win_screen && !run_pay_due;
    const int settled = results_in && run_prize_due() < 0;
    /* After a win: the victory screen, animating every other frame, until
     * the game behind it has settled and the player presses X. */
    if (run_victory_hold) {
        if (!run_victory_ready) {
            if (!run_show_font()) { run_victory_hold = 0; goto plain; }
            run_backdrop_frame();
            run_victory_prepare();
            if (moa_chime_play()) psx_mod_counter_add("moa.run.victory_chime", 1);
        }
        run_victory_frames++;
        struct moa_screen_victory typed;                         /* the prompt waits for the lines */
        memset(&typed, 0, sizeof typed);
        typed.line[0] = run_victory_l0;
        typed.line[1] = run_victory_l1;
        typed.t = (float)run_victory_frames / 60.0f;
        const int ready = results_in && run_victory_frames > 90u && moa_screen_victory_typed(&typed);
        const uint16_t b = psx_mod_host_pad_buttons(0);          /* active low */
        const uint16_t down = (uint16_t)(run_victory_held & ~b);
        run_victory_held = b;
        if (b == 0xFFFFu) run_victory_armed = 1;
        if (ready && run_victory_armed && (down & (RUN_PAD_X | RUN_PAD_START))) {
            run_victory_hold = 0;
            run_victory_who = -1;
            run_se(RUN_SE_OK);
            run_log("victory: X after %u frames", (unsigned)run_victory_frames);
            return;                                              /* the prize screen opens next, then the hub overlay */
        }
        if (!run_cover_drawn || (run_victory_frames & 1u)) {
            if (run_victory_draw(ready)) { run_cover_drawn = 1; return; }
            run_victory_hold = 0;                                /* not drawable: the plain cover */
            run_victory_who = -1;
        } else return;
    }
plain:
    if (settled) {
        if (++run_cover_idle > 120u) { run_cover_end("the hub overlay did not open"); return; }
    } else run_cover_idle = 0;
    if (!run_cover_drawn || run_cover_frames % 20u == 0u) {
        if (!run_show_font() || !run_cover_draw()) { run_cover_end("could not be drawn"); return; }
        run_cover_drawn = 1;
    }
}

/* The game's loading screen before an engine (exe 0x80012348): the overlay
 * hands the screen back here, before the next match. */
static void run_loading_screen_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    if (!run_on()) return;
    if (run_at_hub && !run_autopilot) run_music_fight = 1;   /* the game's own fight music from here */
    if (run_hx) {
        run_log("hub overlay: released at the game's loading screen");
        run_hx_close();
    }
    run_cover_end("the game's loading screen");
}

/* The opaque screens' backdrop: the belt tile of the run's rank, as the hub. */
static void run_backdrop_frame(void)
{
    static int last = -1;
    if (!run_state_ok) return;
    const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
    const int rank = (int)(MOA_RUN_SLOTS - wins);
    if (rank == last) return;
    last = rank;
    const uint8_t *pix = NULL;
    const uint16_t *clut = NULL;
    (void)moa_hub_rank_tile(rank, &pix, &clut);
    if (!pix || !clut) { moa_screen_set_backdrop(NULL, 0, 0); return; }
    static uint32_t rgb[64 * 64];
    for (int i = 0; i < 64 * 64; i++) {
        const uint16_t c = clut[(pix[i >> 1] >> ((i & 1) * 4)) & 15u];
        const uint32_t r = (c & 31u) * 255u / 31u, g = ((c >> 5) & 31u) * 255u / 31u, b = ((c >> 10) & 31u) * 255u / 31u;
        rgb[i] = r << 16 | g << 8 | b;
    }
    moa_screen_set_backdrop(rgb, 64, 64);
}

static void run_disc_frame(void)
{
    if (run_disc_slow || !run_at_hub || run_autopilot) return;
    const char *e = getenv("PSX_MOA_RUN_FAST_LOAD");
    if (e && e[0] == '1') {
        run_disc_slow = -1;
        run_log("disc: fast loading kept (PSX_MOA_RUN_FAST_LOAD=1)");
        return;
    }
    const uint32_t rd = psx_mod_read_word(RUN_CDQ + 0x37Cu);
    if (rd != psx_mod_read_word(RUN_CDQ + 0x378u) || psx_mod_read_byte(rd) != 0u) return;  /* the game's queue */
    if (!psx_mod_set_disc_speed_live(1u, 0u)) return;      /* netplay, or the drive is busy: next frame */
    run_disc_slow = 1;
    psx_mod_counter_add("moa.run.fast_load_off", 1);
    run_log("disc: fast loading off from the hub on (the drive's real speed)");
}

/* ---- boot black screen (run_boot_black) ------------------------------------ */
static void run_boot_black_draw(void)
{
    static const uint32_t k_black[8 * 8] = {
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u };
    if (run_boot_black != 2 && psx_mod_set_overlay_image(k_black, 8, 8)) { run_boot_black = 1; return; }
    run_boot_black = 2;                         /* this renderer can't overlay: the plain black screen */
    psx_mod_set_loading_screen(" ", -1);
}

static void run_boot_black_up(void)
{
    if (run_boot_black) return;
    run_boot_black_frames = run_boot_black_after = 0;
    run_boot_black_draw();
    run_log("boot: black screen up until the player has control");
}

/* Ends it. hand_on: another picture has taken the screen (leave it be). */
static void run_boot_black_down(const char *why, int hand_on)
{
    if (!run_boot_black) return;
    if (!hand_on) {
        if (run_boot_black == 1) (void)psx_mod_set_overlay_image(NULL, 0, 0);
        else psx_mod_set_loading_screen(NULL, 0);
    }
    run_boot_black = 0;
    run_log("boot: black screen down (%s, %u frames)", why, (unsigned)run_boot_black_frames);
}

static void run_boot_black_frame(void)
{
    if (!run_boot_black) return;
    run_boot_black_frames++;
    if (run_hx && run_hx_overlay) { run_boot_black_down("the hub overlay is up", 1); return; }
    if (run_prize_stage || run_end_stage) { run_boot_black_down("a run screen took over", 1); return; }
    if (run_state_ok && run_state.run.over) { run_boot_black_down("the run is over", 0); return; }
    if (run_at_hub && !run_autopilot && ++run_boot_black_after > RUN_SLOW(600u)) {
        run_boot_black_down("the hub overlay did not open", 0);
        return;
    }
    if (run_boot_black_frames > 60u * 120u) { run_boot_black_down("timed out", 0); return; }
    if (run_boot_black == 2 || run_boot_black_frames % 30u == 0u) run_boot_black_draw();
}

/* What the music plays for (moa_music.h). */
static int run_music_scene(void)
{
    if (!run_on()) return MOA_MUSIC_OFF;
    if (run_autopilot || run_boot_black) return MOA_MUSIC_BOOT;   /* until the player has control */
    if (!run_at_hub) return MOA_MUSIC_OFF;
    if (run_vs) return MOA_MUSIC_OFF;                            /* silent, as the game's loading */
    if (run_music_fight) return MOA_MUSIC_FIGHT;
    if (run_state_ok && run_state.run.over) return MOA_MUSIC_LOSS;
    if (run_cover && run_victory_hold) return MOA_MUSIC_VICTORY;
    return MOA_MUSIC_HUB;
}

static void run_frame_hook(void)
{
    run_check_session();
    if (!run_on()) return;
    run_apply_fixes();
    run_disc_frame();
    run_backdrop_frame();
    if (run_se_queue >= 0 && ++run_se_age > 6u) run_se_queue = -1;   /* no menu to play it */
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
    if (run_ghost_beaten_n && run_prize_where && !psx_mod_read_byte(RUN_FIGHT_DONE))
        run_ghost_save(run_ghost_beaten[--run_ghost_beaten_n]);
    run_prize_frame();
    if (run_loss_wait) run_loss_wait--;      /* the return itself is made in a guest hook */
    run_end_frame();
    run_result_ran = 0;
    run_hx_frame();
    run_boot_black_frame();
    run_cover_frame();
    run_vs_frame();
    run_vs_pre_frame();
    {
        const unsigned wins = run_state.run.wins < MOA_RUN_SLOTS ? run_state.run.wins : MOA_RUN_SLOTS - 1u;
        moa_music_frame(run_music_scene(), (int)(MOA_RUN_SLOTS - wins));
    }
    if (run_autopilot && run_stage == STAGE_BACK) {
        if (++run_frames - run_back_start >= (run_direct ? RUN_DIRECT_HOLD : RUN_BACK_FRAMES))
            run_finish_handover();
        else if (!run_quiet()) psx_mod_set_loading_screen("ROGUE ARENA  Preparing your run", 98);
        return;
    }
    if (run_autopilot) {
        if (!run_boot_music && !psx_mod_fast_forward_requested()) psx_mod_request_fast_forward(1);
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
    (void)psx_game_register_offline_function_entry(RUN_LOADING_SCREEN, run_loading_screen_entry);
    (void)psx_game_register_offline_function_filter(RUN_SS_KEYON, run_keyon_filter);
    (void)psx_game_register_offline_function_filter(RUN_MUSIC_START, moa_music_start_filter);
    mod_register_frame_hook(run_frame_hook);
}
