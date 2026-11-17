/*
 * Armored Core: Master of Arena - OP-WLINK, a custom option part.
 *
 * With OP-WLINK fitted, weapons of the same family fire together from one
 * press: missiles with missiles, rockets with rockets, and two back cannons.
 * Weapon arms can link with back weapons; hand weapons never link. Linked
 * weapons share averaged lock-on and reload values, keep their own damage
 * and ammo, count as one entry for the weapon switch (triangle), and show
 * their own type and ammo on the HUD, with ARM / BACK L / BACK R added.
 * Design: docs/custom-parts/op-wlink.md. Addresses: docs/mapping/.
 *
 * Menu module (FDAT 206):
 *   - Option ID 13 is one of six "DUMMY" records (0x800D1CDC, 44 bytes) the
 *     menus already loop over (IDs 0-18). The menu module's memory is never
 *     written: the ten places that load the table's address (lui 0x800D /
 *     addiu 0x1AA0) get our copy of the table instead, with ID 13 as
 *     OP-WLINK. (The first test build wrote the record into the loaded menu
 *     module from a VBlank callback and the game hung on its first loading
 *     screen; writes there are watched as code changes.)
 *   - Owned from the start: bit 13 of the owned-options word 0x8004B558 is
 *     set just before menu code reads it (AC SETUP's "No OPTIONAL parts"
 *     check at 0x8007D764 and the option list helpers).
 *   - The description table 0x800CDE8C has only 13 entries: the read at
 *     0x80092004 is pointed at our own entry for ID 13.
 *   - After compile(player) 0x800A7BB4 builds the stat block, each weapon slot
 *     gets the link data in its unused bytes +59..+61 (no reader in any
 *     engine; +58 stays 0, it ends the HUD name), and linked slots get the averaged lock and reload values.
 *
 * Split-screen VS engine (FDAT 209) only, for now:
 *   - Fire: the selected weapon's fire routine (gun 0x80080DF4, missile
 *     0x80081888, cannon 0x80081B18, a0 = AC object, a1 = slot) runs as
 *     usual; then each linked weapon's routine runs with its own burst and
 *     cooldown (AC +0x47 / +0x48) swapped in. The AC object and the lock
 *     state are put back afterwards, so a linked shot never moves the AC,
 *     never plays its animation and never speeds up the lock. A missile
 *     salvo keeps its own copy of the locks until it has fired.
 *   - Weapon switch: at 0x80083030 the next selection skips the selected
 *     weapon's partners. When the selected weapon runs dry, the selection
 *     moves to a partner that still has ammo.
 *   - HUD: the name lines at 0x80056DC8 / 0x80056EC4 get "<type> <slot>",
 *     right-aligned where the game's name ended. Partners stay in the
 *     game's weapon list below with their own ammo.
 *
 * Every hook checks the code words it relies on and the mode byte.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <string.h>

/* --- Shared addresses ---------------------------------------------------- */
#define WL_MODE_BYTE      0x8003EA2Au  /* 2 = split-screen VS */
#define WL_MODE_VS        2u
#define WL_PLAYER_PTR     0x8003EAA0u  /* current player block */
#define WL_PLAYER_BLOCK   0x80045198u  /* player 1; player 2 at +928 */
#define WL_PLAYER_BYTES   928u
#define WL_LOADOUT        0x80036FB0u  /* garage parts at compile time */
#define WL_FITTED_MASK    0x8003EA70u  /* fitted options at compile time */
#define WL_OWNED_MASK     0x8004B558u  /* owned options (list mask) */

/* Player block fields. */
#define PB_ACTIVE         0x10u        /* weapon in use */
#define PB_PENDING        0x11u        /* weapon picked (applied when ready) */
#define PB_LOCK_A         0x1Au        /* lock state: +0x1A..+0x2F */
#define PB_LOCK_A_BYTES   22u
#define PB_LOCK_B         0x78u        /* target + 8 lock records: +0x78..+0x1FB */
#define PB_LOCK_B_BYTES   388u
#define PB_COUNT          0x2D7u       /* usable weapons (compiled +0x87) */
#define PB_SLOTS          0x2D8u       /* weapon slots (compiled +0x88) */
#define PB_AC             0x398u       /* AC object pointer */
#define WL_SLOT_BYTES     62u
#define WL_SLOTS          3u

/* AC object fields. */
#define AC_BYTES          376u
#define AC_BURST          0x47u
#define AC_COOLDOWN       0x48u
#define AC_HOLD           0x6Du

