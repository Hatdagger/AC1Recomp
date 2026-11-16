/* Master of Arena: netplay auto-setup.
 *
 * When the host's title options (PSX_NET_GAME_OPTS, settled for every peer
 * at match start) carry "autosetup=1", both PCs go from boot to the fight by
 * themselves, controllers locked:
 *
 *   boot -> versus menu (START taps, logos and intro skipped, VS Mode forced),
 *   a hold while the peers settle, 1P AC SETUP > SYSTEM > DATA LOAD > SLOT 1 >
 *   save 1 on controller 1, 2P the same from SLOT 2 on controller 2, a three
 *   second countdown, then VS START. The stage ("stage=N", default Green
 *   Silo) is written into both player blocks when the link engine starts.
 *
 * Every input is written into the menu's pad buffers inside the simulation,
 * the same on both peers. Rollback: the script's state is kept per
 * simulation tick (a ring indexed by psx_netplay_sim_tick), and a tick that
 * runs again starts from the state it had the first time, so a resimulated
 * tick never advances the script twice.
 *
 * The overlay (the AI Battle loading picture with this mode's own steps) and
 * the countdown tones are presentation only, on each PC.
 *
 * Leaving the versus menu (to the title) or coming back to it after a match
 * ends the session for both peers: back to the launcher's netplay room.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#include "mod_plugins.h"
#include "cpu_state.h"
#include "psx_netplay.h"
#include "psx_scheduler.h"
#include "moa_card.h"
#include "moa_screen.h"

#define NP_MENU_ENTRY      0x800A69C8u
#define NP_SIG_MENU_ENTRY  0x27BDFEB0u
#define NP_MENU_INPUT_A    0x800639A4u
#define NP_SIG_INPUT_A     0x27BDFFE8u
#define NP_MENU_INPUT_B    0x800AF590u
#define NP_SIG_INPUT_B     0x27BDFFD0u
#define NP_MODE_CONFIRM    0x800AB500u
#define NP_SIG_MODE_CONFIRM 0x27BDFFC8u
#define NP_MODE_ROW_Y      80u
#define NP_MODE_ROW_VERSUS 192u
#define NP_MENU_FLAGS      0x801B1CD0u   /* bit 9: second controller port usable */
#define NP_TITLE_OPEN      0x800AAE94u
#define NP_SIG_TITLE_OPEN  0x27BDFFE0u
#define NP_MOVIE_PLAY      0x80054B54u
#define NP_SIG_MOVIE_PLAY  0x27BDFFD8u
#define NP_PAD1            0x800458D8u   /* status, id, buttons (low = held) */
#define NP_PAD2            0x800458FAu
#define NP_VS_ROW          0x801B2038u   /* 0 VS START, 1 1P AC SETUP, 2 2P AC SETUP */
#define NP_STAGE_P1        0x800453EDu   /* player blocks +597 */
#define NP_STAGE_P2        0x8004578Du
#define NP_STAGE_DEFAULT   27u           /* Green Silo */

#define PAD_START 0x0008u
#define PAD_UP    0x0010u
#define PAD_DOWN  0x0040u
#define PAD_X     0x4000u
#define PAD_O     0x2000u

/* ---- options ------------------------------------------------------------- */

static int np_opt(const char *key, int fallback)
{
    const char *o = getenv("PSX_NET_GAME_OPTS");
    const size_t n = strlen(key);
    while (o && *o) {
        if (strncmp(o, key, n) == 0 && o[n] == '=') return atoi(o + n + 1);
        o = strchr(o, ';');
        if (o) o++;
    }
    return fallback;
}

static int np_on(void)
{
    return psx_netplay_active() && np_opt("autosetup", 0) == 1;
}

/* ---- the script ---------------------------------------------------------- */

enum { K_WAIT, K_PRESS, K_ROW, K_LOAD, K_END };
enum { PH_INIT, PH_SYNC, PH_HOST, PH_GUEST, PH_COUNT, PH_DONE };
struct np_step { uint8_t kind, port, phase; uint16_t buttons, hold, wait; };

static struct np_step np_script[64];
static int np_script_len;

