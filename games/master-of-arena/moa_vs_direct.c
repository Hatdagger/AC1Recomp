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

#include <stdio.h>
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


/* ---- netplay: from boot to the versus menu, both ACs loaded --------------------
 *
 * A netplay match boots on both PCs in lockstep, so nothing can be fast
 * forwarded. The first title of a session becomes the versus menu's setup
 * (as offline). Each player's AC is the first Master of Arena save on that
 * player's match card (slot 1 = player 1 / host, slot 2 = player 2 / guest;
 * the launcher puts the chosen AC there). It goes into the versus menu the
 * way the menu's own 1P / 2P AC SETUP + DATA LOAD would put it there, by
 * calling the game's routines directly (no menu screens, no button presses),
 * and then the players have the menu: they press VS START themselves.
 *
 * Nothing is hidden or muted: both PCs see and hear the versus menu open.
 *
 *   1. The menu opens as usual (default AC in both slots: its first state
 *      0x800B0FE4 puts it there and resets the settings on a first visit, so
 *      nothing can be written earlier) and reaches its input state
 *      0x800B22D0 (the routine task +0x78 holds; menus-screens.md §2f lists
 *      0x800B22D8, the instruction after the menu's `lui`).
 *   2. LOADING = FAST ends there (moa_np_room_fast_load_end): the ACs and the
 *      fight load at the drive's real speed.
 *   3. The menu is paused (task +0x78 = 0x800B1650, the menu's own empty
 *      routine) and, per player p = 0 then 1:
 *        a. the card's AC into p's staging (moa_vs_direct_set_unit);
 *        b. AC SETUP's entry (0x800B2748): owned parts -> 0x8003EB20,
 *           names -> 0x800371EC, extras -> 0x8003EAC8, then the game's
 *           0x800B3584(p, task): staging -> garage, p's part-file table ->
 *           the working one, p's texture set in front (task +0x85);
 *        c. DATA LOAD's finish (boot-loads.md §4a/§4d): part lists
 *           0x80090FD8, stand-ins 0x80067244(1), slot rules 0x80083130,
 *           the save's emblem, garage rebuild 0x80081E20;
 *        d. wait for the disc queue (0x800589DC), then AC SETUP's exit copy
 *           (state 0x800B4408): garage -> p's staging, working part files ->
 *           p's table (0x8004AB60 + 120p, sizes 0x8004A9E0 + 60p).
 *      AC SETUP's key / pad swaps for player 2 are left out: nobody drives
 *      AC SETUP here.
 *   4. The room's time limit (after the ACs: a save brings its own
 *      settings), the menu back in its input state, the progress cleared.
 *      From then on nothing here touches the menu or the pads.
 *
 * Both peers must do exactly the same thing, also when rollback runs frames
 * again. So: the cards are the session's synced match cards (identical on
 * both PCs), and the progress lives in guest memory, not in host statics:
 * 8 bytes of the link struct's send packet (0x80043C70 + 944), which only the
 * link engine uses and which is idle from boot until that engine starts. The
 * log lines are the only host state; they never reach the simulation.
 *
 * A player whose card holds no save keeps the menu's default AC; with no
 * save on either card the title opens as before. If the menu never reaches
 * its input state, the start gives up and the players have the menu.
 */
#define VSD_NP_STATE        (0x80043C70u + 944u)  /* 8 bytes, see above */
#define VSD_NP_MAGIC0       0x56u              /* 'V' */
#define VSD_NP_MAGIC1       0x44u              /* 'D' */
#define VSD_MODE_LINK       1u
#define VSD_TITLE_FLAG      0x8003EA7Au        /* 1 until the first title has opened */
#define VSD_MENU_VS_START   0x800B2D7Cu        /* versus menu: VS START state (signature check) */
#define VSD_SIG_VS_START    0x27BDFFD0u
#define VSD_MENU_VS_INPUT   0x800B22D0u        /* versus menu: input state, as task +0x78 holds it */
#define VSD_MENU_IDLE       0x800B1650u        /* the menu's empty routine (jr ra; nop) */
#define VSD_SIG_IDLE        0x03E00008u
#define VSD_TASKS_FIRST     0x801B1CE8u        /* the menu's task area (menus-screens.md §1) */
#define VSD_TASKS_END       0x801D0AF8u
#define VSD_TASK_SIZE       0xF0u
#define VSD_TASK_STATE      0x78u              /* a menu task's state routine */
#define VSD_INPUT_A         0x800639A4u        /* menu: controller sampling, once a frame */
#define VSD_SIG_INPUT_A     0x27BDFFE8u
#define VSD_INPUT_B         0x800AF590u
#define VSD_SIG_INPUT_B     0x27BDFFD0u
#define VSD_PAD1            0x800458D8u        /* port 1: status, id, buttons (low = held) */
#define VSD_PAD2            0x800458FAu
#define VSD_NP_GIVE_UP      1800u              /* samples waiting for the input state */
#define VSD_FAST_WAIT       120u               /* frames to wait for the drive to slow down */

