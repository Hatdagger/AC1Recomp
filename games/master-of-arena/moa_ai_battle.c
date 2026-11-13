/* Master of Arena: AI Battle (computer against computer).
 *
 * The arena engine has two AC slots. Slot 1 is flown from the controller;
 * slot 2 by the opponent AI, which writes a button word for it every frame
 * (both ACs then run through the same flying code). The AI is written as
 * "slot 2 fights slot 1" with every address fixed.
 *
 * To let the same AI fly slot 1 as well, each AI entry point is run a second
 * time with the two sides exchanged in memory: the two slot blocks and the
 * two AC objects trade places (pointers between them are rebased), and the
 * AI's own state is exchanged with a second copy kept here. After the call
 * everything is put back, so the button word the AI wrote for "slot 2" is
 * now in slot 1.
 *
 * Offline only (psx_game_register_offline_*), and only when the launcher asked
 * for it (PSX_MOA_AI_BATTLE=1). Every engine of this game loads at the same
 * address, so each hook first checks that the arena engine is in memory.
 *
 * The second AI state is host memory: savestates and rewind do not cover it.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <dirent.h>

#include "moa_card.h"

#define MOA_MODE_BYTE     0x8003EA2Au  /* engine the exe launches next */
#define MOA_MODE_ARENA    3u

#define MOA_SLOT1         0x80045198u  /* player block; slot 2 follows it */
#define MOA_SLOT_BYTES    928u
#define MOA_SLOT_PAD      4u           /* button word the AC code reads */
#define MOA_SLOT_PAD_PREV 6u
#define MOA_SLOT_OBJECT   920u         /* pointer to the slot's AC object */
#define MOA_OBJECT_BYTES  376u

#define MOA_AI_RECORD     0x8004AC80u  /* 36 bytes handed over by the menu */
#define MOA_AI_RECORD_BYTES 36u
#define MOA_AI_STATE      0x801E2730u  /* AI working memory and settings */
#define MOA_AI_STATE_END  0x801E29C4u
#define MOA_AI_TABLES     0x80098890u  /* per-opponent tables the setup fills */
#define MOA_AI_TABLES_END 0x80098A70u

/* Arena engine (FDAT entry 210). */
#define MOA_AI_FRAME      0x8008D4ACu  /* opponent AI, once per frame */
#define MOA_AI_INIT       0x8008DC98u  /* clears the AI state at match start */
#define MOA_AI_SETUP      0x80055768u  /* AI record -> working settings */
#define MOA_SIG_AI_FRAME  0x27BDFFB0u
#define MOA_SIG_AI_INIT   0x27BDFFE0u
#define MOA_SIG_AI_SETUP  0x27BDFFB0u

#define MOA_PAD_START     0x0008u

static int moa_ai_enabled = -1;
static uint32_t moa_random(uint32_t below);
static int moa_ai_nested;

static uint8_t moa_side_state[MOA_AI_STATE_END - MOA_AI_STATE];
static uint8_t moa_side_tables[MOA_AI_TABLES_END - MOA_AI_TABLES];
static uint8_t moa_side_record[MOA_AI_RECORD_BYTES];
static int moa_side_record_set;

/* The launcher sets PSX_MOA_AI_BATTLE=1 for an AI Battle session. */
static int moa_ai_battle_on(void)
{
    if (moa_ai_enabled < 0) {
        const char *e = getenv("PSX_MOA_AI_BATTLE");
        moa_ai_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return moa_ai_enabled;
}

static int moa_arena_engine(uint32_t address, uint32_t signature)
{
    return moa_ai_battle_on() &&
           psx_mod_read_byte(MOA_MODE_BYTE) == MOA_MODE_ARENA &&
           psx_mod_read_word(address) == signature;
}

static void moa_exchange_host(uint32_t address, uint8_t *side, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++) {
        const uint8_t guest = psx_mod_read_byte(address + i);
        psx_mod_write_byte(address + i, side[i]);
        side[i] = guest;
    }
}

static void moa_exchange_guest(uint32_t a, uint32_t b, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i += 4) {
        const uint32_t wa = psx_mod_read_word(a + i);
        psx_mod_write_word(a + i, psx_mod_read_word(b + i));
        psx_mod_write_word(b + i, wa);
    }
}

/* A pointer into one side's block now has to mean the other side's. */
static void moa_rebase(uint32_t block, uint32_t bytes, uint32_t object1)
{
    const uint32_t object2 = object1 + MOA_OBJECT_BYTES;
    const uint32_t slot2 = MOA_SLOT1 + MOA_SLOT_BYTES;
    for (uint32_t i = 0; i < bytes; i += 4) {
        const uint32_t v = psx_mod_read_word(block + i);
        uint32_t r = v;
        if (v >= object1 && v < object2) r = v + MOA_OBJECT_BYTES;
        else if (v >= object2 && v < object2 + MOA_OBJECT_BYTES) r = v - MOA_OBJECT_BYTES;
        else if (v >= MOA_SLOT1 && v < slot2) r = v + MOA_SLOT_BYTES;
        else if (v >= slot2 && v < slot2 + MOA_SLOT_BYTES) r = v - MOA_SLOT_BYTES;
        if (r != v) psx_mod_write_word(block + i, r);
    }
}

/* Trade the two sides. Its own inverse. `object1` is slot 1's AC object, or
 * zero before the objects exist (match setup). */
static void moa_trade_sides(uint32_t object1)
{
    const uint32_t slot2 = MOA_SLOT1 + MOA_SLOT_BYTES;
    moa_exchange_guest(MOA_SLOT1, slot2, MOA_SLOT_BYTES);
    if (object1) {
        moa_exchange_guest(object1, object1 + MOA_OBJECT_BYTES, MOA_OBJECT_BYTES);
        moa_rebase(MOA_SLOT1, 2u * MOA_SLOT_BYTES, object1);
        moa_rebase(object1, 2u * MOA_OBJECT_BYTES, object1);
    }
    moa_exchange_host(MOA_AI_STATE, moa_side_state, sizeof moa_side_state);
    moa_exchange_host(MOA_AI_TABLES, moa_side_tables, sizeof moa_side_tables);
    moa_exchange_host(MOA_AI_RECORD, moa_side_record, sizeof moa_side_record);
}

/* The AC objects sit side by side, slot 1's first. Zero until both exist. */
static uint32_t moa_object1(void)
{
    const uint32_t o1 = psx_mod_read_word(MOA_SLOT1 + MOA_SLOT_OBJECT);
    const uint32_t o2 = psx_mod_read_word(MOA_SLOT1 + MOA_SLOT_BYTES + MOA_SLOT_OBJECT);
    if (o1 < 0x80100000u || o1 >= 0x80200000u || (o1 & 3u)) return 0;
    return o2 == o1 + MOA_OBJECT_BYTES ? o1 : 0;
}

/* Run the hooked function for slot 2 as the game does, then once more with
 * the sides traded so it does the same work for slot 1. */
