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
#include "psx_netplay.h"
#include "spu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MOA_MODE_BYTE      0x8003EA2Au  /* engine the exe launches next */
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
#define MOA_RA_ROLE_WAIT   0x8005567Cu

/* Host state of the guest's own view (see below); presentation only. */
static int moa_camera_valid;

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

/* First link code of a match. The role negotiation itself still runs (its
 * "already decided" flag also means "common textures already loaded", so it
 * must stay clear on the first match); moa_link_wait_entry answers it. */
static void moa_link_roles_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    if (!moa_link_engine_loaded()) return;
    /* The engine still calls read() and write() on its two "sio:" handles.
     * The serial driver's write waits for the other console to acknowledge
     * each packet on the DSR line and gives up after 300 ticks, which ends
     * the match. Point both handles at a file slot that is never open: the
     * BIOS then rejects the calls at once and the driver never runs. This is
     * the first link code of a match, before any packet. */
    moa_camera_valid = 0; /* new match: the guest's camera starts over */
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
        /* Each peer hears its own player's channel (see "Sound for two
         * listeners"); renewed every frame, lapses after the match. */
        spu_output_mono_request(psx_netplay_local_slot() == 1 ? 1 : 0, 30);
    } else if (ra == MOA_RA_ROLE_WAIT) {
        /* Role negotiation: we offered 'M'aster, the other side 'S'lave. The
         * local player is then the player-1 block, the remote the player-2
         * block, as the versus menu filled them. */
        psx_mod_write_byte(MOA_SEND + 1u, 77);
        psx_mod_write_byte(MOA_RECV + 1u, 83);
    } else if (ra == MOA_RA_PAUSE_END) {
        /* Leaving pause: the engine strips Start from both saved pads so the
         * press that closed the menu does not reopen it. */
        psx_mod_write_half(MOA_REMOTE_PAD,
            (uint16_t)(psx_mod_read_half(MOA_REMOTE_PAD) & ~0x0008u));
    }
}

/* ---- The guest's own view ----------------------------------------------
 *
 * The simulation draws one view: the "local" player's, which is player 1
 * (the host). The guest (session slot 1) redraws each frame from player 2's
 * side inside the framework's sandbox (psx_mod_render_local_view): the
 * sandbox restores the whole machine afterwards, so the simulation never
 * sees it; only this peer's presenter keeps the image.
 *
 * Where: the main loop's call of the scene render (0x80067FE4 from
 * 0x80052914), before it runs. The previous frame is complete in the draw
 * area the render is about to leave (buffer index `idx`), and this frame's
 * end flips the display to it.
 *
 * What: swap the local and remote player pointers and the viewed player, run
 * the camera routine and the scene render again for that same draw area.
 * The camera keeps smoothing state between frames in one global block; the
 * guest's copy of it is host state on this peer (presentation only). */
#define MOA_VIEWED_PLAYER  0x8003EAA0u
#define MOA_LOCAL_PLAYER   0x8003EAA4u
#define MOA_REMOTE_PLAYER  0x8003EAA8u
#define MOA_GFX            0x8019D4F0u  /* +0 buffer index, +4 two 128-byte
                                         * buffer records (+4 DRAWENV) */
#define MOA_GFX_FRAME_TIME (MOA_GFX + 25928u)
#define MOA_LINK_SOUND_BEGIN 0x80053FE4u /* frame begin: reverb/voice upkeep */
#define MOA_LINK_SOUND_END  0x80053808u  /* frame end: queued sounds */
#define MOA_LINK_SOUND_PLAY 0x80053498u  /* start a sound effect */
#define MOA_LINK_CAMERA    0x80088988u
#define MOA_LINK_RENDER    0x80067FE4u
#define MOA_LINK_CAMERA_SET 0x80053280u
#define MOA_ELAPSED        0x80016724u  /* exe: hblank ticks since a0 */
#define MOA_RA_RENDER      0x8005291Cu
#define MOA_RA_NESTED      0x80052924u

