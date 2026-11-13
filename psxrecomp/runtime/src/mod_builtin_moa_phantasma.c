/*
 * Armored Core: Master of Arena - Project Phantasma arena (arena 10).
 *
 * A mod package generated from the player's own discs supplies the data and
 * the small menu-module patches (tenth Ex Arena list entry, its name, and
 * the arena-10 entries of the two arena tables: 50 entries, start 177). The
 * data is appended to a copy of MENU_TIM.T in the disc's padding area; the
 * file's own index is left exactly as shipped. This plugin supplies the
 * logic that does not fit in existing instructions.
 *
 * A. File index. The game reads each file's index from a single sector
 *    into a pool shared by all 18 data files, and the stock index already
 *    uses most of that sector, so the index on disc is left as shipped. The
 *    arena-10 block's layout is fixed by the generator, so the plugin builds
 *    a longer index in mod memory - the stock offsets followed by the
 *    arena-10 offsets - and points the menu file's index pointer at it.
 *    Index word j is the start sector of entry j; one more word ends the
 *    last entry.
 *
 * B. Opponent flags. The arena screen stores two pointers for the arena it
 *    opens (s4 = arena number, s2 = screen state):
 *      800bb9cc  sw v0,84(s2)   ; rank byte  = 0x8004B67D + arena
 *      800bb9f4  sw v0,88(s2)   ; flag bytes = 0x8004B5D6 + start[arena]
 *    Arena 10's rank byte, 0x8004B687, is an unused byte the game already
 *    writes to its save file. Its 50 flag bytes have no free home within
 *    reach of a one-byte start offset, so the hook at 800bb9f4 points them
 *    at mod memory. A flag is 1 once that opponent is beaten and the ladder
 *    moves one opponent at a time, so the flags are rebuilt from the saved
 *    rank whenever the arena opens.
 *
 * C. Statistics. After a fight the game adds to three per-arena arrays of
 *    nine entries, skipping arena 9 (custom):
 *      800bbee8  beq v1,v0,+26  ; v1 = arena, v0 = 9
 *    Arena 10 would index past them into other save data, so the hook makes
 *    it take the same skip.
 *
 * D. New game. The loop that sets each arena's rank to its entry count
 *    covers arenas 0-8; the hook after it sets arena 10's rank to 50.
 *
 * All hooks are on disc-loaded (interpreted) code, at instructions the mod
 * package does not patch, and are guarded by exact instruction words.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#define PP_ARENA            10u
#define PP_ENTRIES          50u
#define PP_START            177u
#define PP_RANK_ADDR        0x8004B687u

/* File table: 12 bytes per file, word 0 = pointer to the in-RAM index. */
#define PP_FILE_TABLE       0x8004402Cu
#define PP_FILE_COUNT       18u
#define PP_STOCK_ENTRIES    885u   /* stock MENU_TIM.T: entries 0-884 */
#define PP_STOCK_ENTRY1     37u    /* start sector of stock entry 1 */
#define PP_STOCK_END        2906u  /* stock file length in sectors */
#define PP_FIRST_ENTRY      888u   /* 140 + 4*start + 4*arena */
#define PP_INDEX_WORDS      1093u  /* entries 0-1091 and the end word */
#define PP_MEMORY_BYTES     4096u
#define PP_FLAGS_OFFSET     2304u  /* flags live after the index */

/* Hook sites. */
#define PP_OPEN_PC          0x800BB93Cu
#define PP_OPEN_INSN        0x27BDFFC0u /* arena screen: addiu sp,sp,-64 */
#define PP_FLAGS_PC         0x800BB9F4u
#define PP_FLAGS_INSN       0xAE420058u /* sw v0,88(s2) */
#define PP_FLAGS_GUARD_PC   0x800BB9CCu
#define PP_FLAGS_GUARD_INSN 0xAE420054u /* sw v0,84(s2) */
#define PP_STATS_PC         0x800BBEE8u
#define PP_STATS_INSN       0x1062001Au /* beq v1,v0,0x800bbf54 */
#define PP_STATS_GUARD_PC   0x800BBEE0u
#define PP_STATS_GUARD_INSN 0x9203005Du /* lbu v1,93(s0) */
#define PP_NEWGAME_PC         0x800BA8ACu
#define PP_NEWGAME_INSN       0x00003021u /* addu a2,zero,zero */
#define PP_NEWGAME_GUARD_PC   0x800BA898u
#define PP_NEWGAME_GUARD_INSN 0xA06500DDu /* sb a1,221(v1) */
#define PP_LOAD_A_PC        0x800C01E8u
#define PP_LOAD_A_INSN      0x24040004u /* fight setup: addiu a0,zero,4 */
#define PP_LOAD_B_PC        0x800C0460u
#define PP_LOAD_B_INSN      0x24040004u
#define PP_LOAD_C_PC        0x800BD5B8u
#define PP_LOAD_C_INSN      0x3C02800Du /* lui v0,0x800d */

