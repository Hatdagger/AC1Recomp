/* Master of Arena: start a versus match with as little menu as possible.
 * See moa_vs_direct.h and docs/mapping/mode-launch.md.
 *
 * Offline: AI Battle drives it (boot arm, units, VS START loader).
 * Netplay: its own hooks below (the netplay section) start the match from
 * both players' memory cards. Each hook checks that the menu module is the
 * code in memory, since every engine loads at the same address.
 */
#include "moa_vs_direct.h"
#include "moa_card.h"
#include "moa_netplay_room.h"

#include "mod_plugins.h"
#include "cpu_state.h"
#include "memcard.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

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
static void vsd_upload_emblem_data(struct CPUState *cpu, const uint16_t clut[16],
                                   const uint8_t emblem[2048])
{
    enum { BYTES = 2048 + 32 };
    const uint32_t scratch = ((cpu->gpr[29] - 0x2000u) & ~3u) - BYTES;
    if (scratch < 0x80010000u || scratch >= 0x80200000u) return;
    uint8_t keep[BYTES];
    for (unsigned i = 0; i < BYTES; i++) keep[i] = psx_mod_read_byte(scratch + i);
    (void)psx_mod_call_guest(cpu, VSD_DRAW_SYNC, cpu->gpr[31], 0, 0, 0, 0);
    for (unsigned i = 0; i < 2048; i++) psx_mod_write_byte(scratch + i, emblem[i]);
    for (unsigned i = 0; i < 16; i++) psx_mod_write_half(scratch + 2048u + 2u * i, clut[i]);
    const int ok = psx_mod_psyq_load_image(VSD_EMBLEM_X, VSD_EMBLEM_Y, 16, 64, scratch) &&
                   psx_mod_psyq_load_image(VSD_CLUT_X, VSD_CLUT_Y, 16, 1, scratch + 2048u);
    for (unsigned i = 0; i < BYTES; i++) psx_mod_write_byte(scratch + i, keep[i]);
    psx_mod_counter_add(ok ? "moa.vsd.emblem" : "moa.vsd.emblem_failed", 1);
}

/* Offline: the emblems kept by moa_vs_direct_set_unit. */
static void vsd_upload_emblem(struct CPUState *cpu, unsigned player)
{
    if (vsd_has_emblem[player]) vsd_upload_emblem_data(cpu, vsd_clut[player], vsd_emblem[player]);
}

typedef void (*vsd_emblem_fn)(struct CPUState *cpu, unsigned player);

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
static void vsd_load_unit(struct CPUState *cpu, unsigned player, vsd_emblem_fn emblem)
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
    emblem(cpu, player);
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

/* The steps, one per call, with their progress in `*step` / `*frames` (the
 * caller decides where those live: host statics offline, guest memory under
 * netplay, where rollback must be able to rewind them). */
static int vsd_load_run(struct CPUState *cpu, uint32_t vs_state, unsigned *step, uint32_t *frames,
                        vsd_emblem_fn emblem, int *timed_out)
{
    if (timed_out) *timed_out = 0;
    if (*step == 0) {
        if (psx_mod_read_word(VSD_CD_BUSY) != VSD_SIG_CD_BUSY ||
            psx_mod_read_word(VSD_REBUILD) != VSD_SIG_REBUILD ||
            psx_mod_read_word(VSD_AC_SWAP) != VSD_SIG_AC_SWAP) {
            psx_mod_counter_add("moa.vsd.no_loader", 1);
            return 0;                          /* default colours, but a match */
        }
        *step = 1;
        *frames = 0;
    }
    if (vsd_cd_busy(cpu) && ++*frames < VSD_LOAD_FRAMES_MAX)
        return 1;                              /* VS START runs again next frame */
    switch (*step) {
    case 1:                                    /* player 1's set in front */
        if (psx_mod_read_byte(vs_state + VSD_VS_FRONT) == 1) {
            vsd_swap(cpu);
            psx_mod_write_byte(vs_state + VSD_VS_FRONT, 0);
        }
        vsd_load_unit(cpu, 0, emblem);
        *step = 2;
        return 1;
    case 2:                                    /* unit 1 done; player 2's set */
        vsd_commit_unit(0);
        vsd_swap(cpu);
        vsd_load_unit(cpu, 1, emblem);
        *step = 3;
        return 1;
    default:                                   /* unit 2 done; player 1 in front */
        vsd_commit_unit(1);
        vsd_swap(cpu);
        *step = 0;
        if (*frames >= VSD_LOAD_FRAMES_MAX) {
            if (timed_out) *timed_out = 1;
            psx_mod_counter_add("moa.vsd.load_timeout", 1);
        }
        psx_mod_counter_add("moa.vsd.loaded", 1);
        return 0;                              /* VS START goes ahead */
    }
}

