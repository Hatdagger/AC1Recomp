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
 */
#include "mod_plugins.h"
#include "psx_netplay.h"

#include <stdint.h>

/* Sectors delivered per frame (the drive's real 2x rate is 2.5). */
#define MOA_SECTORS_PER_FRAME 16u

static void moa_fast_loading_activate(void) {
    if (psx_netplay_active()) {
        psx_mod_counter_add("moa.fast-loading.netplay_skipped", 1);
        return;
    }
    if (psx_mod_set_disc_speed(0u, MOA_SECTORS_PER_FRAME))
        psx_mod_counter_add("moa.fast-loading.enabled", 1);
    else
        psx_mod_counter_add("moa.fast-loading.rejected", 1);
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_instant_loading_plugin) {
    (void)psx_mod_register_activation_plugin(
        "moa.instant-loading", moa_fast_loading_activate);
}
