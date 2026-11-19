/* Master of Arena: start a versus match with as little menu as possible.
 * See moa_vs_direct.h and docs/mapping/mode-launch.md.
 *
 * Offline hooks only (netplay sessions park them); each hook checks that the
 * menu module is the code in memory, since every engine loads at the same
 * address.
 */
#include "moa_vs_direct.h"
#include "moa_card.h"

#include "mod_plugins.h"
#include "cpu_state.h"

#include <string.h>

/* ---- the game (menu module, FDAT 206) ----------------------------------- */

#define VSD_MODE_BYTE       0x8003EA2Au  /* engine the exe loads next; 2 = split-screen VS */
#define VSD_MODE_VERSUS     2u
#define VSD_TITLE_OPEN      0x800AAE94u  /* menu entry opens the title here */
#define VSD_SIG_TITLE_OPEN  0x27BDFFE0u
#define VSD_MENU_VERSUS     0x800B0CB4u  /* versus menu setup (the title's VS row calls it) */
#define VSD_SIG_MENU_VERSUS 0x27BDFFC0u
#define VSD_SCREEN_RESET    0x80074DC0u  /* the VS row calls these two first */
#define VSD_GARAGE_DEFAULT  0x800633A4u  /* garage := the default AC */
#define VSD_FADE            0x80057CB0u  /* (4, 0), as the menu entry does before 0x800B0CB4 */
#define VSD_SIG_FADE        0x27BDFFE8u
#define VSD_PLUS_FLAGS      0x8003EAC8u  /* 8 bytes the VS row clears */
#define VSD_MENU_FLAGS      0x801B1CD0u  /* bit 9: second controller usable */
#define VSD_DRAW_SYNC       0x8002C4B0u  /* boot exe: DrawSync(0) */

#define VSD_VS_PARTS        0x80045940u  /* 12 part ids per player */
#define VSD_VS_PAINT        0x8004B730u  /* 48 colour bytes per player */
#define VSD_VS_OPTIONS      0x8003EAC0u  /* u16 per player */
#define VSD_VS_PLUS         0x8004A7D0u  /* 8 bytes per player */
#define VSD_VS_NAMES        0x8004AB18u  /* 36 bytes per player: AC name, pilot name */
#define VSD_VS_PATTERN      0x8003EAD2u  /* + player */
#define VSD_VS_VARIANT      0x8003EAB8u  /* + player */

#define VSD_CD_BUSY         0x800589DCu  /* nonzero while disc loads are queued */
#define VSD_SIG_CD_BUSY     0x27BDFFE8u
#define VSD_REBUILD         0x80081E20u  /* garage AC: textures + paint, part files */
#define VSD_SIG_REBUILD     0x27BDFFE8u
#define VSD_AC_SWAP         0x800C0E7Cu  /* trade the two players' texture sets */
#define VSD_SIG_AC_SWAP     0x27BDFFC0u
#define VSD_VS_FRONT        133u         /* VS menu state: 1 = player 2's set in front */
#define VSD_GARAGE_PATTERN  0x8003EA72u
#define VSD_GARAGE_PARTS    0x80036FB0u
#define VSD_GARAGE_PAINT    0x80036FBCu
#define VSD_GARAGE_OPTIONS  0x8003EA70u
#define VSD_WORK_FILES      0x801FBB00u  /* working table: file per slot */
#define VSD_WORK_SIZES      0x801D0B28u
#define VSD_PLAYER_FILES    0x8004AB60u  /* + 60 per player */
#define VSD_PLAYER_SIZES    0x8004A9E0u  /* + 30 per player */
#define VSD_FILE_FIRST      3u           /* slots 3..14 are the AC's parts */
#define VSD_FILE_END        15u
#define VSD_LOAD_FRAMES_MAX 900u

/* The garage emblem in video memory: 64 x 64 at 4 bits (16 x 64 halfwords)
 * and its 16-colour table; the card loader (0x80073B70) uploads a save's
 * emblem to exactly these rectangles (RECTs at 0x800CC070 / 0x800CC078). */
#define VSD_EMBLEM_X 544u
#define VSD_EMBLEM_Y 384u
#define VSD_CLUT_X   256u
#define VSD_CLUT_Y   510u