int moa_vs_direct_load_frame(struct CPUState *cpu, uint32_t vs_state, int *timed_out)
{
    return vsd_load_run(cpu, vs_state, &vsd_step, &vsd_frames, vsd_upload_emblem, timed_out);
}

void moa_vs_direct_reset(void)
{
    vsd_boot_armed = 0;
    vsd_step = 0;
    vsd_frames = 0;
    moa_vs_direct_clear();
}


/* ---- netplay: from boot straight to the match ---------------------------------
 *
 * A netplay match boots on both PCs in lockstep, so nothing can be fast
 * forwarded. The first title of a session becomes the versus menu's setup
 * (as offline), each player's unit is the first Master of Arena save on that
 * player's match card (slot 1 = player 1 / host, slot 2 = player 2 / guest;
 * the launcher puts the chosen AC there), and the match starts from there.
 *
 * No menu is ever on screen and nothing presses its buttons (2026-11-19):
 *   - A black cover goes up at the title (presentation only, each PC its
 *     own) and comes down at the exe's loading screen before the link engine.
 *   - The versus menu's setup still runs behind it: it loads what the match
 *     needs besides the ACs (docs/mapping/boot-loads.md §9, "VS without
 *     menus": the saved VRAM blocks, sound bank, menu pictures, model slots).
 *   - When the menu reaches its input state (0x800B22D8, every load done),
 *     the state is switched straight to VS START (0x800B2D7C), the state the
 *     menu's own VS START row goes to. Both pads read idle until then.
 *   - VS START loads the units' graphics (filter below), then goes ahead with
 *     the game's own build check, stat blocks, stage, music and launch.
 *
 * Both peers must do exactly the same thing, also when rollback runs frames
 * again. So: the cards are the session's synced match cards (identical on
 * both PCs), and the progress lives in guest memory, not in host statics:
 * 8 bytes of the link struct's send packet (0x80043C70 + 944), which only the
 * link engine uses and which is idle from boot until that engine starts.
 * They are cleared again before VS START goes ahead. The cover is the only
 * host state, and it never reaches the simulation.
 *
 * A player whose card holds no save keeps the versus menu's default AC; with
 * no save on either card the title opens as before (players use the menu).
 * If the menu never reaches its input state, the start gives up: the cover
 * comes down and the players see the versus menu.
 */
#define VSD_NP_STATE        (0x80043C70u + 944u)  /* 8 bytes, see above */
#define VSD_NP_MAGIC0       0x56u              /* 'V' */
#define VSD_NP_MAGIC1       0x44u              /* 'D' */
#define VSD_MODE_LINK       1u
#define VSD_TITLE_FLAG      0x8003EA7Au        /* 1 until the first title has opened */
#define VSD_MENU_VS_START   0x800B2D7Cu        /* versus menu: VS START state */
#define VSD_SIG_VS_START    0x27BDFFD0u
#define VSD_MENU_VS_INPUT   0x800B22D8u        /* versus menu: input state (menus-screens.md §2f) */
#define VSD_TASK_STATE      0x78u              /* a menu task's state routine (menus-screens.md §1) */
#define VSD_INPUT_A         0x800639A4u        /* menu: controller sampling, once a frame */
#define VSD_SIG_INPUT_A     0x27BDFFE8u
#define VSD_INPUT_B         0x800AF590u
#define VSD_SIG_INPUT_B     0x27BDFFD0u
#define VSD_PAD1            0x800458D8u        /* port 1: status, id, buttons (low = held) */
#define VSD_PAD2            0x800458FAu
#define VSD_NP_GIVE_UP      3600u              /* samples without the input state: give up */
#define VSD_NP_START_AGAIN  600u               /* samples without VS START running: set it again */
#define VSD_EXE_LOADING     0x80012348u        /* exe: loading screen before an engine */
#define VSD_SIG_EXE_LOADING 0x27BDFF60u        /* addiu sp,sp,-160 */
#define VSD_COVER_MAX_MS    30000u             /* the cover never stays longer than this */