static void moa_run_both(struct CPUState *cpu, uint32_t address)
{
    const uint32_t ra = cpu->gpr[31];
    const uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5];
    const uint32_t a2 = cpu->gpr[6], a3 = cpu->gpr[7];
    moa_ai_nested = 1;
    (void)psx_mod_call_guest(cpu, address, ra, a0, a1, a2, a3);
    const uint32_t object1 = moa_object1();
    moa_trade_sides(object1);
    (void)psx_mod_call_guest(cpu, address, ra, a0, a1, a2, a3);
    moa_trade_sides(object1);
    moa_ai_nested = 0;
}

/* How often the AI holds each button, per slot (mod_counters). */
static const char *const moa_bits1[16] = {
    "moa.ai.u1.select", "moa.ai.u1.b1", "moa.ai.u1.b2", "moa.ai.u1.start",
    "moa.ai.u1.up", "moa.ai.u1.right", "moa.ai.u1.down", "moa.ai.u1.left",
    "moa.ai.u1.l2", "moa.ai.u1.r2", "moa.ai.u1.l1", "moa.ai.u1.r1",
    "moa.ai.u1.triangle", "moa.ai.u1.circle", "moa.ai.u1.cross", "moa.ai.u1.square" };
static const char *const moa_bits2[16] = {
    "moa.ai.u2.select", "moa.ai.u2.b1", "moa.ai.u2.b2", "moa.ai.u2.start",
    "moa.ai.u2.up", "moa.ai.u2.right", "moa.ai.u2.down", "moa.ai.u2.left",
    "moa.ai.u2.l2", "moa.ai.u2.r2", "moa.ai.u2.l1", "moa.ai.u2.r1",
    "moa.ai.u2.triangle", "moa.ai.u2.circle", "moa.ai.u2.cross", "moa.ai.u2.square" };

static void moa_count_bits(uint16_t pad, const char *const *names)
{
    for (unsigned i = 0; i < 16; i++)
        if (pad & (1u << i)) psx_mod_counter_add(names[i], 1);
}

static int moa_ai_setup_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_ai_nested || !moa_arena_engine(address, MOA_SIG_AI_SETUP)) return 0;
    if (!moa_side_record_set)
        for (unsigned i = 0; i < MOA_AI_RECORD_BYTES; i++)
            moa_side_record[i] = psx_mod_read_byte(MOA_AI_RECORD + i);
    /* Unit 1's AI starts from the same engine data as unit 2's: the tables
     * hold constants the setup only partly rewrites. */
    for (uint32_t i = 0; i < sizeof moa_side_state; i++)
        moa_side_state[i] = psx_mod_read_byte(MOA_AI_STATE + i);
    for (uint32_t i = 0; i < sizeof moa_side_tables; i++)
        moa_side_tables[i] = psx_mod_read_byte(MOA_AI_TABLES + i);
    moa_run_both(cpu, address);
    psx_mod_counter_add("moa.ai.setup", 1);
    return 1;
}

static int moa_ai_init_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_ai_nested || !moa_arena_engine(address, MOA_SIG_AI_INIT)) return 0;
    moa_run_both(cpu, address);
    psx_mod_counter_add("moa.ai.init", 1);
    return 1;
}

/* The launcher's "Unit 1: You": player 1's controller flies unit 1 and the AI
 * flies unit 2 alone, as in the arena itself. Read once per match. */
static int moa_manual_unit1(void)
{
    const char *v = getenv("PSX_MOA_AI_P1");
    return v && v[0] == '1';
}

static uint32_t moa_ai_frames_seen;            /* AI frames run this session */

static int moa_ai_frame_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_ai_nested || !moa_arena_engine(address, MOA_SIG_AI_FRAME)) return 0;
    if (moa_manual_unit1()) {
        /* Only the game's own call for slot 2; slot 1's word stays the
         * controller's. */
        const uint32_t ra = cpu->gpr[31];
        const uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5];
        const uint32_t a2 = cpu->gpr[6], a3 = cpu->gpr[7];
        moa_ai_nested = 1;
        (void)psx_mod_call_guest(cpu, address, ra, a0, a1, a2, a3);
        moa_ai_nested = 0;
        psx_mod_counter_add("moa.ai.frame", 1);
        moa_ai_frames_seen++;
        moa_count_bits(psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_BYTES + MOA_SLOT_PAD), moa_bits2);
        return 1;
    }
    /* The controller sampler has already moved slot 1's word to "previous"
     * and stored the controller in its place. Put last frame's word back so
     * the AI's own previous/current step sees what slot 2's would. */
    const uint16_t human = psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_PAD);
    psx_mod_write_half(MOA_SLOT1 + MOA_SLOT_PAD,
                       psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_PAD_PREV));
    moa_run_both(cpu, address);
    /* Start still reaches the game, for the pause menu. */
    psx_mod_write_half(MOA_SLOT1 + MOA_SLOT_PAD,
        (uint16_t)((psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_PAD) & ~MOA_PAD_START) |
                   (human & MOA_PAD_START)));
    psx_mod_counter_add("moa.ai.frame", 1);
    moa_ai_frames_seen++;
    moa_count_bits(psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_PAD), moa_bits1);
    moa_count_bits(psx_mod_read_half(MOA_SLOT1 + MOA_SLOT_BYTES + MOA_SLOT_PAD), moa_bits2);
    return 1;
}

/* ---- units ------------------------------------------------------------------
 *
 * Every VS START draws two units: unit cards from the memory-card folder
 * first, then arena opponents from the player's own disc (MS\MENU_TIM.T holds
 * each arena's AC and AI tables) for any unit still missing. They are entered
 * as player 1 and player 2 of the versus menu, each with its own AI, and the
 * versus menu then launches the arena engine in place of the split-screen one.
 */
#define MOA_MENU_VS_START   0x800B2D7Cu  /* menu: VS START pressed */
#define MOA_SIG_VS_START    0x27BDFFD0u
#define MOA_MODE_VERSUS     2u
#define MOA_ARENA_ENTRIES   0x800D5A08u  /* menu: opponents per arena */
#define MOA_ARENA_STARTS    0x800D5A14u  /* menu: first opponent per arena */
#define MOA_ARENAS          9u           /* main arena and the eight Ex arenas */
#define MOA_VS_PARTS        0x80045940u  /* 12 part ids per player */
#define MOA_VS_PLUS         0x8004A7D0u  /* 8 bytes of Human PLUS state per player */
#define MOA_VS_PAINT        0x8004B730u  /* 48 colour bytes per player */
#define MOA_PAINT_BYTES      48u
#define MOA_VS_OPTIONS      0x8003EAC0u  /* 16-bit optional-parts mask per player */
#define MOA_RECORD_BYTES    64u
#define MOA_MENU_TIM_PATH   "GG/MS/MENU_TIM.T"

static uint8_t *moa_menu_tim;
static uint32_t moa_menu_tim_bytes;