/* ---- units ------------------------------------------------------------------ */

static uint8_t vsd_has_emblem[2];
static uint16_t vsd_clut[2][16];
static uint8_t vsd_emblem[2][2048];

void moa_vs_unit_from_card(const struct moa_card *c, uint8_t plus_level, int waive,
                           struct moa_vs_unit *u)
{
    memset(u, 0, sizeof *u);
    const uint8_t *r = c->rec;
    /* Arena record order -> the garage's: core, booster, head, arms, blade,
     * hand weapon, back left, back right, generator, FCS, (none), legs.
     * Caterpillar legs (27 and up) have no booster. */
    const uint8_t booster = r[3] >= 27u && r[3] != 0xFFu ? 0xFFu : r[12];
    const uint8_t parts[12] = { r[0], booster, r[1], r[2], r[5], moa_card_game_hand(r),
                                r[6], r[7], r[9], r[8], 0xFFu, r[3] };
    memcpy(u->parts, parts, 12);
    for (unsigned i = 0; i < 48; i++) u->paint[i] = c->paint[i] > 64 ? 64 : c->paint[i];
    u->options = (uint16_t)(r[10] | (r[11] << 8));
    u->plus[0] = plus_level;
    u->plus[2] = (uint8_t)(plus_level || waive ? 1 : 0);
    moa_card_put_name(u->names, c->ac);
    moa_card_put_name(u->names + 18, c->pilot);
    u->has_names = 1;
    for (unsigned i = 0; i < 16; i++) u->clut[i] = (uint16_t)(c->palette[i] & 0x7FFFu);
    for (unsigned i = 0; i < 2048; i++)
        u->emblem[i] = (uint8_t)((c->emblem[2 * i] & 15) | ((c->emblem[2 * i + 1] & 15) << 4));
    u->has_emblem = 1;
}

void moa_vs_direct_clear(void)
{
    vsd_has_emblem[0] = vsd_has_emblem[1] = 0;
}

void moa_vs_direct_set_unit(unsigned player, const struct moa_vs_unit *u)
{
    if (player > 1u || !u) return;
    for (unsigned i = 0; i < 12; i++) psx_mod_write_byte(VSD_VS_PARTS + 12u * player + i, u->parts[i]);
    for (unsigned i = 0; i < 48; i++) psx_mod_write_byte(VSD_VS_PAINT + 48u * player + i, u->paint[i]);
    psx_mod_write_half(VSD_VS_OPTIONS + 2u * player, u->options);
    for (unsigned i = 0; i < 8; i++) psx_mod_write_byte(VSD_VS_PLUS + 8u * player + i, u->plus[i]);
    /* Custom paint (pattern 0) shows the colours above. */
    psx_mod_write_byte(VSD_VS_PATTERN + player, 0);
    psx_mod_write_byte(VSD_VS_VARIANT + player, 0);
    if (u->has_names)
        for (unsigned i = 0; i < 36; i++) psx_mod_write_byte(VSD_VS_NAMES + 36u * player + i, u->names[i]);
    vsd_has_emblem[player] = u->has_emblem;
    if (u->has_emblem) {
        memcpy(vsd_clut[player], u->clut, sizeof vsd_clut[player]);
        memcpy(vsd_emblem[player], u->emblem, sizeof vsd_emblem[player]);
    }
    psx_mod_counter_add("moa.vsd.unit_set", 1);
}

/* Upload a unit's emblem where the garage rebuild reads it. The pixels go
 * through a scratch area below the guest stack (unused at this moment, and
 * put back right after: the upload is synchronous). */
