/*
 * Armored Core: Master of Arena - fast loading.
 *
 * Game data reads use the emulated drive's bounded "instant" mode: data
 * sectors arrive up to MOA_SECTORS_PER_FRAME per video frame instead of the
 * drive's real 2x rate (2.5 per frame), so loads take a small fraction of
 * their original time. XA music and voice, and FMV, keep their real timing
 * (cdrom.c never speeds those up).
 *
 * Why not complete the reads with zero guest time: the first version did
 * (it answered the game's read_sectors routine at 0x800167A0 itself), and the
 * HUD and stage textures came out garbled. The game does texture work in the
 * frames between sector reads, so guest time must keep running while a load
 * is in progress. The drive's instant mode gives every load at least one
 * frame per MOA_SECTORS_PER_FRAME sectors, which keeps that work in order.
 *
 * Not applied during netplay: both sides must see the same drive timing, so
 * online matches keep the original speed.
 *
 * Boot only (2026-11-18): the fast drive covers the initial load and is then
 * put back to the drive's real speed for the rest of the session, so later
 * loads (hub screens, fights) keep their original timing and no new timing
 * issues can come from it. "Initial load done" = the first screen the player
 * can use after boot (the hub for LOAD GAME / NEW GAME / Rogue Arena, the
 * title for LAUNCH GAME) has opened and the game's disc queue has then stayed
 * empty for MOA_BOOT_IDLE_FRAMES frames in a row.
 */
#include "mod_plugins.h"
#include "psx_netplay.h"

#include <stdint.h>

/* Sectors delivered per frame (the drive's real 2x rate is 2.5). */
#define MOA_SECTORS_PER_FRAME 16u

/* Menu routines that open the first usable screen (entries listed in
 * game.toml mod_function_entry_funcs), with their first word so a hook only
 * counts while the menu module is in memory (every engine loads there). */
#define MOA_HUB_OPEN       0x800552C0u
#define MOA_SIG_HUB_OPEN   0x27BDFFB8u    /* addiu sp,sp,-72 */
#define MOA_TITLE_OPEN     0x800AAE94u
#define MOA_SIG_TITLE_OPEN 0x27BDFFE0u    /* addiu sp,sp,-32 */
/* The menu's disc queue (exe): write and read positions, equal = empty. */
#define MOA_CDQ_WRITE      0x80043FE8u
#define MOA_CDQ_READ       0x80043FECu
/* Frames the queue must stay empty after that screen opens (half a second):
 * the hub loads in several steps with short gaps between them. */
#define MOA_BOOT_IDLE_FRAMES 30u

#define MOA_PLUGIN_ID "moa.instant-loading"

static int moa_fast_on;           /* the fast drive is set this session */
static int moa_boot_screen;       /* the first usable screen has opened */
static uint32_t moa_idle_frames;

static void moa_fast_loading_activate(void) {
    moa_fast_on = 0;
    moa_boot_screen = 0;
    moa_idle_frames = 0;
    if (psx_netplay_active()) {
        psx_mod_counter_add("moa.fast-loading.netplay_skipped", 1);
        return;
    }
    if (psx_mod_set_disc_speed(0u, MOA_SECTORS_PER_FRAME)) {
        moa_fast_on = 1;
        psx_mod_counter_add("moa.fast-loading.enabled", 1);
    } else {
        psx_mod_counter_add("moa.fast-loading.rejected", 1);
    }
}

static void moa_boot_screen_entry(struct CPUState *cpu, uint32_t address) {
    (void)cpu;
    if (!moa_fast_on || moa_boot_screen) return;
    const uint32_t sig = address == MOA_HUB_OPEN ? MOA_SIG_HUB_OPEN : MOA_SIG_TITLE_OPEN;
    if (psx_mod_read_word(address) != sig) return;
    moa_boot_screen = 1;
    moa_idle_frames = 0;
    psx_mod_counter_add("moa.fast-loading.boot_screen", 1);
}

static void moa_fast_loading_vblank(void) {
    if (!moa_fast_on || !moa_boot_screen) return;
    if (psx_mod_read_word(MOA_CDQ_WRITE) != psx_mod_read_word(MOA_CDQ_READ)) {
        moa_idle_frames = 0;
        return;
    }
    if (++moa_idle_frames < MOA_BOOT_IDLE_FRAMES) return;
    /* Divisor 1 = the drive's authentic timing, applied to the running drive.
     * (psx_mod_set_disc_speed only takes effect when a session starts, so it
     * left the drive fast for the whole session.) The live call is refused
     * while a read is in progress: it is tried again on the next frame. */
    if (psx_mod_set_disc_speed_live(1u, 0u)) {
        moa_fast_on = 0;
        psx_mod_counter_add("moa.fast-loading.boot_done", 1);
    }
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_instant_loading_plugin) {
    (void)psx_mod_register_activation_plugin(MOA_PLUGIN_ID, moa_fast_loading_activate);
    (void)psx_mod_register_vblank_plugin(MOA_PLUGIN_ID, moa_fast_loading_vblank);
    (void)psx_mod_register_function_entry_plugin(MOA_PLUGIN_ID, MOA_HUB_OPEN,
                                                 moa_boot_screen_entry);
    (void)psx_mod_register_function_entry_plugin(MOA_PLUGIN_ID, MOA_TITLE_OPEN,
                                                 moa_boot_screen_entry);
}