static const struct { uint32_t address, bytes; } moa_camera_state[] = {
    /* Everything the camera routine and its helpers write in the block
     * 0x801D1CD8..0x801D1D27; the mode word (0x801D1CDC) and 0x801D1CF0 are
     * set by the simulation and stay as they are. */
    {0x801D1CD8u, 4u}, {0x801D1CE0u, 16u}, {0x801D1CF2u, 54u},
};
static uint8_t moa_camera[96], moa_camera_next[96];
static int moa_in_view;

static void moa_camera_copy(uint8_t *buffer, int to_guest)
{
    unsigned n = 0;
    for (unsigned r = 0; r < sizeof moa_camera_state / sizeof moa_camera_state[0]; r++)
        for (uint32_t i = 0; i < moa_camera_state[r].bytes; i++, n++) {
            const uint32_t a = moa_camera_state[r].address + i;
            if (to_guest) psx_mod_write_byte(a, buffer[n]);
            else buffer[n] = psx_mod_read_byte(a);
        }
}

typedef struct MoaCall {
    struct CPUState *cpu;
    uint32_t sp;
    int broken;
} MoaCall;

static uint32_t moa_call(MoaCall *c, uint32_t fn, uint32_t a0)
{
    struct CPUState *cpu = c->cpu;
    if (c->broken) return 0;
    cpu->gpr[4] = a0;
    cpu->gpr[5] = cpu->gpr[6] = cpu->gpr[7] = 0;
    cpu->gpr[29] = c->sp;
    cpu->gpr[31] = MOA_RA_NESTED;
    psx_dispatch_call(cpu, fn, MOA_RA_NESTED);
    if (cpu->gpr[29] != c->sp) c->broken = 1; /* left its frame: discard */
    return cpu->gpr[2];
}

static int moa_draw_guest_view(struct CPUState *cpu, void *user,
                               uint32_t alpha_q16)
{
    (void)user; (void)alpha_q16;
    MoaCall call = {cpu, (cpu->gpr[29] - 0x200u) & ~7u, 0};
    const uint32_t camera = call.sp + 0x100u; /* scratch above the callees */
    const uint32_t local = psx_mod_read_word(MOA_LOCAL_PLAYER);
    const uint32_t remote = psx_mod_read_word(MOA_REMOTE_PLAYER);
    const uint32_t viewed = psx_mod_read_word(MOA_VIEWED_PLAYER);
    psx_mod_write_word(MOA_LOCAL_PLAYER, remote);
    psx_mod_write_word(MOA_REMOTE_PLAYER, local);
    psx_mod_write_word(MOA_VIEWED_PLAYER, viewed == local ? remote : local);
    if (moa_camera_valid) moa_camera_copy(moa_camera, 1);
    (void)moa_call(&call, MOA_LINK_CAMERA, camera);
    moa_camera_copy(moa_camera_next, 0);
    /* The main loop records each frame's camera (position and heading) after
     * drawing it; the HUD compass reads that record. Record this view's. */
    (void)moa_call(&call, MOA_LINK_CAMERA_SET, camera);
    /* The render's frame end waits out the frame time; make the frame look
     * old enough. Its two sound routines are skipped (moa_link_sound_filter). */
    const uint32_t now = moa_call(&call, MOA_ELAPSED, 0);
    psx_mod_write_word(MOA_GFX_FRAME_TIME, (now - 800u) & 0xFFFFu);
    /* The render starts by switching to the other buffer: start from the
     * other one so it draws the area the next flip shows. */
    psx_mod_write_word(MOA_GFX, psx_mod_read_word(MOA_GFX) ^ 1u);
    (void)moa_call(&call, MOA_LINK_RENDER, camera);
    return !call.broken;
}

/* The scene render's sound upkeep reads the sound chip, which the sandbox
 * does not allow (and the real frame already did it). Skip it while the
 * guest's view is drawn; it runs untouched in the simulation. */
static int moa_link_sound_filter(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!moa_in_view) return 0;
    cpu->gpr[2] = 0;
    return 1;
}

