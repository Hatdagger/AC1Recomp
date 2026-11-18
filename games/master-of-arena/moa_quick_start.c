/* Master of Arena: the launcher's START GAME choices.
 *
 *   PSX_MOA_START=load   LOAD GAME: the save in PSX_MOA_START_SAVE (an 8 KB
 *                        save block the launcher read from memory card 1)
 *                        goes into RAM and the hub opens. No logos, title,
 *                        NEW/LOAD menu or card screens.
 *   PSX_MOA_START=new    NEW GAME: straight to the game's name entry.
 *   PSX_MOA_START=title  LAUNCH GAME: the title, without the logos and intro
 *                        movie unless PSX_MOA_FULL_BOOT=1.
 *
 * Each direct start replays what the game itself does on that road, mapped
 * in docs/mapping/boot-loads.md (§3 SCENARIO, §4a LOAD and the card screen's
 * finish, §4b NEW GAME). It runs in place of the title (the menu entry calls
 * 0x800AAE94 when it would open the title). If any code word or the save
 * does not check out, nothing is changed and the title opens as normal.
 *
 * Offline only. The launcher clears PSX_MOA_START for every other mode
 * (Rogue Arena, AI Battle, netplay), so those keep their own boots.
 */
#include "moa_direct_start.h"
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the game (boot exe + menu module FDAT 206), see boot-loads.md ---------- */

#define QS_MENU_ENTRY      0x800A69C8u
#define QS_TITLE_OPEN      0x800AAE94u
#define QS_TO_TITLE        0x8003EA7Au  /* byte: the menu opens the title */
#define QS_NO_LOGOS        0x8003EA7Fu  /* byte: title opens without logos / intro */
#define QS_LINK_RETURN     0x8003EA80u  /* byte: back to the link versus menu */
#define QS_SCORE           0x8003EA94u  /* byte: 255 on SCENARIO */
#define QS_MODE            0x8003EA2Au  /* engine mode byte: 0 */
#define QS_PLUS            0x8003EAC8u  /* 8 bytes cleared on SCENARIO */
#define QS_MAIL_SHOWN      0x8003EA82u  /* LOAD sets it */
#define QS_OWNED           0x80036FECu  /* owned parts, 256 bytes */
#define QS_OWNED_COPY      0x8003EB20u  /* the card screen's copy */
#define QS_GARAGE          0x80036FB0u  /* 12 part ids */

#define QS_SCENARIO_TEAM   0x800631FCu  /* garage = the scenario starting AC */
#define QS_ARENA_RESET     0x800BA854u
#define QS_VS_RESET        0x800C566Cu
#define QS_SETTINGS_DEF    0x800B69F8u
#define QS_SETTINGS_APPLY  0x800B6A80u
#define QS_KILL_GROUP      0x80057B28u  /* (group) */
#define QS_PICTURE         0x80057CB0u  /* (archive, entry): sync, to VRAM */
#define QS_TIM_QUEUE       0x800586A0u  /* (archive, entry): queued, to VRAM */
#define QS_NEW_TASK        0x800575B8u  /* (group) -> task */
#define QS_PART_LISTS      0x80090FD8u
#define QS_SLOT_DEFAULTS   0x80083130u
#define QS_EMPTY_SLOTS     0x80067244u  /* (1) */
#define QS_AC_TEXTURES     0x800822B0u  /* queue the garage AC's part textures */
#define QS_MENU_ALLOC      0x800583A0u  /* menu heap: (bytes) -> buffer in main RAM */
#define QS_MENU_FREE       0x800583F8u  /* (buffer) */
#define QS_DISC_BUSY       0x800589DCu  /* non-zero (low 16 bits) = disc queue still loading */
#define QS_DISC_WAIT_MAX   20000000u    /* safety cap: each check runs a little game time */
#define QS_HUB_OPEN        0x800552C0u
#define QS_HUB_INPUT       0x80055848u  /* hub state: input (✕ = open the selected entry) */
#define QS_HUB_ENTRY       0xBEu        /* hub task: selected entry (signed byte 0-7) */
#define QS_PAD             0x801B1CB8u  /* +0x18 new presses, +0x1C the mask they must lack */
#define QS_PAD_X           0x4000u
#define QS_NEWGAME_STATE   0x800AC820u  /* NEW GAME: windows, then 0x800ACA0C name entry */
#define QS_NULL_DRAW       0x80057C24u
#define QS_LOAD_N          0x80073480u  /* LOAD: addiu v0,zero,'N' */
#define QS_LOAD_COPY       0x80073620u  /* LOAD: addiu t4,v0,0x6FB0 (first copy) */