static void vsd_upload_emblem(struct CPUState *cpu, unsigned player)
{
    if (!vsd_has_emblem[player]) return;
    enum { BYTES = 2048 + 32 };
    const uint32_t scratch = ((cpu->gpr[29] - 0x2000u) & ~3u) - BYTES;
    if (scratch < 0x80010000u || scratch >= 0x80200000u) return;
    uint8_t keep[BYTES];
    for (unsigned i = 0; i < BYTES; i++) keep[i] = psx_mod_read_byte(scratch + i);
    (void)psx_mod_call_guest(cpu, VSD_DRAW_SYNC, cpu->gpr[31], 0, 0, 0, 0);
    for (unsigned i = 0; i < 2048; i++) psx_mod_write_byte(scratch + i, vsd_emblem[player][i]);
    for (unsigned i = 0; i < 16; i++) psx_mod_write_half(scratch + 2048u + 2u * i, vsd_clut[player][i]);
    const int ok = psx_mod_psyq_load_image(VSD_EMBLEM_X, VSD_EMBLEM_Y, 16, 64, scratch) &&
                   psx_mod_psyq_load_image(VSD_CLUT_X, VSD_CLUT_Y, 16, 1, scratch + 2048u);
    for (unsigned i = 0; i < BYTES; i++) psx_mod_write_byte(scratch + i, keep[i]);
    psx_mod_counter_add(ok ? "moa.vsd.emblem" : "moa.vsd.emblem_failed", 1);
}

/* ---- 1. boot ---------------------------------------------------------------- */

static int vsd_boot_armed;

void moa_vs_direct_arm_boot(int on) { vsd_boot_armed = on ? 1 : 0; }
int moa_vs_direct_boot_armed(void) { return vsd_boot_armed; }

/* The menu entry is about to open the title (logos and intro first). Do
 * what the title's VS MODE row does instead (0x800AB500, row 192, two
 * controllers): reset the screen and the garage, clear the Human PLUS bytes,
 * mode 2, open the versus menu. The menu entry's own "back to the versus
 * menu" path (0x800A6C98) calls the same setup at this same point. */
static int vsd_title_filter(struct CPUState *cpu, uint32_t address)
{
    if (!vsd_boot_armed || psx_mod_read_word(address) != VSD_SIG_TITLE_OPEN) return 0;
    if (psx_mod_read_word(VSD_MENU_VERSUS) != VSD_SIG_MENU_VERSUS ||
        psx_mod_read_word(VSD_FADE) != VSD_SIG_FADE)
        return 0;                              /* not the menu module: leave it */
    vsd_boot_armed = 0;
    const uint32_t ra = cpu->gpr[31];
    (void)psx_mod_call_guest(cpu, VSD_SCREEN_RESET, ra, 0, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, VSD_GARAGE_DEFAULT, ra, 0, 0, 0, 0);
    for (unsigned i = 0; i < 8; i++) psx_mod_write_byte(VSD_PLUS_FLAGS + i, 0);
    (void)psx_mod_call_guest(cpu, VSD_FADE, ra, 4, 0, 0, 0);
    psx_mod_write_byte(VSD_MODE_BYTE, VSD_MODE_VERSUS);
    psx_mod_write_word(VSD_MENU_FLAGS, psx_mod_read_word(VSD_MENU_FLAGS) | 0x200u);
    (void)psx_mod_call_guest(cpu, VSD_MENU_VERSUS, ra, 0, 0, 0, 0);
    psx_mod_counter_add("moa.vsd.boot_to_versus", 1);
    return 1;                                  /* the title is not opened */
}

/* ---- 3. graphics ------------------------------------------------------------- */

static unsigned vsd_step;
static uint32_t vsd_frames;

int moa_vs_direct_loading(void) { return vsd_step != 0; }

static int vsd_cd_busy(struct CPUState *cpu)
{
    return (psx_mod_call_guest(cpu, VSD_CD_BUSY, cpu->gpr[31], 0, 0, 0, 0) & 0xFFFFu) != 0;
}