enum { NP_PHASE_WAIT = 0, NP_PHASE_LOADING };

struct vsd_np {                                /* the 8 guest bytes */
    uint8_t phase, step;
    uint16_t t, frames;
};

static int vsd_np_get(struct vsd_np *n)
{
    if (psx_mod_read_byte(VSD_NP_STATE) != VSD_NP_MAGIC0 ||
        psx_mod_read_byte(VSD_NP_STATE + 1u) != VSD_NP_MAGIC1)
        return 0;
    n->phase = psx_mod_read_byte(VSD_NP_STATE + 2u);
    n->step = psx_mod_read_byte(VSD_NP_STATE + 3u);
    n->t = psx_mod_read_half(VSD_NP_STATE + 4u);
    n->frames = psx_mod_read_half(VSD_NP_STATE + 6u);
    return 1;
}

static void vsd_np_put(const struct vsd_np *n)
{
    psx_mod_write_byte(VSD_NP_STATE, VSD_NP_MAGIC0);
    psx_mod_write_byte(VSD_NP_STATE + 1u, VSD_NP_MAGIC1);
    psx_mod_write_byte(VSD_NP_STATE + 2u, n->phase);
    psx_mod_write_byte(VSD_NP_STATE + 3u, n->step);
    psx_mod_write_half(VSD_NP_STATE + 4u, n->t);
    psx_mod_write_half(VSD_NP_STATE + 6u, n->frames);
}

/* ---- the cover (host state, presentation only) ---- */

static int vsd_cover_up;
static uint64_t vsd_cover_ms;

static uint64_t vsd_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void vsd_cover(int on, const char *why)
{
    static const uint32_t k_black[4 * 4] = {
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u,
        0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u };
    if (on) {
        if (!vsd_cover_up) vsd_cover_ms = vsd_now_ms();
        vsd_cover_up = 1;
        psx_mod_set_loading_image(k_black, 4, 4);  /* a picture hides the text */
        psx_game_netplay_loading_screen("LOADING", -1);
    } else {
        if (!vsd_cover_up) return;
        vsd_cover_up = 0;
        psx_game_netplay_loading_screen(NULL, 0);
    }
    psx_mod_counter_add(why, 1);
}

static void vsd_cover_check(void)
{
    if (vsd_cover_up && vsd_now_ms() - vsd_cover_ms > VSD_COVER_MAX_MS)
        vsd_cover(0, "moa.vsd.net_cover_timeout");
}

static void vsd_np_end(const char *why)
{
    for (unsigned i = 0; i < 8; i++) psx_mod_write_byte(VSD_NP_STATE + i, 0);
    psx_mod_counter_add(why, 1);
}

/* Player `player`'s unit from that player's match card. */
static int vsd_np_card_unit(unsigned player, struct moa_vs_unit *u)
{
    static struct moa_card card;
    uint8_t *mc = (uint8_t *)malloc(131072);
    if (!mc) return 0;
    const int ok = memcard_export_raw((int)player, mc) == 0 &&
                   moa_memcard_first_unit(mc, &card) != 0;
    free(mc);
    if (!ok) return 0;
    moa_vs_unit_from_card(&card, card.plus, 0, u);
    return 1;
}

static void vsd_np_upload_emblem(struct CPUState *cpu, unsigned player)
{
    static struct moa_vs_unit u;
    if (vsd_np_card_unit(player, &u)) vsd_upload_emblem_data(cpu, u.clut, u.emblem);
}