#define QS_LOAD_IMAGE      0x8002C75Cu  /* boot exe */
#define QS_MOVE_IMAGE      0x8002C81Cu
#define QS_STORE_IMAGE     0x8002C7BCu  /* (rect, dst) */
#define QS_HUB_CLUT_RECT   0x800D460Cu  /* (416,0,16,64): MENU_TIM 0 colours */
#define QS_HUB_CLUT_SAVE   0x8004ACA8u
#define QS_HUB_ROW_RECT    0x800D4614u  /* (368,225,16,1) */
#define QS_HUB_ROW_SAVE    0x8004B6F0u
#define QS_DRAW_SYNC       0x8002C4B0u
#define QS_EMBLEM_RECT     0x800CC070u
#define QS_CLUT_RECT       0x800CC078u
#define QS_WEAPON_RECT     0x800CB8B8u
#define QS_WEAPON_CLUT     0x800CB8C0u

#define ARCH_MENU_TIM      4u

/* Every routine called, with its first code word: the menu module must be
 * the one in memory (all modules load at 0x80050DD8). */
static const struct { uint32_t at, word; } k_code[] = {
    { QS_MENU_ENTRY,     0x27BDFEB0u }, { QS_TITLE_OPEN,    0x27BDFFE0u },
    { QS_SCENARIO_TEAM,  0x00002821u }, { QS_ARENA_RESET,   0x00003021u },
    { QS_VS_RESET,       0x27BDFFE0u }, { QS_SETTINGS_DEF,  0x3C028005u },
    { QS_SETTINGS_APPLY, 0x27BDFFD8u }, { QS_KILL_GROUP,    0x2484FFFEu },
    { QS_PICTURE,        0x27BDFFE8u }, { 0x80099098u,      0x3C02801Au },
    { 0x800941F4u,       0x24050018u }, { 0x80093BC4u,      0x3C02801Au },
    { 0x800A2318u,       0x27BDFFE8u }, { 0x800A2020u,      0x3C02801Au },
    { QS_PART_LISTS,     0x27BDFFC8u }, { QS_SLOT_DEFAULTS, 0x27BDFFE0u },
    { QS_EMPTY_SLOTS,    0x27BDFFE0u }, { QS_AC_TEXTURES,   0x27BDFFE0u },
    { QS_HUB_OPEN,       0x27BDFFB8u }, { QS_HUB_INPUT,     0x27BDFFC0u }, { QS_NEW_TASK,      0x00804021u },
    { QS_TIM_QUEUE,      0x27BDFFD8u }, { QS_NEWGAME_STATE, 0x27BDFFD0u },
    { 0x800ACA0Cu,       0x27BDFFE8u }, { QS_LOAD_N,        0x2402004Eu },
    { QS_DISC_BUSY,      0x27BDFFE8u }, { QS_MENU_ALLOC,     0x27BDFFE0u },
    { QS_MENU_FREE,      0x27BDFFE8u },
    { QS_HUB_CLUT_RECT,  0x000001A0u }, { QS_HUB_ROW_RECT,   0x00E10170u },  /* RECT x,y */
    { QS_LOAD_COPY,      0x244C6FB0u },
};

/* LOAD's inline unpack for Master of Arena's own saves (0x80073610 ..
 * 0x80073B90): every byte it writes comes from the save, unchanged. */
static const struct { uint32_t dst; uint16_t from, bytes; } k_unpack[] = {
    { 0x80036FB0u,  440,  12 },   /* garage parts */
    { 0x80036FBCu,  592, 560 },   /* paint, owned parts (+640), shop stock (+896) ... */
    { 0x800371ECu,  460,  36 },   /* AC name, pilot name */
    { 0x8003EA70u,  454,   2 },   /* options */
    { 0x8003EA72u,  452,   2 },   /* paint pattern / variant */
    { 0x8003EA74u,  456,   4 },   /* credits */
    { 0x8003EAC8u, 3564,   8 },   /* Human PLUS and the flags after it */
    { 0x8004AC50u, 3232,  44 },
    { 0x8004B508u,  496,  96 },
    { 0x8004B568u, 3276, 288 },   /* settings */
    { 0x8004B688u, 3572, 100 },
};
/* LOAD's calls after the unpack. */
static const uint32_t k_after_load[] = {
    QS_SETTINGS_APPLY, 0x80099098u, 0x800941F4u, 0x80093BC4u, 0x800A2318u, 0x800A2020u };