/* Weapon slot fields (stat-block.md). */
#define SL_KIND           0u
#define SL_PATTERN        9u
#define SL_AMMO           16u
#define SL_LOCK_RANGE     20u
#define SL_LOCK_BOX       24u          /* 4 x u16 */
#define SL_LOCK_VALUE     32u
#define SL_LOCKS          34u
#define SL_INTERVAL       35u
#define SL_NAME           42u          /* 16 chars, right-aligned */
#define SL_TERM           58u          /* 0: ends the HUD name; kept 0 */
#define SL_GROUP          59u          /* ours: family, 0 = not linked */
#define SL_INFO           60u          /* ours: WL_MAGIC | place, when fitted */
#define SL_SIZE           61u          /* ours: weapons in this slot's group */
#define WL_MAGIC          0xA0u        /* high nibble of SL_INFO */
#define WL_NONE           0u
#define WL_LAST_NONE      0xFFu

enum { FAM_MISSILE = 1, FAM_ROCKET = 2, FAM_CANNON = 3 };
enum { PLACE_ARM, PLACE_HAND, PLACE_BACK_L, PLACE_BACK_R };

/* --- Menu module (FDAT 206) ---------------------------------------------- */
#define WL_OPTION_ID      13u
#define WL_OPTIONS        0x800D1AA0u
#define WL_OPTION_BYTES   44u
#define WL_RECORD         (WL_OPTIONS + WL_OPTION_ID * WL_OPTION_BYTES)
#define WL_RECORD_PREV    (WL_OPTIONS + 12u * WL_OPTION_BYTES) /* SP-ABS_Re */
#define WL_DESC_TABLE     0x800CDE8Cu
#define WL_DESC_PC        0x80092004u
#define WL_DESC_INSN      0x8C450000u  /* lw a1,0(v0) */
#define WL_COMPILE_FN     0x800A7BB4u
#define WL_COMPILE_SIG0   0x27BDFF90u  /* addiu sp,sp,-112 */
#define WL_COMPILE_SIG1   0x00007021u  /* addu t6,zero,zero */
#define WL_COMPILED       0x800453E8u  /* compiled block, player 1 */

/* --- VS engine (FDAT 209) ------------------------------------------------ */
#define WL_FIRE_GUN       0x80080DF4u
#define WL_FIRE_MISSILE   0x80081888u
#define WL_FIRE_CANNON    0x80081B18u
#define WL_SIG_GUN        0x27BDFF60u
#define WL_SIG_MISSILE    0x27BDFFD8u
#define WL_SIG_CANNON     0x27BDFF80u
#define WL_MATCH_START    0x8008342Cu
#define WL_CYCLE_PC       0x80083030u
#define WL_CYCLE_INSN     0x0C014C36u  /* jal 0x800530D8 (switch sound) */
#define WL_HUD_SEL_PC     0x80056DC8u  /* selected weapon: name line */
#define WL_HUD_LIST_PC    0x80056EC4u  /* weapon list: name line */
#define WL_HUD_NAME_INSN  0xAFA20014u  /* sw v0,20(sp) */
#define WL_FONT_MAP       0x8008FDE8u  /* glyph index by char (char - 32 + this + 32) */
#define WL_FONT_GLYPHS    0x80091A28u  /* 12 bytes each, +6 = width */

#define REG_V0 2
#define REG_A0 4
#define REG_A1 5
#define REG_A2 6
#define REG_A3 7
#define REG_S1 17
#define REG_RA 31

/* --- Plugin memory (guest RAM, so save states and rollback keep it) ------ */
#define MEM_DESC          0x000u       /* description text, 48 bytes */
#define MEM_DESC_PTR      0x030u       /* pointer to it (stands in for table[13]) */
#define MEM_LAST          0x034u       /* last active weapon per player (2 bytes) */
#define MEM_HUD           0x040u       /* 2 players x 3 slots x 32 bytes */
#define MEM_READY         0x036u       /* 1 once the table copy is built */
#define MEM_SHADOW        0x100u       /* 2 players x 3 slots x SHADOW_BYTES */
#define SH_BURST          0u
#define SH_COOLDOWN       1u
#define SH_LOCK_A         4u
#define SH_LOCK_B         (SH_LOCK_A + PB_LOCK_A_BYTES)
#define SHADOW_BYTES      416u
#define MEM_TABLE         (MEM_SHADOW + 2u * WL_SLOTS * SHADOW_BYTES)
#define WL_OPTION_COUNT   19u
#define MEM_BYTES         (MEM_TABLE + WL_OPTION_COUNT * WL_OPTION_BYTES)

static const char kDescription[] = "Fires same-type weapons together.";
static const char kName[] = "OP-WLINK";
static const char kType[] = "WEAPON LINKER";
#define WL_PRICE          24000u
#define WL_SLOT_COST      1u

static uint32_t s_wl_memory;
static int s_wl_nested;

static uint32_t wl_memory(void) {
    if (!s_wl_memory) {
        s_wl_memory = psx_mod_alloc_guest_memory(MEM_BYTES, 4u);
        if (!s_wl_memory) {
            psx_mod_counter_add("moa.wlink.no_memory", 1);
            return 0;
        }
        /* The allocator zeroes it; the text is filled in on first use. */
        psx_mod_write_byte(s_wl_memory + MEM_LAST, WL_LAST_NONE);
        psx_mod_write_byte(s_wl_memory + MEM_LAST + 1u, WL_LAST_NONE);
    }
    return s_wl_memory;
}

