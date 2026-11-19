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
 *  - The pilots' names from the match cards, for the hold-START menu's
 *    lines (moa_netplay_menu.c).
 *
 * There is no VS screen before the fight: it was dropped (user, 2026-11-18;
 * the host's never showed). The game's own screens show while the start
 * runs.
 *
 * Guest memory is written only from hooks the guest's own code runs, so both
 * PCs write the same bytes at the same instant and rollback replays them.
 * Everything else here (leaving at the end of the set) is this PC's own:
 * host statics the simulation never reads.
 */
#include "moa_netplay_room.h"
#include "moa_card.h"

#include "mod_plugins.h"
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
#define NP_ROLES_DONE    0x8003EA91u  /* link engine: roles settled (0 on a set's first round) */
#define NP_LEAVE_AFTER_MS 4500u       /* the set's last result on screen this long */

static const struct { int id; const char *name; } k_np_stages[] = {
    { 27, "GREEN SILO" }, { 39, "OCEAN PLATFORM" }, { 29, "SEABED" }, { 36, "NIGHT CITY" },
    { 25, "GARDEN PLAZA" }, { 20, "LUNAR BASE" }, { 22, "PILLAR HALL" }, { 24, "SCAFFOLD YARD" },
    { 33, "FOREST" },
};

static struct {
    int active, stage, time, rounds;
    /* the set (host statics: leaving only) */
    int fight_seen, decided, left;
    uint64_t decided_ms;
    char pilot[2][18];        /* for the hold-START menu */
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

/* ---- the pilots' names -------------------------------------------------------- */

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
    psx_mod_counter_add(done ? "moa.room.set_over" : "moa.room.set_left", 1);
    psx_request_return_to_lobby();
}

/* ---- for the hold-START menu (moa_netplay_menu.c) ------------------------------ */

int moa_np_room_stage_count(void)
{
    return (int)(sizeof k_np_stages / sizeof k_np_stages[0]);
}

int moa_np_room_stage_at(int i, const char **name)
{
    if (i < 0 || i >= moa_np_room_stage_count()) return 0;
    if (name) *name = k_np_stages[i].name;
    return k_np_stages[i].id;
}

int moa_np_room_stage_now(void)
{
    const int now = psx_mod_read_byte(NP_STAGE_P2);
    return now >= 20 && now <= 39 ? now : R.stage;
}

void moa_np_room_set_stage(int id)
{
    if (id < 20 || id > 39) return;
    psx_mod_write_byte(NP_STAGE_P1, (uint8_t)id);
    psx_mod_write_byte(NP_STAGE_P2, (uint8_t)id);
}

int moa_np_room_set_over(void)
{
    np_read_opts();
    if (!R.active) return 0;
    const unsigned w1 = psx_mod_read_half(NP_WINS_1P), w2 = psx_mod_read_half(NP_WINS_2P);
    const unsigned need = (unsigned)(R.rounds + 1) / 2u;
    return w1 >= need || w2 >= need;
}

const char *moa_np_room_pilot(int slot)
{
    if (slot < 0 || slot > 1 || !R.pilot[slot][0]) return slot ? "PILOT 2" : "PILOT 1";
    return R.pilot[slot];
}

void moa_np_room_leave(void)
{
    if (!R.active) return;
    np_leave(R.decided);
}

/* ---- the hooks' side ------------------------------------------------------------ */

/* The boot's title: a new session. */
static void np_session_reset(void)
{
    memset(&R, 0, sizeof R);
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
    /* The pilots' names, for the hold-START menu (this PC's screen only). */
    for (int k = 0; k < 2; k++) {
        struct moa_card unit;
        const int have = np_unit(k, &unit);
        np_name(R.pilot[k], have ? unit.pilot : NULL, k ? "PLAYER 2" : "PLAYER 1");
    }
    psx_mod_counter_add("moa.room.match", 1);
}

void moa_np_room_link_start(void)
{
    np_read_opts();
    if (!R.active) return;
    /* The set's first round (roles flag clear: the exe came from the menu):
     * the room's stage. Later rounds (the exe reloads the engine straight
     * from CONTINUE?, the flag stays set): the stage the set is on now, kept
     * in player 2's block. The engine restores only player 1's block at its
     * exit, and the cable exchange that would refill player 2's is skipped,
     * so it survives the reload; CHANGE STAGE writes it there
     * (moa_netplay_menu.c). The engine reads player 1's stage on even rounds
     * and player 2's on odd ones, so both get it. */
    int stage = R.stage;
    if (psx_mod_read_byte(NP_ROLES_DONE)) {
        const int now = psx_mod_read_byte(NP_STAGE_P2);
        if (now >= 20 && now <= 39) stage = now;
    }
    if (stage < 20 || stage > 39) return;
    psx_mod_write_byte(NP_STAGE_P1, (uint8_t)stage);
    psx_mod_write_byte(NP_STAGE_P2, (uint8_t)stage);
}

void moa_np_room_fight_frame(void)
{
    if (!R.active || R.left) return;
    const uint64_t now = np_now_ms();
    R.fight_seen = 1;
    const unsigned w1 = psx_mod_read_half(NP_WINS_1P), w2 = psx_mod_read_half(NP_WINS_2P);
    const unsigned need = (unsigned)(R.rounds + 1) / 2u;
    const int over = w1 >= need || w2 >= need;
    if (over && !R.decided) { R.decided = 1; R.decided_ms = now; np_write_result(1); }
    if (!over && R.decided) { R.decided = 0; np_write_result(0); }  /* rollback took it back */
    if (R.decided && now - R.decided_ms >= NP_LEAVE_AFTER_MS) np_leave(1);
}

/* LOADING = FAST (fastdisc=N): the fast drive covers the boot and the menu's
 * setup only (user, 2026-11-19: "switch back prior to VS START"); VS START's
 * AC graphics and the fight's load run at the drive's real speed, as do the
 * rest of the session's loads. Called only from guest hooks, so the switch
 * lands at the same emulated instant on both PCs and rollback replays it;
 * every condition is guest-visible: the drive's speed (in the snapshots) and
 * the settled options (the same on every PC). A data read in progress
 * refuses it (returns 0): the caller tries again on a later frame. */
int moa_np_room_fast_load_end(void)
{
    if (psx_game_disc_speed() == 1) return 1;   /* real speed already (or ORIGINAL) */
    const char *o = getenv("PSX_NET_GAME_OPTS");
    if (!o || np_opt(o, "room", 0) != 1) return 1;
    if (!psx_game_set_disc_speed_in_hook(1u, 0u)) return 0;
    psx_mod_counter_add("moa.room.fast_load_off", 1);
    return 1;
}

/* The same in the fight, should the start have reached it another way (the
 * menu handed to the players after the direct start gave up). */
void moa_np_room_disc_frame(void)
{
    (void)moa_np_room_fast_load_end();
}

void moa_np_room_menu_back(void)
{
    if (!R.active || R.left || !R.fight_seen) return;
    np_leave(R.decided);
}