#define PP_REG_V0 2
#define PP_REG_V1 3
#define PP_REG_S0 16
#define PP_REG_S4 20

static uint32_t s_pp_memory;

static uint32_t pp_memory(void) {
    if (!s_pp_memory) {
        s_pp_memory = psx_mod_alloc_guest_memory(PP_MEMORY_BYTES, 4u);
        if (!s_pp_memory)
            psx_mod_counter_add("moa.phantasma-arena.no_memory", 1);
    }
    return s_pp_memory;
}

/* Sectors of arena-10 block entry n (0-203), as generated: 50 descriptions
 * (1), 49 emblems (2) and an empty player slot, 50 portraits (8), 50 second
 * descriptions (1), then AC builds (2), names (2), rewards (1), AI (1). */
static uint32_t pp_block_sectors(uint32_t n) {
    if (n < 50u) return 1u;
    if (n < 99u) return 2u;
    if (n == 99u) return 0u;
    if (n < 150u) return 8u;
    if (n < 200u) return 1u;
    if (n < 202u) return 2u;
    return 1u;
}

/* Point the menu file at an index that also covers the arena-10 block. */
static void pp_install_index(void) {
    const uint32_t memory = pp_memory();
    if (!memory) return;
    for (uint32_t file = 0; file < PP_FILE_COUNT; ++file) {
        const uint32_t slot = PP_FILE_TABLE + file * 12u;
        const uint32_t index = psx_mod_read_word(slot);
        if (index == memory) return;               /* already installed */
        if ((index >> 24) != 0x80u) continue;
        /* MENU_TIM.T: entry 0 at sector 1, entry 1 at 37, end at 2906. */
        if (psx_mod_read_half(index) != 1u ||
            psx_mod_read_half(index + 2u) != PP_STOCK_ENTRY1 ||
            psx_mod_read_half(index + PP_STOCK_ENTRIES * 2u) != PP_STOCK_END)
            continue;
        for (uint32_t j = 0; j < PP_STOCK_ENTRIES; ++j)
            psx_mod_write_half(memory + j * 2u,
                               psx_mod_read_half(index + j * 2u));
        /* Entries 885-887 are empty; the block starts where the stock file
         * ended. */
        uint32_t offset = PP_STOCK_END;
        for (uint32_t entry = PP_STOCK_ENTRIES; entry < PP_FIRST_ENTRY; ++entry)
            psx_mod_write_half(memory + entry * 2u, (uint16_t)offset);
        for (uint32_t n = 0; n < 4u * PP_ENTRIES + 4u; ++n) {
            psx_mod_write_half(memory + (PP_FIRST_ENTRY + n) * 2u,
                               (uint16_t)offset);
            offset += pp_block_sectors(n);
        }
        psx_mod_write_half(memory + (PP_INDEX_WORDS - 1u) * 2u, (uint16_t)offset);
        psx_mod_write_word(slot, memory);
        psx_mod_counter_add("moa.phantasma-arena.index_installed", 1);
        return;
    }
    psx_mod_counter_add("moa.phantasma-arena.menu_file_not_found", 1);
}

static void pp_on_load(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    pp_install_index();
}