static int wl_text_is(uint32_t address, const char *text) {
    for (uint32_t i = 0;; ++i) {
        const uint8_t c = psx_mod_read_byte(address + i);
        if (c != (uint8_t)text[i]) return 0;
        if (!c) return 1;
    }
}

static void wl_write_text(uint32_t address, const char *text, uint32_t field) {
    uint32_t i = 0;
    for (; text[i] && i + 1u < field; ++i) psx_mod_write_byte(address + i, (uint8_t)text[i]);
    for (; i < field; ++i) psx_mod_write_byte(address + i, 0);
}

/* ===================== Menu: the option itself ============================ */

/* The menu module is resident when option 12 is SP-ABS_Re and option 13 is
 * the stock DUMMY. Read only. */
static int wl_menu_resident(void) {
    return wl_text_is(WL_RECORD_PREV, "SP-ABS_Re") && wl_text_is(WL_RECORD, "DUMMY");
}

/* Our copy of the option table (IDs 0-18) with ID 13 as OP-WLINK, and the
 * description entry. Built once from the loaded menu module. */
static uint32_t wl_table(void) {
    const uint32_t memory = wl_memory();
    if (!memory) return 0;
    if (psx_mod_read_byte(memory + MEM_READY)) return memory + MEM_TABLE;
    if (!wl_menu_resident()) return 0;
    const uint32_t table = memory + MEM_TABLE;
    for (uint32_t i = 0; i < WL_OPTION_COUNT * WL_OPTION_BYTES; ++i)
        psx_mod_write_byte(table + i, psx_mod_read_byte(WL_OPTIONS + i));
    const uint32_t record = table + WL_OPTION_ID * WL_OPTION_BYTES;
    wl_write_text(record, kName, 18u);
    wl_write_text(record + 18u, kType, 18u);
    psx_mod_write_word(record + 36u, WL_PRICE);
    psx_mod_write_half(record + 40u, WL_SLOT_COST);
    psx_mod_write_half(record + 42u, 0);
    for (uint32_t i = 0; i < sizeof kDescription; ++i)
        psx_mod_write_byte(memory + MEM_DESC + i, (uint8_t)kDescription[i]);
    psx_mod_write_word(memory + MEM_DESC_PTR, memory + MEM_DESC);
    psx_mod_write_byte(memory + MEM_READY, 1u);
    psx_mod_counter_add("moa.wlink.table_built", 1);
    return table;
}

/* The instruction after each "addiu X,R,0x1AA0": X holds the table address
 * the game just made; give it our copy instead. */
typedef struct { uint32_t pc, insn; uint8_t reg; } WlTableSite;
static const WlTableSite kTableSites[] = {
    { 0x80087A50u, 0x3C03800Du,  8 },   /* t0  (lui v1,0x800D)   */
    { 0x80091ADCu, 0x00A82821u,  8 },   /* t0  (addu a1,a1,t0)   */
    { 0x80091DF4u, 0x3C03800Du,  6 },   /* a2  (lui v1,0x800D)   */
    { 0x80091E70u, 0x324200FFu, 17 },   /* s1  (andi v0,s2,0xFF) */
    { 0x80092420u, 0x3C03800Du,  8 },   /* t0  (lui v1,0x800D)   */
    { 0x800924A0u, 0x312300FFu,  4 },   /* a0  (andi v1,t1,0xFF) */
    { 0x80092C30u, 0x3C028004u,  7 },   /* a3  (lui v0,0x8004)   */
    { 0x8009B3E4u, 0x03E00008u,  3 },   /* v1  (jr ra)           */
    { 0x8009B574u, 0x30A300FFu,  4 },   /* a0  (andi v1,a1,0xFF) */
    { 0x8009FAB8u, 0x00151400u, 18 },   /* s2  (sll v0,s5,16)    */
};

static void wl_table_hook(struct CPUState *cpu, uint32_t address) {
    for (unsigned i = 0; i < sizeof kTableSites / sizeof kTableSites[0]; ++i) {
        if ((kTableSites[i].pc ^ address) & 0x1FFFFFFFu) continue;
        const uint8_t reg = kTableSites[i].reg;
        if (cpu->gpr[reg] != WL_OPTIONS) return;
        const uint32_t table = wl_table();
        if (table) cpu->gpr[reg] = table;
        return;
    }
}

/* Owned from the start: set bit 13 just before any menu code reads the
 * owned word (lw rt,-19112(v0), v0 = 0x80050000). 0x8007D764 is the AC
 * SETUP check that shows "No OPTIONAL parts available." when the word is 0;
 * the rest are the option list helpers. */
