/*
 * Armored Core: Master of Arena - in-between frames (render passes), test 1:
 * split-screen VS, camera only.
 *
 * The game draws at most ~22 frames a second (each frame ends by waiting
 * until 715 scanlines have passed since the last flip). Between two of its
 * frames this plugin redraws the scene with the game's own draw code, with
 * both players' cameras placed part of the way from one logic step to the
 * next. Everything else is drawn as the newest step left it: ACs, shots and
 * effects are not blended yet.
 *
 * Mapping: docs/mapping/renderer.md, "Frame anatomy (VS)" and "Camera (VS)".
 *
 * Where: the entry of the scene render 0x80067404(camera P1, camera P2),
 * called once per step by the VS main loop (from 0x80052690). The logic of
 * step n+1 is done; frame n is complete in the area the render is about to
 * leave, and this frame's end flips the display to it (PENDING). Hooking the
 * entry, before the real render runs, means the passes start from state the
 * render has not touched yet, so nothing the render itself steps on (effect
 * animation, counters) is stepped twice.
 *
 * Each pass calls the scene render again with the blended cameras, from the
 * other buffer index so its swap picks the area the next flip shows. The
 * frame end inside it waits for the frame time (made to look old enough) and
 * its sound routines are skipped. The framework restores the whole machine
 * after every pass.
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_netplay.h"
#include "psx_video_timing.h"
#include "timers.h"

#include <stdint.h>

#define MOA_FR_PACKAGE "acrecomp.moa.frame-rate"
#define MOA_FR_FEATURE "frame-rate"
#define MOA_FR_PLUGIN  "moa.frame-rate"

/* VS engine (FDAT 209), loaded at 0x80050DD8 like every fight engine. */
#define MOA_ENGINE_ENTRY    0x80050DDCu  /* word: the engine's entry point */
#define MOA_VS_ENTRY        0x80051F4Cu
#define MOA_VS_RENDER       0x80067404u  /* scene render (camera P1, camera P2) */
#define MOA_VS_RA_RENDER    0x80052698u  /* the main loop's per-step call */
#define MOA_VS_FRAME_END    0x8005C050u
#define MOA_VS_SOUND_QUEUE  0x800535F4u  /* queued sounds, in the frame end */
#define MOA_VS_SOUND_BEGIN  0x80053DD0u  /* reverb/voice upkeep at the swap */
#define MOA_VS_SOUND_PLAY   0x80053284u  /* start a sound effect */
#define MOA_VS_GFX_INDEX    0x801A2568u  /* buffer index 0 / 1 (-1 at start) */
#define MOA_VS_GFX_CURRENT  0x801A27E4u  /* current buffer record */
#define MOA_VS_FRAME_START  0x801A8C28u  /* Timer 1 count at the last flip */
#define MOA_VS_TICK         0x80097EB2u  /* match struct +0x12: steps */
#define MOA_VS_TICK_CAP     0x4000u      /* the count stops here (0x800526AC) */
#define MOA_FRAME_LINES     715u         /* the frame-end wait (slti 0x2CB) */

/* A camera is an eye point and a look-at point, 8 halves: x, y, z, unused,
 * twice. The view matrix is built from the two points only (0x80014344, no
 * roll). A coordinate that moves further than this in one step is a cut:
 * that camera is not blended. */
#define MOA_VS_CUT 2048

static int moa_fr_enabled, moa_fr_in_pass;
static int16_t moa_cam_prev[16], moa_cam_now[16];
static int moa_prev_valid;
static uint16_t moa_prev_tick;
static int moa_cut[2];

static int moa_code_is(uint32_t address, uint32_t w0, uint32_t w1)
{
    return psx_mod_read_word(address) == w0 &&
           psx_mod_read_word(address + 4u) == w1;
}

static int moa_vs_loaded(void)
{
    return psx_mod_read_word(MOA_ENGINE_ENTRY) == MOA_VS_ENTRY &&
           moa_code_is(MOA_VS_RENDER, 0x27BDFFA0u, 0xAFBF005Cu) &&
           moa_code_is(MOA_VS_FRAME_END, 0x27BDFFE8u, 0x00002021u) &&
           moa_code_is(MOA_VS_SOUND_QUEUE, 0x27BDFFE0u, 0x3C028004u) &&
           moa_code_is(MOA_VS_SOUND_BEGIN, 0x27BDFFD0u, 0xAFBF0028u) &&
           moa_code_is(MOA_VS_SOUND_PLAY, 0x27BDFFB8u, 0xAFB3002Cu);
}

typedef struct MoaCall {
    struct CPUState *cpu;
    uint32_t sp;
    int broken;
} MoaCall;

static void moa_call(MoaCall *c, uint32_t fn, uint32_t a0, uint32_t a1)
{
    struct CPUState *cpu = c->cpu;
    if (c->broken) return;
    cpu->gpr[4] = a0;
    cpu->gpr[5] = a1;
    cpu->gpr[6] = cpu->gpr[7] = 0;
    cpu->gpr[29] = c->sp;
    cpu->gpr[31] = MOA_VS_RA_RENDER;
    psx_dispatch_call(cpu, fn, MOA_VS_RA_RENDER);
    if (cpu->gpr[29] != c->sp) c->broken = 1; /* left its frame: discard */
}

