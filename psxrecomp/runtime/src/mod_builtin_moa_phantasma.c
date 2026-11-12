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
 * Project Phantasma missions, test build (plugin ids
 * "moa.phantasma-mission-01" .. "-17").
 *
 * A mod package supplies a pack in the disc's padding area with four
 * checksummed blocks per Phantasma mission: its texture pack, a copy of the
 * common battle data with that mission's messages, the mission code (ported
 * to this game's engine) and the map data. The battle engine loads those four
 * blocks with the game's loader, 0x80016a58(file, entry, destination):
 *      800556b4  lbu a1,3504(s2) ; 800556b8 jal  -> textures, file 1, entry = slot
 *      800556d4  jal                              -> common,   file 2, entry 205
 *      80055874  jal ; addu a1,a1,v0              -> code,     file 2, entry slot*5
 *      80055894  jal                              -> map,      file 2, entry slot*5+4
 * The game's file table has 12 bytes per file (index pointer, disc position,
 * size) and never opens file 3. The plugin describes the pack as file 3 and,
 * while Master of Arena's first mission slot is loading, points those four
 * loads at the blocks of the Phantasma mission its plugin id names. The
 * texture load takes its entry from a byte read by the instruction at the
 * hook, so that byte is set for the load and put back straight after.
 * All hooks are on disc-loaded (interpreted) code and guarded by exact
 * instruction words; if file 3 is not free the mission loads unchanged.
 */
/* Generated by the mission package builder; matches MoA-PhantasmaMissionTest 3.2.0. */
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
#define PPM_SLOT            0u          /* Master of Arena mission slot used for the test */
#define PPM_FILE            3u
#define PPM_MISSION_ADDR    0x800453EDu /* mission slot chosen in the menu */
#define PPM_LOADING_OFFSET  3504u       /* engine's copy, at s2 + 3504 */
#define PPM_LOADING_ADDR    0x800A0DB0u
#define PPM_REG_A0 4
#define PPM_REG_A1 5
#define PPM_REG_S2 18

static uint32_t s_ppm_memory;

/* True while the test slot is loading and file 3 describes the pack. */
static int ppm_ready(void) {
    if (psx_mod_read_byte(PPM_MISSION_ADDR) != PPM_SLOT) return 0;
    const uint32_t slot = PP_FILE_TABLE + PPM_FILE * 12u;
    if (!s_ppm_memory) {
        s_ppm_memory = psx_mod_alloc_guest_memory(PPM_INDEX_WORDS * 2u, 4u);
        if (!s_ppm_memory) {
            psx_mod_counter_add("moa.phantasma-missions.no_memory", 1);
            return 0;
        }
    }
    const uint32_t index = psx_mod_read_word(slot);
    if (index != s_ppm_memory) {
        /* Free, or left by an earlier session of this plugin (saved state). */
        if (index != 0u && psx_mod_read_word(slot + 4u) != PPM_PACK_LOC) {
            psx_mod_counter_add("moa.phantasma-missions.file_slot_in_use", 1);
            return 0;
        }
        for (uint32_t j = 0; j < PPM_INDEX_WORDS; ++j)
            psx_mod_write_half(s_ppm_memory + j * 2u, ppm_index[j]);
        psx_mod_write_word(slot + 4u, PPM_PACK_LOC);
        psx_mod_write_word(slot + 8u, PPM_PACK_BYTES);
        psx_mod_write_word(slot, s_ppm_memory);
        psx_mod_counter_add("moa.phantasma-missions.file_installed", 1);
    }
    return 1;
}

static void ppm_textures(struct CPUState* cpu, uint32_t mission) {
    if (cpu->gpr[PPM_REG_S2] + PPM_LOADING_OFFSET != PPM_LOADING_ADDR ||
        cpu->gpr[PPM_REG_A0] != 1u) {
        psx_mod_counter_add("moa.phantasma-missions.guard_rejected", 1);
        return;
    }
    if (!ppm_ready()) return;
    psx_mod_write_byte(PPM_LOADING_ADDR, (uint8_t)(mission * 4u + PPM_ENTRY_TEXTURES));
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
}