static const uint8_t *moa_menu_tim_entry(uint32_t index, uint32_t *bytes)
{
    *bytes = 0;
    if (!moa_menu_tim) {
        uint32_t size = 0;
        if (!psx_mod_read_disc_file(MOA_MENU_TIM_PATH, NULL, 0, &size) || size < 4096u) {
            psx_mod_counter_add("moa.ai.disc_file_missing", 1);
            return NULL;
        }
        uint8_t *data = (uint8_t *)malloc(size);
        if (!data) return NULL;
        if (!psx_mod_read_disc_file(MOA_MENU_TIM_PATH, data, size, &moa_menu_tim_bytes)) {
            free(data);
            return NULL;
        }
        moa_menu_tim = data;
    }
    /* Header: two words, then one 16-bit sector offset per entry. */
    if (index + 4u > 1023u) return NULL;
    const uint8_t *t = moa_menu_tim + 4u + 2u * index;
    const uint32_t a = (uint32_t)(t[0] | (t[1] << 8)) * 2048u;
    const uint32_t b = (uint32_t)(t[2] | (t[3] << 8)) * 2048u;
    if (b <= a || b > moa_menu_tim_bytes) {
        psx_mod_counter_add("moa.ai.disc_entry_bad", 1);
        return NULL;
    }
    *bytes = b - a;
    return moa_menu_tim + a;
}

/* One arena opponent: its 64-byte AC record and 36-byte AI record. */
static int moa_arena_unit(uint32_t arena, uint32_t rank,
                          uint8_t *record, uint8_t *ai)
{
    const uint32_t entries = psx_mod_read_byte(MOA_ARENA_ENTRIES + arena);
    const uint32_t start = psx_mod_read_byte(MOA_ARENA_STARTS + arena);
    if (rank >= entries) return 0;
    const uint32_t base = (start + entries) * 4u + arena * 4u + 140u - 1u;
    uint32_t record_bytes = 0, ai_bytes = 0;
    const uint8_t *records = moa_menu_tim_entry(base, &record_bytes);
    const uint8_t *ais = moa_menu_tim_entry(base + 3u, &ai_bytes);
    if (!records || !ais || (rank + 1u) * MOA_RECORD_BYTES > record_bytes ||
        (rank + 1u) * MOA_AI_RECORD_BYTES > ai_bytes)
        return 0;
    memcpy(record, records + rank * MOA_RECORD_BYTES, MOA_RECORD_BYTES);
    memcpy(ai, ais + rank * MOA_AI_RECORD_BYTES, MOA_AI_RECORD_BYTES);
    /* An unused slot has no core. */
    return record[0] != 0xFFu && record[1] != 0xFFu;
}

/* ---- loading the units' graphics --------------------------------------------
 *
 * The match draws each AC from its player's own part files: one heap copy per
 * part (archive 5), painted in place, listed in the player's file table. The
 * garage works on one player at a time: a working table that points at that
 * player's files, and a working set of textures and colour tables in video
 * memory (the other player's set is kept aside and traded in by a swap). The
 * steps below are the garage's own: bring the player's set to the front,
 * point the working table at the player's files, set the garage's parts and
 * paint, rebuild (each part's textures and file are loaded, the file it
 * replaces freed, the paint baked), wait for the disc, and hand the table back
 * to the player (as leaving AC SETUP does).
 *
 * VS START is a menu state the game calls every frame until it moves on, so the
 * steps run one per frame while VS START is held back.
 */
#define MOA_CD_BUSY         0x800589DCu  /* nonzero while disc loads are queued */
#define MOA_SIG_CD_BUSY     0x27BDFFE8u
#define MOA_REBUILD         0x80081E20u  /* garage AC: textures + paint, part files */
#define MOA_SIG_REBUILD     0x27BDFFE8u
#define MOA_AC_SWAP         0x800C0E7Cu  /* trade the two players' AC graphics */
#define MOA_SIG_AC_SWAP     0x27BDFFC0u
#define MOA_VS_FRONT        133u         /* VS menu state: 1 = player 2's set in front */
#define MOA_GARAGE_PATTERN  0x8003EA72u  /* pattern, variant (0 = custom paint) */
#define MOA_VS_PATTERN      0x8003EAD2u  /* + player */
#define MOA_VS_VARIANT      0x8003EAB8u  /* + player */
#define MOA_GARAGE_PARTS    0x80036FB0u  /* current AC: 12 part ids, then paint */
#define MOA_GARAGE_PAINT    0x80036FBCu
#define MOA_GARAGE_OPTIONS  0x8003EA70u
#define MOA_WORK_FILES      0x801FBB00u  /* working table: file per slot */
#define MOA_WORK_SIZES      0x801D0B28u  /* sizes in halves */
#define MOA_PLAYER_FILES    0x8004AB60u  /* + 60 per player */
#define MOA_PLAYER_SIZES    0x8004A9E0u  /* + 30 per player */
#define MOA_FILE_FIRST      3u           /* slots 3..14 are the AC's parts */
#define MOA_FILE_END        15u
#define MOA_LOAD_FRAMES_MAX 900u         /* give up waiting after ~15 seconds */

static int moa_cd_busy(struct CPUState *cpu)
{
    return (psx_mod_call_guest(cpu, MOA_CD_BUSY, cpu->gpr[31], 0, 0, 0, 0) & 0xFFFFu) != 0;
}

/* Garage := player `player`'s files, the unit's parts and paint; rebuild. */
static void moa_load_unit(struct CPUState *cpu, unsigned player)
{
    for (uint32_t i = MOA_FILE_FIRST; i < MOA_FILE_END; i++) {
        psx_mod_write_word(MOA_WORK_FILES + 4u * i,
                           psx_mod_read_word(MOA_PLAYER_FILES + 60u * player + 4u * i));
        psx_mod_write_word(MOA_WORK_SIZES + 4u * i,
                           psx_mod_read_half(MOA_PLAYER_SIZES + 30u * player + 2u * i));
    }
    for (unsigned i = 0; i < 12; i++)
        psx_mod_write_byte(MOA_GARAGE_PARTS + i,
                           psx_mod_read_byte(MOA_VS_PARTS + 12u * player + i));
    for (unsigned i = 0; i < MOA_PAINT_BYTES; i++)
        psx_mod_write_byte(MOA_GARAGE_PAINT + i,
                           psx_mod_read_byte(MOA_VS_PAINT + MOA_PAINT_BYTES * player + i));
    psx_mod_write_half(MOA_GARAGE_OPTIONS, psx_mod_read_half(MOA_VS_OPTIONS + 2u * player));
    psx_mod_write_byte(MOA_GARAGE_PATTERN, 0);
    psx_mod_write_byte(MOA_GARAGE_PATTERN + 1u, 0);
    psx_mod_write_byte(MOA_VS_PATTERN + player, 0);
    psx_mod_write_byte(MOA_VS_VARIANT + player, 0);
    (void)psx_mod_call_guest(cpu, MOA_REBUILD, cpu->gpr[31], 0, 0, 0, 0);
}

/* The rebuilt part files belong to player `player` from now on. */
static void moa_commit_unit(unsigned player)
{
    for (uint32_t i = MOA_FILE_FIRST; i < MOA_FILE_END; i++) {
        psx_mod_write_word(MOA_PLAYER_FILES + 60u * player + 4u * i,
                           psx_mod_read_word(MOA_WORK_FILES + 4u * i));
        psx_mod_write_half(MOA_PLAYER_SIZES + 30u * player + 2u * i,
                           (uint16_t)psx_mod_read_word(MOA_WORK_SIZES + 4u * i));
    }
}