/* ---- Sound for two listeners --------------------------------------------
 *
 * The link engine mixes for one listener: the console's own player. Every
 * sound is one voice with a left and a right volume, set from the distance
 * and direction to the camera. Sound is part of the shared simulation, so
 * both peers must mix the same thing. In a netplay match the two volumes
 * are used differently: LEFT is how loud the sound is for player 1, RIGHT
 * how loud it is for player 2 (no direction). Each peer then plays its own
 * channel on both speakers (spu_output_mono_request).
 *
 * - Positional sounds (0x80052C80 with pitch shift, 0x80052EE4): the routine
 *   is run twice as a probe, once per listener, with the final "start the
 *   sound" call captured instead of executed; the real call then gets the
 *   two loudness values. Listener 1 is the camera record as always, listener
 *   2 the position of player 2's AC.
 * - Cues the engine plays only for the console's own player (0x800532FC,
 *   which tests "the player being updated is the local one"): player 1's on
 *   the left only, player 2's on the right only.
 * - Everything else keeps equal volumes: both players hear it. */
#define MOA_CAMERA_RECORD   0x800441D8u /* x, y, z halves; listener */
#define MOA_SOUND_ONCE_BITS (MOA_CAMERA_RECORD + 4000u) /* per-frame id mask */
#define MOA_SOUND_AT_PITCH  0x80052C80u
#define MOA_SOUND_AT        0x80052EE4u
#define MOA_SOUND_OWN_CUE   0x800532FCu

static int moa_sound_probe;
static struct {
    int hit;
    uint32_t id, handle, left, right, pitch;
} moa_sound_captured;

static int moa_link_sound_play_filter(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (moa_in_view) {
        cpu->gpr[2] = 0;
        return 1;
    }
    if (!moa_sound_probe) return 0;
    moa_sound_captured.hit = 1;
    moa_sound_captured.id = cpu->gpr[4];
    moa_sound_captured.handle = cpu->gpr[5];
    moa_sound_captured.left = cpu->gpr[6];
    moa_sound_captured.right = cpu->gpr[7];
    moa_sound_captured.pitch = psx_mod_read_word(cpu->gpr[29] + 16u);
    return 1;
}

static void moa_gpr_copy(uint32_t *to, const uint32_t *from)
{
    for (unsigned i = 1; i < 32; i++) to[i] = from[i];
}

/* The loudness of a panned pair: left = v*sin, right = v*cos. */
static uint32_t moa_sound_loudness(uint32_t left, uint32_t right)
{
    const uint32_t sum = left * left + right * right;
    uint32_t v = 0;
    while ((v + 1u) * (v + 1u) <= sum) v++;
    /* A centered sound has left = right = 0.707 v; keep that level. */
    v = (v * 181u) >> 8;
    return v > 127u ? 127u : v;
}

static void moa_sound_start(struct CPUState *cpu, const uint32_t *saved,
                            uint32_t id, uint32_t handle, uint32_t left,
                            uint32_t right, uint32_t pitch)
{
    const uint32_t sp = (saved[29] - 0x40u) & ~7u;
    moa_gpr_copy(cpu->gpr, saved);
    cpu->gpr[4] = id;
    cpu->gpr[5] = handle;
    cpu->gpr[6] = left;
    cpu->gpr[7] = right;
    cpu->gpr[29] = sp;
    cpu->gpr[31] = MOA_RA_NESTED;
    psx_mod_write_word(sp + 16u, pitch);
    psx_dispatch_call(cpu, MOA_LINK_SOUND_PLAY, MOA_RA_NESTED);
}