static void pp_redirect_flags(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[PP_REG_S4] != PP_ARENA) return;
    if (psx_mod_read_word(PP_FLAGS_GUARD_PC) != PP_FLAGS_GUARD_INSN) {
        psx_mod_counter_add("moa.phantasma-arena.guard_rejected", 1);
        return;
    }
    uint8_t rank = psx_mod_read_byte(PP_RANK_ADDR);
    if (rank == 0u || rank > PP_ENTRIES) {
        rank = (uint8_t)PP_ENTRIES;          /* never opened: bottom of the list */
        psx_mod_write_byte(PP_RANK_ADDR, rank);
    }
    const uint32_t memory = pp_memory();
    if (!memory) return;
    const uint32_t flags = memory + PP_FLAGS_OFFSET;
    for (uint32_t i = 0; i < PP_ENTRIES; ++i) {
        /* Opponent i started at rank i+1; it is beaten once the player is at
         * or above that rank. The last entry is the player's own slot. */
        const int beaten = i + 1u < PP_ENTRIES && i + 1u >= rank;
        psx_mod_write_byte(flags + i, beaten ? 1u : 0u);
    }
    cpu->gpr[PP_REG_V0] = flags;
    psx_mod_counter_add("moa.phantasma-arena.opened", 1);
}

static void pp_skip_stats(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[PP_REG_V1] != PP_ARENA) return;
    if (psx_mod_read_word(PP_STATS_GUARD_PC) != PP_STATS_GUARD_INSN) {
        psx_mod_counter_add("moa.phantasma-arena.guard_rejected", 1);
        return;
    }
    cpu->gpr[PP_REG_V1] = 9u; /* take the custom-arena skip */
}

static void pp_new_game(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (psx_mod_read_word(PP_NEWGAME_GUARD_PC) != PP_NEWGAME_GUARD_INSN) {
        psx_mod_counter_add("moa.phantasma-arena.guard_rejected", 1);
        return;
    }
    psx_mod_write_byte(PP_RANK_ADDR, (uint8_t)PP_ENTRIES);
}

static void pp_activate(void) {
    s_pp_memory = 0; /* this session's plan gets its own allocation */
    (void)pp_memory();
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_phantasma_arena_plugin) {
    static const char id[] = "moa.phantasma-arena";
    (void)psx_mod_register_activation_plugin(id, pp_activate);
    (void)psx_mod_register_instruction_plugin(id, PP_OPEN_PC, PP_OPEN_INSN, pp_on_load);
    (void)psx_mod_register_instruction_plugin(id, PP_LOAD_A_PC, PP_LOAD_A_INSN, pp_on_load);
    (void)psx_mod_register_instruction_plugin(id, PP_LOAD_B_PC, PP_LOAD_B_INSN, pp_on_load);
    (void)psx_mod_register_instruction_plugin(id, PP_LOAD_C_PC, PP_LOAD_C_INSN, pp_on_load);
    (void)psx_mod_register_instruction_plugin(id, PP_FLAGS_PC, PP_FLAGS_INSN, pp_redirect_flags);
    (void)psx_mod_register_instruction_plugin(id, PP_STATS_PC, PP_STATS_INSN, pp_skip_stats);
    (void)psx_mod_register_instruction_plugin(id, PP_NEWGAME_PC, PP_NEWGAME_INSN, pp_new_game);
}


/*
 * Project Phantasma missions (plugin id "moa.phantasma-missions").
 *
 * The mod package (built from the player's own discs) supplies:
 *  - a pack in the disc's padding area with four checksummed blocks per
 *    Phantasma mission: its textures, the common battle data with that
 *    mission's messages, the mission code (ported to this game's engine) and
 *    the map;
 *  - a copy of GG/MS/MIS.T with Phantasma's own MIS.T appended (its header
 *    included), the directory record pointing at the copy.
 *
 * Master of Arena's mission screen lists slots 0-23 from a table at
 * 0x8019F7A8 (T): T+2 story chapter, T+20 one flag byte per slot (0x02 =
 * done), T+56 the list, T+80 a copy of MIS.T entry 0 (T+80 windows [lo,hi],
 * T+176 names of 32 bytes, T+1040 rewards, T+1136 engine mission numbers,
 * T+1184 briefing page records: 32 bytes a slot, image entries then text
 * entries). Slot 20 is an unused "Dummy" slot; it becomes the next unfinished
 * Phantasma mission:
 *  A. 8009417C (after T+80 is copied): slot 20 gets the Phantasma mission's
 *     name ("PP:"), reward, page record and a window open in the current
 *     chapter; the other names get "M:". The MIS.T index is extended so the
 *     page record reaches Phantasma's entries (entry 410+k = its entry k). A
 *     done flag on slot 20 is recorded in the progress file beside the memory
 *     card and cleared, so the next mission takes the slot.
 *  B. 800943AC (list builder return): slot 20 moves to the top of the list.
 *  C. Battle engine: while slot 20 loads, four loads are pointed at the
 *     pack's blocks (file 3, which the game's file table never uses):
 *      800556b8 jal -> textures, file 1, entry = slot (byte at s2+3504)
 *      800556d4 jal -> common,   file 2, entry 205
 *      80055874 jal -> code,     file 2, entry slot*5
 *      80055894 jal -> map,      file 2, entry slot*5+4
 * All hooks are on disc-loaded (interpreted) code and guarded by exact
 * instruction words.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generated by the mission package builder (pack layout, 10455 sectors). */