#define SAVE_BYTES   3712u        /* 0xE80, what LOAD reads */
#define SUM_AT       384u         /* word: sum of ~byte over 3240 bytes from 432 */
#define SUM_FROM     432u
#define SUM_BYTES    3240u
#define VERSION_AT   3672u        /* byte: 1 */
#define EMBLEM_AT    1152u
#define CLUT_AT      3200u

/* ---- session ---------------------------------------------------------------- */

enum { QS_NONE = 0, QS_LOAD, QS_NEW, QS_TITLE };
static int qs_mode = -1;
static int qs_done;                 /* the direct start ran (or was given up) */

static void qs_session(void)
{
    static char last[32] = "-";
    const char *e = getenv("PSX_MOA_RUN_SESSION");
    if (!e) e = "";
    if (strncmp(last, e, sizeof last - 1) != 0) {
        snprintf(last, sizeof last, "%s", e);
        qs_mode = -1;
        qs_done = 0;
    }
    if (qs_mode >= 0) return;
    const char *m = getenv("PSX_MOA_START");
    qs_mode = !m ? QS_NONE : !strcmp(m, "load") ? QS_LOAD : !strcmp(m, "new") ? QS_NEW
            : !strcmp(m, "title") ? QS_TITLE : QS_NONE;
}

static int qs_full_boot(void)
{
    const char *e = getenv("PSX_MOA_FULL_BOOT");
    return e && e[0] == '1';
}

static int qs_code_ok(void)
{
    for (size_t i = 0; i < sizeof k_code / sizeof k_code[0]; i++)
        if (psx_mod_read_word(k_code[i].at) != k_code[i].word) return 0;
    return 1;
}

static void qs_log(const char *what)
{
    psx_mod_counter_add(what, 1);
    fprintf(stderr, "[moa start] %s\n", what);
}

static uint32_t call(struct CPUState *cpu, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2)
{
    return psx_mod_call_guest(cpu, fn, cpu->gpr[31], a0, a1, a2, 0);
}

/* §3: what the title's SCENARIO row does before NEW/LOAD (0x800AB58C). */
static void qs_scenario(struct CPUState *cpu)
{
    psx_mod_write_byte(QS_SCORE, 255);
    psx_mod_write_byte(QS_MODE, 0);
    call(cpu, QS_SCENARIO_TEAM, 0, 0, 0);
    call(cpu, QS_ARENA_RESET, 0, 0, 0);
    call(cpu, QS_VS_RESET, 0, 0, 0);
    call(cpu, QS_SETTINGS_DEF, 0, 0, 0);
    call(cpu, QS_SETTINGS_APPLY, 0, 0, 0);
    for (uint32_t i = 0; i < 8u; i++) psx_mod_write_byte(QS_PLUS + i, 0);
    call(cpu, QS_KILL_GROUP, 2, 0, 0);
    call(cpu, QS_PICTURE, ARCH_MENU_TIM, 0, 0);
    call(cpu, QS_PICTURE, ARCH_MENU_TIM, 1, 0);
    call(cpu, QS_PICTURE, ARCH_MENU_TIM, 4, 0);
}

static int qs_save_ok(const uint8_t save[SAVE_BYTES])
{
    if (save[0] != 'S' || save[1] != 'C' || save[VERSION_AT] != 1u) return 0;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < SUM_BYTES; i++) sum += ~(uint32_t)save[SUM_FROM + i];
    const uint32_t want = (uint32_t)save[SUM_AT] | (uint32_t)save[SUM_AT + 1] << 8 |
                          (uint32_t)save[SUM_AT + 2] << 16 | (uint32_t)save[SUM_AT + 3] << 24;
    return sum == want;
}

static int qs_read_save(uint8_t save[SAVE_BYTES])
{
    const char *path = getenv("PSX_MOA_START_SAVE");
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const int ok = fread(save, 1, SAVE_BYTES, f) == SAVE_BYTES;
    fclose(f);
    return ok && qs_save_ok(save);
}

/* A new game's save state (as moa_card_to_save now writes it). */
static const struct { uint16_t at, n; uint8_t b[64]; } k_new_game[] = {
#include "moa_arena_run_save.inc"
};