static void moa_swap_sets(struct CPUState *cpu)
{
    (void)psx_mod_call_guest(cpu, MOA_AC_SWAP, cpu->gpr[31], 0, 0, 0, 0);
}

/* ---- cards ------------------------------------------------------------------
 *
 * Unit cards (Raven Card Builder images, .png) are read from the folder that
 * holds the player's memory card. A card carries its own AI (format 4: preset
 * or the game's AI editor sliders, plus advanced overrides); a card without
 * AI, and a random arena opponent, fly the Balanced preset.
 */
/* Whether the game's load and energy checks are waived for a unit: Human
 * PLUS does that in the garage; arena opponents are never checked (45 of the
 * 158 are over their legs' load without Human PLUS), so a random arena unit
 * gets the waiver too. */
static uint8_t moa_unit_waive[2];
static uint32_t moa_matches;                   /* matches started this session */

#define MOA_CARDS_MAX 64
static char *moa_card_paths[MOA_CARDS_MAX];
static int moa_card_paths_count;

static int moa_has_suffix(const char *name, const char *suffix)
{
    const size_t n = strlen(name), k = strlen(suffix);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = name[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return 0;
    }
    return 1;
}

static void moa_scan_cards(void)
{
    for (int i = 0; i < moa_card_paths_count; i++) free(moa_card_paths[i]);
    moa_card_paths_count = 0;
    const char *dir = getenv("PSX_MEMCARD_HOME");
    if (!dir || !dir[0]) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && moa_card_paths_count < MOA_CARDS_MAX) {
        if (!moa_has_suffix(e->d_name, ".png")) continue;
        const size_t n = strlen(dir) + strlen(e->d_name) + 2;
        char *path = (char *)malloc(n);
        if (!path) break;
        snprintf(path, n, "%s/%s", dir, e->d_name);
        moa_card_paths[moa_card_paths_count++] = path;
    }
    closedir(d);
}

static int moa_read_card(const char *path, struct moa_card *card)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n > 0 && n < 32L * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0) {
            uint8_t *data = (uint8_t *)malloc((size_t)n);
            if (data && fread(data, 1, (size_t)n, f) == (size_t)n)
                ok = moa_card_decode(data, (size_t)n, card);
            free(data);
        }
    }
    fclose(f);
    return ok;
}

/* A card as a unit: its record and its own AI record. */
static int moa_card_unit(const char *path, uint8_t *record, uint8_t *ai)
{
    static struct moa_card card;
    if (!moa_read_card(path, &card)) {
        psx_mod_counter_add("moa.ai.card_unreadable", 1);
        return 0;
    }
    moa_card_record(&card, record);
    moa_card_ai_record(&card, ai);
    psx_mod_counter_add(card.has_ai ? "moa.ai.card_unit" : "moa.ai.card_unit_balanced", 1);
    return 1;
}

/* Two units. A unit the launcher chose (PSX_MOA_AI_UNIT1 / PSX_MOA_AI_UNIT2:
 * a card's path) is that card; a random one is another card from the folder
 * (never one already in the match), else a random arena opponent. */
static int moa_pick_units(uint8_t record[2][MOA_RECORD_BYTES],
                          uint8_t ai[2][MOA_AI_RECORD_BYTES])
{
    moa_scan_cards();
    const char *chosen[2] = { getenv("PSX_MOA_AI_UNIT1"), getenv("PSX_MOA_AI_UNIT2") };
    int have[2] = { 0, 0 };
    int used[2] = { -1, -1 };
    moa_unit_waive[0] = moa_unit_waive[1] = 0;
    for (unsigned u = 0; u < 2; u++) {
        if (!chosen[u] || !chosen[u][0]) continue;
        have[u] = moa_card_unit(chosen[u], record[u], ai[u]);
        for (int k = 0; k < moa_card_paths_count; k++)
            if (strcmp(moa_card_paths[k], chosen[u]) == 0) used[u] = k;
        if (!have[u]) psx_mod_counter_add("moa.ai.chosen_card_unreadable", 1);
    }
    for (unsigned u = 0; u < 2; u++) {
        if (have[u]) continue;
        int free_cards = 0;
        for (int k = 0; k < moa_card_paths_count; k++)
            if (k != used[0] && k != used[1]) free_cards++;
        for (unsigned tries = 0; tries < 4u * MOA_CARDS_MAX && free_cards > 0 && !have[u]; tries++) {
            const int k = (int)moa_random((uint32_t)moa_card_paths_count);
            if (k == used[0] || k == used[1]) continue;
            used[u] = k;                       /* tried, readable or not */
            free_cards--;
            have[u] = moa_card_unit(moa_card_paths[k], record[u], ai[u]);
        }
    }
    uint32_t picked = 0xFFFFFFFFu;
    for (unsigned u = 0; u < 2; u++) {
        for (unsigned tries = 0; tries < 64 && !have[u]; tries++) {
            const uint32_t arena = moa_random(MOA_ARENAS);
            const uint32_t entries = psx_mod_read_byte(MOA_ARENA_ENTRIES + arena);
            if (entries < 2u) continue;
            const uint32_t rank = moa_random(entries - 1u);  /* last = player's slot */
            if (arena * 256u + rank == picked) continue;
            have[u] = moa_arena_unit(arena, rank, record[u], ai[u]);
            if (have[u]) {
                picked = arena * 256u + rank;
                moa_unit_waive[u] = 1;
                /* Its build, the Balanced AI with its own Human PLUS level. */
                moa_ai_build_record(ai[u][0], NULL, NULL, NULL, ai[u]);
            }
        }
        if (!have[u]) return 0;
    }
    return 1;
}


/* Optional parts and Human PLUS: DATA LOAD does not leave the latter on the
 * versus player, so these are set at VS START either way. */
static void moa_enter_unit_state(unsigned player, const uint8_t *r, const uint8_t *ai)
{
    /* Optional parts: one bit per part, in the option table's order. */
    psx_mod_write_half(MOA_VS_OPTIONS + 2u * player, (uint16_t)(r[10] | (r[11] << 8)));
    /* Human PLUS level from the AI record; with it, the game waives the
     * load and energy limits (byte 2). */
    const uint8_t plus[8] = { ai[0], 0, (uint8_t)(ai[0] || moa_unit_waive[player] ? 1 : 0),
                              0, 0, 0, 0, 0 };
    for (unsigned i = 0; i < 8; i++)
        psx_mod_write_byte(MOA_VS_PLUS + 8u * player + i, plus[i]);
}

static void moa_enter_unit(unsigned player, const uint8_t *r, const uint8_t *ai)
{
    /* Arena record order -> the garage's: core, booster, head, arms, blade,
     * hand weapon, back left, back right, generator, FCS, (none), legs. */
    /* Caterpillar legs (27 and up) have their own drive: no booster, even
     * on a card made before the builder enforced it. */
    const uint8_t booster = r[3] >= 27u && r[3] != 0xFFu ? 0xFFu : r[12];
    const uint8_t parts[12] = { r[0], booster, r[1], r[2], r[5], r[4],
                                r[6], r[7], r[9], r[8], 0xFFu, r[3] };
    for (unsigned i = 0; i < 12; i++)
        psx_mod_write_byte(MOA_VS_PARTS + 12u * player + i, parts[i]);
    for (unsigned i = 0; i < MOA_PAINT_BYTES; i++)
        psx_mod_write_byte(MOA_VS_PAINT + MOA_PAINT_BYTES * player + i, r[16u + i]);
    moa_enter_unit_state(player, r, ai);
}