#define PPM_MISSIONS        17u
#define PPM_PACK_LOC        0x00003863u /* disc position of the pack (LBA 286200), minute/second/frame in BCD */
#define PPM_PACK_BYTES      21411840u
#define PPM_INDEX_WORDS     69u
static const uint16_t ppm_index[PPM_INDEX_WORDS] = {
    0, 163, 296, 320, 598, 759, 893, 913, 1208, 1331, 1465, 1482, 1829, 1983, 2116, 2137, 2556, 2701, 2835, 2857, 3150, 3328, 3462, 3478, 3765, 3920, 4053, 4080, 4471, 4627, 4761, 4785, 5224, 5312, 5446, 5471, 5701, 5863, 5996, 6019, 6425, 6565, 6698, 6716, 6897, 7028, 7161, 7171, 7440, 7596, 7729, 7750, 8099, 8259, 8392, 8418, 8676, 8824, 8957, 8979, 9331, 9489, 9622, 9643, 9988, 10122, 10255, 10264, 10455
};   /* four entries per mission: textures, common, code, map */
#define PPM_ENTRY_TEXTURES  0u
#define PPM_ENTRY_COMMON    1u
#define PPM_ENTRY_CODE      2u
#define PPM_ENTRY_MAP       3u
#define PPM_TEX_PC          0x800556B4u
#define PPM_TEX_INSN        0x92450DB0u /* lbu a1,3504(s2) */
#define PPM_TEXDONE_PC      0x800556C0u
#define PPM_TEXDONE_INSN    0x0C0047CAu /* jal 0x80011f28 */
#define PPM_COMMON_PC       0x800556D4u
#define PPM_COMMON_INSN     0x0C005A96u /* jal 0x80016a58 */
#define PPM_CODE_PC         0x80055874u
#define PPM_CODE_INSN       0x0C005A96u /* jal 0x80016a58 */
#define PPM_MAP_PC          0x80055894u
#define PPM_MAP_INSN        0x0C005A96u /* jal 0x80016a58 */
#define PPM_SLOT            20u         /* the unused Master of Arena slot used for Phantasma */
#define PPM_FILE            3u
#define PPM_MISSION_ADDR    0x800453EDu /* mission slot chosen in the menu */
#define PPM_LOADING_OFFSET  3504u       /* engine's copy, at s2 + 3504 */
#define PPM_LOADING_ADDR    0x800A0DB0u
#define PPM_REG_A0 4
#define PPM_REG_A1 5
#define PPM_REG_A3 7
#define PPM_REG_S0 16
#define PPM_REG_S2 18

/* Mission screen. */
#define PPM_T               0x8019F7A8u
#define PPM_T_CHAPTER       2u
#define PPM_T_FLAGS         20u
#define PPM_T_LIST          56u
#define PPM_T_WINDOWS       80u
#define PPM_T_NAMES         176u
#define PPM_T_REWARDS       1040u
#define PPM_T_ENGINE        1136u       /* engine mission number per slot */
#define PPM_T_PAGES         1184u       /* 32 bytes per slot: image entries, text entries */
#define PPM_NAME_BYTES      32u
#define PPM_MOA_MISSIONS    20u
#define PPM_TABLE_PC        0x8009417Cu
#define PPM_TABLE_INSN      0x2610FFB0u /* addiu s0,s0,-80 (after the MIS.T entry 0 copy) */
#define PPM_LIST_PC         0x800943ACu
#define PPM_LIST_INSN       0x03E00008u /* jr ra */
#define PPM_LIST_GUARD_PC   0x800943A8u
#define PPM_LIST_GUARD_INSN 0x00E01021u /* addu v0,a3,zero */

/* MIS.T: stock file then Phantasma's MIS.T (its header first). */
#define PPM_MIS_FILE        19u
#define PPM_MIS_ENTRIES     409u
#define PPM_MIS_END         460u        /* stock length in sectors */
#define PPM_MIS_PP_ENTRIES  833u