static void np_add(uint8_t kind, uint8_t phase, uint8_t port, uint16_t b, uint16_t hold, uint16_t wait)
{
    if (np_script_len >= (int)(sizeof np_script / sizeof np_script[0])) return;
    struct np_step s = { kind, port, phase, b, hold, wait };
    np_script[np_script_len++] = s;
}
#define TAP(ph, port, b, wait) np_add(K_PRESS, ph, port, b, 4, wait)

/* DATA LOAD of save 1 from the player's own slot: the AC SETUP list is
 * driven by that player's controller, the versus row by controller 1. */
static void np_script_load(unsigned player)
{
    const uint8_t ph = player ? PH_GUEST : PH_HOST, port = (uint8_t)(player + 1);
    np_add(K_ROW, ph, 1, (uint16_t)(1 + player), 0, 0);
    TAP(ph, 1, PAD_X, 40);
    for (int i = 0; i < 4; i++) TAP(ph, port, PAD_DOWN, 10);   /* SYSTEM */
    TAP(ph, port, PAD_X, 40);                                    /* DATA LOAD */
    TAP(ph, port, PAD_X, 70);                                    /* SLOT 1 / SLOT 2 */
    if (player) TAP(ph, port, PAD_DOWN, 12);                     /* SLOT 2 */
    /* X every 16 frames: the slot, save 1, "Load?" Yes -- each as soon as the
     * menu takes it -- until the save has been read (its checksum runs). */
    np_add(K_LOAD, ph, port, PAD_X, 4, 16);
    np_add(K_WAIT, ph, 0, 0, 0, 100);                            /* "Load completed." closes */
    TAP(ph, port, PAD_O, 30);                                    /* out of SYSTEM */
    TAP(ph, port, PAD_O, 90);                                   /* out of AC SETUP */
}

static void np_script_build(void)
{
    np_script_len = 0;
    np_add(K_WAIT, PH_INIT, 0, 0, 0, 240);                       /* ~4 s */
    np_add(K_WAIT, PH_SYNC, 0, 0, 0, 220);
    np_add(K_ROW, PH_SYNC, 1, 1, 0, 0);       /* the menu answering: it takes input */
    np_add(K_ROW, PH_SYNC, 1, 0, 0, 0);
    np_add(K_WAIT, PH_SYNC, 0, 0, 0, 100);                       /* ~6.5 s in all */
    np_script_load(0);
    np_script_load(1);
    np_add(K_ROW, PH_COUNT, 1, 0, 0, 0);                         /* VS START row */
    np_add(K_WAIT, PH_COUNT, 0, 0, 0, 180);                      /* 3, 2, 1 */
    TAP(PH_COUNT, 1, PAD_X, 0);                                  /* VS START */
    np_add(K_END, PH_DONE, 0, 0, 0, 0);
}

/* ---- simulation state, kept per tick for rollback ------------------------ */

struct np_state {
    uint8_t autopilot, reached_versus, lock, ticked, row_answered;
    uint8_t matches;
    uint8_t loads, loads_mark;               /* save reads seen (checksum calls) */
    uint16_t inject[2];                      /* active high */
    uint32_t autopilot_ticks;
    int32_t pos;
    uint32_t step_time;
};
#define NP_RING 1024u
static struct { uint32_t tick; int used; struct np_state s; } np_ring[NP_RING];
static struct np_state S;
static uint32_t np_cur_tick;
static int np_have_tick;

static void np_reset(void)
{
    memset(&S, 0, sizeof S);
    memset(np_ring, 0, sizeof np_ring);
    np_have_tick = 0;
    np_cur_tick = 0;
}

/* Enter tick t: a tick seen before starts again from its saved state. */
static void np_enter_tick(void)
{
    const uint32_t t = psx_netplay_sim_tick();
    if (np_have_tick && t == np_cur_tick) return;
    const uint32_t i = t % NP_RING;
    if (np_ring[i].used && np_ring[i].tick == t) {
        S = np_ring[i].s;
        if (t != np_cur_tick + 1u) psx_mod_counter_add("moa.np.setup_rewound", 1);
    } else {
        if (np_have_tick && t != np_cur_tick + 1u) psx_mod_counter_add("moa.np.setup_tick_gap", 1);
        np_ring[i].used = 1;
        np_ring[i].tick = t;
        np_ring[i].s = S;
    }
    S.ticked = 0;
    np_cur_tick = t;
    np_have_tick = 1;
}