typedef struct { uint32_t pc, insn; } WlOwnedSite;
static const WlOwnedSite kOwnedSites[] = {
    { 0x8007D764u, 0x8C42B558u }, { 0x8009179Cu, 0x8C43B558u },
    { 0x800917F4u, 0x8C43B558u }, { 0x80091838u, 0x8C44B558u },
    { 0x8009187Cu, 0x8C43B558u }, { 0x800918BCu, 0x8C43B558u },
    { 0x8009190Cu, 0x8C45B558u }, { 0x80091A1Cu, 0x8C52B558u },
};
static void wl_owned_hook(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[REG_V0] != 0x80050000u || !wl_menu_resident()) return;
    const uint32_t owned = psx_mod_read_word(WL_OWNED_MASK);
    if (!(owned & (1u << WL_OPTION_ID))) {
        psx_mod_write_word(WL_OWNED_MASK, owned | (1u << WL_OPTION_ID));
        psx_mod_counter_add("moa.wlink.owned_set", 1);
    }
}

/* Description lookup: table[13] would read past the 13-entry table. */
static void wl_desc_hook(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[REG_V0] != WL_DESC_TABLE + WL_OPTION_ID * 4u) return;
    if (!wl_table()) return;
    cpu->gpr[REG_V0] = wl_memory() + MEM_DESC_PTR;
}

/* ===================== Menu: link data at compile ========================= */

static int wl_back_usable(uint8_t id) {
    if (id == 0xFFu || id == 15u || id == 57u) return 0;   /* none, M118-TD, P77-ST */
    if (id >= 40u && id < 52u) return 0;                   /* radars */
    return 1;
}

/* The slot's HUD type name without its leading spaces. */
static void wl_slot_name(uint32_t slot, char out[17]) {
    uint32_t n = 0;
    int started = 0;
    for (uint32_t i = 0; i < 16u; ++i) {
        const char c = (char)psx_mod_read_byte(slot + SL_NAME + i);
        if (!c) break;
        if (c == ' ' && !started) continue;
        started = 1;
        out[n++] = c;
    }
    while (n && out[n - 1u] == ' ') --n;
    out[n] = 0;
}

static uint8_t wl_family(uint32_t slot, uint8_t place) {
    if (place == PLACE_HAND) return WL_NONE;
    char name[17];
    wl_slot_name(slot, name);
    const uint8_t kind = psx_mod_read_byte(slot + SL_KIND);
    static const char *const missiles[] = {
        "DUAL MISSILE", "SMALL MISSILE", "MISSILE", "LARGE MISSILE",
        "MULTI MISSILE", "TRIPLE MISSILE", "SPECIAL MISSILE" };
    static const char *const rockets[] = {
        "SMALL ROCKET", "ROCKET", "LARGE ROCKET", "SPECIAL ROCKET" };
    if (kind == 1u)
        for (unsigned i = 0; i < sizeof missiles / sizeof missiles[0]; ++i)
            if (!strcmp(name, missiles[i])) return FAM_MISSILE;
    for (unsigned i = 0; i < sizeof rockets / sizeof rockets[0]; ++i)
        if (!strcmp(name, rockets[i])) return FAM_ROCKET;
    if (kind == 2u && (place == PLACE_BACK_L || place == PLACE_BACK_R)) return FAM_CANNON;
    return WL_NONE;
}

static void wl_average_u16(const uint32_t *slots, unsigned n, uint32_t field,
                           uint32_t lo, uint32_t hi) {
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; ++i) sum += psx_mod_read_half(slots[i] + field);
    uint32_t v = sum / n;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    for (unsigned i = 0; i < n; ++i) psx_mod_write_half(slots[i] + field, (uint16_t)v);
}

static void wl_average_u8(const uint32_t *slots, unsigned n, uint32_t field,
                          uint32_t lo, uint32_t hi) {
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; ++i) sum += psx_mod_read_byte(slots[i] + field);
    uint32_t v = sum / n;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    for (unsigned i = 0; i < n; ++i) psx_mod_write_byte(slots[i] + field, (uint8_t)v);
}

static void wl_reset_shadows(uint32_t player) {
    const uint32_t memory = wl_memory();
    if (!memory || player > 1u) return;
    for (uint32_t s = 0; s < WL_SLOTS; ++s) {
        const uint32_t sh = memory + MEM_SHADOW + (player * WL_SLOTS + s) * SHADOW_BYTES;
        psx_mod_write_byte(sh + SH_BURST, 0);
        psx_mod_write_byte(sh + SH_COOLDOWN, 0);
    }
    psx_mod_write_byte(memory + MEM_LAST + player, WL_LAST_NONE);
}

