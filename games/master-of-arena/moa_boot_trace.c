/* moa_boot_trace.c — Rogue Arena boot trace (diagnostic, 2026-11-17).
 *
 * Why: the direct boot (moa_arena_run_boot.c, FAST BOOT in the launcher)
 * skips the title, LOAD GAME and card screens. With it, the hub has no
 * backdrop and the player's AC is missing in the first fight (it appears in
 * the second). The menu route has neither problem. This trace shows what the
 * menu route runs that the direct boot doesn't.
 *
 * What: only when PSX_MOA_BOOT_TRACE=1 (off by default) and Rogue Arena is
 * on, from the boot (menu entry with the
 * "go to the title" flag 0x8003EA7A still 1) until the second fight starts:
 *   <run file>.boot-<menu|direct>.csv  one row per call of the routines in
 *                                       k_bt (frame, address, name, a0-a3, ra);
 *                                       repeats of the same row are counted
 *   <run file>.boot-<route>.<tag>.ram  main RAM, 2 MB
 *   <run file>.boot-<route>.<tag>.vram VRAM, 1024 x 512 16-bit (CPU mirror)
 * with tag "hub" (the Arena is opened from the hub), "fight1", "fight2"
 * (first sampler call of that fight).
 *
 * Menu-module routines are only logged while the menu is in memory: every
 * engine loads at the same address, so each entry is checked against the
 * routine's first word (read from the USA disc 1, FDAT 206). Read-only:
 * nothing in the game changes.
 *
 * Every address here must also be in game.toml mod_function_entry_funcs, or
 * compiled code never calls the hook.
 */
#include "moa_boot_trace.h"
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t s_frame_count;
extern const uint16_t *gpu_get_vram(void);

#define BT_TITLE_FLAG   0x8003EA7Au   /* 1 until the first title opened: a fresh boot */
#define BT_MENU_ENTRY   0x800A69C8u
#define BT_ARENA_OPEN   0x800BA64Cu
#define BT_SAMPLER      0x8007F69Cu   /* arena engine controller sampler */
#define BT_SIG_SAMPLER  0x27BDFFE8u
#define BT_ROWS_MAX     40000u

static const struct { uint32_t a, sig; const char *name; } k_bt[] = {
    /* menu module (FDAT 206 at 0x80050DD8): entry, title, Scenario row */
    { 0x800A69C8u, 0x27BDFEB0u, "menu_entry" },
    { 0x800AAE94u, 0x27BDFFE0u, "title_open" },
    { 0x800AB500u, 0x27BDFFC8u, "title_scenario_row" },
    { 0x800544A0u, 0x27BDFFC8u, "intro_movie" },
    /* loaders: pictures, MENU_TIM entries, models, sound banks, hub ring (boot-loads.md) */
    { 0x80057CB0u, 0x27BDFFE8u, "menu_pictures" },
    { 0x800586A0u, 0x27BDFFD8u, "menu_tim_load" },
    { 0x80057ED4u, 0x27BDFFD8u, "menu_start_setup" },
    { 0x80057F74u, 0x27BDFFD0u, "model_load" },        /* (slot, MENU_TMD entry) */
    { 0x80058700u, 0x27BDFFC8u, "sound_bank" },        /* MENU_VAB n, n+1 */
    { 0x80057B28u, 0x2484FFFEu, "tasks_clear" },
    { 0x80057554u, 0x3C02801Bu, "fn_80057554" },
    { 0x80056DBCu, 0x27BDFFE8u, "hub_ring_task" },
    { 0x80056564u, 0x27BDFFD0u, "hub_open_layer" },
    { 0x800552C0u, 0x27BDFFB8u, "hub_open" },
    { 0x80056528u, 0x27BDFFE8u, "hub_back" },
    /* LOAD GAME and the card screen */
    { 0x800AC8E8u, 0x27BDFFE8u, "load_game" },
    { 0x80059554u, 0x27BDFFC8u, "card_screen" },
    { 0x800AC93Cu, 0x27BDFFD0u, "load_callback" },
    { 0x8005CE68u, 0x27BDFFB8u, "card_finish" },       /* after a good LOAD */
    { 0x80072E54u, 0x27BDFDE0u, "import_unpack" },
    /* after-load calls and the Scenario row's calls (both replayed by the direct boot) */
    { 0x800B6A80u, 0x27BDFFD8u, "after_load_1" },
    { 0x80099098u, 0x3C02801Au, "after_load_2" },
    { 0x800941F4u, 0x24050018u, "after_load_3" },
    { 0x80093BC4u, 0x3C02801Au, "after_load_4" },
    { 0x800A2318u, 0x27BDFFE8u, "after_load_5" },
    { 0x800A2020u, 0x3C02801Au, "after_load_6" },
    { 0x800631FCu, 0x00002821u, "scenario_1" },
    { 0x800BA854u, 0x00003021u, "scenario_2" },
    { 0x800C566Cu, 0x27BDFFE0u, "scenario_3" },
    { 0x800B69F8u, 0x3C028005u, "scenario_4" },
    /* garage, arena, the way back from a fight */
    { 0x8007C6B0u, 0x27BDFFE0u, "garage_open" },
    { 0x80090FD8u, 0x27BDFFC8u, "garage_check" },
    { 0x80074DC0u, 0x27BDFFE8u, "screen_reset" },
    { 0x800633A4u, 0x00002821u, "garage_default" },
    { 0x800BA64Cu, 0x27BDFFE8u, "arena_open" },
    { 0x800C0E7Cu, 0x27BDFFC0u, "arena_return_vram" },
    { 0x800822B0u, 0x27BDFFE0u, "ac_textures" },
    /* Boot exe routines (LoadImage 0x8002C75C, MoveImage 0x8002C81C ...) are
     * not traced: the exe's C is generated ahead and committed, so a new hook
     * there needs that code regenerated. The VRAM snapshots cover them. */
};
#define BT_N (sizeof k_bt / sizeof k_bt[0])

