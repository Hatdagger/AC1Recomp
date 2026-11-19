/* Master of Arena: the launcher's versus room, in the game (netplay only).
 *
 * The room (recomp-ui moa_menu_net.inc) settles the match at START into
 * PSX_NET_GAME_OPTS, identical on both PCs:
 *
 *   room=1;stage=S;time=T;rounds=R;garage=G;fastdisc=N
 *
 * and puts each pilot's AC alone on that pilot's match card (slot 1 = host,
 * slot 2 = guest), which moa_vs_direct.c's netplay start loads. `garage` is
 * the room's own rule and `fastdisc` the runtime's (main.cpp: the drive's
 * speed for the whole session); this file adds the rest to that start:
 *
 *  - TIME, the VS TIME LIMIT option byte (0x8004B59C: 0 = 2:00, 1 = 5:00,
 *    2 = none), written when the boot's title becomes the versus menu, so VS
 *    START compiles both players with it (stat block +0x00,
 *    docs/mapping/stat-block.md).
 *  - STAGE (id 20..39) into both player blocks +0x255 (0x800453ED /
 *    0x8004578D) when the link engine starts its match, after VS START wrote
 *    the menu's own map there. The link engine loads a stage's 5s+0 and 5s+4
 *    entries, which every stage has (claude/netplay-room-rework-handover.md).
 *  - ROUNDS (1, 3 or 5): the set is over when a pilot has (R + 1) / 2 wins,
 *    read from the win table the versus engines keep in the exe's data
 *    (0x8003EAB0 1P wins, 0x8003EAB4 2P wins; docs/mapping/battle-system.md,
 *    measured in the split-screen engine). The result goes to the file in
 *    PSX_MOA_ROOM_RESULT for the room, then both PCs go back to the room.
 *    Between rounds the game's own CONTINUE? answers (X: next round); O there
 *    ends the set early (the versus menu opens: back to the room).
 *  - The VS screen (moa_screen.c, Rogue Arena's): both ACs, their pilots,
 *    stage, time and score, over the start and each round's load, until the
 *    fight runs. Renders from the 3D model install (PSX_MOA_MODELS_DIR), off
 *    the game's thread (moa_render_queue.c); without it, the emblems.
 *
 * Guest memory is written only from hooks the guest's own code runs, so both
 * PCs write the same bytes at the same instant and rollback replays them.
 * Everything else here (the screen, leaving at the end of the set) is this
 * PC's own: host statics the simulation never reads.
 */
#include "moa_netplay_room.h"
#include "moa_card.h"
#include "moa_screen.h"
#include "moa_render_queue.h"

#include "mod_plugins.h"
#include "cpu_state.h"
#include "memcard.h"
#include "psx_scheduler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define NP_TIME_OPTION   0x8004B59Cu  /* options record +0x34: VS TIME LIMIT */
#define NP_STAGE_P1      0x800453EDu  /* player 1 block +0x255 */
#define NP_STAGE_P2      0x8004578Du  /* player 2 block +0x255 */
#define NP_WINS_1P       0x8003EAB0u  /* u16, the versus win table */
#define NP_WINS_2P       0x8003EAB4u
#define NP_ENGINE_LOAD   0x80012348u  /* exe: loading screen before an engine */
#define NP_SIG_ENGINE_LOAD 0x27BDFF60u  /* addiu sp,sp,-160 */
#define NP_DISC_READ     0x800167A0u  /* exe: archive sector read */
#define NP_RENDER        900          /* VS screen portrait renders, square */
#define NP_LEAVE_AFTER_MS 4500u       /* the set's last result on screen this long */

static const struct { int id; const char *name; } k_np_stages[] = {
    { 27, "GREEN SILO" }, { 39, "OCEAN PLATFORM" }, { 29, "SEABED" }, { 36, "NIGHT CITY" },
    { 25, "GARDEN PLAZA" }, { 20, "LUNAR BASE" }, { 22, "PILLAR HALL" }, { 24, "SCAFFOLD YARD" },
    { 33, "FOREST" },
};