static void np_script_step(void)
{
    if (!S.lock || S.pos >= np_script_len) return;
    const struct np_step *st = &np_script[S.pos];
    const uint32_t t = S.step_time++;
    S.inject[0] = S.inject[1] = 0;
    switch (st->kind) {
    case K_WAIT:
        if (t + 1 >= st->wait) { S.pos++; S.step_time = 0; }
        return;
    case K_PRESS:
        if (t < st->hold) S.inject[st->port - 1] = st->buttons;
        if (t + 1 >= (uint32_t)st->hold + st->wait) { S.pos++; S.step_time = 0; }
        return;
    case K_LOAD:
        if (t == 0) S.loads_mark = S.loads;
        if (S.loads != S.loads_mark) { S.pos++; S.step_time = 0; return; }
        if (t > 3000) {                       /* never read: hand over */
            psx_mod_counter_add("moa.np.setup_load_timeout", 1);
            S.lock = 0;
            S.pos = np_script_len;
            return;
        }
        if (t % st->wait < st->hold) S.inject[st->port - 1] = st->buttons;
        return;
    case K_ROW: {
        const int row = psx_mod_read_byte(NP_VS_ROW);
        if (row == st->buttons && t >= 20) { S.pos++; S.step_time = 0; return; }
        if (t > 4000) {                       /* the menu never answered: hand over */
            psx_mod_counter_add("moa.np.setup_row_timeout", 1);
            S.lock = 0;
            S.pos = np_script_len;
            return;
        }
        if (row != st->buttons && (t % 30u) < 4u) S.inject[0] = row < st->buttons ? PAD_DOWN : PAD_UP;
        return;
    }
    default:                                  /* VS START is in: controllers back */
        S.lock = 0;
        S.pos = np_script_len;
        psx_mod_counter_add("moa.np.setup_done", 1);
        return;
    }
}

static void np_end_session(const char *why)
{
    psx_mod_counter_add(why, 1);
    S.lock = 0;
    psx_request_return_to_lobby();            /* both peers get here on the same tick */
}

/* ---- hooks --------------------------------------------------------------- */

static void np_menu_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!np_on() || psx_mod_read_word(address) != NP_SIG_MENU_ENTRY) return;
    np_enter_tick();
    if (S.matches > 0) { np_end_session("moa.np.setup_return_after_match"); return; }
    S.autopilot = 1;
    S.autopilot_ticks = 0;
}

/* Called by moa_link_netplay.c's versus-menu hook (one hook per address). */
void moa_np_setup_menu_versus(void)
{
    if (!np_on()) return;
    np_enter_tick();
    S.autopilot = 0;
    if (S.reached_versus) return;
    S.reached_versus = 1;
    psx_mod_write_word(NP_MENU_FLAGS, psx_mod_read_word(NP_MENU_FLAGS) | 0x200u);
    S.pos = 0;
    S.step_time = 0;
    S.lock = 1;
    psx_mod_counter_add("moa.np.setup_script_start", 1);
}

/* Called by moa_link_netplay.c when the link engine starts (role answer and
 * the AC data trade): the chosen stage for both players. */
void moa_np_setup_link_start(void)
{
    if (!np_on()) return;
    int stage = np_opt("stage", NP_STAGE_DEFAULT);
    if (stage <= 0 || stage > 255) stage = NP_STAGE_DEFAULT;
    psx_mod_write_byte(NP_STAGE_P1, (uint8_t)stage);
    psx_mod_write_byte(NP_STAGE_P2, (uint8_t)stage);
    np_enter_tick();
    if (S.matches < 255) S.matches = 1;
}

static void np_menu_input(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!np_on()) return;
    const uint32_t word = psx_mod_read_word(address);
    if (word != NP_SIG_INPUT_A && word != NP_SIG_INPUT_B) return;
    np_enter_tick();
    if (!S.ticked) {                          /* the script moves once per tick */
        S.ticked = 1;
        if (S.autopilot) S.autopilot_ticks++;
        else np_script_step();
    }
    if (S.autopilot) {                        /* START tapped: 3 samples in 24 */
        const int held = (S.autopilot_ticks % 24u) < 3u;
        psx_mod_write_half(NP_PAD1 + 2u, held ? (uint16_t)~PAD_START : 0xFFFFu);
        psx_mod_write_half(NP_PAD2 + 2u, 0xFFFFu);
        return;
    }
    if (!S.lock) return;
    psx_mod_write_byte(NP_PAD1, 0);
    psx_mod_write_byte(NP_PAD1 + 1u, 0x41);
    psx_mod_write_half(NP_PAD1 + 2u, (uint16_t)~S.inject[0]);
    psx_mod_write_byte(NP_PAD2, 0);
    psx_mod_write_byte(NP_PAD2 + 1u, 0x41);
    psx_mod_write_half(NP_PAD2 + 2u, (uint16_t)~S.inject[1]);
}