static void ppm_textures_done(struct CPUState* cpu, uint32_t mission) {
    (void)cpu; (void)mission;
    if (psx_mod_read_byte(PPM_MISSION_ADDR) != PPM_SLOT) return;
    psx_mod_write_byte(PPM_LOADING_ADDR, (uint8_t)PPM_SLOT);
}

static void ppm_common(struct CPUState* cpu, uint32_t mission) {
    if (cpu->gpr[PPM_REG_A0] != 2u || cpu->gpr[PPM_REG_A1] != 205u) {
        psx_mod_counter_add("moa.phantasma-missions.guard_rejected", 1);
        return;
    }
    if (!ppm_ready()) return;
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
    cpu->gpr[PPM_REG_A1] = mission * 4u + PPM_ENTRY_COMMON;
}

static void ppm_code(struct CPUState* cpu, uint32_t mission) {
    /* a1 = slot*4 here; the delay slot adds v0 (= slot). */
    if (!ppm_ready()) return;
    if (cpu->gpr[PPM_REG_A0] != 2u || cpu->gpr[PP_REG_V0] != PPM_SLOT ||
        cpu->gpr[PPM_REG_A1] != PPM_SLOT * 4u) {
        psx_mod_counter_add("moa.phantasma-missions.guard_rejected", 1);
        return;
    }
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
    cpu->gpr[PPM_REG_A1] = mission * 4u + PPM_ENTRY_CODE - cpu->gpr[PP_REG_V0];
}

static void ppm_map(struct CPUState* cpu, uint32_t mission) {
    if (!ppm_ready()) return;
    if (cpu->gpr[PPM_REG_A0] != 2u || cpu->gpr[PPM_REG_A1] != PPM_SLOT * 5u + 4u) {
        psx_mod_counter_add("moa.phantasma-missions.guard_rejected", 1);
        return;
    }
    cpu->gpr[PPM_REG_A0] = PPM_FILE;
    cpu->gpr[PPM_REG_A1] = mission * 4u + PPM_ENTRY_MAP;
    psx_mod_counter_add("moa.phantasma-missions.loaded", 1);
}

/* Test diagnostics. The crash reports include the 1 KiB scratchpad, so four
 * probes record the status register (bit 0 = interrupts enabled) in a part
 * of it the game leaves at zero: per probe, a hit count, the latest status
 * and return address, and the return address of the first hit that found
 * interrupts disabled. */
#define PPM_PROBE_BASE      0x1F800280u
#define PPM_PROBE_LOAD_PC   0x80055628u
#define PPM_PROBE_LOAD_INSN 0x27BDFFD0u /* loader entry: addiu sp,sp,-48 */
#define PPM_PROBE_INIT_PC   0x80055B00u
#define PPM_PROBE_INIT_INSN 0x0C0154ACu /* after the mission's start callback: jal 0x800552b0 */
#define PPM_PROBE_MSG_PC    0x80058B1Cu
#define PPM_PROBE_MSG_INSN  0x27BDFFE8u /* show message: addiu sp,sp,-24 */
#define PPM_PROBE_PLAY_PC   0x80053CA0u
#define PPM_PROBE_PLAY_INSN 0x27BDFFD8u /* start voice: addiu sp,sp,-40 */

static void ppm_probe(struct CPUState* cpu, uint32_t address) {
    uint32_t n;
    switch (address & 0x1FFFFFFFu) {
    case PPM_PROBE_LOAD_PC & 0x1FFFFFFFu: n = 0; break;
    case PPM_PROBE_INIT_PC & 0x1FFFFFFFu: n = 1; break;
    case PPM_PROBE_MSG_PC & 0x1FFFFFFFu:  n = 2; break;
    case PPM_PROBE_PLAY_PC & 0x1FFFFFFFu: n = 3; break;
    default: return;
    }
    if (psx_mod_read_byte(PPM_MISSION_ADDR) != PPM_SLOT) return;
    const uint32_t at = PPM_PROBE_BASE + n * 16u;
    const uint32_t status = cpu->cop0[12];
    if (n == 0) /* a new load starts a new record */
        for (uint32_t j = 0; j < 64u; j += 4u) psx_mod_write_word(PPM_PROBE_BASE + j, 0u);
    psx_mod_write_word(at, psx_mod_read_word(at) + 1u);
    psx_mod_write_word(at + 4u, status);
    psx_mod_write_word(at + 8u, cpu->gpr[31]);
    if (!(status & 1u) && psx_mod_read_word(at + 12u) == 0u)
        psx_mod_write_word(at + 12u, cpu->gpr[31]);
}