static int moa_link_positional_filter(struct CPUState *cpu, uint32_t address)
{
    if (moa_in_view || moa_sound_probe || !moa_link_engine_loaded()) return 0;
    uint32_t saved[32];
    moa_gpr_copy(saved, cpu->gpr);
    saved[0] = 0;
    uint32_t loud[2] = {0, 0};
    uint32_t id = 0, handle = 0, pitch = 0;
    int any = 0;
    const uint32_t player2 = psx_mod_read_word(MOA_REMOTE_PLAYER);
    const uint32_t ac2 = player2 ? psx_mod_read_word(player2 + 920u) : 0;
    for (int listener = 0; listener < 2; listener++) {
        uint16_t record[3] = {0, 0, 0};
        if (listener == 1) {
            if (!ac2) break;
            for (unsigned i = 0; i < 3; i++) {
                record[i] = psx_mod_read_half(MOA_CAMERA_RECORD + 2u * i);
                psx_mod_write_half(MOA_CAMERA_RECORD + 2u * i,
                                   psx_mod_read_half(ac2 + 8u + 2u * i));
            }
        }
        moa_sound_captured.hit = 0;
        moa_sound_probe = 1;
        moa_gpr_copy(cpu->gpr, saved);
        cpu->gpr[31] = MOA_RA_NESTED;
        psx_dispatch_call(cpu, address, MOA_RA_NESTED);
        moa_sound_probe = 0;
        if (listener == 1)
            for (unsigned i = 0; i < 3; i++)
                psx_mod_write_half(MOA_CAMERA_RECORD + 2u * i, record[i]);
        if (moa_sound_captured.hit) {
            loud[listener] = moa_sound_loudness(moa_sound_captured.left,
                                                moa_sound_captured.right);
            if (!any) {
                id = moa_sound_captured.id;
                handle = moa_sound_captured.handle;
                pitch = moa_sound_captured.pitch;
            }
            any = 1;
        }
    }
    if (any) moa_sound_start(cpu, saved, id, handle, loud[0], loud[1], pitch);
    moa_gpr_copy(cpu->gpr, saved);
    cpu->gpr[2] = any ? 1u : 0u;
    return 1;
}

static int moa_link_own_cue_filter(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (moa_in_view) return 1;
    if (moa_sound_probe || !moa_link_engine_loaded()) return 0;
    const uint32_t id = cpu->gpr[4], volume = cpu->gpr[5];
    const uint32_t player = psx_mod_read_word(MOA_VIEWED_PLAYER);
    const int first = player == psx_mod_read_word(MOA_LOCAL_PLAYER);
    const int second = player == psx_mod_read_word(MOA_REMOTE_PLAYER);
    if (!first && !second) return 1;
    uint32_t saved[32];
    moa_gpr_copy(saved, cpu->gpr);
    saved[0] = 0;
    /* A sound id starts once per frame. The two players' cues are separate
     * sounds here, so let this one start even if the other player's did. */
    uint32_t bits = 0, bit = 0, old = 0;
    if (id < 256u) {
        bits = MOA_SOUND_ONCE_BITS + 4u * (id >> 5);
        bit = 1u << (id & 31u);
        old = psx_mod_read_word(bits);
        psx_mod_write_word(bits, old & ~bit);
    }
    moa_sound_start(cpu, saved, id, 0, first ? volume : 0, second ? volume : 0, 0);
    if (bits) psx_mod_write_word(bits, psx_mod_read_word(bits) | (old & bit));
    moa_gpr_copy(cpu->gpr, saved);
    return 1;
}