/* Garage := player `player`'s files, parts, paint and emblem; rebuild. */
static void vsd_load_unit(struct CPUState *cpu, unsigned player)
{
    for (uint32_t i = VSD_FILE_FIRST; i < VSD_FILE_END; i++) {
        psx_mod_write_word(VSD_WORK_FILES + 4u * i,
                           psx_mod_read_word(VSD_PLAYER_FILES + 60u * player + 4u * i));
        psx_mod_write_word(VSD_WORK_SIZES + 4u * i,
                           psx_mod_read_half(VSD_PLAYER_SIZES + 30u * player + 2u * i));
    }
    for (unsigned i = 0; i < 12; i++)
        psx_mod_write_byte(VSD_GARAGE_PARTS + i, psx_mod_read_byte(VSD_VS_PARTS + 12u * player + i));
    for (unsigned i = 0; i < 48; i++)
        psx_mod_write_byte(VSD_GARAGE_PAINT + i, psx_mod_read_byte(VSD_VS_PAINT + 48u * player + i));
    psx_mod_write_half(VSD_GARAGE_OPTIONS, psx_mod_read_half(VSD_VS_OPTIONS + 2u * player));
    psx_mod_write_byte(VSD_GARAGE_PATTERN, 0);
    psx_mod_write_byte(VSD_GARAGE_PATTERN + 1u, 0);
    psx_mod_write_byte(VSD_VS_PATTERN + player, 0);
    psx_mod_write_byte(VSD_VS_VARIANT + player, 0);
    vsd_upload_emblem(cpu, player);
    (void)psx_mod_call_guest(cpu, VSD_REBUILD, cpu->gpr[31], 0, 0, 0, 0);
}

/* The rebuilt part files belong to player `player` from now on. */
static void vsd_commit_unit(unsigned player)
{
    for (uint32_t i = VSD_FILE_FIRST; i < VSD_FILE_END; i++) {
        psx_mod_write_word(VSD_PLAYER_FILES + 60u * player + 4u * i,
                           psx_mod_read_word(VSD_WORK_FILES + 4u * i));
        psx_mod_write_half(VSD_PLAYER_SIZES + 30u * player + 2u * i,
                           (uint16_t)psx_mod_read_word(VSD_WORK_SIZES + 4u * i));
    }
}

static void vsd_swap(struct CPUState *cpu)
{
    (void)psx_mod_call_guest(cpu, VSD_AC_SWAP, cpu->gpr[31], 0, 0, 0, 0);
}

int moa_vs_direct_load_frame(struct CPUState *cpu, uint32_t vs_state, int *timed_out)
{
    if (timed_out) *timed_out = 0;
    if (vsd_step == 0) {
        if (psx_mod_read_word(VSD_CD_BUSY) != VSD_SIG_CD_BUSY ||
            psx_mod_read_word(VSD_REBUILD) != VSD_SIG_REBUILD ||
            psx_mod_read_word(VSD_AC_SWAP) != VSD_SIG_AC_SWAP) {
            psx_mod_counter_add("moa.vsd.no_loader", 1);
            return 0;                          /* default colours, but a match */
        }
        vsd_step = 1;
        vsd_frames = 0;
    }
    if (vsd_cd_busy(cpu) && ++vsd_frames < VSD_LOAD_FRAMES_MAX)
        return 1;                              /* VS START runs again next frame */
    switch (vsd_step) {
    case 1:                                    /* player 1's set in front */
        if (psx_mod_read_byte(vs_state + VSD_VS_FRONT) == 1) {
            vsd_swap(cpu);
            psx_mod_write_byte(vs_state + VSD_VS_FRONT, 0);
        }
        vsd_load_unit(cpu, 0);
        vsd_step = 2;
        return 1;
    case 2:                                    /* unit 1 done; player 2's set */
        vsd_commit_unit(0);
        vsd_swap(cpu);
        vsd_load_unit(cpu, 1);
        vsd_step = 3;
        return 1;
    default:                                   /* unit 2 done; player 1 in front */
        vsd_commit_unit(1);
        vsd_swap(cpu);
        vsd_step = 0;
        if (vsd_frames >= VSD_LOAD_FRAMES_MAX) {
            if (timed_out) *timed_out = 1;
            psx_mod_counter_add("moa.vsd.load_timeout", 1);
        }
        psx_mod_counter_add("moa.vsd.loaded", 1);
        return 0;                              /* VS START goes ahead */
    }
}

void moa_vs_direct_reset(void)
{
    vsd_boot_armed = 0;
    vsd_step = 0;
    vsd_frames = 0;
    moa_vs_direct_clear();
}

PSX_MOD_CONSTRUCTOR(moa_vs_direct_register)
{
    (void)psx_game_register_offline_function_filter(VSD_TITLE_OPEN, vsd_title_filter);
}
