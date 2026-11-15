/* Master of Arena: Arena Run, straight to the hub.
 *
 * An Arena Run session goes from boot to the scenario hub by itself, the way
 * an AI Battle session goes to the versus menu (moa_ai_battle.c): fast
 * forward, the controllers locked out and a loading screen over the picture
 * while the menus are driven underneath.
 *
 *   boot -> logos and intro skipped (title opened with a0 = 1, movies
 *   "skipped"), START tapped through the title, the mode row forced to
 *   SCENARIO MODE; on LOAD GAME / NEW GAME the cursor is set to NEW GAME
 *   and X tapped (that menu ignores START); then START and X in turn through
 *   the name entry (defaults kept) -> the hub opens: the run's AC goes into
 *   the garage, the overlay fades and the player takes over.
 *
 * Every run session is a new scenario game: the run itself lives in the run
 * file (PSX_MOA_RUN_FILE), never in the player's own scenario save.
 *
 * If the hub does not come up in time the overlay is dropped and the
 * player is given the menus, so a wrong step can never leave a black screen.
 *
 * Offline only, and only when the launcher asked for it (PSX_MOA_ARENA_RUN=1).
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
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

#define RUN_GARAGE_PARTS     0x80036FB0u  /* 12 part ids, versus-menu order */
#define RUN_GARAGE_OPTIONS   0x8003EA70u

#define RUN_HUB_TIMEOUT      (60u * 90u)  /* frames; fast forward makes this short */
#define RUN_FADE_FRAMES      24u

static int run_enabled = -1;
static int run_autopilot;                 /* driving the menus */
static int run_at_hub;                    /* the player has the hub */
static uint32_t run_ticks, run_frames, run_fade;
static struct moa_run_state run_state;
static int run_state_ok;
enum { STAGE_TITLE = 0, STAGE_NEWGAME, STAGE_NAMES };
static int run_stage;
static uint32_t run_newgame_seen;          /* run_frames when that menu last ran */

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
    const char *path = getenv("PSX_MOA_RUN_FILE");
    run_state_ok = path && path[0] && moa_run_load(path, &run_state);
    if (!run_state_ok) { psx_mod_counter_add("moa.run.file_missing", 1); return; }
    const uint8_t *r = run_state.start_ac;
    const uint8_t booster = r[3] >= 27u && r[3] != 0xFFu ? 0xFFu : r[12];
    const uint8_t p[12] = { r[0], booster, r[1], r[2], r[5], r[4], r[6], r[7], r[9], r[8], 0xFFu, r[3] };
    for (unsigned i = 0; i < 12; i++) psx_mod_write_byte(RUN_GARAGE_PARTS + i, p[i]);
    psx_mod_write_half(RUN_GARAGE_OPTIONS, 0);
    /* Step 1 still to do here: money -> P, owned parts, arena rank. */
    psx_mod_counter_add("moa.run.state_applied", 1);
}

static void run_reach_hub(const char *why)
{
    if (!run_autopilot) return;
    run_autopilot = 0;
    run_at_hub = 1;
    run_apply_state();
    run_fade = RUN_FADE_FRAMES;
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
    psx_mod_write_byte(st + RUN_NEWGAME_CURSOR, 1);      /* NEW GAME, never the player's save */
    if (run_stage != STAGE_NEWGAME) psx_mod_counter_add("moa.run.new_game", 1);
    run_stage = STAGE_NEWGAME;
    run_newgame_seen = run_frames;
}

static void run_menu_input_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!run_autopilot || !run_on()) return;
    const uint32_t w = psx_mod_read_word(address);
    if (w != RUN_SIG_INPUT_A && w != RUN_SIG_INPUT_B) return;
    if (run_stage == STAGE_NEWGAME && run_frames - run_newgame_seen > 10u)
        run_stage = STAGE_NAMES;                         /* that menu has closed */
    /* One button held for 3 samples out of every 24: START on the title, X
     * on NEW GAME, then START and X in turn through the name entry.
     * CONFIRM the name entry on a test run. */
    const uint32_t t = run_ticks++;
    uint16_t b = RUN_PAD_START;
    if (run_stage == STAGE_NEWGAME) b = RUN_PAD_X;
    else if (run_stage == STAGE_NAMES) b = ((t / 24u) & 1u) ? RUN_PAD_X : RUN_PAD_START;
    psx_mod_write_half(RUN_PAD_BUFFER + 2u, (t % 24u) < 3u ? (uint16_t)~b : (uint16_t)0xFFFFu);
}

static void run_hub_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!run_on() || psx_mod_read_word(address) != RUN_SIG_HUB_RESUME) return;
    run_reach_hub("moa.run.hub_reached");
}

static void run_frame_hook(void)
{
    if (!run_on()) return;
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
    mod_register_frame_hook(run_frame_hook);
}