/* AC SETUP (menus-screens.md §2f), read from the menu code 2026-11-19. */
#define VSD_SETUP_ENTRY     0x800B3584u        /* (a0 = p, a1 = task): staging -> garage */
#define VSD_SIG_SETUP_ENTRY 0x27BDFFE0u
#define VSD_PART_LISTS      0x80090FD8u        /* owned-part bitmasks (boot-loads.md §4d) */
#define VSD_STAND_INS       0x80067244u        /* (1): models for empty slots */
#define VSD_SLOT_RULES      0x80083130u        /* fill empty slots, fit rules */
#define VSD_OWNED_STAGE     0x8004A7E0u        /* + 256 per player */
#define VSD_OWNED_COPY      0x8003EB20u        /* the garage pages' copy */
#define VSD_GARAGE_OWNED    0x80036FECu
#define VSD_GARAGE_NAMES    0x800371ECu
#define VSD_FILES_STRIDE    120u               /* 0x8004AB60 + 120p (the menu code's 15p * 8) */
#define VSD_SIZES_STRIDE    60u                /* 0x8004A9E0 + 60p */

enum { NP_PHASE_WAIT = 0, NP_PHASE_LOAD };
/* NP_PHASE_LOAD steps: player p's AC = 2p (set up, queue the loads) and
 * 2p + 1 (wait for the disc, then commit); 4 = done. */
enum { NP_STEP_DONE = 4 };

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

/* ---- host side: log lines (presentation only) ---- */

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

/* Each step also goes to start-log.txt beside the room's result file
 * (saves/netplay), new for each session, so a test can be read back. */
static int vsd_log_new = 1;
static void vsd_log(const char *text)
{
    fprintf(stdout, "moa netplay start: %s\n", text);
    fflush(stdout);
    const char *res = getenv("PSX_MOA_ROOM_RESULT");
    if (!res || !res[0]) return;
    char path[1024];
    snprintf(path, sizeof path, "%s", res);
    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) slash[1] = '\0'; else path[0] = '\0';
    strncat(path, "start-log.txt", sizeof path - strlen(path) - 1);
    FILE *f = fopen(path, vsd_log_new ? "w" : "a");
    if (!f) return;
    vsd_log_new = 0;
    fprintf(f, "%llu ms  %s\n", (unsigned long long)vsd_now_ms(), text);
    fclose(f);
}

/* The fight is running: one log line per session (what the start looked
 * like from the fight's side). */
static int vsd_np_fight_logged;
void moa_vs_direct_np_fight_frame(void)
{
    if (vsd_np_fight_logged) return;
    vsd_np_fight_logged = 1;
    char line[96];
    snprintf(line, sizeof line, "the fight is running (drive speed divisor %u)",
             (unsigned)psx_game_disc_speed());
    vsd_log(line);
}

/* ---- the simulation side ---- */

