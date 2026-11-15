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
 * 0x8019F7A8 (T): T+0 slot being played, T+2 story chapter, T+20 one flag
 * byte per slot (0x02 = done), T+56 the list, T+80 a copy of MIS.T entry 0
 * (T+80 windows [lo,hi,..], T+176 names of 32 bytes, T+1040 rewards, T+1136
 * engine mission numbers, T+1160 a byte per slot, T+1184 briefing page
 * records: 32 bytes a slot, image entries then text entries).
 *
 * Mission source picker. MISSIONS first opens a three-row list - ARMORED
 * CORE (greyed, not installed yet), PROJECT PHANTASMA, MASTER OF ARENA - in
 * the window and code of the Ex Arena category list (800BACE8 and its
 * states), steered by register-only hooks while the picker is up:
 *  P1. 800943D8: the mission screen's fade gets the Ex Arena opener
 *      (800BA684) as its callback; 800BA6B8/800BA6E0 make it take the list
 *      branch.
 *  P2. 800BA750: the list's item flags (grey) and starting row.
 *  P3. Window height for three rows (open 800BAFC8, close 800BB4C4/5B0),
 *      three bars (800BB1B0), no scroll (800BB398/3B0), no down arrow
 *      (800BB864), our names (800BB7E8) and descriptions (800BB920).
 *  P4. Confirm (800BB6D8, once the window has closed): instead of opening an
 *      arena, the list task becomes the mission screen's loader (800943E8).
 *      Cancel (800BB5FC, 800BA784): the mission header is closed instead of
 *      the arena's; the game returns to the main menu.
 *
 * Project Phantasma view (source 1). For testing, every working Phantasma
 * mission is listed: mission k takes slot k (slots 0-16); slots 17-23 are
 * hidden. The battle runs the Dummy slot's engine number (20).
 *  A. 8009417C (after T+80 is copied): Master of Arena's done flags are kept
 *     aside and the Phantasma slots are filled (name, reward, page record,
 *     window in the current chapter). The MIS.T index is extended so the page
 *     records reach Phantasma's entries (entry 410+k = its entry k).
 *  B. 800943AC (list builder return): done marks from the progress file.
 *  C. 800976C0 (mission chosen): which Phantasma mission is played.
 *  D. Results 80094068 / 800940C8 and leaving the screen 80095730: a win is
 *     recorded in the progress file beside the memory card and Master of
 *     Arena's done flags are put back, so its progress is never changed.
 *  E. Battle engine: while engine mission 20 loads a Phantasma mission, four
 *     loads are pointed at the pack's blocks (file 3, which the game's file
 *     table never uses):
 *      800556b8 jal -> textures, file 1, entry = slot
 *      800556d4 jal -> common,   file 2, entry 205
 *      80055874 jal -> code,     file 2, entry slot*5
 *      80055894 jal -> map,      file 2, entry slot*5+4
 * Master of Arena view (source 2): the stock list (the Dummy slot hidden).
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
#define PPM_TEX_PC          0x800556B8u
#define PPM_TEX_INSN        0x0C005A96u /* jal 0x80016a58 (a1 = slot, loaded just before) */
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
#define PPM_T_BYTE          1160u       /* one byte per slot (copied from the Dummy slot) */
#define PPM_SLOTS           24u
#define PPM_NAME_BYTES      32u
#define PPM_TABLE_PC        0x8009417Cu
#define PPM_TABLE_INSN      0x2610FFB0u /* addiu s0,s0,-80 (after the MIS.T entry 0 copy) */
#define PPM_LIST_PC         0x800943ACu
#define PPM_LIST_INSN       0x03E00008u /* jr ra */
#define PPM_LIST_GUARD_PC   0x800943A8u
#define PPM_LIST_GUARD_INSN 0x00E01021u /* addu v0,a3,zero */
#define PPM_CHOSEN_PC       0x800976C0u
#define PPM_CHOSEN_INSN     0xA04353EDu /* sb v1,0x53ed(v0): engine number of slot a3 */
#define PPM_WIN_PC          0x80094068u
#define PPM_WIN_INSN        0x8C82000Cu /* lw v0,12(a0), after the done flag */
#define PPM_LOSS_PC         0x800940C8u
#define PPM_LOSS_INSN       0x8C820010u /* lw v0,16(a0), after the failed flag */
#define PPM_EXIT_PC         0x80095730u
#define PPM_EXIT_INSN       0x0C01594Au /* jal 0x80056528 (main menu) */
#define PPM_REG_V0 2
#define PPM_REG_V1 3
#define PPM_REG_A2 6
#define PPM_REG_S1 17
#define PPM_REG_S3 19