static void ppm_activate(void) {
    s_ppm_memory = 0; /* this session's plan gets its own allocation */
}

/* One plugin id per Phantasma mission; n is the mission's index (0-16). */
#define PPM_MISSION(n, name)                                                      \
    static void ppm_tex_##n(struct CPUState* c, uint32_t a) { (void)a; ppm_textures(c, n##u); }      \
    static void ppm_texdone_##n(struct CPUState* c, uint32_t a) { (void)a; ppm_textures_done(c, n##u); } \
    static void ppm_common_##n(struct CPUState* c, uint32_t a) { (void)a; ppm_common(c, n##u); }     \
    static void ppm_code_##n(struct CPUState* c, uint32_t a) { (void)a; ppm_code(c, n##u); }         \
    static void ppm_map_##n(struct CPUState* c, uint32_t a) { (void)a; ppm_map(c, n##u); }           \
    PSX_MOD_CONSTRUCTOR(psx_register_moa_phantasma_mission_##n) {                 \
        static const char id[] = name;                                            \
        (void)psx_mod_register_activation_plugin(id, ppm_activate);               \
        (void)psx_mod_register_instruction_plugin(id, PPM_TEX_PC, PPM_TEX_INSN, ppm_tex_##n);             \
        (void)psx_mod_register_instruction_plugin(id, PPM_TEXDONE_PC, PPM_TEXDONE_INSN, ppm_texdone_##n); \
        (void)psx_mod_register_instruction_plugin(id, PPM_COMMON_PC, PPM_COMMON_INSN, ppm_common_##n);    \
        (void)psx_mod_register_instruction_plugin(id, PPM_CODE_PC, PPM_CODE_INSN, ppm_code_##n);          \
        (void)psx_mod_register_instruction_plugin(id, PPM_MAP_PC, PPM_MAP_INSN, ppm_map_##n);             \
        (void)psx_mod_register_instruction_plugin(id, PPM_PROBE_LOAD_PC, PPM_PROBE_LOAD_INSN, ppm_probe); \
        (void)psx_mod_register_instruction_plugin(id, PPM_PROBE_INIT_PC, PPM_PROBE_INIT_INSN, ppm_probe); \
        (void)psx_mod_register_instruction_plugin(id, PPM_PROBE_MSG_PC, PPM_PROBE_MSG_INSN, ppm_probe);   \
        (void)psx_mod_register_instruction_plugin(id, PPM_PROBE_PLAY_PC, PPM_PROBE_PLAY_INSN, ppm_probe); \
    }
PPM_MISSION(0, "moa.phantasma-mission-01")
PPM_MISSION(1, "moa.phantasma-mission-02")
PPM_MISSION(2, "moa.phantasma-mission-03")
PPM_MISSION(3, "moa.phantasma-mission-04")
PPM_MISSION(4, "moa.phantasma-mission-05")
PPM_MISSION(5, "moa.phantasma-mission-06")
PPM_MISSION(6, "moa.phantasma-mission-07")
PPM_MISSION(7, "moa.phantasma-mission-08")
PPM_MISSION(8, "moa.phantasma-mission-09")
PPM_MISSION(9, "moa.phantasma-mission-10")
PPM_MISSION(10, "moa.phantasma-mission-11")
PPM_MISSION(11, "moa.phantasma-mission-12")
PPM_MISSION(12, "moa.phantasma-mission-13")
PPM_MISSION(13, "moa.phantasma-mission-14")
PPM_MISSION(14, "moa.phantasma-mission-15")
PPM_MISSION(15, "moa.phantasma-mission-16")
PPM_MISSION(16, "moa.phantasma-mission-17")