static FILE *bt_f;
static int bt_on;                          /* tracing this session */
static char bt_base[1024];
static uint32_t bt_rows, bt_fights, bt_snap_hub;
static uint64_t bt_last_sampler;
/* The last row, held back so repeats can be counted. */
static struct { int have; uint32_t frame, a, args[4], ra; unsigned n; const char *name; } bt_prev;

static void bt_flush_prev(void)
{
    if (!bt_f || !bt_prev.have) return;
    fprintf(bt_f, "%u,%08X,%s,%08X,%08X,%08X,%08X,%08X,%u\n", bt_prev.frame, bt_prev.a,
            bt_prev.name, bt_prev.args[0], bt_prev.args[1], bt_prev.args[2], bt_prev.args[3],
            bt_prev.ra, bt_prev.n);
    bt_prev.have = 0;
    bt_rows++;
}

static void bt_note(const char *what)
{
    if (!bt_f) return;
    bt_flush_prev();
    fprintf(bt_f, "%u,,# %s,,,,,,\n", (unsigned)s_frame_count, what);
    fflush(bt_f);
}

static void bt_end(const char *why)
{
    if (!bt_f) { bt_on = 0; return; }
    bt_note(why);
    fclose(bt_f);
    bt_f = NULL;
    bt_on = 0;
}

static void bt_begin(void)
{
    if (bt_f) bt_end("a new boot");
    /* Opt-in since 2026-11-20: the trace writes about 9 MB (three RAM and
     * VRAM snapshots) at the hub and the first two fights, a hitch every
     * Rogue Arena boot. Set PSX_MOA_BOOT_TRACE=1 to record it. */
    const char *want = getenv("PSX_MOA_BOOT_TRACE");
    if (!want || want[0] != '1') return;
    const char *on = getenv("PSX_MOA_ARENA_RUN");
    const char *run = getenv("PSX_MOA_RUN_FILE");
    if (!on || on[0] != '1' || !run || !run[0]) return;
    const char *d = getenv("PSX_MOA_RUN_DIRECT");
    const char *route = (d && d[0] == '0') ? "menu" : "direct";   /* as run_direct_wanted */
    snprintf(bt_base, sizeof bt_base, "%s.boot-%s", run, route);
    char p[1100];
    snprintf(p, sizeof p, "%s.csv", bt_base);
    bt_f = fopen(p, "w");
    if (!bt_f) return;
    fprintf(bt_f, "# Rogue Arena boot trace, route %s (moa_boot_trace.c)\n"
                  "frame,address,name,a0,a1,a2,a3,ra,times\n", route);
    bt_on = 1;
    bt_rows = bt_fights = bt_snap_hub = 0;
    bt_last_sampler = 0;
    memset(&bt_prev, 0, sizeof bt_prev);
}

/* Main RAM and VRAM as they are now, beside the trace. */
static void bt_snapshot(const char *tag)
{
    char p[1100];
    static uint8_t ram[2u * 1024u * 1024u];
    for (uint32_t i = 0; i < sizeof ram; i += 4u) {
        const uint32_t w = psx_mod_read_word(0x80000000u + i);
        ram[i] = (uint8_t)w; ram[i + 1] = (uint8_t)(w >> 8);
        ram[i + 2] = (uint8_t)(w >> 16); ram[i + 3] = (uint8_t)(w >> 24);
    }
    snprintf(p, sizeof p, "%s.%s.ram", bt_base, tag);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(ram, 1, sizeof ram, f); fclose(f); }
    const uint16_t *v = gpu_get_vram();
    if (v) {
        snprintf(p, sizeof p, "%s.%s.vram", bt_base, tag);
        f = fopen(p, "wb");
        if (f) { fwrite(v, 2, 1024u * 512u, f); fclose(f); }
    }
    char what[64];
    snprintf(what, sizeof what, "snapshot %s%s", tag, v ? "" : " (no VRAM)");
    bt_note(what);
}