/* Mission source picker (the Ex Arena category list). */
#define PPS_FADE_PC         0x800943D8u
#define PPS_FADE_INSN       0x8FBF0010u /* lw ra,16(sp), after the fade is set up (v0 = fade+0x80) */
#define PPS_MISSION_CB      0x800943E8u /* the mission screen's loader */
#define PPS_LIST_CB         0x800BA684u /* the Ex Arena opener */
#define PPS_BRANCH_A_PC     0x800BA6B8u
#define PPS_BRANCH_A_INSN   0x14400007u /* bnez v0 (Ex Arena, not Arena) */
#define PPS_BRANCH_B_PC     0x800BA6E0u
#define PPS_BRANCH_B_INSN   0x10400018u /* beqz v0 (list, not back into an arena) */
#define PPS_MADE_PC         0x800BA750u
#define PPS_MADE_INSN       0x0802E9E5u /* j 0x800ba794, after the list is made (v0) */
#define PPS_OPEN_PC         0x800BAFC8u
#define PPS_WINDOW_INSN     0x0C018FBDu /* jal 0x80063ef4 (window) */
#define PPS_CLOSE_A_PC      0x800BB4C4u
#define PPS_CLOSE_B_PC      0x800BB5B0u
#define PPS_CLOSE_INSN      0x0C0190D2u /* jal 0x80064348 (closing window) */
#define PPS_BARS_PC         0x800BB1B0u
#define PPS_BARS_INSN       0x28420004u /* slti v0,v0,4 */
#define PPS_DOWN_PC         0x800BB398u
#define PPS_DOWN_INSN       0x10620003u /* beq v1,v0 (cursor on the last row) */
#define PPS_SCROLL_PC       0x800BB3B0u
#define PPS_SCROLL_INSN     0x106200A5u /* beq v1,v0 (list at its end) */
#define PPS_ARROW_PC        0x800BB864u
#define PPS_ARROW_INSN      0x14400004u /* bnez v0 (more below: down arrow) */
#define PPS_NAME_PC         0x800BB7E8u
#define PPS_DESC_PC         0x800BB920u
#define PPS_TEXT_INSN       0x0C0194CCu /* jal 0x80065330 (text) */
#define PPS_CHOSEN_PC       0x800BB6D8u
#define PPS_CHOSEN_INSN     0x14430011u /* bne v0,v1 (window still closing) */
#define PPS_CANCEL_PC       0x800BB5FCu
#define PPS_FADE_CANCEL_PC  0x800BA784u
#define PPS_FIND_INSN       0x0C015D1Cu /* jal 0x80057470 (find a task by its maker) */
#define PPS_ARENA_HEADER    0x800BA7B0u
#define PPS_MISSION_HEADER  0x80097CA8u
#define PPS_ROWS            3u
#define PPS_WINDOW_H        0x48u       /* three rows of 24 (the arena list: 0x60, four) */
#define PPS_LIST_FLAGS      0xA4u       /* task+0x80+0x24: a flag byte per item (0x02 grey) */
#define PPS_LIST_ROW        0xAEu
#define PPS_LIST_TOP        0xAFu
#define PPS_TASK_UPDATE     0x78u
#define PPS_TASK_READY      0x8Au       /* the loader's "fade done" byte */
#define PPS_SOURCE_AC       0u
#define PPS_SOURCE_PP       1u
#define PPS_SOURCE_MOA      2u

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
#define PPM_MEM_STR         ((PPM_MEM_MIS + 2u * PPM_MIS_INDEX + 64u + 15u) & ~15u)
#define PPM_MEM_STR_BYTES   256u
#define PPM_MEM_BYTES       (PPM_MEM_STR + PPM_MEM_STR_BYTES)

static uint32_t s_ppm_memory;
static int      s_picker;                /* the source list is up */
static uint8_t  s_source = PPS_SOURCE_PP;/* last source chosen */
static int      s_view;                  /* Phantasma view in T (Master of Arena's flags kept aside) */
static uint8_t  s_moa_flags[PPM_SLOTS];
static int8_t   s_slot_pp[PPM_SLOTS];    /* Phantasma mission in each slot, -1 none */
static int      s_battle_pp = -1;        /* Phantasma mission chosen to play */
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