static struct {
    int active, stage, time, rounds;
    /* the set (host statics: leaving and the screen only) */
    int fight_seen, decided, left, round;
    uint64_t decided_ms;
    /* the VS screen */
    int vs_up;
    unsigned vs_key;
    uint64_t vs_open_ms, vs_shut_ms, last_draw_ms;
    int have[2], has_emblem[2];
    struct moa_card unit[2];
    uint32_t emblem[2][64 * 64];
    char pilot[2][18], ac[2][18];
    moa_rq_job job[2];
    uint8_t *px[2];
} R;

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

/* One integer from the settled options; `fallback` when absent. */
static int np_opt(const char *opts, const char *key, int fallback)
{
    const size_t n = strlen(key);
    for (const char *p = opts; p && *p;) {
        if (strncmp(p, key, n) == 0 && p[n] == '=') return atoi(p + n + 1);
        const char *semi = strchr(p, ';');
        p = semi ? semi + 1 : NULL;
    }
    return fallback;
}

/* The settled options (the same text on both PCs for the whole session). */
static void np_read_opts(void)
{
    const char *o = getenv("PSX_NET_GAME_OPTS");
    R.active = o && strncmp(o, "room=1", 6) == 0;
    if (!R.active) return;
    R.stage = np_opt(o, "stage", 27);
    R.time = np_opt(o, "time", 0);
    R.rounds = np_opt(o, "rounds", 1);
    if (R.rounds != 3 && R.rounds != 5) R.rounds = 1;
}

int moa_np_room_active(void)
{
    np_read_opts();
    return R.active;
}

static const char *np_stage_name(int id)
{
    for (size_t i = 0; i < sizeof k_np_stages / sizeof k_np_stages[0]; i++)
        if (k_np_stages[i].id == id) return k_np_stages[i].name;
    return "STAGE";
}

/* ---- the VS screen ------------------------------------------------------------ */

static int np_font(void)
{
    static int state;                    /* 0 not tried, 1 ok, -1 failed */
    static uint8_t *tim;
    if (state) return state > 0;
    state = -1;
    uint32_t size = 0, got = 0;
    if (!psx_mod_read_disc_file("GG/MS/MENU_TIM.T", NULL, 0, &size) || size < 4096u) return 0;
    tim = (uint8_t *)malloc(size);
    if (tim && psx_mod_read_disc_file("GG/MS/MENU_TIM.T", tim, size, &got) &&
        moa_screen_load_font(tim, got))
        state = 1;
    return state > 0;
}

/* A card's emblem as 0xAARRGGBB (colour value 0 is see-through, as on the PS1). */
static void np_emblem(const struct moa_card *c, uint32_t out[64 * 64])
{
    for (unsigned i = 0; i < 64u * 64u; i++) {
        const uint32_t v = c->palette[c->emblem[i] & 15u];
        const uint32_t r = (v & 31u) << 3, g = ((v >> 5) & 31u) << 3, b = ((v >> 10) & 31u) << 3;
        out[i] = (v ? 0xFF000000u : 0u) | (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5);
    }
}

/* Match card `slot`'s unit (slot 0 = player 1, the host's; 1 = the guest's). */
static int np_unit(int slot, struct moa_card *out)
{
    uint8_t *mc = (uint8_t *)malloc(131072);
    if (!mc) return 0;
    const int ok = memcard_export_raw(slot, mc) == 0 && moa_memcard_first_unit(mc, out) != 0;
    free(mc);
    return ok;
}