static void wl_mark_block(uint32_t player) {
    const uint32_t block = WL_COMPILED + player * WL_PLAYER_BYTES;
    const uint32_t first = block + 0x88u;
    const uint8_t count = psx_mod_read_byte(block + 0x87u);
    const int fitted = (psx_mod_read_half(WL_FITTED_MASK) >> WL_OPTION_ID) & 1u;

    for (uint32_t s = 0; s < WL_SLOTS; ++s) {
        const uint32_t slot = first + s * WL_SLOT_BYTES;
        psx_mod_write_byte(slot + SL_TERM, 0);
        psx_mod_write_byte(slot + SL_GROUP, WL_NONE);
        psx_mod_write_byte(slot + SL_INFO, 0);
        psx_mod_write_byte(slot + SL_SIZE, 0);
    }
    wl_reset_shadows(player);
    if (!fitted || count == 0u || count > WL_SLOTS) return;

    /* Slot order, as compile builds it (0x800A8838): weapon arms or the hand
     * weapon, then back L, then back R, skipping radars and the like. */
    uint8_t place[WL_SLOTS];
    unsigned n = 0;
    const uint8_t arms = psx_mod_read_byte(WL_LOADOUT + 3u);
    const uint8_t hand = psx_mod_read_byte(WL_LOADOUT + 5u);
    if (hand != 0xFFu || arms >= 12u)
        place[n++] = arms >= 12u ? PLACE_ARM : PLACE_HAND;
    if (n < WL_SLOTS && wl_back_usable(psx_mod_read_byte(WL_LOADOUT + 6u)))
        place[n++] = PLACE_BACK_L;
    if (n < WL_SLOTS && wl_back_usable(psx_mod_read_byte(WL_LOADOUT + 7u)))
        place[n++] = PLACE_BACK_R;
    if (n != count) {
        psx_mod_counter_add("moa.wlink.slot_order_mismatch", 1);
        return;
    }

    uint8_t family[WL_SLOTS];
    for (uint32_t s = 0; s < count; ++s) {
        const uint32_t slot = first + s * WL_SLOT_BYTES;
        family[s] = wl_family(slot, place[s]);
        psx_mod_write_byte(slot + SL_INFO, (uint8_t)(WL_MAGIC | place[s]));
    }
    for (uint8_t f = FAM_MISSILE; f <= FAM_CANNON; ++f) {
        uint32_t members[WL_SLOTS];
        unsigned m = 0;
        for (uint32_t s = 0; s < count; ++s)
            if (family[s] == f) members[m++] = first + s * WL_SLOT_BYTES;
        if (m < 2u) continue;
        for (unsigned i = 0; i < m; ++i) {
            psx_mod_write_byte(members[i] + SL_GROUP, f);
            psx_mod_write_byte(members[i] + SL_SIZE, (uint8_t)m);
        }
        /* One shared lock-on and reload (fire interval). */
        wl_average_u16(members, m, SL_LOCK_RANGE, 0u, 0xFFFFu);
        for (uint32_t k = 0; k < 4u; ++k)
            wl_average_u16(members, m, SL_LOCK_BOX + 2u * k, 0u, 0xFFFFu);
        wl_average_u16(members, m, SL_LOCK_VALUE, 1u, 255u);
        wl_average_u8(members, m, SL_LOCKS, 1u, 255u);
        wl_average_u8(members, m, SL_INTERVAL, 1u, 255u);
        if (f == FAM_MISSILE)                    /* missile lock threshold */
            wl_average_u8(members, m, SL_PATTERN, 1u, 64u);
        psx_mod_counter_add("moa.wlink.group_linked", 1);
    }
}

static int wl_compile_filter(struct CPUState *cpu, uint32_t address) {
    if (s_wl_nested) return 0;
    if (psx_mod_read_word(WL_COMPILE_FN) != WL_COMPILE_SIG0 ||
        psx_mod_read_word(WL_COMPILE_FN + 4u) != WL_COMPILE_SIG1 ||
        !wl_menu_resident())
        return 0;
    const uint32_t player = cpu->gpr[REG_A0];
    if (player > 1u) return 0;
    s_wl_nested = 1;
    (void)psx_mod_call_guest(cpu, address, cpu->gpr[REG_RA], cpu->gpr[REG_A0],
                             cpu->gpr[REG_A1], cpu->gpr[REG_A2], cpu->gpr[REG_A3]);
    s_wl_nested = 0;
    wl_mark_block(player);
    return 1;
}

/* ===================== VS engine ========================================== */

static int wl_vs_engine(void) {
    return psx_mod_read_byte(WL_MODE_BYTE) == WL_MODE_VS &&
           psx_mod_read_word(WL_FIRE_GUN) == WL_SIG_GUN &&
           psx_mod_read_word(WL_FIRE_MISSILE) == WL_SIG_MISSILE &&
           psx_mod_read_word(WL_FIRE_CANNON) == WL_SIG_CANNON;
}

/* Current player block and its index, or 0 when it isn't one of the two. */
static uint32_t wl_player_block(uint32_t *player) {
    const uint32_t pb = psx_mod_read_word(WL_PLAYER_PTR);
    if (pb == WL_PLAYER_BLOCK) { *player = 0; return pb; }
    if (pb == WL_PLAYER_BLOCK + WL_PLAYER_BYTES) { *player = 1; return pb; }
    return 0;
}