/* Missions left out of the rotation for now: 1, 7, 9 and 14 hang on a black
 * screen after loading, 11 loads with a broken view (ported code faults that
 * are not yet found). */
static const uint8_t k_pp_skip[PPM_MISSIONS] = {
    1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0,
};

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

/* ---- the Phantasma view in the mission table ---- */

/* Master of Arena's done flags go back into T (its progress is saved from
 * there). */
static void ppm_restore_flags(void) {
    if (!s_view) return;
    for (uint32_t i = 0; i < PPM_SLOTS; ++i)
        psx_mod_write_byte(PPM_T + PPM_T_FLAGS + i, s_moa_flags[i]);
    s_view = 0;
}

static int ppm_pp_available(void) {
    return ppm_load_tables();
}

/* A. After the menu copies MIS.T entry 0 into T+80. */
static void ppm_on_table(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[PPM_REG_S0] != PPM_T + PPM_T_WINDOWS) {
        psx_mod_counter_add("moa.phantasma-missions.guard_rejected", 1);
        return;
    }
    ppm_restore_flags();                  /* left over (no result seen) */
    memset(s_slot_pp, -1, sizeof s_slot_pp);
    /* The Dummy slot is never listed. */
    const uint32_t dummy_window = PPM_T + PPM_T_WINDOWS + PPM_SLOT * 4u;
    const uint8_t dummy_w2 = psx_mod_read_byte(dummy_window + 2u);
    const uint8_t dummy_w3 = psx_mod_read_byte(dummy_window + 3u);
    const uint8_t dummy_byte = psx_mod_read_byte(PPM_T + PPM_T_BYTE + PPM_SLOT);
    psx_mod_write_byte(dummy_window, 0xFFu);
    psx_mod_write_byte(dummy_window + 1u, 0xFFu);
    if (s_source != PPS_SOURCE_PP || !ppm_pp_available()) {
        psx_mod_counter_add("moa.phantasma-missions.view_moa", 1);
        return;
    }
    ppm_progress_read();
    for (uint32_t i = 0; i < PPM_SLOTS; ++i)
        s_moa_flags[i] = psx_mod_read_byte(PPM_T + PPM_T_FLAGS + i);
    s_view = 1;
    const uint8_t chapter = psx_mod_read_byte(PPM_T + PPM_T_CHAPTER);
    for (uint32_t slot = 0; slot < PPM_SLOTS; ++slot) {
        const uint32_t window = PPM_T + PPM_T_WINDOWS + slot * 4u;
        if (slot >= PPM_MISSIONS || k_pp_skip[slot]) {
            psx_mod_write_byte(window, 0xFFu);
            psx_mod_write_byte(window + 1u, 0xFFu);
            continue;
        }
        const uint32_t m = slot;       /* testing: mission k in slot k */
        s_slot_pp[slot] = (int8_t)m;
        psx_mod_write_byte(window, chapter);
        psx_mod_write_byte(window + 1u, chapter);
        psx_mod_write_byte(window + 2u, dummy_w2);
        psx_mod_write_byte(window + 3u, dummy_w3);
        ppm_put_name(PPM_T + PPM_T_NAMES + slot * PPM_NAME_BYTES, "",
                     k_pp_short[m] ? (const uint8_t *)k_pp_short[m] : s_pp_names[m]);
        psx_mod_write_word(PPM_T + PPM_T_REWARDS + slot * 4u, s_pp_rewards[m]);
        psx_mod_write_byte(PPM_T + PPM_T_ENGINE + slot, (uint8_t)PPM_SLOT);
        psx_mod_write_byte(PPM_T + PPM_T_BYTE + slot, dummy_byte);
        /* Briefing pages: Phantasma's entry v+1 is our entry 410+v+1. */
        for (uint32_t j = 0; j < 16u; ++j) {
            const int16_t v = s_pp_pages[m][j];
            psx_mod_write_half(PPM_T + PPM_T_PAGES + slot * 32u + j * 2u,
                               (uint16_t)(v < 0 ? -1 : v + (int)PPM_MIS_ENTRIES + 1));
        }
        /* Listed whether done or not (testing); marks come after the list. */
        psx_mod_write_byte(PPM_T + PPM_T_FLAGS + slot, 0u);
    }
    ppm_install_mis_index();
    psx_mod_counter_add("moa.phantasma-missions.view_pp", 1);
}