static int vsd_np_menu_in_memory(void)
{
    return psx_mod_read_word(VSD_MENU_VERSUS) == VSD_SIG_MENU_VERSUS &&
           psx_mod_read_word(VSD_FADE) == VSD_SIG_FADE &&
           psx_mod_read_word(VSD_MENU_VS_START) == VSD_SIG_VS_START;
}

static int vsd_np_title_filter(struct CPUState *cpu, uint32_t address)
{
    if (psx_mod_read_word(address) != VSD_SIG_TITLE_OPEN ||
        psx_mod_read_byte(VSD_TITLE_FLAG) != 1u ||          /* only the boot's title */
        !vsd_np_menu_in_memory())
        return 0;
    /* A new session: the versus room forgets the last match (before the
     * versus menu's setup below runs its hooks). */
    moa_np_room_session_start();
    static struct moa_vs_unit unit[2];
    int have[2];
    for (unsigned p = 0; p < 2; p++) have[p] = vsd_np_card_unit(p, &unit[p]);
    if (!have[0] && !have[1]) {
        psx_mod_counter_add("moa.vsd.net_no_saves", 1);
        return 0;                                  /* the title, as before */
    }
    vsd_cover(1, "moa.vsd.net_cover_up");
    const uint32_t ra = cpu->gpr[31];
    (void)psx_mod_call_guest(cpu, VSD_SCREEN_RESET, ra, 0, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, VSD_GARAGE_DEFAULT, ra, 0, 0, 0, 0);
    for (unsigned i = 0; i < 8; i++) psx_mod_write_byte(VSD_PLUS_FLAGS + i, 0);
    (void)psx_mod_call_guest(cpu, VSD_FADE, ra, 4, 0, 0, 0);
    psx_mod_write_byte(VSD_MODE_BYTE, VSD_MODE_LINK);
    psx_mod_write_word(VSD_MENU_FLAGS, psx_mod_read_word(VSD_MENU_FLAGS) | 0x200u);
    (void)psx_mod_call_guest(cpu, VSD_MENU_VERSUS, ra, 0, 0, 0, 0);
    /* After the menu's setup: its first visit copies the garage into both
     * players' staging, which the cards' units now replace. */
    for (unsigned p = 0; p < 2; p++)
        if (have[p]) moa_vs_direct_set_unit(p, &unit[p]);
    /* The launcher's versus room: its time limit (guest memory, before VS
     * START compiles the players) and the pilots' names (moa_netplay_room.c). */
    moa_np_room_title();
    const struct vsd_np n = { NP_PHASE_WAIT, 0, 0, 0 };
    vsd_np_put(&n);
    psx_mod_counter_add(have[0] && have[1] ? "moa.vsd.net_boot" : "moa.vsd.net_boot_one_card", 1);
    return 1;                                      /* the title is not opened */
}

/* The menu's controller sampling, once a frame: while the start is ours,
 * both pads read idle (nothing on the menu moves), and the start's clocks
 * run. */
static void vsd_np_input_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    vsd_cover_check();
    struct vsd_np n;
    if (!vsd_np_get(&n)) return;
    const uint32_t word = psx_mod_read_word(address);
    if (word != VSD_SIG_INPUT_A && word != VSD_SIG_INPUT_B) return;
    if (!vsd_np_menu_in_memory()) {                   /* not the menu */
        vsd_np_end("moa.vsd.net_left_menu");
        return;
    }
    if (n.phase == NP_PHASE_WAIT && n.t > VSD_NP_GIVE_UP) {
        /* The menu never got to its input state: hand it to the players. */
        vsd_np_end("moa.vsd.net_menu_timeout");
        vsd_cover(0, "moa.vsd.net_cover_gave_up");
        return;
    }
    if (n.phase == NP_PHASE_LOADING && n.step == 0 && n.t > VSD_NP_START_AGAIN) {
        n.phase = NP_PHASE_WAIT;                   /* VS START never ran: set it again */
        n.t = 0;
        psx_mod_counter_add("moa.vsd.net_start_again", 1);
    }
    if (n.t < 0xFFFFu) n.t++;
    vsd_np_put(&n);
    psx_mod_write_byte(VSD_PAD1, 0);
    psx_mod_write_byte(VSD_PAD1 + 1u, 0x41);
    psx_mod_write_half(VSD_PAD1 + 2u, 0xFFFFu);
    psx_mod_write_byte(VSD_PAD2, 0);
    psx_mod_write_byte(VSD_PAD2 + 1u, 0x41);
    psx_mod_write_half(VSD_PAD2 + 2u, 0xFFFFu);
}