static uint32_t moa_random(uint32_t below)
{
    static uint32_t state;
    if (!state) state = (uint32_t)time(NULL) | 1u;
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return below ? state % below : 0;
}

/* VS START, held back for as many frames as the units' graphics take. */
static unsigned moa_load_step;
static uint32_t moa_load_frames;
static uint8_t moa_unit_record[2][MOA_RECORD_BYTES];
static uint8_t moa_unit_ai[2][MOA_AI_RECORD_BYTES];

static void moa_units_ready(void)
{
    memcpy(moa_side_record, moa_unit_ai[0], MOA_AI_RECORD_BYTES);
    moa_side_record_set = 1;
    for (unsigned i = 0; i < MOA_AI_RECORD_BYTES; i++)
        psx_mod_write_byte(MOA_AI_RECORD + i, moa_unit_ai[1][i]);
    psx_mod_write_byte(MOA_MODE_BYTE, MOA_MODE_ARENA);
    moa_matches++;
    psx_mod_counter_add("moa.ai.match", 1);
}

static uint8_t moa_menu_loaded[2];             /* unit came in through DATA LOAD */
static int moa_vs_start_armed;                 /* the script is done with the menu */

static int moa_vs_start_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_load_step == 0) moa_ai_enabled = -1;  /* read the option per match */
    if (!moa_ai_battle_on() || psx_mod_read_word(address) != MOA_SIG_VS_START)
        return 0;
    /* The same routine compiles an AC after DATA LOAD: only the real VS START,
     * once the setup script has reached it. */
    if (!moa_vs_start_armed) return 0;
    const uint8_t mode = psx_mod_read_byte(MOA_MODE_BYTE);
    if (mode != MOA_MODE_VERSUS && mode != MOA_MODE_ARENA) return 0;
    const int can_load = psx_mod_read_word(MOA_CD_BUSY) == MOA_SIG_CD_BUSY &&
                         psx_mod_read_word(MOA_REBUILD) == MOA_SIG_REBUILD &&
                         psx_mod_read_word(MOA_AC_SWAP) == MOA_SIG_AC_SWAP;
    const uint32_t state = cpu->gpr[4];
    if (moa_load_step == 0) {
        if (!moa_pick_units(moa_unit_record, moa_unit_ai)) {
            psx_mod_counter_add("moa.ai.roster_failed", 1);
            return 0;                          /* stock versus match instead */
        }
        if (moa_menu_loaded[0] && moa_menu_loaded[1]) {
            /* Both builds came in through the game's own DATA LOAD (parts,
             * paint, names, emblem): only the AI is ours to hand over. */
            psx_mod_counter_add("moa.ai.menu_loaded_match", 1);
            moa_enter_unit_state(0, moa_unit_record[0], moa_unit_ai[0]);
            moa_enter_unit_state(1, moa_unit_record[1], moa_unit_ai[1]);
            moa_units_ready();
            return 0;
        }
        moa_enter_unit(0, moa_unit_record[0], moa_unit_ai[0]);
        moa_enter_unit(1, moa_unit_record[1], moa_unit_ai[1]);
        if (!can_load) {                       /* default colours, but a match */
            moa_units_ready();
            return 0;
        }
        moa_load_step = 1;
        moa_load_frames = 0;
    }
    if (moa_cd_busy(cpu) && ++moa_load_frames < MOA_LOAD_FRAMES_MAX)
        return 1;                              /* VS START runs again next frame */
    switch (moa_load_step) {
    case 1:                                    /* player 1's set in front */
        if (psx_mod_read_byte(state + MOA_VS_FRONT) == 1) {
            moa_swap_sets(cpu);
            psx_mod_write_byte(state + MOA_VS_FRONT, 0);
        }
        moa_load_unit(cpu, 0);
        moa_load_step = 2;
        return 1;
    case 2:                                    /* unit 1 done; player 2's set */
        moa_commit_unit(0);
        moa_swap_sets(cpu);
        moa_load_unit(cpu, 1);
        moa_load_step = 3;
        return 1;
    default:                                   /* unit 2 done; player 1 in front */
        moa_commit_unit(1);
        moa_swap_sets(cpu);
        moa_load_step = 0;
        if (moa_load_frames >= MOA_LOAD_FRAMES_MAX)
            psx_mod_counter_add("moa.ai.load_timeout", 1);
        psx_mod_counter_add("moa.ai.painted", 1);
        moa_units_ready();
        return 0;                              /* VS START goes ahead */
    }
}

/* ---- straight to the versus menu ----------------------------------------------
 *
 * An AI Battle session goes from boot to the versus menu by itself. The menu
 * module's input routines are fed a tapped START until the versus menu opens:
 * that skips the logos and the intro video and passes the title screen. The
 * mode choice that follows ("Scenario Mode / VS Mode") is settled when its
 * confirm handler runs: the highlighted row is made the versus one.
 */
#define MOA_MENU_ENTRY      0x800A69C8u  /* menu module entry */
#define MOA_SIG_MENU_ENTRY  0x27BDFEB0u
#define MOA_MENU_VERSUS     0x800B0CB4u  /* menu: versus setup */
#define MOA_SIG_MENU_VERSUS 0x27BDFFC0u
#define MOA_MENU_INPUT_A    0x800639A4u  /* menu: controller sampling */
#define MOA_SIG_INPUT_A     0x27BDFFE8u
#define MOA_MENU_INPUT_B    0x800AF590u
#define MOA_SIG_INPUT_B     0x27BDFFD0u
#define MOA_MODE_CONFIRM    0x800AB500u  /* title: mode row confirmed */
#define MOA_SIG_MODE_CONFIRM 0x27BDFFC8u
#define MOA_MODE_ROW_Y      80u          /* row widget: 176 scenario, 192 versus */
#define MOA_MODE_ROW_VERSUS 192u
#define MOA_MENU_FLAGS      0x801B1CD0u  /* bit 9: second controller port usable */
#define MOA_PAD_BUFFER      0x800458D8u  /* port 1: status, id, buttons (low = held) */

static int moa_autopilot;
static uint32_t moa_autopilot_ticks;

/* ---- unattended setup ---------------------------------------------------------
 *
 * From boot to the moment the player takes over, an AI Battle session runs by
 * itself in fast forward, with the controllers locked out:
 *
 *   boot -> versus menu (autopilot above), then a script on the versus menu:
 *   1P AC SETUP > SYSTEM > DATA LOAD > SLOT 2 > save 1 (unit 1), the same for
 *   2P with save 2 (the launcher puts both units on a session-only card in
 *   slot 2, PSX_MOA_AI_LOADMENU=1), SELECT MAP to PSX_MOA_AI_MAP (1..10),
 *   back to VS START.
 *
 * Player vs AI (PSX_MOA_AI_P1=1): the player takes over there. AI vs AI: VS
 * START, the fight in fast forward, START at the result to watch the replay;
 * the player takes over when the replay begins. A loaded build is checked
 * against its card; a unit that did not come through falls back to the
 * direct entry at VS START.
 *
 * Back on the menu after a match, or out to the title screen, the session
 * returns to the launcher.
 */