/* Mod memory: pack index (69 halves), MIS.T index (stock 410 halves, then
 * entry 410+k = Phantasma's entry k). */
#define PPM_MEM_PACK        0u
#define PPM_MEM_MIS         256u
#define PPM_MIS_INDEX       (PPM_MIS_ENTRIES + 1u + PPM_MIS_PP_ENTRIES + 1u)
#define PPM_MEM_BYTES       (PPM_MEM_MIS + 2u * PPM_MIS_INDEX + 64u)

static uint32_t s_ppm_memory;
static int      s_ppm_loaded;            /* Phantasma table read from the disc */
static uint16_t s_mis_stock[PPM_MIS_ENTRIES + 1u];
static uint16_t s_mis_pp[PPM_MIS_PP_ENTRIES + 1u];
static uint8_t  s_pp_names[PPM_MISSIONS][PPM_NAME_BYTES];
static uint32_t s_pp_rewards[PPM_MISSIONS];
static int16_t  s_pp_pages[PPM_MISSIONS][16];
static uint8_t  s_done[PPM_MISSIONS];    /* progress, from the file beside the card */
static int      s_done_read;

static uint32_t ppm_memory(void) {
    if (!s_ppm_memory) {
        s_ppm_memory = psx_mod_alloc_guest_memory(PPM_MEM_BYTES, 4u);
        if (!s_ppm_memory) psx_mod_counter_add("moa.phantasma-missions.no_memory", 1);
    }
    return s_ppm_memory;
}

/* ---- progress file ---- */

static int ppm_progress_path(char *out, size_t cap) {
    const char *dir = getenv("PSX_MEMCARD_HOME");
    if (!dir || !dir[0]) return 0;
    return snprintf(out, cap, "%s/Phantasma Missions.txt", dir) < (int)cap;
}

static void ppm_progress_read(void) {
    if (s_done_read) return;
    s_done_read = 1;
    memset(s_done, 0, sizeof s_done);
    char path[1100];
    if (!ppm_progress_path(path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "done=", 5) != 0) continue;
        for (uint32_t i = 0; i < PPM_MISSIONS && line[5 + i]; ++i)
            s_done[i] = line[5 + i] == '1';
    }
    fclose(f);
}

static void ppm_progress_write(void) {
    char path[1100];
    if (!ppm_progress_path(path, sizeof path)) {
        psx_mod_counter_add("moa.phantasma-missions.no_progress_path", 1);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { psx_mod_counter_add("moa.phantasma-missions.progress_write_failed", 1); return; }
    char bits[PPM_MISSIONS + 1u];
    for (uint32_t i = 0; i < PPM_MISSIONS; ++i) bits[i] = s_done[i] ? '1' : '0';
    bits[PPM_MISSIONS] = '\0';
    fprintf(f, "# Project Phantasma missions finished in Master of Arena (1 = done)\n"
               "done=%s\n", bits);
    fclose(f);
}

/* The next unfinished mission, or PPM_MISSIONS when all are done. */
static uint32_t ppm_current(void) {
    ppm_progress_read();
    for (uint32_t i = 0; i < PPM_MISSIONS; ++i) if (!s_done[i]) return i;
    return PPM_MISSIONS;
}

/* ---- Phantasma's mission table, from the extended MIS.T ---- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int ppm_load_tables(void) {
    if (s_ppm_loaded) return s_ppm_loaded > 0;
    s_ppm_loaded = -1;
    uint32_t size = 0;
    if (!psx_mod_read_disc_file("GG/MS/MIS.T", NULL, 0, &size) ||
        size < (PPM_MIS_END + 8u) * 2048u) {
        psx_mod_counter_add("moa.phantasma-missions.mis_not_extended", 1);
        return 0;
    }
    uint8_t *d = (uint8_t *)malloc(size);
    if (!d) return 0;
    int ok = 0;
    if (psx_mod_read_disc_file("GG/MS/MIS.T", d, size, &size) &&
        le16(d) == PPM_MIS_ENTRIES && le16(d + 2u + 2u * PPM_MIS_ENTRIES) == PPM_MIS_END) {
        const uint8_t *pp = d + (size_t)PPM_MIS_END * 2048u;
        if (le16(pp) == PPM_MIS_PP_ENTRIES) {
            for (uint32_t k = 0; k <= PPM_MIS_ENTRIES; ++k) s_mis_stock[k] = le16(d + 2u + 2u * k);
            for (uint32_t k = 0; k <= PPM_MIS_PP_ENTRIES; ++k)
                s_mis_pp[k] = (uint16_t)(PPM_MIS_END + le16(pp + 2u + 2u * k));
            const uint8_t *e0 = d + (size_t)s_mis_pp[0] * 2048u;
            if ((size_t)s_mis_pp[PPM_MIS_PP_ENTRIES] * 2048u <= size) {
                /* Phantasma's entry 0: 64 windows, 64 names of 32 bytes, rewards at
                 * 2560, briefing page records (32 bytes) at 2944. */
                for (uint32_t i = 0; i < PPM_MISSIONS; ++i) {
                    for (uint32_t j = 0; j < 16u; ++j)
                        s_pp_pages[i][j] = (int16_t)le16(e0 + 2944u + i * 32u + j * 2u);
                    memcpy(s_pp_names[i], e0 + 256u + i * PPM_NAME_BYTES, PPM_NAME_BYTES);
                    const uint8_t *r = e0 + 2560u + i * 4u;
                    s_pp_rewards[i] = (uint32_t)r[0] | (uint32_t)r[1] << 8 |
                                      (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24;
                }
                ok = 1;
            }
        }
    }
    free(d);
    if (!ok) { psx_mod_counter_add("moa.phantasma-missions.mis_unexpected", 1); return 0; }
    s_ppm_loaded = 1;
    return 1;
}