static void vsd_np_end(const char *why)
{
    for (unsigned i = 0; i < 8; i++) psx_mod_write_byte(VSD_NP_STATE + i, 0);
    psx_mod_counter_add(why, 1);
    vsd_log(why);
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

static int vsd_np_menu_in_memory(void)
{
    return psx_mod_read_word(VSD_MENU_VERSUS) == VSD_SIG_MENU_VERSUS &&
           psx_mod_read_word(VSD_FADE) == VSD_SIG_FADE &&
           psx_mod_read_word(VSD_MENU_VS_START) == VSD_SIG_VS_START;
}

static int vsd_np_loader_in_memory(void)
{
    return psx_mod_read_word(VSD_SETUP_ENTRY) == VSD_SIG_SETUP_ENTRY &&
           psx_mod_read_word(VSD_MENU_IDLE) == VSD_SIG_IDLE &&
           psx_mod_read_word(VSD_CD_BUSY) == VSD_SIG_CD_BUSY &&
           psx_mod_read_word(VSD_REBUILD) == VSD_SIG_REBUILD;
}

/* The versus menu's task whose state routine is `state`, or 0. The frame
 * loop (0x800A722C) runs the live tasks of 0x801B1CE8 ... 0x801D0AF8, 0xF0
 * bytes each (+0 live, +0x78 the state routine, called with a0 = the task). */
static uint32_t vsd_np_task_in(uint32_t state)
{
    for (uint32_t t = VSD_TASKS_FIRST; t + VSD_TASK_SIZE <= VSD_TASKS_END; t += VSD_TASK_SIZE)
        if (psx_mod_read_byte(t) && psx_mod_read_word(t + VSD_TASK_STATE) == state)
            return t;
    return 0;
}

static int vsd_np_seen_task;                   /* log only */
static int vsd_np_seen_sampler;                /* log only */
static void vsd_np_step(struct CPUState *cpu, struct vsd_np n);

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
    vsd_log_new = 1;
    vsd_np_seen_task = 0;
    vsd_np_seen_sampler = 0;
    vsd_np_fight_logged = 0;
    if (!have[0] && !have[1]) {
        psx_mod_counter_add("moa.vsd.net_no_saves", 1);
        vsd_log("no save on either match card: the title opens");
        return 0;                                  /* the title, as before */
    }
    const uint32_t ra = cpu->gpr[31];
    (void)psx_mod_call_guest(cpu, VSD_SCREEN_RESET, ra, 0, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, VSD_GARAGE_DEFAULT, ra, 0, 0, 0, 0);
    for (unsigned i = 0; i < 8; i++) psx_mod_write_byte(VSD_PLUS_FLAGS + i, 0);
    (void)psx_mod_call_guest(cpu, VSD_FADE, ra, 4, 0, 0, 0);
    psx_mod_write_byte(VSD_MODE_BYTE, VSD_MODE_LINK);
    psx_mod_write_word(VSD_MENU_FLAGS, psx_mod_read_word(VSD_MENU_FLAGS) | 0x200u);
    (void)psx_mod_call_guest(cpu, VSD_MENU_VERSUS, ra, 0, 0, 0, 0);
    const struct vsd_np n = { NP_PHASE_WAIT, 0, 0, 0 };
    vsd_np_put(&n);
    psx_mod_counter_add(have[0] && have[1] ? "moa.vsd.net_boot" : "moa.vsd.net_boot_one_card", 1);
    char line[128];
    snprintf(line, sizeof line, "versus menu opening (card 1 %s, card 2 %s, drive speed divisor %u)",
             have[0] ? "has an AC" : "EMPTY", have[1] ? "has an AC" : "EMPTY",
             (unsigned)psx_game_disc_speed());
    vsd_log(line);
    return 1;                                      /* the title is not opened */
}

/* Test aid: about once a second while the start runs, what it sees (log
 * only): its bytes, the disc, and every live task's state routine that lies
 * in the versus menu's code. */
static void vsd_np_heartbeat(void)
{
    static uint32_t calls;
    if ((++calls % 60u) != 1u) return;
    char line[512];
    int len = snprintf(line, sizeof line,
                       "sample #%u: phase %u step %u t %u frames %u; menu %s; drive divisor %u; tasks:",
                       (unsigned)calls, psx_mod_read_byte(VSD_NP_STATE + 2u),
                       psx_mod_read_byte(VSD_NP_STATE + 3u), psx_mod_read_half(VSD_NP_STATE + 4u),
                       psx_mod_read_half(VSD_NP_STATE + 6u),
                       vsd_np_menu_in_memory() ? "in memory" : "NOT in memory",
                       (unsigned)psx_game_disc_speed());
    for (uint32_t t = VSD_TASKS_FIRST; t + VSD_TASK_SIZE <= VSD_TASKS_END && len < (int)sizeof line - 24;
         t += VSD_TASK_SIZE) {
        if (!psx_mod_read_byte(t)) continue;
        const uint32_t st = psx_mod_read_word(t + VSD_TASK_STATE);
        if (st >= 0x800B0CB4u && st < 0x800B6A80u)
            len += snprintf(line + len, sizeof line - (size_t)len, " %08X", (unsigned)st);
    }
    vsd_log(line);
}