/* The last menu routines called, kept all session (not only during the
 * boot trace) for the run's hang watch (moa_btrace_recent). */
#define BT_RECENT 24u
static struct { uint32_t frame, ra; const char *name; } bt_recent[BT_RECENT];
static unsigned bt_recent_at, bt_recent_n;

static void bt_remember(uint32_t a, uint32_t ra)
{
    for (unsigned k = 0; k < BT_N; k++) {
        if (k_bt[k].a != a) continue;
        if (psx_mod_read_word(a) != k_bt[k].sig) return;
        const unsigned last = (bt_recent_at + BT_RECENT - 1u) % BT_RECENT;
        if (bt_recent_n && bt_recent[last].name == k_bt[k].name && bt_recent[last].ra == ra) {
            bt_recent[last].frame = (uint32_t)s_frame_count;   /* repeated: keep the latest */
            return;
        }
        bt_recent[bt_recent_at].frame = (uint32_t)s_frame_count;
        bt_recent[bt_recent_at].ra = ra;
        bt_recent[bt_recent_at].name = k_bt[k].name;
        bt_recent_at = (bt_recent_at + 1u) % BT_RECENT;
        if (bt_recent_n < BT_RECENT) bt_recent_n++;
        return;
    }
}

void moa_btrace_recent(char *out, unsigned size)
{
    unsigned o = 0;
    if (!out || !size) return;
    out[0] = 0;
    for (unsigned i = 0; i < bt_recent_n && o + 1 < size; i++) {
        const unsigned k = (bt_recent_at + BT_RECENT - bt_recent_n + i) % BT_RECENT;
        const int w = snprintf(out + o, size - o, "%s%u:%s", i ? " " : "",
                               (unsigned)bt_recent[k].frame, bt_recent[k].name);
        if (w < 0) break;
        o += (unsigned)w;
    }
}

static void bt_entry(struct CPUState *cpu, uint32_t address)
{
    const uint32_t a = (address & 0x1FFFFFFFu) | 0x80000000u;
    bt_remember(a, cpu->gpr[31]);
    if (a == BT_MENU_ENTRY && psx_mod_read_word(a) == k_bt[0].sig &&
        psx_mod_read_byte(BT_TITLE_FLAG) == 1u)
        bt_begin();                                 /* a fresh boot: a new trace */
    if (!bt_on || !bt_f) return;
    unsigned k = 0;
    while (k < BT_N && k_bt[k].a != a) ++k;
    if (k == BT_N) return;
    if (psx_mod_read_word(a) != k_bt[k].sig) return;   /* not the menu's code */
    const uint32_t frame = (uint32_t)s_frame_count;
    const uint32_t args[4] = { cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7] };
    const uint32_t ra = cpu->gpr[31];
    if (bt_prev.have && bt_prev.a == a && bt_prev.ra == ra &&
        !memcmp(bt_prev.args, args, sizeof args)) {
        bt_prev.n++;
    } else {
        bt_flush_prev();
        bt_prev.have = 1;
        bt_prev.frame = frame; bt_prev.a = a; bt_prev.ra = ra; bt_prev.n = 1;
        bt_prev.name = k_bt[k].name;
        memcpy(bt_prev.args, args, sizeof args);
    }
    if (a == BT_ARENA_OPEN && !bt_snap_hub) {        /* the hub, as it was drawn */
        bt_snap_hub = 1;
        bt_snapshot("hub");
    }
    if (bt_rows >= BT_ROWS_MAX) bt_end("row limit");
}

static void bt_sampler(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!bt_on || !bt_f || psx_mod_read_word(address) != BT_SIG_SAMPLER) return;
    const uint64_t now = s_frame_count;
    const int fresh = !bt_last_sampler || now - bt_last_sampler > 60u;
    bt_last_sampler = now;
    if (!fresh) return;
    bt_fights++;
    char tag[16];
    snprintf(tag, sizeof tag, "fight%u", (unsigned)bt_fights);
    bt_snapshot(tag);
    if (bt_fights >= 2u) bt_end("second fight started: trace done");
}

void moa_btrace_register(void)
{
    for (unsigned k = 0; k < BT_N; k++)
        (void)psx_game_register_offline_function_entry(k_bt[k].a, bt_entry);
    (void)psx_game_register_offline_function_entry(BT_SAMPLER, bt_sampler);
}