/* The versus menu's input state (a0 = the menu's task): every load of its
 * opening is done. Instead of reading the pads, go to VS START, as the
 * menu's own VS START row does. */
static int vsd_np_vs_input_filter(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    struct vsd_np n;
    if (!vsd_np_get(&n) || n.phase != NP_PHASE_WAIT || !vsd_np_menu_in_memory()) return 0;
    const uint32_t task = cpu->gpr[4];
    if (task < 0x801B1CE8u || task >= 0x801D0AF8u) {   /* not a menu task: leave it */
        psx_mod_counter_add("moa.vsd.net_input_not_task", 1);
        return 0;
    }
    psx_mod_write_word(task + VSD_TASK_STATE, VSD_MENU_VS_START);
    n.phase = NP_PHASE_LOADING;
    n.step = 0;
    n.t = 0;
    n.frames = 0;
    vsd_np_put(&n);
    psx_mod_counter_add("moa.vsd.net_to_vs_start", 1);
    return 1;                                      /* no input this frame */
}

/* VS START, held back while the units' graphics load; then it goes ahead
 * (build check, stat blocks, the link engine). Later matches in the session
 * use the versus menu as usual and are left alone. */
static int vsd_np_vs_start_filter(struct CPUState *cpu, uint32_t address)
{
    struct vsd_np n;
    if (!vsd_np_get(&n) || n.phase != NP_PHASE_LOADING ||
        psx_mod_read_word(address) != VSD_SIG_VS_START)
        return 0;
    unsigned step = n.step;
    uint32_t frames = n.frames;
    int timed_out = 0;
    const int more = vsd_load_run(cpu, cpu->gpr[4], &step, &frames, vsd_np_upload_emblem, &timed_out);
    if (more) {
        n.step = (uint8_t)step;
        n.frames = (uint16_t)(frames > 0xFFFFu ? 0xFFFFu : frames);
        if (n.step == 0) n.step = 1;               /* keep "loading" visible to the input hook */
        vsd_np_put(&n);
        return 1;
    }
    vsd_np_end(timed_out ? "moa.vsd.net_loaded_timeout" : "moa.vsd.net_loaded");
    return 0;                                      /* VS START goes ahead */
}

/* The exe's loading screen before an engine: the game's own pictures take
 * over from the cover (the exe is always in memory). */
static void vsd_np_exe_loading_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (psx_mod_read_word(address) != VSD_SIG_EXE_LOADING) return;
    vsd_cover(0, "moa.vsd.net_cover_down");
}

PSX_MOD_CONSTRUCTOR(moa_vs_direct_register)
{
    (void)psx_game_register_offline_function_filter(VSD_TITLE_OPEN, vsd_title_filter);
    (void)psx_game_register_netplay_function_filter(VSD_TITLE_OPEN, vsd_np_title_filter);
    (void)psx_game_register_netplay_function_entry(VSD_INPUT_A, vsd_np_input_entry);
    (void)psx_game_register_netplay_function_entry(VSD_INPUT_B, vsd_np_input_entry);
    (void)psx_game_register_netplay_function_filter(VSD_MENU_VS_INPUT, vsd_np_vs_input_filter);
    (void)psx_game_register_netplay_function_filter(VSD_MENU_VS_START, vsd_np_vs_start_filter);
    (void)psx_game_register_netplay_function_entry(VSD_EXE_LOADING, vsd_np_exe_loading_entry);
}