#define MOA_PAD2_BUFFER     0x800458FAu  /* port 2: status, id, buttons */
#define MOA_VS_MAP          0x8003EA81u  /* SELECT MAP: 1..10 */
#define MOA_REPLAY_FLAG     0x8003EA92u  /* nonzero while the arena engine replays */
#define MOA_SAMPLER         0x8007F69Cu  /* arena engine: controller sampler */
#define MOA_SIG_SAMPLER     0x27BDFFE8u
#define MOA_ENGINE_FLAGS    0x8009EFD4u  /* bit 0x400: no game pad */
#define MOA_PAD_UP          0x0010u
#define MOA_PAD_RIGHT       0x0020u
#define MOA_PAD_DOWN        0x0040u
#define MOA_PAD_X           0x4000u
#define MOA_PAD_O           0x2000u

enum { STEP_PRESS, STEP_CHECK, STEP_MAP, STEP_HANDOVER, STEP_FIGHT, STEP_ROW };
#define MOA_VS_ROW          0x801B2038u  /* versus menu cursor: 0 VS START .. 3 SELECT MAP */
struct moa_step { uint8_t kind, port; uint16_t buttons, hold, wait; };

static struct moa_step moa_script[80];
static int moa_script_len, moa_script_pos;
static uint32_t moa_step_time;
static int moa_map_presses;
static int moa_map_target;
static int moa_lock;                           /* the script owns the controllers */
static uint16_t moa_inject[2];                 /* buttons held, active high */
static int moa_live_lock;                      /* AI vs AI fight: no controller */
static uint32_t moa_live_ticks, moa_live_ai_mark, moa_live_quiet;
static int moa_reached_versus;                 /* the versus menu came up once */
static uint8_t moa_expect_parts[2][12];
static int moa_expect_set[2];

static int moa_env_flag(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] == '1';
}

static void moa_return_to_launcher(const char *why)
{
    psx_mod_counter_add(why, 1);
    psx_mod_request_fast_forward(0);
    moa_lock = moa_live_lock = 0;
    if (moa_env_flag("PSX_MOA_AI_NO_RETURN")) return;   /* headless tests */
    psx_mod_request_return_to_launcher();
}

static void moa_add(uint8_t kind, uint8_t port, uint16_t buttons, uint16_t hold, uint16_t wait)
{
    if (moa_script_len >= (int)(sizeof moa_script / sizeof moa_script[0])) return;
    struct moa_step st = { kind, port, buttons, hold, wait };
    moa_script[moa_script_len++] = st;
}
static void moa_tap(uint8_t port, uint16_t buttons, uint16_t wait) { moa_add(STEP_PRESS, port, buttons, 4, wait); }

/* The build a card should leave in the versus player's 12 part bytes. */
static int moa_card_parts(const char *path, uint8_t parts[12])
{
    static struct moa_card card;
    if (!path || !path[0] || !moa_read_card(path, &card)) return 0;
    const uint8_t *r = card.rec;
    const uint8_t booster = r[3] >= 27u && r[3] != 0xFFu ? 0xFFu : r[12];
    const uint8_t p[12] = { r[0], booster, r[1], r[2], r[5], r[4], r[6], r[7], r[9], r[8], 0xFFu, r[3] };
    memcpy(parts, p, 12);
    return 1;
}

/* DATA LOAD for one player: the AC SETUP list is driven by that player's
 * controller; the versus menu row by controller 1. */
static void moa_script_load(unsigned player)
{
    const uint8_t port = (uint8_t)(player + 1);
    moa_add(STEP_ROW, 1, (uint16_t)(1 + player), 0, 0); /* 1P / 2P AC SETUP */
    moa_tap(1, MOA_PAD_X, 120);
    for (int i = 0; i < 4; i++) moa_tap(port, MOA_PAD_DOWN, 30);   /* SYSTEM */
    moa_tap(port, MOA_PAD_X, 120);                      /* SYSTEM: DATA LOAD / OPTION */
    moa_tap(port, MOA_PAD_X, 180);                      /* DATA LOAD -> SLOT 1 / SLOT 2 */
    moa_tap(port, MOA_PAD_DOWN, 40);                    /* SLOT 2 */
    moa_tap(port, MOA_PAD_X, 600);                      /* search the card */
    if (player) moa_tap(port, MOA_PAD_DOWN, 40);        /* save 2 */
    moa_tap(port, MOA_PAD_X, 120);                      /* "Load the saved data?" */
    moa_tap(port, MOA_PAD_X, 700);                      /* Yes, load */
    moa_tap(port, MOA_PAD_O, 90);                       /* out of SYSTEM */
    moa_tap(port, MOA_PAD_O, 240);                      /* out of AC SETUP: commits */
    moa_add(STEP_CHECK, (uint8_t)player, 0, 0, 0);
}

static void moa_script_build(void)
{
    moa_script_len = moa_script_pos = 0;
    moa_step_time = 0;
    moa_map_presses = 0;
    moa_menu_loaded[0] = moa_menu_loaded[1] = 0;
    if (moa_env_flag("PSX_MOA_AI_LOADMENU")) {
        const char *card[2] = { getenv("PSX_MOA_AI_UNIT1"), getenv("PSX_MOA_AI_UNIT2") };
        for (unsigned u = 0; u < 2; u++) moa_expect_set[u] = moa_card_parts(card[u], moa_expect_parts[u]);
        if (moa_expect_set[0] && moa_expect_set[1]) {
            moa_script_load(0);
            moa_script_load(1);
        }
    }
    moa_add(STEP_ROW, 1, 3, 0, 0);                      /* SELECT MAP */
    moa_tap(1, MOA_PAD_X, 40);                          /* arrows on */
    moa_add(STEP_MAP, 1, 0, 0, 0);
    moa_tap(1, MOA_PAD_X, 40);                          /* settle the map */
    moa_add(STEP_ROW, 1, 0, 0, 0);                      /* VS START */
    if (moa_manual_unit1()) {
        moa_add(STEP_HANDOVER, 1, 0, 0, 0);
    } else {
        moa_add(STEP_FIGHT, 1, 0, 0, 0);
        moa_tap(1, MOA_PAD_X, 0);                       /* VS START */
    }
}