/* Name `src` (ends with '>') behind `prefix`, in a 32-byte slot. This
 * game's font has no "{4" narrow-space code (Phantasma's long names use it),
 * so it is dropped. */
static void ppm_put_name(uint32_t at, const char *prefix, const uint8_t *src) {
    uint8_t out[PPM_NAME_BYTES];
    memset(out, 0, sizeof out);
    size_t n = strlen(prefix);
    memcpy(out, prefix, n);
    for (size_t i = 0; i < PPM_NAME_BYTES && n < PPM_NAME_BYTES - 1u; ++i) {
        if (src[i] == '>' || src[i] == 0) break;
        if (src[i] == '{' && i + 1u < PPM_NAME_BYTES && src[i + 1u] >= '0' && src[i + 1u] <= '9') {
            ++i;
            continue;
        }
        out[n++] = src[i];
    }
    out[n] = '>';
    for (uint32_t i = 0; i < PPM_NAME_BYTES; ++i) psx_mod_write_byte(at + i, out[i]);
}

/* The list has room for 23 characters before the reward: Phantasma names
 * longer than 20 get a short form (our wording). */
static const char *const k_pp_short[PPM_MISSIONS] = {
    "Infiltr. Amber Base>", NULL, NULL, NULL, "Disrupt Ship Lanes>", NULL, NULL,
    "Destroy Facility>", NULL, NULL, NULL, NULL, "Disrupt Mil. Units>",
    "Destroy Recv. Base>", "Destroy Detachment>", "Intercept Enemies>", NULL,
};

/* ---- file table: the pack (file 3) and the MIS.T index ---- */

#define PPM_FILE_TABLE 0x8004402Cu

static int ppm_install_pack(void) {
    const uint32_t memory = ppm_memory();
    if (!memory) return 0;
    const uint32_t slot = PPM_FILE_TABLE + PPM_FILE * 12u;
    const uint32_t index = psx_mod_read_word(slot);
    const uint32_t mine = memory + PPM_MEM_PACK;
    if (index == mine) return 1;
    /* Free, or left by an earlier session of this plugin (saved state). */
    if (index != 0u && psx_mod_read_word(slot + 4u) != PPM_PACK_LOC) {
        psx_mod_counter_add("moa.phantasma-missions.file_slot_in_use", 1);
        return 0;
    }
    for (uint32_t j = 0; j < PPM_INDEX_WORDS; ++j)
        psx_mod_write_half(mine + j * 2u, ppm_index[j]);
    psx_mod_write_word(slot + 4u, PPM_PACK_LOC);
    psx_mod_write_word(slot + 8u, PPM_PACK_BYTES);
    psx_mod_write_word(slot, mine);
    return 1;
}