/* The loader's checksum call: a save has just been read. */
#define NP_SAVE_SUM      0x80070F1Cu
#define NP_SIG_SAVE_SUM  0x00002821u
static void np_save_sum(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!np_on() || psx_mod_read_word(address) != NP_SIG_SAVE_SUM) return;
    np_enter_tick();
    S.loads++;
}

static void np_mode_confirm(struct CPUState *cpu, uint32_t address)
{
    if (!np_on() || psx_mod_read_word(address) != NP_SIG_MODE_CONFIRM) return;
    np_enter_tick();
    if (!S.autopilot) return;
    const uint32_t row = psx_mod_read_word(cpu->gpr[4] + 148u);
    if (row < 0x80000000u || row >= 0x80200000u) return;
    psx_mod_write_word(row + NP_MODE_ROW_Y, NP_MODE_ROW_VERSUS);
    psx_mod_write_word(NP_MENU_FLAGS, psx_mod_read_word(NP_MENU_FLAGS) | 0x200u);
}

static void np_title_open(struct CPUState *cpu, uint32_t address)
{
    if (!np_on() || psx_mod_read_word(address) != NP_SIG_TITLE_OPEN) return;
    np_enter_tick();
    if (S.reached_versus && !S.autopilot) { np_end_session("moa.np.setup_return_from_menu"); return; }
    if (S.autopilot) cpu->gpr[4] = 1;         /* no logos, no intro */
}

static int np_movie(struct CPUState *cpu, uint32_t address)
{
    if (!np_on() || psx_mod_read_word(address) != NP_SIG_MOVIE_PLAY) return 0;
    np_enter_tick();
    if (!S.autopilot) return 0;
    cpu->gpr[2] = 1;                          /* "skipped" */
    return 1;
}

/* ---- overlay and tones (each PC, presentation only) ---------------------- */

static uint64_t np_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static uint8_t *np_menu_tim;
static int np_font_tried;

static void np_font(void)
{
    if (moa_screen_ready() || np_font_tried) return;
    np_font_tried = 1;
    uint32_t size = 0, bytes = 0;
    if (!psx_mod_read_disc_file("GG/MS/MENU_TIM.T", NULL, 0, &size) || size < 4096u) return;
    np_menu_tim = (uint8_t *)malloc(size);
    if (!np_menu_tim) return;
    if (psx_mod_read_disc_file("GG/MS/MENU_TIM.T", np_menu_tim, size, &bytes))
        (void)moa_screen_load_font(np_menu_tim, bytes);
}

/* This PC's own AC (the launcher's PSX_MOA_NP_CARD) on its side of the
 * picture; the other side stays an empty frame. */
static void np_units(void)
{
    static int done;
    if (done) return;
    done = 1;
    static struct moa_screen_unit unit[2];
    memset(unit, 0, sizeof unit);
    const char *path = getenv("PSX_MOA_NP_CARD");
    const int me = psx_netplay_local_port() == 1 ? 1 : 0;
    FILE *f = path && path[0] ? fopen(path, "rb") : NULL;
    if (f) {
        static uint8_t data[4 * 1024 * 1024];
        static struct moa_card card;
        const size_t n = fread(data, 1, sizeof data, f);
        fclose(f);
        if (n > 0 && moa_card_decode(data, n, &card)) {
            struct moa_screen_unit *u = &unit[me];
            u->have = 1;
            snprintf(u->ac, sizeof u->ac, "%s", card.ac);
            snprintf(u->pilot, sizeof u->pilot, "%s", card.pilot);
            snprintf(u->style, sizeof u->style, "%s", me ? "GUEST" : "HOST");
            for (unsigned k = 0; k < 64u * 64u; k++) {
                const uint16_t c = card.palette[card.emblem[k] & 15u];
                u->emblem[k] = (uint32_t)(c & 31u) << 19 | (uint32_t)((c >> 5) & 31u) << 11 |
                               (uint32_t)((c >> 10) & 31u) << 3;
            }
        }
    }
    moa_screen_set_units(&unit[0], &unit[1]);
}