/* The menu's controller sampling, once a frame: while the start runs, both
 * pads read idle (nothing on the menu moves) and the start takes one step. */
static void vsd_np_input_entry(struct CPUState *cpu, uint32_t address)
{
    struct vsd_np n;
    if (!vsd_np_get(&n)) return;                   /* not starting: the players' menu */
    const uint32_t word = psx_mod_read_word(address);
    if (word != VSD_SIG_INPUT_A && word != VSD_SIG_INPUT_B) return;
    if (!vsd_np_menu_in_memory()) return;          /* an engine: the link engine clears it */
    if (!vsd_np_seen_sampler) {
        vsd_np_seen_sampler = 1;
        vsd_log("the menu's pad sampler is running");
    }
    vsd_np_heartbeat();
    if (n.phase == NP_PHASE_WAIT && n.t > VSD_NP_GIVE_UP) {
        vsd_np_end("gave up: the versus menu never reached its input state; the players have the menu");
        return;
    }
    if (n.t < 0xFFFFu) n.t++;
    vsd_np_put(&n);
    vsd_np_step(cpu, n);
    if (!vsd_np_get(&n)) return;                   /* done this frame: the pads are the players' */
    psx_mod_write_byte(VSD_PAD1, 0);
    psx_mod_write_byte(VSD_PAD1 + 1u, 0x41);
    psx_mod_write_half(VSD_PAD1 + 2u, 0xFFFFu);
    psx_mod_write_byte(VSD_PAD2, 0);
    psx_mod_write_byte(VSD_PAD2 + 1u, 0x41);
    psx_mod_write_half(VSD_PAD2 + 2u, 0xFFFFu);
}

static void vsd_copy(uint32_t dst, uint32_t src, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) psx_mod_write_byte(dst + i, psx_mod_read_byte(src + i));
}

/* AC SETUP's entry for player p (0x800B2748 without the key / pad swaps),
 * then DATA LOAD's finish with the card's AC: queues every load. */
static void vsd_np_setup_player(struct CPUState *cpu, uint32_t task, unsigned p,
                                const struct moa_vs_unit *u)
{
    const uint32_t ra = cpu->gpr[31];
    moa_vs_direct_set_unit(p, u);                                  /* the card's AC -> p's staging */
    vsd_copy(VSD_OWNED_COPY, VSD_OWNED_STAGE + 256u * p, 256);     /* owned parts */
    vsd_copy(VSD_GARAGE_NAMES, VSD_VS_NAMES + 36u * p, 36);        /* AC and pilot names */
    vsd_copy(VSD_PLUS_FLAGS, VSD_VS_PLUS + 8u * p, 8);             /* Human PLUS and flags */
    (void)psx_mod_call_guest(cpu, VSD_SETUP_ENTRY, ra, p, task, 0, 0);
    (void)psx_mod_call_guest(cpu, VSD_PART_LISTS, ra, 0, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, VSD_STAND_INS, ra, 1, 0, 0, 0);
    (void)psx_mod_call_guest(cpu, VSD_SLOT_RULES, ra, 0, 0, 0, 0);
    if (u->has_emblem) vsd_upload_emblem_data(cpu, u->clut, u->emblem);
    (void)psx_mod_call_guest(cpu, VSD_REBUILD, ra, 0, 0, 0, 0);
}

/* AC SETUP's exit copy for player p (state 0x800B4408): the garage and the
 * working part files become p's. */
static void vsd_np_commit_player(unsigned p)
{
    vsd_copy(VSD_OWNED_STAGE + 256u * p, VSD_GARAGE_OWNED, 256);
    vsd_copy(VSD_VS_PAINT + 48u * p, VSD_GARAGE_PAINT, 48);
    vsd_copy(VSD_VS_PARTS + 12u * p, VSD_GARAGE_PARTS, 12);
    psx_mod_write_half(VSD_VS_OPTIONS + 2u * p, psx_mod_read_half(VSD_GARAGE_OPTIONS));
    for (uint32_t i = VSD_FILE_FIRST; i < VSD_FILE_END; i++) {
        psx_mod_write_word(VSD_PLAYER_FILES + VSD_FILES_STRIDE * p + 4u * i,
                           psx_mod_read_word(VSD_WORK_FILES + 4u * i));
        psx_mod_write_half(VSD_PLAYER_SIZES + VSD_SIZES_STRIDE * p + 2u * i,
                           (uint16_t)psx_mod_read_word(VSD_WORK_SIZES + 4u * i));
    }
    psx_mod_write_byte(VSD_VS_PATTERN + p, psx_mod_read_byte(VSD_GARAGE_PATTERN));
    psx_mod_write_byte(VSD_VS_VARIANT + p, psx_mod_read_byte(VSD_GARAGE_PATTERN + 1u));
    vsd_copy(VSD_VS_NAMES + 36u * p, VSD_GARAGE_NAMES, 36);
}