/* B. List builder return: done marks for finished Phantasma missions. */
static void ppm_on_list(struct CPUState* cpu, uint32_t address) {
    (void)cpu;
    (void)address;
    if (!s_view) return;
    if (psx_mod_read_word(PPM_LIST_GUARD_PC) != PPM_LIST_GUARD_INSN) {
        psx_mod_counter_add("moa.phantasma-missions.guard_rejected", 1);
        return;
    }
    for (uint32_t slot = 0; slot < PPM_SLOTS; ++slot)
        if (s_slot_pp[slot] >= 0)
            psx_mod_write_byte(PPM_T + PPM_T_FLAGS + slot, s_done[s_slot_pp[slot]] ? 0x02u : 0u);
}

/* C. The mission chosen (a3 = slot, s3 = T). */
static void ppm_on_chosen(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[PPM_REG_S3] != PPM_T) return;
    const uint32_t slot = cpu->gpr[PPM_REG_A3];
    s_battle_pp = (s_view && slot < PPM_SLOTS) ? s_slot_pp[slot] : -1;
}

/* D. Mission result (a0 = T, v1 = T + slot, the flag just set) and leaving
 * the screen. */
static void ppm_result(struct CPUState* cpu, int won) {
    if (cpu->gpr[PPM_REG_A0] != PPM_T) return;
    if (s_view && s_battle_pp >= 0 && won) {
        s_done[s_battle_pp] = 1;
        ppm_progress_write();
        psx_mod_counter_add("moa.phantasma-missions.finished", 1);
    }
    ppm_restore_flags();
}

static void ppm_on_win(struct CPUState* cpu, uint32_t address) {
    (void)address;
    ppm_result(cpu, 1);
}

static void ppm_on_loss(struct CPUState* cpu, uint32_t address) {
    (void)address;
    ppm_result(cpu, 0);
}

static void ppm_on_exit(struct CPUState* cpu, uint32_t address) {
    (void)cpu;
    (void)address;
    ppm_restore_flags();
}

/* ---- mission source picker ---- */

static const char *const k_pps_names[PPS_ROWS] = {
    "ARMORED CORE", "PROJECT PHANTASMA", "MASTER OF ARENA",
};
static const char *const k_pps_desc[PPS_ROWS] = {
    "Not installed.", "Project Phantasma missions.", "Master of Arena missions.",
};

/* Text lives in mod memory: names at +0 (32 each), descriptions at +96
 * (40 each), an empty string at +224. */
static uint32_t pps_text(uint32_t which) {
    const uint32_t memory = ppm_memory();
    return memory ? memory + PPM_MEM_STR + which : 0u;
}

static void pps_write_text(void) {
    const uint32_t base = pps_text(0);
    if (!base) return;
    for (uint32_t i = 0; i < PPM_MEM_STR_BYTES; ++i) psx_mod_write_byte(base + i, 0u);
    for (uint32_t r = 0; r < PPS_ROWS; ++r) {
        for (uint32_t i = 0; k_pps_names[r][i]; ++i)
            psx_mod_write_byte(base + r * 32u + i, (uint8_t)k_pps_names[r][i]);
        for (uint32_t i = 0; k_pps_desc[r][i]; ++i)
            psx_mod_write_byte(base + 96u + r * 40u + i, (uint8_t)k_pps_desc[r][i]);
    }
}

static int pps_greyed(uint32_t row) {
    return row == PPS_SOURCE_AC || (row == PPS_SOURCE_PP && !ppm_pp_available());
}

/* P1. The mission screen's fade is set up (v0 = fade + 0x80, +4 callback). */
static void pps_on_fade(struct CPUState* cpu, uint32_t address) {
    (void)address;
    const uint32_t fade = cpu->gpr[PPM_REG_V0];
    if ((fade >> 24) != 0x80u || psx_mod_read_word(fade + 4u) != PPS_MISSION_CB) return;
    if (!pps_text(0)) return;
    pps_write_text();
    psx_mod_write_word(fade + 4u, PPS_LIST_CB);
    s_picker = 1;
    psx_mod_counter_add("moa.phantasma-missions.picker_opened", 1);
}

static void pps_set_v0(struct CPUState* cpu, uint32_t address) {
    if (!s_picker) return;
    cpu->gpr[PPM_REG_V0] = address == PPS_BRANCH_A_PC ? 1u : 0u;
}