/* MIS.T index: the stock one, then Phantasma's entries (entry 409 is
 * Phantasma's header sector, entry 410+k its entry k). */
static void ppm_install_mis_index(void) {
    const uint32_t memory = ppm_memory();
    if (!memory) return;
    const uint32_t slot = PPM_FILE_TABLE + PPM_MIS_FILE * 12u;
    const uint32_t index = psx_mod_read_word(slot);
    const uint32_t mine = memory + PPM_MEM_MIS;
    if (index == mine) return;
    if ((index >> 24) != 0x80u || psx_mod_read_half(index) != s_mis_stock[0] ||
        psx_mod_read_half(index + 2u * PPM_MIS_ENTRIES) != PPM_MIS_END) {
        psx_mod_counter_add("moa.phantasma-missions.mis_index_unexpected", 1);
        return;
    }
    for (uint32_t k = 0; k <= PPM_MIS_ENTRIES; ++k)
        psx_mod_write_half(mine + 2u * k, s_mis_stock[k]);
    for (uint32_t k = 0; k <= PPM_MIS_PP_ENTRIES; ++k)
        psx_mod_write_half(mine + 2u * (PPM_MIS_ENTRIES + 1u + k), s_mis_pp[k]);
    psx_mod_write_word(slot, mine);
}

/* A. After the menu copies MIS.T entry 0 into T+80. */
static void ppm_on_table(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[PPM_REG_S0] != PPM_T + PPM_T_WINDOWS) {
        psx_mod_counter_add("moa.phantasma-missions.guard_rejected", 1);
        return;
    }
    if (!ppm_load_tables()) return;
    ppm_progress_read();
    /* A finished Phantasma mission leaves slot 20's done flag set. */
    const uint32_t flag = PPM_T + PPM_T_FLAGS + PPM_SLOT;
    uint32_t cur = ppm_current();
    if (psx_mod_read_byte(flag) & 0x02u) {
        if (cur < PPM_MISSIONS) {
            s_done[cur] = 1;
            ppm_progress_write();
            psx_mod_counter_add("moa.phantasma-missions.finished", 1);
        }
        psx_mod_write_byte(flag, 0u);
        cur = ppm_current();
    }
    for (uint32_t i = 0; i < PPM_MOA_MISSIONS; ++i) {
        const uint32_t at = PPM_T + PPM_T_NAMES + i * PPM_NAME_BYTES;
        uint8_t was[PPM_NAME_BYTES];
        for (uint32_t b = 0; b < PPM_NAME_BYTES; ++b) was[b] = psx_mod_read_byte(at + b);
        if (was[0] == 'M' && was[1] == ':') continue;
        ppm_put_name(at, "M:", was);
    }
    const uint32_t window = PPM_T + PPM_T_WINDOWS + PPM_SLOT * 4u;
    if (cur < PPM_MISSIONS) {
        const uint8_t chapter = psx_mod_read_byte(PPM_T + PPM_T_CHAPTER);
        psx_mod_write_byte(window, chapter);
        psx_mod_write_byte(window + 1u, chapter);
        ppm_put_name(PPM_T + PPM_T_NAMES + PPM_SLOT * PPM_NAME_BYTES, "PP:",
                     k_pp_short[cur] ? (const uint8_t *)k_pp_short[cur] : s_pp_names[cur]);
        psx_mod_write_word(PPM_T + PPM_T_REWARDS + PPM_SLOT * 4u, s_pp_rewards[cur]);
        psx_mod_write_byte(PPM_T + PPM_T_ENGINE + PPM_SLOT, (uint8_t)PPM_SLOT);
        /* Briefing pages: Phantasma's entry v+1 is our entry 410+v+1. */
        for (uint32_t j = 0; j < 16u; ++j) {
            const int16_t v = s_pp_pages[cur][j];
            psx_mod_write_half(PPM_T + PPM_T_PAGES + PPM_SLOT * 32u + j * 2u,
                               (uint16_t)(v < 0 ? -1 : v + (int)PPM_MIS_ENTRIES + 1));
        }
        ppm_install_mis_index();
    } else {
        psx_mod_write_byte(window, 0xFFu);   /* all done: the slot is not listed */
        psx_mod_write_byte(window + 1u, 0xFFu);
    }
}