static uint32_t wl_slot(uint32_t pb, uint32_t index) {
    return pb + PB_SLOTS + index * WL_SLOT_BYTES;
}

static int wl_linked(uint32_t slot) {
    return (psx_mod_read_byte(slot + SL_INFO) & 0xF0u) == WL_MAGIC &&
           psx_mod_read_byte(slot + SL_GROUP) != WL_NONE;
}

static int wl_same_group(uint32_t a, uint32_t b) {
    return wl_linked(a) && wl_linked(b) &&
           psx_mod_read_byte(a + SL_GROUP) == psx_mod_read_byte(b + SL_GROUP);
}

static uint32_t wl_fire_routine(uint8_t kind) {
    if (kind == 0u) return WL_FIRE_GUN;
    if (kind == 1u) return WL_FIRE_MISSILE;
    if (kind == 2u) return WL_FIRE_CANNON;
    return 0;
}

static void wl_copy_guest(uint32_t to, uint32_t from, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i)
        psx_mod_write_byte(to + i, psx_mod_read_byte(from + i));
}

/* Run one linked weapon's fire routine with its own burst and cooldown, then
 * put the AC and the lock state back as the selected weapon left them. */
static void wl_run_partner(struct CPUState *cpu, uint32_t pb, uint32_t ac,
                           uint32_t index, uint32_t shadow) {
    const uint32_t slot = wl_slot(pb, index);
    const uint32_t routine = wl_fire_routine(psx_mod_read_byte(slot + SL_KIND));
    if (!routine) return;

    uint8_t ac_saved[AC_BYTES];
    uint8_t lock_a[PB_LOCK_A_BYTES];
    uint8_t lock_b[PB_LOCK_B_BYTES];
    for (uint32_t i = 0; i < AC_BYTES; ++i) ac_saved[i] = psx_mod_read_byte(ac + i);
    for (uint32_t i = 0; i < PB_LOCK_A_BYTES; ++i) lock_a[i] = psx_mod_read_byte(pb + PB_LOCK_A + i);
    for (uint32_t i = 0; i < PB_LOCK_B_BYTES; ++i) lock_b[i] = psx_mod_read_byte(pb + PB_LOCK_B + i);
    const uint8_t active = psx_mod_read_byte(pb + PB_ACTIVE);

    const uint8_t burst = psx_mod_read_byte(shadow + SH_BURST);
    if (burst) {    /* a salvo in progress fires at the locks it started with */
        wl_copy_guest(pb + PB_LOCK_A, shadow + SH_LOCK_A, PB_LOCK_A_BYTES);
        wl_copy_guest(pb + PB_LOCK_B, shadow + SH_LOCK_B, PB_LOCK_B_BYTES);
    }
    psx_mod_write_byte(ac + AC_BURST, burst);
    psx_mod_write_byte(ac + AC_COOLDOWN, psx_mod_read_byte(shadow + SH_COOLDOWN));
    psx_mod_write_byte(ac + AC_HOLD, 0xFFu);
    psx_mod_write_byte(pb + PB_ACTIVE, (uint8_t)index);

    (void)psx_mod_call_guest(cpu, routine, cpu->gpr[REG_RA], ac, slot,
                             cpu->gpr[REG_A2], cpu->gpr[REG_A3]);

    const uint8_t new_burst = psx_mod_read_byte(ac + AC_BURST);
    psx_mod_write_byte(shadow + SH_BURST, new_burst);
    psx_mod_write_byte(shadow + SH_COOLDOWN, psx_mod_read_byte(ac + AC_COOLDOWN));
    if (new_burst) {
        wl_copy_guest(shadow + SH_LOCK_A, pb + PB_LOCK_A, PB_LOCK_A_BYTES);
        wl_copy_guest(shadow + SH_LOCK_B, pb + PB_LOCK_B, PB_LOCK_B_BYTES);
    }

    for (uint32_t i = 0; i < AC_BYTES; ++i) psx_mod_write_byte(ac + i, ac_saved[i]);
    for (uint32_t i = 0; i < PB_LOCK_A_BYTES; ++i) psx_mod_write_byte(pb + PB_LOCK_A + i, lock_a[i]);
    for (uint32_t i = 0; i < PB_LOCK_B_BYTES; ++i) psx_mod_write_byte(pb + PB_LOCK_B + i, lock_b[i]);
    psx_mod_write_byte(pb + PB_ACTIVE, active);
    psx_mod_counter_add("moa.wlink.partner_run", 1);
}