/* P2. The list task is made (v0). */
static void pps_on_made(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_picker) return;
    const uint32_t task = cpu->gpr[PPM_REG_V0];
    if ((task >> 24) != 0x80u) { s_picker = 0; return; }
    for (uint32_t i = 0; i < 9u; ++i)
        psx_mod_write_byte(task + PPS_LIST_FLAGS + i, i < PPS_ROWS && !pps_greyed(i) ? 0u : 0x02u);
    uint32_t row = s_source;
    if (row >= PPS_ROWS || pps_greyed(row)) row = PPS_SOURCE_MOA;
    psx_mod_write_byte(task + PPS_LIST_ROW, (uint8_t)row);
    psx_mod_write_byte(task + PPS_LIST_TOP, 0u);
}

/* P3. Window height, bars, cursor and arrow limits, text. */
static void pps_window(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (s_picker && cpu->gpr[PPM_REG_A3] == 0x60u) cpu->gpr[PPM_REG_A3] = PPS_WINDOW_H;
}

static void pps_bars(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (s_picker && (int32_t)cpu->gpr[PPM_REG_V0] >= (int32_t)PPS_ROWS) cpu->gpr[PPM_REG_V0] = 4u;
}

static void pps_down(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (s_picker) cpu->gpr[PPM_REG_V0] = PPS_ROWS - 1u;   /* last row: try to scroll */
}

static void pps_scroll(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (s_picker) cpu->gpr[PPM_REG_V0] = cpu->gpr[PPM_REG_V1];   /* never scrolls */
}

static void pps_arrow(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (s_picker) cpu->gpr[PPM_REG_V0] = 0u;
}

static void pps_name(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_picker) return;
    const uint32_t item = cpu->gpr[PPM_REG_S1];
    const uint32_t text = pps_text(item < PPS_ROWS ? item * 32u : 224u);
    if (text) cpu->gpr[PPM_REG_A1] = text;
}

static void pps_desc(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_picker) return;
    const uint32_t item = cpu->gpr[PPM_REG_S1];
    if (item >= PPS_ROWS) return;
    uint32_t text = pps_text(96u + item * 40u);
    if (item == PPS_SOURCE_PP && pps_greyed(item)) text = pps_text(96u);   /* "Not installed." */
    if (text) cpu->gpr[PPM_REG_A1] = text;
}

/* P4. Confirmed: once the window has closed (v0 == v1), the list task (s0)
 * becomes the mission screen's loader instead of opening an arena. */
static void pps_chosen(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_picker) return;
    if (cpu->gpr[PPM_REG_V0] != cpu->gpr[PPM_REG_V1]) return;   /* still closing */
    const uint32_t task = cpu->gpr[PPM_REG_S0];
    const uint32_t row = psx_mod_read_byte(task + PPS_LIST_ROW) + psx_mod_read_byte(task + PPS_LIST_TOP);
    s_source = (uint8_t)(row < PPS_ROWS ? row : PPS_SOURCE_MOA);
    psx_mod_write_byte(task + PPS_TASK_READY, 1u);
    psx_mod_write_word(task + PPS_TASK_UPDATE, PPS_MISSION_CB);
    cpu->gpr[PPM_REG_V0] = cpu->gpr[PPM_REG_V1] + 4u;   /* skip the arena */
    s_picker = 0;
    psx_mod_counter_add(s_source == PPS_SOURCE_PP ? "moa.phantasma-missions.picked_pp"
                                                  : "moa.phantasma-missions.picked_moa", 1);
}

/* Cancelled: close the mission header, not the arena's. */
static void pps_cancel(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_picker || cpu->gpr[PPM_REG_A0] != PPS_ARENA_HEADER) return;
    cpu->gpr[PPM_REG_A0] = PPS_MISSION_HEADER;
    s_picker = 0;
}

/* E. Battle engine loads while engine mission 20 runs a Phantasma mission. */
static int ppm_ready(uint32_t *mission) {
    if (psx_mod_read_byte(PPM_MISSION_ADDR) != PPM_SLOT) return 0;
    if (s_battle_pp < 0 || (uint32_t)s_battle_pp >= PPM_MISSIONS) return 0;
    if (!ppm_install_pack()) return 0;
    *mission = (uint32_t)s_battle_pp;
    return 1;
}

/* The texture call's registers are set directly. (Writing the entry number
 * into the engine's slot byte for the load, as the test build did, left the
 * byte at 0 for Phantasma mission 1 - Master of Arena's first mission - and
 * that mission crashed while loading.) */