static int np_overlay_up;

static void np_overlay_off(void)
{
    if (!np_overlay_up) return;
    np_overlay_up = 0;
    psx_mod_set_game_audio_muted(0);
    psx_game_netplay_loading_screen(NULL, 0);
    psx_mod_set_loading_image(NULL, 0, 0);
    moa_screen_set_steps(NULL, 0);
}

static void np_frame_hook(void)
{
    static int last_digit = -1, go_played;
    if (!psx_netplay_active()) {              /* between sessions: start clean */
        if (np_have_tick) np_reset();
        last_digit = -1;
        go_played = 0;
        np_overlay_off();
        return;
    }
    if (!np_on()) { np_overlay_off(); return; }
    const int running = !S.reached_versus || S.lock;
    if (!running) {
        np_overlay_off();
        if (S.reached_versus && !go_played) { go_played = 1; psx_mod_play_tone(1320, 380); }
        return;
    }
    int phase = PH_INIT, digit = 0;
    if (S.reached_versus && S.pos < np_script_len) {
        const struct np_step *st = &np_script[S.pos];
        phase = st->phase;
        if (phase == PH_COUNT) {
            digit = 3;
            if (st->kind == K_WAIT) digit = 3 - (int)(S.step_time / 60u);
            else if (st->kind == K_PRESS) digit = 1;
            if (digit < 1) digit = 1;
        }
    }
    if (phase == PH_COUNT && digit != last_digit && !psx_netplay_is_resimulating()) {
        last_digit = digit;
        psx_mod_play_tone(880, 140);
    }
    static const char *const steps[5] = {
        "1 INITIALIZING", "2 SYNCING", "3 HOST AC", "4 GUEST AC", "5 BATTLE" };
    static const char *const status[5] = {
        "Initializing...", "Syncing machines and using Rollback Netcode...",
        "Populating Host AC Data...", "Populating Guest AC Data...", "Battle Starting" };
    char line[64];
    const char *text = status[phase > PH_COUNT ? PH_COUNT : phase];
    if (phase == PH_COUNT) { snprintf(line, sizeof line, "Battle Starting  %d", digit); text = line; }
    const int permille = phase * 200 + (phase == PH_COUNT ? (4 - digit) * 50 : 0);
    psx_game_netplay_loading_screen(text, permille / 10);
    if (!np_overlay_up) psx_mod_set_game_audio_muted(1);
    np_overlay_up = 1;
    np_font();
    if (!moa_screen_ready()) return;          /* the plain loading screen */
    static uint64_t last_ms;
    static int last_key = -1;
    const uint64_t now = np_now_ms();
    const int key = phase * 16 + digit;
    if (key == last_key && now - last_ms < 250u) return;
    last_key = key;
    last_ms = now;
    np_units();
    moa_screen_set_steps(steps, 5);
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render(ww, wh, text, phase > PH_COUNT ? PH_COUNT : phase,
                                           permille, 0, 0.0f, now, &w, &h);
    if (px) psx_mod_set_loading_image(px, w, h);
}

PSX_MOD_CONSTRUCTOR(moa_np_setup_register)
{
    np_script_build();
    (void)psx_game_register_netplay_function_entry(NP_MENU_ENTRY, np_menu_entry);
    (void)psx_game_register_netplay_function_entry(NP_MENU_INPUT_A, np_menu_input);
    (void)psx_game_register_netplay_function_entry(NP_MENU_INPUT_B, np_menu_input);
    (void)psx_game_register_netplay_function_entry(NP_MODE_CONFIRM, np_mode_confirm);
    (void)psx_game_register_netplay_function_entry(NP_SAVE_SUM, np_save_sum);
    (void)psx_game_register_netplay_function_entry(NP_TITLE_OPEN, np_title_open);
    (void)psx_game_register_netplay_function_filter(NP_MOVIE_PLAY, np_movie);
    mod_register_frame_hook(np_frame_hook);
}