static void np_name(char dst[18], const char *src, const char *fallback)
{
    snprintf(dst, 18, "%s", src && src[0] ? src : fallback);
    for (char *p = dst; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
}

static void np_vs_close(void)
{
    if (!R.vs_up) return;
    R.vs_up = 0;
    R.vs_shut_ms = 0;
    psx_game_netplay_loading_screen(NULL, 0);
    psx_mod_counter_add("moa.room.vs_closed", 1);
}

static void np_vs_draw(int force)
{
    if (!R.vs_up) return;
    /* The emulation waits while the picture is made (both PCs, in step):
     * about 12 pictures a second while it stands by, 30 for the shutters. */
    const uint64_t now = np_now_ms();
    if (!force && now - R.last_draw_ms < (R.vs_shut_ms ? 33u : 80u)) return;
    R.last_draw_ms = now;
    for (int k = 0; k < 2; k++) {                          /* renders as they land */
        if (!R.job[k]) continue;
        uint8_t *px = NULL;
        const int r = moa_rq_take(R.job[k], &px);
        if (r == 1) { R.job[k] = 0; free(R.px[k]); R.px[k] = px; R.vs_key++; }
        else if (r < 0) R.job[k] = 0;
    }
    const float sh = R.vs_shut_ms ? (float)(now - R.vs_shut_ms) / 1000.0f : -1.0f;
    if (sh >= MOA_VS_SHUTTER_MEET + 0.12f) { np_vs_close(); return; }
    struct moa_screen_vs v;
    memset(&v, 0, sizeof v);
    v.key = R.vs_key;
    for (int k = 0; k < 2; k++) {
        struct moa_screen_vs_side *s = k ? &v.foe : &v.you;
        s->rgba = R.px[k];
        s->size = NP_RENDER;
        s->emblem = R.has_emblem[k] ? R.emblem[k] : NULL;
        s->pilot = R.pilot[k];
        s->ac = R.ac[k];
    }
    v.band = 0xF0A020u;
    v.t = (float)(now - R.vs_open_ms) / 1000.0f;
    v.standby = sh < 0.0f;
    v.shutter = sh;
    v.mode = "NETWORK VERSUS";
    v.title = R.rounds == 5 ? "BEST OF 5" : R.rounds == 3 ? "BEST OF 3" : "VERSUS";
    char big[8], foot[96];
    if (R.rounds > 1) {
        snprintf(big, sizeof big, "%d", R.round);
        v.center_small = "ROUND";
        v.center_big = big;
    }
    static const char *const times[3] = { "2:00", "5:00", "NO LIMIT" };
    const unsigned w1 = psx_mod_read_half(NP_WINS_1P), w2 = psx_mod_read_half(NP_WINS_2P);
    if (R.rounds > 1 && R.round > 1)
        snprintf(foot, sizeof foot, "%s    TIME %s    %u - %u", np_stage_name(R.stage),
                 times[R.time >= 0 && R.time <= 2 ? R.time : 0], w1, w2);
    else
        snprintf(foot, sizeof foot, "%s    TIME %s", np_stage_name(R.stage),
                 times[R.time >= 0 && R.time <= 2 ? R.time : 0]);
    v.footer = foot;
    int ww = 0, wh = 0, w = 0, h = 0;
    psx_mod_window_size(&ww, &wh);
    const uint32_t *px = moa_screen_render_vs(ww, wh, &v, &w, &h);
    if (!px) { np_vs_close(); return; }
    psx_mod_set_loading_image(px, w, h);
    psx_game_netplay_loading_screen("VERSUS", -1);
}

static void np_vs_open(void)
{
    if (!np_font()) return;
    R.vs_up = 1;
    R.vs_key++;
    R.vs_open_ms = np_now_ms();
    R.vs_shut_ms = 0;
    R.last_draw_ms = 0;
    np_vs_draw(1);
    psx_mod_counter_add("moa.room.vs_open", 1);
}

/* ---- leaving at the end of the set -------------------------------------------- */

/* The set's result for the room. Written the moment the set is decided too:
 * the PC that leaves first ends the session for both, and the other one goes
 * back through the runtime's "the other player left", not np_leave. */
static void np_write_result(int done)
{
    const unsigned w1 = psx_mod_read_half(NP_WINS_1P), w2 = psx_mod_read_half(NP_WINS_2P);
    const char *path = getenv("PSX_MOA_ROOM_RESULT");
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "p1=%u;p2=%u;rounds=%d;done=%d\n", w1, w2, R.rounds, done ? 1 : 0);
    fclose(f);
}

static void np_leave(int done)
{
    if (R.left) return;
    R.left = 1;
    np_write_result(done);
    np_vs_close();
    psx_mod_counter_add(done ? "moa.room.set_over" : "moa.room.set_left", 1);
    psx_request_return_to_lobby();
}

/* ---- the hooks' side ------------------------------------------------------------ */

/* The boot's title: a new session. */
static void np_session_reset(void)
{
    for (int k = 0; k < 2; k++) {
        if (R.job[k]) moa_rq_drop(R.job[k]);
        R.job[k] = 0;
        free(R.px[k]);
        R.px[k] = NULL;
    }
    if (R.vs_up) np_vs_close();
    const unsigned key = R.vs_key;
    memset(&R, 0, sizeof R);
    /* The VS screen keeps its still layer per key, and Rogue Arena's keys
     * (an offline session earlier in this process) count up from 0 too:
     * this screen's keys live far from them. */
    R.vs_key = 0x4E500000u + ((key + 1u) & 0xFFFFFu);
}