static void ppm_textures(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t m;
    if (cpu->gpr[PPM_REG_S2] + PPM_LOADING_OFFSET != PPM_LOADING_ADDR ||
        cpu->gpr[PPM_REG_A0] != 1u || cpu->gpr[PPM_REG_A1] != PPM_SLOT)
        return;
    if (!ppm_ready(&m)) return;
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
    cpu->gpr[PPM_REG_A1] = m * 4u + PPM_ENTRY_TEXTURES;
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
    s_picker = 0;
    s_view = 0;
    s_battle_pp = -1;
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_phantasma_missions) {
    static const char id[] = "moa.phantasma-missions";
    (void)psx_mod_register_activation_plugin(id, ppm_activate);
    (void)psx_mod_register_instruction_plugin(id, PPM_TABLE_PC, PPM_TABLE_INSN, ppm_on_table);
    (void)psx_mod_register_instruction_plugin(id, PPM_LIST_PC, PPM_LIST_INSN, ppm_on_list);
    (void)psx_mod_register_instruction_plugin(id, PPM_CHOSEN_PC, PPM_CHOSEN_INSN, ppm_on_chosen);
    (void)psx_mod_register_instruction_plugin(id, PPM_WIN_PC, PPM_WIN_INSN, ppm_on_win);
    (void)psx_mod_register_instruction_plugin(id, PPM_LOSS_PC, PPM_LOSS_INSN, ppm_on_loss);
    (void)psx_mod_register_instruction_plugin(id, PPM_EXIT_PC, PPM_EXIT_INSN, ppm_on_exit);
    (void)psx_mod_register_instruction_plugin(id, PPM_TEX_PC, PPM_TEX_INSN, ppm_textures);
    (void)psx_mod_register_instruction_plugin(id, PPM_COMMON_PC, PPM_COMMON_INSN, ppm_common);
    (void)psx_mod_register_instruction_plugin(id, PPM_CODE_PC, PPM_CODE_INSN, ppm_code);
    (void)psx_mod_register_instruction_plugin(id, PPM_MAP_PC, PPM_MAP_INSN, ppm_map);
    /* Source picker. */
    (void)psx_mod_register_instruction_plugin(id, PPS_FADE_PC, PPS_FADE_INSN, pps_on_fade);
    (void)psx_mod_register_instruction_plugin(id, PPS_BRANCH_A_PC, PPS_BRANCH_A_INSN, pps_set_v0);
    (void)psx_mod_register_instruction_plugin(id, PPS_BRANCH_B_PC, PPS_BRANCH_B_INSN, pps_set_v0);
    (void)psx_mod_register_instruction_plugin(id, PPS_MADE_PC, PPS_MADE_INSN, pps_on_made);
    (void)psx_mod_register_instruction_plugin(id, PPS_OPEN_PC, PPS_WINDOW_INSN, pps_window);
    (void)psx_mod_register_instruction_plugin(id, PPS_CLOSE_A_PC, PPS_CLOSE_INSN, pps_window);
    (void)psx_mod_register_instruction_plugin(id, PPS_CLOSE_B_PC, PPS_CLOSE_INSN, pps_window);
    (void)psx_mod_register_instruction_plugin(id, PPS_BARS_PC, PPS_BARS_INSN, pps_bars);
    (void)psx_mod_register_instruction_plugin(id, PPS_DOWN_PC, PPS_DOWN_INSN, pps_down);
    (void)psx_mod_register_instruction_plugin(id, PPS_SCROLL_PC, PPS_SCROLL_INSN, pps_scroll);
    (void)psx_mod_register_instruction_plugin(id, PPS_ARROW_PC, PPS_ARROW_INSN, pps_arrow);
    (void)psx_mod_register_instruction_plugin(id, PPS_NAME_PC, PPS_TEXT_INSN, pps_name);
    (void)psx_mod_register_instruction_plugin(id, PPS_DESC_PC, PPS_TEXT_INSN, pps_desc);
    (void)psx_mod_register_instruction_plugin(id, PPS_CHOSEN_PC, PPS_CHOSEN_INSN, pps_chosen);
    (void)psx_mod_register_instruction_plugin(id, PPS_CANCEL_PC, PPS_FIND_INSN, pps_cancel);
    (void)psx_mod_register_instruction_plugin(id, PPS_FADE_CANCEL_PC, PPS_FIND_INSN, pps_cancel);
}