/* Timer 1 (HBlank clock), read without touching it: the game's own frame
 * clock (GetRCnt(RCntCNT1), 16 bits). `into_line`: cycles already counted
 * toward its next tick. */
static uint16_t moa_timer1(uint32_t *into_line)
{
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    timers_get_snapshot(counter, mode, target, irq_line, frac);
    if (into_line) *into_line = frac[1];
    return counter[1];
}

static int moa_pass(struct CPUState *cpu, void *user, uint32_t alpha_q16)
{
    (void)user;
    MoaCall call = {cpu, (cpu->gpr[29] - 0x200u) & ~7u, 0};
    const uint32_t cameras = call.sp + 0x100u; /* scratch above the callees */
    for (unsigned i = 0; i < 16u; i++) {
        int32_t v = moa_cam_now[i];
        if (!moa_cut[i >> 3] && (i & 3u) != 3u) {
            const int32_t from = moa_cam_prev[i];
            v = from + (int32_t)(((int64_t)(v - from) * (int64_t)alpha_q16) >> 16);
        }
        psx_mod_write_half(cameras + 2u * i, (uint16_t)v);
    }
    /* The render starts by switching buffers: start from the other index so
     * it draws into the area frame n is in (the one the next flip shows). */
    psx_mod_write_word(MOA_VS_GFX_INDEX, psx_mod_read_word(MOA_VS_GFX_INDEX) ^ 1u);
    /* Its frame end waits until 715 scanlines have passed since the last
     * flip; make that flip look old enough. */
    psx_mod_write_word(MOA_VS_FRAME_START, (uint32_t)(uint16_t)(moa_timer1(0) - 800u));
    moa_call(&call, MOA_VS_RENDER, cameras, cameras + 16u);
    return !call.broken;
}

static void moa_count_motion(int32_t moved)
{
    psx_mod_counter_add(moved < 64 ? "moa.frame-rate.camera_move_lt64"
                      : moved < 256 ? "moa.frame-rate.camera_move_lt256"
                      : moved < 1024 ? "moa.frame-rate.camera_move_lt1024"
                      : moved <= MOA_VS_CUT ? "moa.frame-rate.camera_move_le2048"
                      : "moa.frame-rate.camera_cut", 1);
}

/* When frame n is shown and for how long, from the game's frame clock:
 * the flip comes when 715 scanlines have passed since the last one (or when
 * the render is done, if later; not known here). `before`: VBlanks that still
 * show the previous frame. `period`: VBlanks frame n stays up if the next
 * frame takes the usual 715 lines (2, or 3 about 72% of the time). */
static void moa_flip_timing(uint32_t *before, uint32_t *period)
{
    const uint32_t vblank = g_psx_vblank_cycles, hblank = g_psx_hblank_cycles;
    const uint32_t since = interrupts_get_cycles_since_vblank();
    const uint32_t to_edge = psx_vblank_cycles_to_edge(since);
    uint32_t into_line = 0;
    const uint16_t elapsed = (uint16_t)(moa_timer1(&into_line) -
                                        (uint16_t)psx_mod_read_word(MOA_VS_FRAME_START));
    const uint32_t lines_left = elapsed < MOA_FRAME_LINES ? MOA_FRAME_LINES - elapsed : 0u;
    /* The wait ends on the Timer 1 tick that makes 715. */
    const uint32_t to_flip = lines_left && into_line < hblank
                           ? lines_left * hblank - into_line : lines_left * hblank;
    uint32_t at; /* cycles since the last VBlank edge when the flip happens */
    if (!vblank || !hblank) {
        *before = 1u;
        *period = 3u;
        return;
    }
    if (to_flip < to_edge) {
        *before = 0u;
        at = (since < vblank ? since : vblank - 1u) + to_flip;
    } else {
        const uint32_t past = to_flip - to_edge;
        *before = 1u + past / vblank;
        at = past % vblank;
    }
    *period = (at + MOA_FRAME_LINES * hblank) / vblank;
    if (*before > 8u) *before = 8u;
    if (*period < 1u) *period = 1u;
    if (*period > 8u) *period = 8u;
}

