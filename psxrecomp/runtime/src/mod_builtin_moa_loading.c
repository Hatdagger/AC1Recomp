/*
 * Armored Core: Master of Arena - instant loading.
 *
 * Every archive the game reads (COM\*.T, MS\*.T, mission and arena data) goes
 * through one boot-EXE routine that reads whole sectors from the disc:
 *
 *   800167A0  read_sectors(CdlLOC *loc, void *dest, int count)
 *               wait for the game's async CD queue to drain   (80015FD0)
 *               retry up to 3 times:
 *                 CdControl(CdlSetloc, loc)                     (80017E00)
 *                 CdRead(count, dest, CdlModeSpeed)             (8001ABB4)
 *                 CdReadSync(1, 0)                              (8001AD50)
 *               return 1 on success
 *
 * Callers: the archive-entry loader (80016A58), the archive header reader
 * (80016A1C) and the direct file loaders (80016868, 80016914, 800169C4), plus
 * any disc-loaded module that calls them.
 *
 * This plugin completes the call itself when the drive is idle: it reads the
 * same sectors from the mounted (effective) disc image and delivers them to
 * RAM through the CD-DMA path (overlay capture and code-page invalidation
 * included), then returns 1 like the original. Guest time does not advance
 * for the read, so a load costs only the game's own unpacking work.
 *
 * It steps aside and lets the original body run whenever anything else is
 * using the drive: queued async CD commands, XA music or voice streaming, an
 * FMV, or a read already in flight. Those keep their real drive timing. The
 * plan's hooks are parked during netplay, so online matches always run the
 * original reads on both sides.
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "cdrom.h"

#include <stdint.h>

#define MOA_READ_FN          0x800167A0u
/* Guard words of the routine (addiu sp; jal CdControl; jal CdRead; jal
 * CdReadSync; li v0,1) so the hook never fires on other code. */
#define MOA_GUARD_ENTRY      0x27BDFFD8u
#define MOA_GUARD_SETLOC_PC  0x800167DCu
#define MOA_GUARD_SETLOC     0x0C005F80u
#define MOA_GUARD_READ_PC    0x800167F4u
#define MOA_GUARD_READ       0x0C006AEDu
#define MOA_GUARD_SYNC_PC    0x80016808u
#define MOA_GUARD_SYNC       0x0C006B54u
#define MOA_GUARD_OK_PC      0x80016834u
#define MOA_GUARD_OK         0x24020001u

/* The game's async CD command queue (drained by 80015FD0 before a read):
 * empty when its read and write pointers match and the slot is clear. */
#define MOA_CDQ_BASE         0x80043C70u
#define MOA_CDQ_WRITE        (MOA_CDQ_BASE + 0x378u)
#define MOA_CDQ_READ         (MOA_CDQ_BASE + 0x37Cu)

#define MOA_MAX_SECTORS      1024u   /* 2 MB: all of main RAM */
#define MOA_RAM_BYTES        0x00200000u

#define REG_V0 2
#define REG_A0 4
#define REG_A1 5
#define REG_A2 6

static uint8_t s_sectors[MOA_MAX_SECTORS * 2048u];

static int bcd_value(uint8_t b, int* out) {
    if ((b & 0x0Fu) > 9u || (b >> 4) > 9u) return 0;
    *out = (int)(b >> 4) * 10 + (int)(b & 0x0Fu);
    return 1;
}

static int moa_guard_ok(void) {
    return psx_mod_read_word(MOA_READ_FN) == MOA_GUARD_ENTRY &&
           psx_mod_read_word(MOA_GUARD_SETLOC_PC) == MOA_GUARD_SETLOC &&
           psx_mod_read_word(MOA_GUARD_READ_PC) == MOA_GUARD_READ &&
           psx_mod_read_word(MOA_GUARD_SYNC_PC) == MOA_GUARD_SYNC &&
           psx_mod_read_word(MOA_GUARD_OK_PC) == MOA_GUARD_OK;
}

static int moa_drive_idle(void) {
    const uint32_t wr = psx_mod_read_word(MOA_CDQ_WRITE);
    const uint32_t rd = psx_mod_read_word(MOA_CDQ_READ);
    if (rd != wr || psx_mod_read_byte(rd) != 0u) {
        psx_mod_counter_add("moa.instant-loading.wait_queue", 1);
        return 0;
    }
    if (cdrom_xa_stream_active() || cdrom_fmv_stream_pending()) {
        psx_mod_counter_add("moa.instant-loading.wait_stream", 1);
        return 0;
    }
    if (cdrom_data_read_active()) {
        psx_mod_counter_add("moa.instant-loading.wait_read", 1);
        return 0;
    }
    return 1;
}

static int moa_instant_read(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!moa_guard_ok()) {
        psx_mod_counter_add("moa.instant-loading.guard_rejected", 1);
        return 0;
    }
    const uint32_t loc = cpu->gpr[REG_A0];
    const uint32_t dest = cpu->gpr[REG_A1];
    const uint32_t count = cpu->gpr[REG_A2];
    if (count == 0u || count > MOA_MAX_SECTORS || (dest & 3u) != 0u ||
        ((dest & 0x1FFFFFFFu) + count * 2048u) > MOA_RAM_BYTES) {
        psx_mod_counter_add("moa.instant-loading.bad_args", 1);
        return 0;
    }
    int mm, ss, ff;
    if (!bcd_value(psx_mod_read_byte(loc), &mm) ||
        !bcd_value(psx_mod_read_byte(loc + 1u), &ss) ||
        !bcd_value(psx_mod_read_byte(loc + 2u), &ff) ||
        ss > 59 || ff > 74) {
        psx_mod_counter_add("moa.instant-loading.bad_loc", 1);
        return 0;
    }
    const int lba = (mm * 60 + ss) * 75 + ff - 150;
    if (lba < 0) {
        psx_mod_counter_add("moa.instant-loading.bad_loc", 1);
        return 0;
    }
    if (!moa_drive_idle()) return 0;
    if (!psx_mod_read_disc_sectors((uint32_t)lba, count, s_sectors)) {
        psx_mod_counter_add("moa.instant-loading.read_failed", 1);
        return 0;
    }
    if (!psx_mod_dma_write_ram(dest, s_sectors, count * 2048u, lba)) {
        psx_mod_counter_add("moa.instant-loading.dma_failed", 1);
        return 0;
    }
    cpu->gpr[REG_V0] = 1u;
    psx_mod_counter_add("moa.instant-loading.reads", 1);
    psx_mod_counter_add("moa.instant-loading.sectors", count);
    return 1;
}

static void moa_instant_loading_activate(void) {
    /* The function-entry table is rebuilt from the registered hooks when the
     * plan activates this plugin id; nothing else to do here. */
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_instant_loading_plugin) {
    (void)psx_mod_register_activation_plugin(
        "moa.instant-loading", moa_instant_loading_activate);
    (void)psx_mod_register_function_filter_plugin(
        "moa.instant-loading", MOA_READ_FN, moa_instant_read);
}