/* Saves the launcher made before moa_card_to_save wrote a new game's state
 * left these blocks all zero. Mission progress +3232 (0x8004AC50, copied to
 * the mission screen's table 0x8019F7A8): T+1 = 0 means "reopen slot 0's
 * briefing", so MISSIONS skipped the mission source picker; the game's own
 * new value is -1. Arena opponent counts +3553 (0x8004B67D = 0x8004B5A0 +
 * 221, from 0x800D5A08 by 0x800BA854) and the arena lock +3562: zero means
 * empty arenas. +3607 (0x8004B6AB) is the rest of the arena state. A game
 * save never has any of them all zero, so only those blocks are filled. */
static void qs_repair_save(uint8_t save[SAVE_BYTES])
{
    static const uint16_t blocks[] = { 3232, 3553, 3607 };
    for (size_t k = 0; k < sizeof k_new_game / sizeof k_new_game[0]; k++) {
        int want = 0;
        for (size_t j = 0; j < sizeof blocks / sizeof blocks[0]; j++)
            want |= k_new_game[k].at == blocks[j];
        if (!want) continue;
        const uint16_t at = k_new_game[k].at;
        const uint16_t n = at == 3232 ? 44 : k_new_game[k].n;   /* the whole progress block */
        int zero = 1;
        for (uint16_t i = 0; i < n; i++) zero &= save[at + i] == 0;
        if (!zero) continue;
        memcpy(save + at, k_new_game[k].b, k_new_game[k].n);
        qs_log(at == 3232 ? "moa.start.load_repaired_progress" : "moa.start.load_repaired_arena");
    }
}

/* ---- the shared direct start (moa_direct_start.h) ------------------------------ */

static int ds_open_entry = -1;      /* entry to open on the hub's first input frame */

int moa_direct_start_load(struct CPUState *cpu, const uint8_t save_in[MOA_DIRECT_SAVE_BYTES])
{
    static uint8_t save[SAVE_BYTES];
    if (!qs_code_ok()) { qs_log("moa.start.code_mismatch"); return 0; }
    memcpy(save, save_in, SAVE_BYTES);
    if (!qs_save_ok(save)) { qs_log("moa.start.load_bad_save"); return 0; }
    /* What opening the title clears (the menu entry clears both after the
     * title-open call too). Left at 1, the menu entry took the title branch
     * after a fight instead of the hub (Rogue Arena's first test). */
    psx_mod_write_byte(QS_TO_TITLE, 0);
    psx_mod_write_byte(QS_NO_LOGOS, 0);
    qs_repair_save(save);
    qs_scenario(cpu);
    /* The title branch of the menu entry keeps two strips of MENU_TIM 0's
     * colours (StoreImage at 0x800A6EB4 / 0x800A6EC8) and the hub open puts
     * them back (LoadImage at 0x800553AC / 0x800553BC). On a fresh boot the
     * hub opened here before that store, so it put back zeros: black hub
     * background. Store them now, from the MENU_TIM 0 just loaded. */
    call(cpu, QS_STORE_IMAGE, QS_HUB_CLUT_RECT, QS_HUB_CLUT_SAVE, 0);
    call(cpu, QS_STORE_IMAGE, QS_HUB_ROW_RECT, QS_HUB_ROW_SAVE, 0);
    call(cpu, QS_DRAW_SYNC, 0, 0, 0);
    /* LOAD ('N' path), after its card read */
    for (size_t k = 0; k < sizeof k_unpack / sizeof k_unpack[0]; k++)
        for (uint32_t i = 0; i < k_unpack[k].bytes; i++)
            psx_mod_write_byte(k_unpack[k].dst + i, save[k_unpack[k].from + i]);
    /* The emblem and its colours go to VRAM by DMA, which reaches main RAM
     * only: mod memory (psx_mod_alloc_guest_memory, 0x81000000) folds onto
     * low RAM there, and the game's next SAVE stored that (code) back as the
     * emblem. The save goes through a menu heap buffer instead, as LOAD's
     * own card buffer does, and is freed at once. */
    const uint32_t buf = call(cpu, QS_MENU_ALLOC, SAVE_BYTES, 0, 0);
    if (buf >= 0x80000000u && buf + SAVE_BYTES <= 0x80200000u) {
        for (uint32_t i = 0; i < SAVE_BYTES; i += 4u)
            psx_mod_write_word(buf + i, (uint32_t)save[i] | (uint32_t)save[i + 1] << 8 |
                                        (uint32_t)save[i + 2] << 16 | (uint32_t)save[i + 3] << 24);
        call(cpu, QS_LOAD_IMAGE, QS_EMBLEM_RECT, buf + EMBLEM_AT, 0);
        call(cpu, QS_LOAD_IMAGE, QS_CLUT_RECT, buf + CLUT_AT, 0);
        call(cpu, QS_DRAW_SYNC, 0, 0, 0);
        call(cpu, QS_MENU_FREE, buf, 0, 0);
    } else {
        qs_log("moa.start.load_no_emblem_buffer");
    }
    for (size_t i = 0; i < sizeof k_after_load / sizeof k_after_load[0]; i++)
        call(cpu, k_after_load[i], 0, 0, 0);
    psx_mod_write_byte(QS_MAIL_SHOWN, 1);
    /* The card screen's finish (0x8005CE68, opened from the title). */
    for (uint32_t i = 0; i < 256u; i++)
        psx_mod_write_byte(QS_OWNED_COPY + i, psx_mod_read_byte(QS_OWNED + i));
    call(cpu, QS_PART_LISTS, 0, 0, 0);
    call(cpu, QS_SLOT_DEFAULTS, 0, 0, 0);
    call(cpu, QS_EMPTY_SLOTS, 1, 0, 0);
    call(cpu, QS_AC_TEXTURES, 0, 0, 0);
    /* Then it waits for the disc queue (0x8005D0C4: loop on 0x800589DC), so
     * the AC's textures are in VRAM before the block below is moved and
     * before the hub opens (the hub's first state does not wait on the disc;
     * its open moves that block back at once). The menu entry's back-from-
     * arena road waits the same way. Rogue Arena's direct boot stalled
     * without it. */
    uint32_t spins = 0;
    while ((call(cpu, QS_DISC_BUSY, 0, 0, 0) & 0xFFFFu) != 0u)
        if (++spins >= QS_DISC_WAIT_MAX) { qs_log("moa.start.load_disc_wait_cap"); break; }
    const uint8_t bl = psx_mod_read_byte(QS_GARAGE + 6u), br = psx_mod_read_byte(QS_GARAGE + 7u);
    if ((bl != 0xFFu && bl >= 52u) || (br != 0xFFu && br >= 52u)) {
        call(cpu, QS_MOVE_IMAGE, QS_WEAPON_RECT, 768, 448);
        call(cpu, QS_MOVE_IMAGE, QS_WEAPON_CLUT, 0, 493);
        call(cpu, QS_MOVE_IMAGE, QS_WEAPON_CLUT, 0, 494);
        call(cpu, QS_DRAW_SYNC, 0, 0, 0);
    }
    return 1;
}