static void moa_render_entry(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (moa_fr_in_pass) return;              /* the passes' own calls */
    if (!moa_fr_enabled) return;
    if (cpu->gpr[31] != MOA_VS_RA_RENDER || !cpu->gpr[4] || !cpu->gpr[5] ||
        !moa_vs_loaded()) {
        moa_prev_valid = 0;
        return;
    }
    const uint16_t tick = psx_mod_read_half(MOA_VS_TICK);
    const uint32_t index = psx_mod_read_word(MOA_VS_GFX_INDEX);
    for (unsigned i = 0; i < 8u; i++) {
        moa_cam_now[i] = (int16_t)psx_mod_read_half(cpu->gpr[4] + 2u * i);
        moa_cam_now[8u + i] = (int16_t)psx_mod_read_half(cpu->gpr[5] + 2u * i);
    }
    /* One step since the last call. The main loop stops counting at 0x4000
     * steps (about 12 minutes, reachable with unlimited time); from then on
     * the count stays put while the steps go on. */
    const int stepped = tick == (uint16_t)(moa_prev_tick + 1u) ||
                        (tick == MOA_VS_TICK_CAP && moa_prev_tick == MOA_VS_TICK_CAP);
    if (moa_prev_valid && stepped && index <= 1u) {
        for (unsigned p = 0; p < 2u; p++) {
            int32_t moved = 0;
            for (unsigned i = 0; i < 8u; i++) {
                if ((i & 3u) == 3u) continue;
                int32_t d = moa_cam_now[8u * p + i] - moa_cam_prev[8u * p + i];
                if (d < 0) d = -d;
                if (d > moved) moved = d;
            }
            moa_cut[p] = moved > MOA_VS_CUT;
            moa_count_motion(moved);
        }
        /* The area frame n was drawn into: its record's full-screen
         * DRAWENV (record +0xC0), = the other record's DISPENV. */
        const uint32_t record = psx_mod_read_word(MOA_VS_GFX_CURRENT);
        PSXModRenderPassFrame frame = {0};
        frame.struct_size = sizeof frame;
        moa_flip_timing(&frame.shown_after_vblanks, &frame.period_vblanks);
        frame.x = psx_mod_read_half(record + 0xC0u);
        frame.y = psx_mod_read_half(record + 0xC2u);
        frame.w = psx_mod_read_half(record + 0xC4u);
        frame.h = psx_mod_read_half(record + 0xC6u);
        /* Why passes could not run, by reason (mod_plugins.h), so one
         * counter dump from a test says what to fix. */
        static const char *const why[] = {
            0, "moa.frame-rate.unavailable_no_presenter",
            "moa.frame-rate.unavailable_backend", "moa.frame-rate.unavailable_disabled",
            "moa.frame-rate.unavailable_session", "moa.frame-rate.unavailable_fast_forward",
            "moa.frame-rate.unavailable_busy"};
        const uint32_t status = psx_mod_render_pass_status();
        if (status != PSX_MOD_RENDER_PASS_READY) {
            psx_mod_counter_add("moa.frame-rate.unavailable", 1);
            psx_mod_counter_add(status < sizeof why / sizeof why[0]
                                    ? why[status] : "moa.frame-rate.unavailable_other", 1);
        }
        if (frame.w && frame.h && frame.w <= 640u && frame.h <= 512u) {
            moa_fr_in_pass = 1;
            const uint32_t kept = psx_mod_render_pass_frame(cpu, &frame, moa_pass, 0);
            moa_fr_in_pass = 0;
            psx_mod_counter_add("moa.frame-rate.frames", 1);
            psx_mod_counter_add("moa.frame-rate.passes", kept);
            psx_mod_counter_add(frame.period_vblanks == 2u
                                    ? "moa.frame-rate.period_2" : "moa.frame-rate.period_3_plus", 1);
        }
    } else {
        psx_mod_counter_add("moa.frame-rate.unpaired", 1);
    }
    for (unsigned i = 0; i < 16u; i++) moa_cam_prev[i] = moa_cam_now[i];
    moa_prev_tick = tick;
    moa_prev_valid = 1;
}

/* The sound chip is out of bounds inside a pass, and the real frame does
 * that work anyway: skip the render's sound routines there. */
static int moa_sound_filter(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!moa_fr_in_pass) return 0;
    cpu->gpr[2] = 0;
    return 1;
}

static void moa_frame_rate_activate(void)
{
    moa_fr_enabled = 0;
    moa_prev_valid = 0;
    if (psx_netplay_active()) {
        psx_mod_counter_add("moa.frame-rate.netplay_skipped", 1);
        return;
    }
    if (psx_mod_activate_render_pass_rate(MOA_FR_PACKAGE, MOA_FR_FEATURE, "rate",
                                          PSX_MOD_RENDER_PASS_FLIP_PENDING)) {
        moa_fr_enabled = 1;
        psx_mod_counter_add("moa.frame-rate.enabled", 1);
    } else {
        psx_mod_counter_add("moa.frame-rate.rejected", 1);
    }
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_frame_rate_plugin)
{
    (void)psx_mod_register_activation_plugin(MOA_FR_PLUGIN, moa_frame_rate_activate);
    (void)psx_mod_register_function_entry_plugin(
        MOA_FR_PLUGIN, MOA_VS_RENDER, moa_render_entry);
    (void)psx_mod_register_function_filter_plugin(
        MOA_FR_PLUGIN, MOA_VS_SOUND_QUEUE, moa_sound_filter);
    (void)psx_mod_register_function_filter_plugin(
        MOA_FR_PLUGIN, MOA_VS_SOUND_BEGIN, moa_sound_filter);
    (void)psx_mod_register_function_filter_plugin(
        MOA_FR_PLUGIN, MOA_VS_SOUND_PLAY, moa_sound_filter);
}
