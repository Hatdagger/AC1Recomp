/* Master of Arena: the link-cable versus engine as the netplay match.
 *
 * The retail game has two versus engines: split screen (two pads, one
 * console) and LINK (two consoles on a serial cable, each showing its own
 * player full screen). A netplay match is one simulation that every peer runs
 * with the same inputs, so it can run the LINK engine as the cable's "master"
 * and feed the "remote" player from controller port 2 (the guest's seat)
 * instead of from the cable. Nothing is sent over an emulated cable.
 *
 * These are trusted game hooks (psx_game_register_netplay_*): they run only
 * in a netplay session and are independent of the mod plan. Every engine of
 * this game is loaded from disc at the same address, so each hook first
 * checks that the code it expects is the code in memory.
 *
 * All state the hooks keep lives in guest memory (rollback and savestates
 * cover it).
 *
 * Link struct 0x80043C70: +932 receive packet, +944 send packet, 8 bytes:
 * ['R', check, pad lo, pad hi, x, x, flag, 'C'].
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>

#define MOA_MODE_BYTE      0x8003EA2Au  /* engine the exe launches next */
#define MOA_ROLE_MASTER    0x8003EA90u
#define MOA_ROLE_DECIDED   0x8003EA91u
#define MOA_LINK           0x80043C70u
#define MOA_RECV           (MOA_LINK + 932u)
#define MOA_SEND           (MOA_LINK + 944u)
/* send[4..5]: unused by the per-frame packet; holds the remote player's pad
 * of the previous frame (the cable delivered pads one frame late, and the
 * local player is delayed by one frame to match). */
#define MOA_REMOTE_PAD     (MOA_LINK + 948u)
#define MOA_CLOSED_FD      15u

#define MOA_MENU_VERSUS    0x800B0CB4u  /* menu: split-screen versus setup */
#define MOA_LINK_WAIT      0x80051F3Cu  /* link engine: wait for a packet */
#define MOA_LINK_EXCHANGE  0x80055428u  /* link engine: trade AC data */
#define MOA_LINK_ROLES     0x80055578u  /* link engine: master/slave */
#define MOA_LINK_FRAME     0x8007E4C8u  /* link engine: per-frame packet */
#define MOA_RA_FRAME_WAIT  0x8007E4F4u
#define MOA_RA_PAUSE_END   0x8007D780u

static int moa_code_is(uint32_t address, uint32_t w0, uint32_t w1)
{
    return psx_mod_read_word(address) == w0 &&
           psx_mod_read_word(address + 4u) == w1;
}

static int moa_link_engine_loaded(void)
{
    return moa_code_is(MOA_LINK_FRAME, 0x3C028009u, 0x90420F90u) &&
           moa_code_is(MOA_LINK_ROLES, 0x3C028004u, 0x9042EA91u);
}

/* The versus menu fills both player blocks (each player's AC and the stage)
 * exactly as the link engine expects them after its cable exchange. The exe
 * reads the mode byte when the menu returns: 1 launches the link engine. */
static void moa_menu_versus_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    if (!moa_code_is(MOA_MENU_VERSUS, 0x27BDFFC0u, 0x00002021u)) return;
    psx_mod_write_byte(MOA_MODE_BYTE, 1);
    psx_mod_counter_add("moa.link.menu_launch", 1);
}

/* Roles: skip the cable negotiation; this simulation is the master, so the
 * local player is the player-1 block and the remote one the player-2 block. */
static void moa_link_roles_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    if (!moa_link_engine_loaded()) return;
    psx_mod_write_byte(MOA_ROLE_MASTER, 1);
    psx_mod_write_byte(MOA_ROLE_DECIDED, 1);
    /* The engine still calls read() and write() on its two "sio:" handles.
     * The serial driver's write waits for the other console to acknowledge
     * each packet on the DSR line and gives up after 300 ticks, which ends
     * the match. Point both handles at a file slot that is never open: the
     * BIOS then rejects the calls at once and the driver never runs. This is
     * the first link code of a match, before any packet. */
    psx_mod_write_word(MOA_LINK + 924u, MOA_CLOSED_FD);
    psx_mod_write_word(MOA_LINK + 928u, MOA_CLOSED_FD);
    psx_mod_counter_add("moa.link.roles", 1);
}

/* AC data exchange: both blocks are already filled by the menu. */
static int moa_link_exchange_filter(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    if (!moa_link_engine_loaded()) return 0;
    psx_mod_counter_add("moa.link.exchange_skipped", 1);
    return 1;
}

/* Every wait for a packet finds one. For the per-frame packet, also supply
 * what the other console would have sent: matching check bytes and the
 * remote player's pad (controller port 2, one frame late). */
static void moa_link_wait_entry(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!moa_link_engine_loaded()) return;
    psx_mod_write_byte(MOA_RECV + 0u, 82); /* 'R' */
    const uint32_t ra = cpu->gpr[31];
    if (ra == MOA_RA_FRAME_WAIT) {
        /* v0 = port-1 pad | port-2 pad << 16, just read by the caller. */
        const uint16_t pad2_now = (uint16_t)(cpu->gpr[2] >> 16);
        psx_mod_write_byte(MOA_RECV + 1u, psx_mod_read_byte(MOA_SEND + 1u));
        psx_mod_write_byte(MOA_RECV + 6u, psx_mod_read_byte(MOA_SEND + 6u));
        psx_mod_write_half(MOA_RECV + 2u, psx_mod_read_half(MOA_REMOTE_PAD));
        psx_mod_write_half(MOA_REMOTE_PAD, pad2_now);
    } else if (ra == MOA_RA_PAUSE_END) {
        /* Leaving pause: the engine strips Start from both saved pads so the
         * press that closed the menu does not reopen it. */
        psx_mod_write_half(MOA_REMOTE_PAD,
            (uint16_t)(psx_mod_read_half(MOA_REMOTE_PAD) & ~0x0008u));
    }
}

PSX_MOD_CONSTRUCTOR(moa_link_netplay_register)
{
    (void)psx_game_register_netplay_function_entry(
        MOA_MENU_VERSUS, moa_menu_versus_entry);
    (void)psx_game_register_netplay_function_entry(
        MOA_LINK_ROLES, moa_link_roles_entry);
    (void)psx_game_register_netplay_function_filter(
        MOA_LINK_EXCHANGE, moa_link_exchange_filter);
    (void)psx_game_register_netplay_function_entry(
        MOA_LINK_WAIT, moa_link_wait_entry);
}