/* B. List builder return: Phantasma's slot goes to the top. */
static void ppm_on_list(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (psx_mod_read_word(PPM_LIST_GUARD_PC) != PPM_LIST_GUARD_INSN) {
        psx_mod_counter_add("moa.phantasma-missions.guard_rejected", 1);
        return;
    }
    const uint32_t count = cpu->gpr[PPM_REG_A3];
    const uint32_t list = PPM_T + PPM_T_LIST;
    if (count < 2u || count > 24u) return;
    for (uint32_t p = 1; p < count; ++p) {
        if (psx_mod_read_byte(list + p) != PPM_SLOT) continue;
        for (uint32_t q = p; q > 0; --q) psx_mod_write_byte(list + q, psx_mod_read_byte(list + q - 1u));
        psx_mod_write_byte(list, (uint8_t)PPM_SLOT);
        break;
    }
}

/* C. Battle engine loads while slot 20 loads. */
static int ppm_ready(uint32_t *mission) {
    if (psx_mod_read_byte(PPM_MISSION_ADDR) != PPM_SLOT) return 0;
    const uint32_t m = ppm_current();
    if (m >= PPM_MISSIONS) return 0;
    if (!ppm_install_pack()) return 0;
    *mission = m;
    return 1;
}

static void ppm_textures(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t m;
    if (cpu->gpr[PPM_REG_S2] + PPM_LOADING_OFFSET != PPM_LOADING_ADDR || cpu->gpr[PPM_REG_A0] != 1u)
        return;
    if (!ppm_ready(&m)) return;
    psx_mod_write_byte(PPM_LOADING_ADDR, (uint8_t)(m * 4u + PPM_ENTRY_TEXTURES));
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
}

static void ppm_textures_done(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (psx_mod_read_byte(PPM_MISSION_ADDR) != PPM_SLOT) return;
    psx_mod_write_byte(PPM_LOADING_ADDR, (uint8_t)PPM_SLOT);
}

static void ppm_common(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t m;
    if (cpu->gpr[PPM_REG_A0] != 2u || cpu->gpr[PPM_REG_A1] != 205u) return;
    if (!ppm_ready(&m)) return;
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
    cpu->gpr[PPM_REG_A1] = m * 4u + PPM_ENTRY_COMMON;
}

static void ppm_code(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t m;
    /* a1 = slot*4 here; the delay slot adds v0 (= slot). */
    if (cpu->gpr[PPM_REG_A0] != 2u || cpu->gpr[PP_REG_V0] != PPM_SLOT ||
        cpu->gpr[PPM_REG_A1] != PPM_SLOT * 4u) return;
    if (!ppm_ready(&m)) return;
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
    cpu->gpr[PPM_REG_A1] = m * 4u + PPM_ENTRY_CODE - cpu->gpr[PP_REG_V0];
}

static void ppm_map(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t m;
    if (cpu->gpr[PPM_REG_A0] != 2u || cpu->gpr[PPM_REG_A1] != PPM_SLOT * 5u + 4u) return;
    if (!ppm_ready(&m)) return;
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
    cpu->gpr[PPM_REG_A1] = m * 4u + PPM_ENTRY_MAP;
    psx_mod_counter_add("moa.phantasma-missions.loaded", 1);
}

static void ppm_activate(void) {
    s_ppm_memory = 0; /* this session's plan gets its own allocation */
    s_done_read = 0;
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_phantasma_missions) {
    static const char id[] = "moa.phantasma-missions";
    (void)psx_mod_register_activation_plugin(id, ppm_activate);
    (void)psx_mod_register_instruction_plugin(id, PPM_TABLE_PC, PPM_TABLE_INSN, ppm_on_table);
    (void)psx_mod_register_instruction_plugin(id, PPM_LIST_PC, PPM_LIST_INSN, ppm_on_list);
    (void)psx_mod_register_instruction_plugin(id, PPM_TEX_PC, PPM_TEX_INSN, ppm_textures);
    (void)psx_mod_register_instruction_plugin(id, PPM_TEXDONE_PC, PPM_TEXDONE_INSN, ppm_textures_done);
    (void)psx_mod_register_instruction_plugin(id, PPM_COMMON_PC, PPM_COMMON_INSN, ppm_common);
    (void)psx_mod_register_instruction_plugin(id, PPM_CODE_PC, PPM_CODE_INSN, ppm_code);
    (void)psx_mod_register_instruction_plugin(id, PPM_MAP_PC, PPM_MAP_INSN, ppm_map);
}