void moa_np_room_session_start(void)
{
    np_session_reset();
    np_read_opts();
}

void moa_np_room_title(void)
{
    np_read_opts();
    if (!R.active) return;
    /* Guest memory, before VS START: the time limit both players compile. */
    if (R.time >= 0 && R.time <= 2) psx_mod_write_byte(NP_TIME_OPTION, (uint8_t)R.time);
    /* The two units, for the screen only. */
    const char *mdir = getenv("PSX_MOA_MODELS_DIR");
    for (int k = 0; k < 2; k++) {
        R.have[k] = np_unit(k, &R.unit[k]);
        if (!R.have[k]) { np_name(R.pilot[k], NULL, k ? "PLAYER 2" : "PLAYER 1"); continue; }
        np_name(R.pilot[k], R.unit[k].pilot, k ? "PLAYER 2" : "PLAYER 1");
        np_name(R.ac[k], R.unit[k].ac, "AC");
        np_emblem(&R.unit[k], R.emblem[k]);
        R.has_emblem[k] = 1;
        if (mdir && mdir[0])
            R.job[k] = moa_rq_submit(mdir, R.unit[k].rec, R.unit[k].paint, NP_RENDER,
                                     k ? -0.55f : 0.55f, 0.06f, 1.3f);
    }
    R.round = 1;
    np_vs_open();
    psx_mod_counter_add("moa.room.match", 1);
}

void moa_np_room_link_start(void)
{
    np_read_opts();
    if (!R.active || R.stage < 20 || R.stage > 39) return;
    psx_mod_write_byte(NP_STAGE_P1, (uint8_t)R.stage);
    psx_mod_write_byte(NP_STAGE_P2, (uint8_t)R.stage);
    np_vs_draw(0);
}

void moa_np_room_fight_frame(void)
{
    if (!R.active || R.left) return;
    const uint64_t now = np_now_ms();
    if (R.vs_up && !R.vs_shut_ms) R.vs_shut_ms = now;       /* the fight runs: shut */
    np_vs_draw(0);
    R.fight_seen = 1;
    const unsigned w1 = psx_mod_read_half(NP_WINS_1P), w2 = psx_mod_read_half(NP_WINS_2P);
    const unsigned need = (unsigned)(R.rounds + 1) / 2u;
    const int over = w1 >= need || w2 >= need;
    if (over && !R.decided) { R.decided = 1; R.decided_ms = now; np_write_result(1); }
    if (!over && R.decided) { R.decided = 0; np_write_result(0); }  /* rollback took it back */
    if (R.decided && now - R.decided_ms >= NP_LEAVE_AFTER_MS) np_leave(1);
}

void moa_np_room_engine_load(void)
{
    if (!R.active || R.left || !R.fight_seen) return;      /* the start has its own screen */
    if (R.decided || R.rounds <= 1) return;
    const unsigned w1 = psx_mod_read_half(NP_WINS_1P), w2 = psx_mod_read_half(NP_WINS_2P);
    R.round = (int)(w1 + w2) + 1;
    np_vs_open();
}

void moa_np_room_menu_back(void)
{
    if (!R.active || R.left || !R.fight_seen) return;
    np_leave(R.decided);
}

void moa_np_room_tick(void)
{
    if (R.active && R.vs_up) np_vs_draw(0);
}

/* The exe's loading screen before an engine and its sector reads: the screen's
 * redraws while the disc works. Code checks: the exe is always in memory. */
static void np_engine_load_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (psx_mod_read_word(address) != NP_SIG_ENGINE_LOAD) return;
    moa_np_room_engine_load();
    moa_np_room_tick();
}

static void np_disc_read_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    moa_np_room_tick();
}

PSX_MOD_CONSTRUCTOR(moa_netplay_room_register)
{
    (void)psx_game_register_netplay_function_entry(NP_ENGINE_LOAD, np_engine_load_entry);
    (void)psx_game_register_netplay_function_entry(NP_DISC_READ, np_disc_read_entry);
}