uint32_t moa_direct_start_hub(struct CPUState *cpu, int entry, int open)
{
    if (psx_mod_read_word(QS_HUB_OPEN) != 0x27BDFFB8u) { qs_log("moa.start.hub_code_mismatch"); return 0; }
    /* LOAD's callback (0x800AC9E8): the hub. */
    const uint32_t hub = call(cpu, QS_HUB_OPEN, 0, 0, 0);
    if (hub < 0x80000000u || hub >= 0x80200000u || hub == 0x801D0BA0u) {
        qs_log("moa.start.hub_no_task");
        return 0;
    }
    if (entry >= 0 && entry < 8) psx_mod_write_byte(hub + QS_HUB_ENTRY, (uint8_t)entry);
    ds_open_entry = (open && entry >= 0 && entry < 8) ? entry : -1;
    qs_log(open ? "moa.start.hub_open_entry" : "moa.start.load_hub");
    return hub;
}

/* The hub's input state (0x80055848, entry hook): opens the wanted entry
 * through the game's own ✕ path, once. ✕ counts when it is in the new
 * presses (+0x18) and not in the second mask (+0x1C); the state reads them
 * right after this hook. Rogue Arena's earlier X at the hub hooked this
 * routine too, but it was missing from game.toml's entry list, so compiled
 * menu code never called it. */