static void moa_link_render_entry(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (moa_in_view || cpu->gpr[31] != MOA_RA_RENDER) return;
    if (!moa_link_engine_loaded() ||
        !moa_code_is(MOA_LINK_RENDER, 0x27BDFF98u, 0xAFB20060u))
        return;
    /* A few bounded lines on stderr (as the framework does for its own pass
     * faults) so a player's captured output shows why the own view is off. */
    static unsigned said_slot, said_status, said_failed, said_ok;
    if (!said_slot) {
        said_slot = 1;
        fprintf(stderr, "moa link: scene render hook reached; local slot %d of %d\n",
                psx_netplay_local_slot(), psx_netplay_seat_count());
    }
    if (psx_netplay_local_slot() != 1) return; /* the host's view is the frame */
    if (psx_mod_render_local_view_status() != PSX_MOD_RENDER_PASS_READY) {
        psx_mod_counter_add("moa.link.guest_view_unavailable", 1);
        if (said_status < 4u) {
            said_status++;
            fprintf(stderr, "moa link: own view unavailable, status %u\n",
                    (unsigned)psx_mod_render_local_view_status());
        }
        return;
    }
    const uint32_t index = psx_mod_read_word(MOA_GFX) & 1u;
    const uint32_t drawenv = MOA_GFX + 4u + index * 128u + 4u;
    PSXModRenderPass rect = {0};
    rect.struct_size = sizeof rect;
    rect.x = psx_mod_read_half(drawenv + 0u);
    rect.y = psx_mod_read_half(drawenv + 2u);
    rect.w = psx_mod_read_half(drawenv + 4u);
    rect.h = psx_mod_read_half(drawenv + 6u);
    if (!rect.w || !rect.h || rect.w > 640u || rect.h > 512u) return;
    moa_in_view = 1;
    const int ok = psx_mod_render_local_view(cpu, &rect, moa_draw_guest_view, 0);
    moa_in_view = 0;
    if (ok) {
        for (unsigned i = 0; i < sizeof moa_camera; i++)
            moa_camera[i] = moa_camera_next[i];
        moa_camera_valid = 1;
        psx_mod_counter_add("moa.link.guest_view", 1);
        if (!said_ok) {
            said_ok = 1;
            fprintf(stderr, "moa link: own view drawn, rect %u,%u %ux%u\n",
                    (unsigned)rect.x, (unsigned)rect.y, (unsigned)rect.w,
                    (unsigned)rect.h);
        }
    } else {
        psx_mod_counter_add("moa.link.guest_view_failed", 1);
        if (said_failed < 4u) {
            said_failed++;
            fprintf(stderr, "moa link: own view refused or rolled back, "
                    "status %u, rect %u,%u %ux%u\n",
                    (unsigned)psx_mod_render_local_view_status(),
                    (unsigned)rect.x, (unsigned)rect.y, (unsigned)rect.w,
                    (unsigned)rect.h);
        }
    }
}

PSX_MOD_CONSTRUCTOR(moa_link_netplay_register)
{
    /* The framework drops a peer's own view when the newest remote input is
     * 4 or more ticks further ahead than the input delay, and brings it back
     * only within 1. A healthy LAN match of this game sits 2-3 ahead and
     * touches 4, so the guest lost its view for good. The view is the point
     * of the mode here: keep it unless the player overrides the switch. */
    if (!getenv("PSX_NET_LOCAL_VIEW_SHED")) {
#ifdef _WIN32
        (void)_putenv_s("PSX_NET_LOCAL_VIEW_SHED", "0");
#else
        (void)setenv("PSX_NET_LOCAL_VIEW_SHED", "0", 0);
#endif
    }
    (void)psx_game_register_netplay_function_entry(
        MOA_MENU_VERSUS, moa_menu_versus_entry);
    (void)psx_game_register_netplay_function_entry(
        MOA_LINK_ROLES, moa_link_roles_entry);
    (void)psx_game_register_netplay_function_filter(
        MOA_LINK_EXCHANGE, moa_link_exchange_filter);
    (void)psx_game_register_netplay_function_entry(
        MOA_LINK_WAIT, moa_link_wait_entry);
    (void)psx_game_register_netplay_function_entry(
        MOA_LINK_RENDER, moa_link_render_entry);
    (void)psx_game_register_netplay_function_filter(
        MOA_LINK_SOUND_BEGIN, moa_link_sound_filter);
    (void)psx_game_register_netplay_function_filter(
        MOA_LINK_SOUND_END, moa_link_sound_filter);
    (void)psx_game_register_netplay_function_filter(
        MOA_LINK_SOUND_PLAY, moa_link_sound_play_filter);
    (void)psx_game_register_netplay_function_filter(
        MOA_SOUND_AT_PITCH, moa_link_positional_filter);
    (void)psx_game_register_netplay_function_filter(
        MOA_SOUND_AT, moa_link_positional_filter);
    (void)psx_game_register_netplay_function_filter(
        MOA_SOUND_OWN_CUE, moa_link_own_cue_filter);
}