/* One step of the start per frame (from the menu's pad sampler, seen to run
 * every frame). The progress is guest memory (`n`, written back here). */
static void vsd_np_step(struct CPUState *cpu, struct vsd_np n)
{
    if (n.phase == NP_PHASE_WAIT) {
        const uint32_t task = vsd_np_task_in(VSD_MENU_VS_INPUT);
        if (!task) return;                         /* the menu is still opening */
        if (!vsd_np_seen_task) {
            vsd_np_seen_task = 1;
            vsd_log("the versus menu reached its input state");
        }
        /* LOADING = FAST covers the boot and the menu's opening; the ACs and
         * the fight load at the drive's real speed. A data read still going:
         * wait, at most VSD_FAST_WAIT frames. */
        if (!moa_np_room_fast_load_end() && n.frames < VSD_FAST_WAIT) {
            n.frames++;
            vsd_np_put(&n);
            return;
        }
        if (!vsd_np_loader_in_memory()) {
            vsd_np_end("no AC loader in memory: the players have the menu with the default ACs");
            return;
        }
        psx_mod_write_word(task + VSD_TASK_STATE, VSD_MENU_IDLE);   /* the menu waits */
        n.phase = NP_PHASE_LOAD;
        n.step = 0;
        n.frames = 0;
        vsd_np_put(&n);
        vsd_log("the versus menu is paused: loading the cards' ACs (AC SETUP + DATA LOAD)");
        return;
    }

    const uint32_t task = vsd_np_task_in(VSD_MENU_IDLE);
    if (!task) {                                   /* should not happen: hand the menu back */
        vsd_np_end("lost the versus menu's task while loading: the players have the menu");
        return;
    }
    if (n.step < NP_STEP_DONE && vsd_cd_busy(cpu) && n.frames < VSD_LOAD_FRAMES_MAX) {
        n.frames++;                                /* the disc queue first */
        vsd_np_put(&n);
        return;
    }
    char line[128];
    const unsigned p = n.step / 2u;
    if (n.step < NP_STEP_DONE && (n.step & 1u) == 0u) {
        static struct moa_vs_unit u;
        if (!vsd_np_card_unit(p, &u)) {
            snprintf(line, sizeof line, "player %u: no AC on the match card, the default AC stays", p + 1u);
            vsd_log(line);
            n.step = (uint8_t)(n.step + 2u);
        } else {
            vsd_np_setup_player(cpu, task, p, &u);
            snprintf(line, sizeof line, "player %u: AC in the garage, loads queued", p + 1u);
            vsd_log(line);
            n.step++;
        }
        n.frames = 0;
        vsd_np_put(&n);
        return;
    }
    if (n.step < NP_STEP_DONE) {                   /* odd: p's loads are done */
        vsd_np_commit_player(p);
        snprintf(line, sizeof line, "player %u: AC loaded%s", p + 1u,
                 n.frames >= VSD_LOAD_FRAMES_MAX ? " (the disc never went quiet, going on)" : "");
        vsd_log(line);
        n.step++;
        n.frames = 0;
        vsd_np_put(&n);
        return;
    }
    /* Both ACs in: the room's time limit, then the menu is the players'. */
    moa_np_room_title();
    psx_mod_write_word(task + VSD_TASK_STATE, VSD_MENU_VS_INPUT);
    vsd_np_end("both ACs loaded, time limit set: the players have the versus menu (VS START is theirs)");
}

PSX_MOD_CONSTRUCTOR(moa_vs_direct_register)
{
    (void)psx_game_register_offline_function_filter(VSD_TITLE_OPEN, vsd_title_filter);
    (void)psx_game_register_netplay_function_filter(VSD_TITLE_OPEN, vsd_np_title_filter);
    (void)psx_game_register_netplay_function_entry(VSD_INPUT_A, vsd_np_input_entry);
    (void)psx_game_register_netplay_function_entry(VSD_INPUT_B, vsd_np_input_entry);
}