static void ds_hub_input(struct CPUState *cpu, uint32_t address)
{
    if (ds_open_entry < 0 || psx_mod_read_word(address) != 0x27BDFFC0u) return;
    const uint32_t hub = cpu->gpr[4];
    if (hub < 0x80000000u || hub >= 0x80200000u) return;
    if ((int8_t)psx_mod_read_byte(hub + QS_HUB_ENTRY) != ds_open_entry)
        psx_mod_write_byte(hub + QS_HUB_ENTRY, (uint8_t)ds_open_entry);
    psx_mod_write_word(QS_PAD + 0x18u, psx_mod_read_word(QS_PAD + 0x18u) | QS_PAD_X);
    psx_mod_write_word(QS_PAD + 0x1Cu, psx_mod_read_word(QS_PAD + 0x1Cu) & ~(uint32_t)QS_PAD_X);
    ds_open_entry = -1;
    qs_log("moa.start.hub_entry_opened");
}

/* LOAD GAME: the shared start, then the hub on Garage (as LOAD lands). */
static int qs_load(struct CPUState *cpu)
{
    static uint8_t save[SAVE_BYTES];
    if (!qs_read_save(save)) { qs_log("moa.start.load_bad_save"); return 0; }
    if (!moa_direct_start_load(cpu, save)) return 0;
    return moa_direct_start_hub(cpu, 0, 0) != 0;
}

/* NEW GAME: §3, then §4b. ✕ on NEW GAME queues the AC's textures and
 * MENU_TIM 7, then the title task runs 0x800AC820 (windows) and 0x800ACA0C
 * (credits, setup, name entry, then the hub). Here a task like the title's
 * is made and started in 0x800AC820, so the game runs that chain itself. */
static int qs_new(struct CPUState *cpu)
{
    qs_scenario(cpu);
    call(cpu, QS_AC_TEXTURES, 0, 0, 0);
    call(cpu, QS_TIM_QUEUE, ARCH_MENU_TIM, 7, 0);
    const uint32_t t = call(cpu, QS_NEW_TASK, 0, 0, 0);
    if (t < 0x80000000u || t >= 0x80200000u || t == 0x801D0BA0u) {
        qs_log("moa.start.new_no_task");
        return 1;    /* SCENARIO is half set: the title would be wrong too */
    }
    psx_mod_write_word(t + 0x74u, QS_TITLE_OPEN);   /* as the title's own task */
    psx_mod_write_word(t + 0x78u, QS_NEWGAME_STATE);
    psx_mod_write_word(t + 0x7Cu, QS_NULL_DRAW);
    psx_mod_write_word(t + 0x70u, 0);
    psx_mod_write_byte(t + 2u, 0);
    psx_mod_write_byte(t + 3u, 0);
    psx_mod_write_byte(t + 5u, 0);
    psx_mod_write_byte(t + 0xA3u, 1);
    qs_log("moa.start.new_name_entry");
    return 1;
}

/* ---- hooks -------------------------------------------------------------------- */

/* Menu entry: on the way to the title, open it without logos and intro
 * (the game's own switch, read just after this). Not on the way back to
 * the link versus menu, where the same byte means something else. */
static void qs_menu_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    qs_session();
    if (qs_mode == QS_NONE || psx_mod_read_word(address) != 0x27BDFEB0u) return;
    if (qs_mode == QS_TITLE && qs_full_boot()) return;
    if (psx_mod_read_byte(QS_TO_TITLE) && !psx_mod_read_byte(QS_LINK_RETURN))
        psx_mod_write_byte(QS_NO_LOGOS, 1);
}

static int qs_title_open(struct CPUState *cpu, uint32_t address)
{
    qs_session();
    if (qs_done || (qs_mode != QS_LOAD && qs_mode != QS_NEW) ||
        psx_mod_read_word(address) != 0x27BDFFE0u)
        return 0;
    qs_done = 1;                        /* once: a later title is the player's */
    if (!qs_code_ok()) { qs_log("moa.start.code_mismatch"); return 0; }
    /* What opening the title clears (the menu entry clears both after this
     * call too). Rogue Arena's first direct-boot test found 0x8003EA7A still
     * 1 after a fight (title branch instead of the hub), so they are cleared
     * here as well. */
    psx_mod_write_byte(QS_TO_TITLE, 0);
    psx_mod_write_byte(QS_NO_LOGOS, 0);
    return qs_mode == QS_LOAD ? qs_load(cpu) : qs_new(cpu);
}

PSX_MOD_CONSTRUCTOR(moa_quick_start_register)
{
    (void)psx_game_register_offline_function_entry(QS_MENU_ENTRY, qs_menu_entry);
    (void)psx_game_register_offline_function_filter(QS_TITLE_OPEN, qs_title_open);
    (void)psx_game_register_offline_function_entry(QS_HUB_INPUT, ds_hub_input);
}