static int wl_fire_filter(struct CPUState *cpu, uint32_t address) {
    if (s_wl_nested || !wl_vs_engine()) return 0;
    uint32_t player;
    const uint32_t pb = wl_player_block(&player);
    if (!pb) return 0;
    const uint32_t ac = cpu->gpr[REG_A0];
    const uint32_t slot = cpu->gpr[REG_A1];
    if (ac != psx_mod_read_word(pb + PB_AC)) return 0;
    const uint32_t first = pb + PB_SLOTS;
    if (slot < first || (slot - first) % WL_SLOT_BYTES) return 0;
    const uint32_t index = (slot - first) / WL_SLOT_BYTES;
    const uint32_t count = psx_mod_read_byte(pb + PB_COUNT);
    if (index >= count || count > WL_SLOTS) return 0;
    if (index != psx_mod_read_byte(pb + PB_ACTIVE) || !wl_linked(slot)) return 0;
    const uint32_t memory = wl_memory();
    if (!memory) return 0;

    /* A new selected weapon starts every partner from rest. */
    if (psx_mod_read_byte(memory + MEM_LAST + player) != index) {
        wl_reset_shadows(player);
        psx_mod_write_byte(memory + MEM_LAST + player, (uint8_t)index);
    }

    s_wl_nested = 1;
    (void)psx_mod_call_guest(cpu, address, cpu->gpr[REG_RA], ac, slot,
                             cpu->gpr[REG_A2], cpu->gpr[REG_A3]);
    for (uint32_t p = 0; p < count; ++p) {
        if (p == index) continue;
        const uint32_t other = wl_slot(pb, p);
        if (!wl_same_group(slot, other)) continue;
        const uint32_t shadow = memory + MEM_SHADOW + (player * WL_SLOTS + p) * SHADOW_BYTES;
        if (!psx_mod_read_half(other + SL_AMMO) && !psx_mod_read_byte(shadow + SH_BURST))
            continue;
        wl_run_partner(cpu, pb, ac, p, shadow);
    }
    s_wl_nested = 0;

    /* Selected weapon dry: hand the selection to a partner with ammo. */
    if (!psx_mod_read_half(slot + SL_AMMO) &&
        psx_mod_read_byte(pb + PB_PENDING) == index) {
        for (uint32_t p = 0; p < count; ++p) {
            const uint32_t other = wl_slot(pb, p);
            if (p != index && wl_same_group(slot, other) && psx_mod_read_half(other + SL_AMMO)) {
                psx_mod_write_byte(pb + PB_PENDING, (uint8_t)p);
                psx_mod_counter_add("moa.wlink.dry_handover", 1);
                break;
            }
        }
    }
    return 1;
}

/* Per-AC match start (VS 0x8008342C): every match starts the partners at rest. */
static void wl_match_start(struct CPUState *cpu, uint32_t address) {
    (void)cpu;
    (void)address;
    if (!wl_vs_engine()) return;
    uint32_t player;
    if (wl_player_block(&player)) wl_reset_shadows(player);
}

/* Weapon switch: the game has picked the next weapon with ammo in +0x11.
 * Step past the selected weapon's partners so a linked group is one entry. */
static void wl_cycle_hook(struct CPUState *cpu, uint32_t address) {
    (void)cpu;
    (void)address;
    if (psx_mod_read_byte(WL_MODE_BYTE) != WL_MODE_VS) return;
    uint32_t player;
    const uint32_t pb = wl_player_block(&player);
    if (!pb) return;
    const uint32_t count = psx_mod_read_byte(pb + PB_COUNT);
    const uint32_t active = psx_mod_read_byte(pb + PB_ACTIVE);
    if (!count || count > WL_SLOTS || active >= count) return;
    const uint32_t current = wl_slot(pb, active);
    if (!wl_linked(current)) return;
    uint32_t pick = active;
    for (uint32_t k = 1; k < count; ++k) {
        const uint32_t c = (active + k) % count;
        const uint32_t other = wl_slot(pb, c);
        if (!psx_mod_read_half(other + SL_AMMO) || wl_same_group(current, other)) continue;
        pick = c;
        break;
    }
    if (psx_mod_read_byte(pb + PB_PENDING) != pick) {
        psx_mod_write_byte(pb + PB_PENDING, (uint8_t)pick);
        psx_mod_counter_add("moa.wlink.cycle_skip", 1);
    }
}

/* Pixel width of a HUD string in the game's proportional font (0x800572B4). */
static uint32_t wl_text_width(uint32_t text, uint32_t font) {
    uint32_t width = 0;
    for (uint32_t i = 0; i < 40u; ++i) {
        const uint8_t c = psx_mod_read_byte(text + i);
        if (!c) break;
        if (c == '\\') { ++i; continue; }
        if (c >= 33u && c < 128u) {
            const int8_t glyph = (int8_t)psx_mod_read_byte(WL_FONT_MAP + c);
            if (glyph < 0) continue;
            width += psx_mod_read_byte(WL_FONT_GLYPHS + ((uint32_t)glyph + font) * 12u + 6u) + 1u;
        } else if (c == ' ') {
            width += psx_mod_read_byte(WL_FONT_GLYPHS + (font + 10u) * 12u + 6u) + 1u;
        }
    }
    return width;
}