static void moa_script_frame(void)
{
    if (!moa_lock || moa_script_pos >= moa_script_len) return;
    struct moa_step *st = &moa_script[moa_script_pos];
    const uint32_t t = moa_step_time++;
    switch (st->kind) {
    case STEP_PRESS:
        moa_inject[0] = moa_inject[1] = 0;
        if (t < st->hold) moa_inject[st->port - 1] = st->buttons;
        if (t + 1 >= (uint32_t)st->hold + st->wait) {
            moa_script_pos++; moa_step_time = 0;
            psx_mod_counter_add("moa.ai.step", 1);
        }
        if (moa_script_pos >= moa_script_len) {           /* script done: let go */
            moa_inject[0] = moa_inject[1] = 0;
            moa_lock = 0;
        }
        return;
    case STEP_CHECK: {
        const unsigned u = st->port;
        int same = moa_expect_set[u];
        for (unsigned i = 0; same && i < 12; i++)
            if (psx_mod_read_byte(MOA_VS_PARTS + 12u * u + i) != moa_expect_parts[u][i]) same = 0;
        moa_menu_loaded[u] = (uint8_t)same;
        psx_mod_counter_add(same ? "moa.ai.menu_load_ok" : "moa.ai.menu_load_failed", 1);
        moa_script_pos++; moa_step_time = 0;
        return;
    }
    case STEP_MAP: {
        if (!moa_map_target) {
            /* The versus maps AI Battle uses: 10 (the default), 1, 3, 6, 9.
             * The others are left out (4, 5, 7 and 8 hang at match start in
             * testing). PSX_MOA_AI_MAP = a map number, "random" (one of
             * these), or unset (map 10). */
            static const uint8_t usable[] = { 10, 1, 3, 6, 9 };
            const char *m = getenv("PSX_MOA_AI_MAP");
            const int asked = m && m[0] ? atoi(m) : 10;
            for (size_t i = 0; i < sizeof usable; i++) if (usable[i] == asked) moa_map_target = asked;
            if (!moa_map_target) moa_map_target = usable[moa_random((uint32_t)sizeof usable)];
        }
        const int want = moa_map_target;
        const int now = psx_mod_read_byte(MOA_VS_MAP);
        moa_inject[0] = moa_inject[1] = 0;
        if (want < 1 || want > 10 || now == want || moa_map_presses >= 24) {
            if (want >= 1 && want <= 10 && now != want) psx_mod_counter_add("moa.ai.map_failed", 1);
            moa_script_pos++; moa_step_time = 0; moa_map_presses = 0; moa_map_target = 0;
            return;
        }
        if (t < 4) moa_inject[0] = MOA_PAD_RIGHT;
        if (t >= 30) { moa_step_time = 0; moa_map_presses++; }
        return;
    }
    case STEP_ROW: {                                    /* move the versus menu cursor */
        const int row = psx_mod_read_byte(MOA_VS_ROW);
        moa_inject[0] = moa_inject[1] = 0;
        if (row == st->buttons && t >= 20) { moa_script_pos++; moa_step_time = 0; return; }
        if (t > 4000) {                                 /* the menu never answered */
            psx_mod_counter_add("moa.ai.menu_row_timeout", 1);
            moa_script_pos++; moa_step_time = 0; return;
        }
        if (row != st->buttons && (t % 30u) < 4u) moa_inject[0] = row < st->buttons ? MOA_PAD_DOWN : MOA_PAD_UP;
        return;
    }
    case STEP_HANDOVER:
        moa_vs_start_armed = 1;
        moa_inject[0] = moa_inject[1] = 0;
        moa_lock = 0;
        psx_mod_request_fast_forward(0);
        psx_mod_counter_add("moa.ai.handover_menu", 1);
        moa_script_pos = moa_script_len;
        return;
    case STEP_FIGHT:                                    /* stays fast through the fight */
        moa_vs_start_armed = 1;
        moa_live_lock = 1;
        moa_live_ticks = moa_live_quiet = 0;
        moa_live_ai_mark = moa_ai_frames_seen;
        moa_script_pos++; moa_step_time = 0;
        return;
    }
}


/* DATA LOAD reads a whole scenario save and also takes the game's settings
 * from it (screen, controls, sound ...; menu 0x8007361C). A unit's save
 * carries only the AC, so right after the read -- the loader's checksum call,
 * 0x80070F1C(buffer + 432) -- every block that is not the AC is filled with
 * the values the game has now, and the checksum is made again: the load then
 * changes the AC alone. */
#define MOA_SAVE_SUM        0x80070F1Cu
#define MOA_SIG_SAVE_SUM    0x00002821u  /* addu a1,zero,zero */
static const struct { uint16_t save; uint32_t ram; uint16_t bytes; } moa_keep[] = {
    { 456, 0x8003EA94u, 4 },   { 496, 0x8004B508u, 96 },  { 640, 0x80036FECu, 256 },
    { 896, 0x800370ECu, 256 }, { 3232, 0x8004AC50u, 32 }, { 3276, 0x8004B568u, 48 },
    { 3332, 0x8004B5A0u, 224 }, { 3572, 0x8004B688u, 96 },
};

static void moa_save_sum_entry(struct CPUState *cpu, uint32_t address)
{
    if (!moa_lock || !moa_ai_battle_on() || psx_mod_read_word(address) != MOA_SIG_SAVE_SUM) return;
    const uint32_t buf = cpu->gpr[4] - 432u;
    if (buf < 0x80000000u || buf >= 0x801FF000u) return;
    if (psx_mod_read_byte(buf) != 'S' || psx_mod_read_byte(buf + 1u) != 'C' ||
        psx_mod_read_byte(buf + 3672u) != 1u)
        return;
    for (size_t k = 0; k < sizeof moa_keep / sizeof moa_keep[0]; k++)
        for (uint32_t i = 0; i < moa_keep[k].bytes; i++)
            psx_mod_write_byte(buf + moa_keep[k].save + i, psx_mod_read_byte(moa_keep[k].ram + i));
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 3240u; i++) sum += 0xFFFFFFFFu - psx_mod_read_byte(buf + 432u + i);
    psx_mod_write_word(buf + 384u, sum);
    psx_mod_counter_add("moa.ai.save_settings_kept", 1);
}

static void moa_frame_hook(void)
{
    if (!moa_ai_battle_on()) return;
    moa_script_frame();
}

/* Arena engine controller sampler, while an AI vs AI fight runs unattended:
 * the controller is not read (no pause, no input); the raw word is 0, or
 * START once the fight is over (the AI has stopped), which the result prompt
 * takes as "watch the replay". When the replay starts, the player has it. */
static int moa_sampler_nested;

static int moa_sampler_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_sampler_nested) return 0;
    if (!moa_live_lock || !moa_ai_battle_on() || psx_mod_read_word(address) != MOA_SIG_SAMPLER)
        return 0;
    if (psx_mod_read_byte(MOA_REPLAY_FLAG) != 0) {
        moa_live_lock = 0;
        psx_mod_request_fast_forward(0);
        psx_mod_counter_add("moa.ai.handover_replay", 1);
        return 0;                                       /* the player's own controller */
    }
    moa_live_ticks++;
    if (moa_ai_frames_seen != moa_live_ai_mark) { moa_live_ai_mark = moa_ai_frames_seen; moa_live_quiet = 0; }
    else moa_live_quiet++;
    const int fought = moa_ai_frames_seen > 0;
    const uint16_t raw = (fought && moa_live_quiet > 60u && (moa_live_ticks % 24u) < 3u) ? MOA_PAD_START : 0;
    /* The game's own sampler still runs (it services the controller driver);
     * its result is then replaced. */
    moa_sampler_nested = 1;
    (void)psx_mod_call_guest(cpu, address, cpu->gpr[31], cpu->gpr[4], cpu->gpr[5],
                             cpu->gpr[6], cpu->gpr[7]);
    moa_sampler_nested = 0;
    psx_mod_write_half(MOA_SLOT1, raw);
    psx_mod_write_half(MOA_SLOT1 + 4u, (psx_mod_read_word(MOA_ENGINE_FLAGS) & 0x400u) ? 0 : raw);
    return 1;
}

/* A new launch from the launcher (PSX_MOA_AI_SESSION changes each time):
 * forget the last session. */
static void moa_session_check(void)
{
    static char token[40];
    const char *t = getenv("PSX_MOA_AI_SESSION");
    if (!t) t = "";
    if (strncmp(token, t, sizeof token - 1) == 0) return;
    snprintf(token, sizeof token, "%s", t);
    moa_matches = 0;
    moa_reached_versus = 0;
    moa_vs_start_armed = 0;
    moa_lock = moa_live_lock = 0;
    moa_menu_loaded[0] = moa_menu_loaded[1] = 0;
    moa_inject[0] = moa_inject[1] = 0;
    moa_ai_frames_seen = 0;
    moa_load_step = 0;
}

static void moa_menu_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    moa_ai_enabled = -1;
    if (!moa_ai_battle_on() || psx_mod_read_word(address) != MOA_SIG_MENU_ENTRY)
        return;
    moa_session_check();
    if (moa_matches > 0) {                     /* back from a match */
        moa_return_to_launcher("moa.ai.return_after_match");
        return;
    }
    moa_autopilot = 1;
    moa_autopilot_ticks = 0;
    psx_mod_request_fast_forward(1);
    psx_mod_counter_add("moa.ai.menu_entry", 1);
}

static void moa_menu_versus_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (psx_mod_read_word(address) != MOA_SIG_MENU_VERSUS) return;
    moa_autopilot = 0;
    if (!moa_ai_battle_on() || moa_reached_versus) return;
    moa_reached_versus = 1;
    psx_mod_write_word(MOA_MENU_FLAGS, psx_mod_read_word(MOA_MENU_FLAGS) | 0x200u);
    if (moa_env_flag("PSX_MOA_AI_NOSCRIPT")) { psx_mod_request_fast_forward(0); return; }  /* tests */
    moa_script_build();
    moa_lock = 1;
    psx_mod_request_fast_forward(1);
    psx_mod_counter_add("moa.ai.script_start", 1);
}

static void moa_menu_input_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (!moa_ai_battle_on()) return;
    const uint32_t word = psx_mod_read_word(address);
    if (word != MOA_SIG_INPUT_A && word != MOA_SIG_INPUT_B) return;
    if (moa_autopilot) {
        /* START held for 3 samples out of every 24. */
        const int held = (moa_autopilot_ticks++ % 24u) < 3u;
        psx_mod_write_half(MOA_PAD_BUFFER + 2u,
                           held ? (uint16_t)~MOA_PAD_START : (uint16_t)0xFFFFu);
        return;
    }
    if (!moa_lock) return;
    psx_mod_write_byte(MOA_PAD_BUFFER, 0);
    psx_mod_write_byte(MOA_PAD_BUFFER + 1u, 0x41);
    psx_mod_write_half(MOA_PAD_BUFFER + 2u, (uint16_t)~moa_inject[0]);
    psx_mod_write_byte(MOA_PAD2_BUFFER, 0);
    psx_mod_write_byte(MOA_PAD2_BUFFER + 1u, 0x41);
    psx_mod_write_half(MOA_PAD2_BUFFER + 2u, (uint16_t)~moa_inject[1]);
}

/* The movie player polls the controller inside its own loop and returns 1
 * when the player skips. During the autopilot every movie is "skipped"
 * before it starts. */
#define MOA_MOVIE_PLAY      0x80054B54u
#define MOA_SIG_MOVIE_PLAY  0x27BDFFD8u

static int moa_movie_filter(struct CPUState *cpu, uint32_t address)
{
    if (!moa_autopilot || !moa_ai_battle_on() ||
        psx_mod_read_word(address) != MOA_SIG_MOVIE_PLAY)
        return 0;
    cpu->gpr[2] = 1;
    psx_mod_counter_add("moa.ai.movie_skipped", 1);
    return 1;
}

/* The title sequence takes one argument: 0 plays the publisher logos and the
 * intro video first, 1 opens the title screen at once (the game's own path
 * back from the versus menu). */
#define MOA_TITLE_OPEN      0x800AAE94u
#define MOA_SIG_TITLE_OPEN  0x27BDFFE0u

static void moa_title_open_entry(struct CPUState *cpu, uint32_t address)
{
    if (!moa_ai_battle_on() || psx_mod_read_word(address) != MOA_SIG_TITLE_OPEN) return;
    if (moa_reached_versus && !moa_autopilot) {   /* the player left the versus menu */
        moa_return_to_launcher("moa.ai.return_from_menu");
        return;
    }
    if (!moa_autopilot) return;
    cpu->gpr[4] = 1;
    psx_mod_counter_add("moa.ai.logos_skipped", 1);
}

static void moa_mode_confirm_entry(struct CPUState *cpu, uint32_t address)
{
    if (!moa_autopilot || !moa_ai_battle_on() ||
        psx_mod_read_word(address) != MOA_SIG_MODE_CONFIRM)
        return;
    const uint32_t row = psx_mod_read_word(cpu->gpr[4] + 148u);
    if (row < 0x80000000u || row >= 0x80200000u) return;
    psx_mod_write_word(row + MOA_MODE_ROW_Y, MOA_MODE_ROW_VERSUS);
    psx_mod_write_word(MOA_MENU_FLAGS, psx_mod_read_word(MOA_MENU_FLAGS) | 0x200u);
    psx_mod_counter_add("moa.ai.mode_forced", 1);
}

PSX_MOD_CONSTRUCTOR(moa_ai_battle_register)
{
    (void)psx_game_register_offline_function_entry(MOA_MENU_ENTRY, moa_menu_entry);
    (void)psx_game_register_offline_function_entry(MOA_MENU_VERSUS, moa_menu_versus_entry);
    (void)psx_game_register_offline_function_entry(MOA_MENU_INPUT_A, moa_menu_input_entry);
    (void)psx_game_register_offline_function_entry(MOA_MENU_INPUT_B, moa_menu_input_entry);
    (void)psx_game_register_offline_function_entry(MOA_MODE_CONFIRM, moa_mode_confirm_entry);
    (void)psx_game_register_offline_function_entry(MOA_TITLE_OPEN, moa_title_open_entry);
    (void)psx_game_register_offline_function_filter(MOA_MOVIE_PLAY, moa_movie_filter);
    (void)psx_game_register_offline_function_filter(MOA_MENU_VS_START, moa_vs_start_filter);
    (void)psx_game_register_offline_function_filter(MOA_AI_SETUP, moa_ai_setup_filter);
    (void)psx_game_register_offline_function_filter(MOA_AI_INIT, moa_ai_init_filter);
    (void)psx_game_register_offline_function_filter(MOA_AI_FRAME, moa_ai_frame_filter);
    (void)psx_game_register_offline_function_filter(MOA_SAMPLER, moa_sampler_filter);
    (void)psx_game_register_offline_function_entry(MOA_SAVE_SUM, moa_save_sum_entry);
    mod_register_frame_hook(moa_frame_hook);
}