static const char *wl_place_tag(uint8_t place) {
    switch (place) {
    case PLACE_ARM: return "ARM";
    case PLACE_BACK_L: return "BACK L";
    case PLACE_BACK_R: return "BACK R";
    default: return 0;
    }
}

/* Name line: "<type> <slot>" for linked weapons, ending where the game's own
 * name ended. a0 = x, a3 = font, v0 = the string about to be stored. */
static void wl_hud_name(struct CPUState *cpu, uint32_t slot, uint32_t pb, uint32_t player) {
    if (!wl_linked(slot) || psx_mod_read_byte(slot + SL_SIZE) < 2u) return;
    const char *tag = wl_place_tag(psx_mod_read_byte(slot + SL_INFO) & 0x0Fu);
    const uint32_t memory = wl_memory();
    if (!tag || !memory) return;
    const uint32_t index = (slot - (pb + PB_SLOTS)) / WL_SLOT_BYTES;
    const uint32_t buffer = memory + MEM_HUD + (player * WL_SLOTS + index) * 32u;
    char name[17];
    wl_slot_name(slot, name);
    char text[32];
    uint32_t n = 0;
    for (uint32_t i = 0; name[i] && n < 24u; ++i) text[n++] = name[i];
    text[n++] = ' ';
    for (uint32_t i = 0; tag[i] && n < 31u; ++i) text[n++] = tag[i];
    text[n] = 0;
    for (uint32_t i = 0; i <= n; ++i) psx_mod_write_byte(buffer + i, (uint8_t)text[i]);

    const uint32_t font = cpu->gpr[REG_A3];
    const int32_t shift = (int32_t)wl_text_width(slot + SL_NAME, font) -
                          (int32_t)wl_text_width(buffer, font);
    cpu->gpr[REG_A0] = (uint32_t)((int32_t)cpu->gpr[REG_A0] + shift);
    cpu->gpr[REG_V0] = buffer;
}

static void wl_hud_selected_hook(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (psx_mod_read_byte(WL_MODE_BYTE) != WL_MODE_VS) return;
    uint32_t player;
    const uint32_t pb = wl_player_block(&player);
    if (!pb) return;
    const uint32_t slot = cpu->gpr[REG_S1];
    if (cpu->gpr[REG_V0] != slot + SL_NAME) return;
    wl_hud_name(cpu, slot, pb, player);
}


static void wl_hud_list_hook(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (psx_mod_read_byte(WL_MODE_BYTE) != WL_MODE_VS) return;
    uint32_t player;
    const uint32_t pb = wl_player_block(&player);
    if (!pb) return;
    const uint32_t slot = cpu->gpr[REG_S1];
    if (cpu->gpr[REG_V0] != slot + SL_NAME) return;
    wl_hud_name(cpu, slot, pb, player);
}

static void wl_activate(void) {
    s_wl_memory = 0;     /* this session's plan gets its own allocation */
    s_wl_nested = 0;
    (void)wl_memory();
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_wlink_plugin) {
    static const char id[] = "moa.op-wlink";
    (void)psx_mod_register_activation_plugin(id, wl_activate);
    for (unsigned i = 0; i < sizeof kTableSites / sizeof kTableSites[0]; ++i)
        (void)psx_mod_register_instruction_plugin(id, kTableSites[i].pc,
                                                  kTableSites[i].insn, wl_table_hook);
    for (unsigned i = 0; i < sizeof kOwnedSites / sizeof kOwnedSites[0]; ++i)
        (void)psx_mod_register_instruction_plugin(id, kOwnedSites[i].pc,
                                                  kOwnedSites[i].insn, wl_owned_hook);
    (void)psx_mod_register_instruction_plugin(id, WL_DESC_PC, WL_DESC_INSN, wl_desc_hook);
    (void)psx_mod_register_function_filter_plugin(id, WL_COMPILE_FN, wl_compile_filter);
    (void)psx_mod_register_function_filter_plugin(id, WL_FIRE_GUN, wl_fire_filter);
    (void)psx_mod_register_function_filter_plugin(id, WL_FIRE_MISSILE, wl_fire_filter);
    (void)psx_mod_register_function_filter_plugin(id, WL_FIRE_CANNON, wl_fire_filter);
    (void)psx_mod_register_function_entry_plugin(id, WL_MATCH_START, wl_match_start);
    (void)psx_mod_register_instruction_plugin(id, WL_CYCLE_PC, WL_CYCLE_INSN, wl_cycle_hook);
    (void)psx_mod_register_instruction_plugin(id, WL_HUD_SEL_PC, WL_HUD_NAME_INSN, wl_hud_selected_hook);
    (void)psx_mod_register_instruction_plugin(id, WL_HUD_LIST_PC, WL_HUD_NAME_INSN, wl_hud_list_hook);
}
