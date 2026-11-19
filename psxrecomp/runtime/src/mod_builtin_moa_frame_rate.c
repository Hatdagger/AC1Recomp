/*
 * Armored Core: Master of Arena - in-between frames (render passes), test 3:
 * split-screen VS, cameras, ACs and the object pool (shots, missiles,
 * smoke, debris).
 *
 * The game draws at most ~22 frames a second (each frame ends by waiting
 * until 715 scanlines have passed since the last flip). Between two of its
 * frames this plugin redraws the scene with the game's own draw code, with
 * both players' cameras, both ACs (position, facing, the shadow's floor
 * height), the ACs' joint poses and the object pool's records placed part
 * of the way from one logic step to the next. The HUD's lock box is drawn as
 * the newest step left it (step 8).
 *
 * Mapping: docs/mapping/renderer.md, "Frame anatomy (VS)", "Camera (VS)",
 * "ACs (VS)" and "Object pool draws (VS)".
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
 *
 * ACs: their position and angles are logic state, so they are blended
 * before the pass calls the render, from the last step's values to this
 * step's. Their poses are not: the render poses each AC itself (0x80059624)
 * before it draws, so at the pass point the joint matrices still hold the
 * pose frame n was drawn with, and the pass's own render computes the next
 * one. That pose is blended inside the pass, at the entry of the first
 * view's draw (0x800670C8), between the two.
 *
 * Object pool: 256 records the logic steps (0x8008C6C4) and the render only
 * reads, so they are blended before the pass calls the render, like the ACs'
 * positions. A record is reused as soon as it is freed, so it pairs with the
 * last step only when it holds the same object (see moa_capture_pool).
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_netplay.h"
#include "psx_video_timing.h"
#include "render_pass_motion.h"
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

/* ACs (renderer.md "ACs (VS)"): two 376-byte objects, P1 then P2. Word +0
 * set = in play. +0x08 position (s16 x, y, z), +0x10 angles (x, facing, z;
 * 4096 a turn), +0x2C the floor height under the AC (the shadow's height;
 * 32767 = nothing below), +0x68 the AC's pose record. */
#define MOA_VS_AC           0x801D4BC0u
#define MOA_VS_AC_STRIDE    0x178u
#define MOA_VS_VIEW_DRAW    0x800670C8u  /* per-view draw: after both poses, before any drawing */
#define MOA_VS_POSE         0x80059624u  /* pose routine (checked, not hooked) */
#define MOA_VS_POSE_WALK    0x800592D4u  /* joint hierarchy (checked, not hooked) */
#define MOA_VS_AC_DRAW      0x80062C00u  /* AC model draw (checked, not hooked) */
/* Pose record: +0 state (2 = posed this frame), +0x1C the model, +0x20 the
 * joint array. Model +5 = joint count. Joint (124 bytes): +0x12 mesh slot
 * (-1 = no joint at this number), +0x28 its model-space MATRIX (rotation,
 * then t[3] at +0x3C), which is what the model and shadow draws read. */
#define MOA_JOINT_SIZE      0x7Cu
#define MOA_JOINT_SLOT      0x12u
#define MOA_JOINT_WORLD     0x28u
#define MOA_MAX_JOINTS      64u
#define MOA_FLOOR_NONE      32767

/* Object pool (renderer.md "Object pool draws (VS)"): 256 records of 228
 * bytes. +0x00 position (s16 x, y, z), +0x06 0 = in use (-1 free), +0x08
 * owner, +0x10 draw kind (0-6 are drawn), +0x16 life (counts down by one a
 * step in 0x8008C6C4; -1 = no limit; the record is freed when it reaches
 * -1), +0x18 update routine. Per kind, what the draw also reads that moves:
 * kind 0 (missile model) angles +0xBC, kind 1 (streak: bullets, shells)
 * tail +0xBC = the position one step earlier, kind 3 (debris) angles +0xCA,
 * kind 4 (rigid model) angles +0xC0. Kind 4's +0xC8 (scale ?) and the
 * billboard sizes (kinds 2, 5, 6) are not known well enough: not blended. */
#define MOA_VS_POOL         0x801D6F6Cu
#define MOA_VS_POOL_COUNT   256u
#define MOA_VS_POOL_STRIDE  0xE4u
#define MOA_VS_POOL_STEP    0x8008C6C4u  /* update + life countdown (checked, not hooked) */
#define MOA_VS_OBJECTS_DRAW 0x80065C44u  /* the pool's draw loop (checked, not hooked) */
/* A rifle bullet moves 701 a step; nothing else is measured yet. A record
 * that moved further than this was placed (a new object the identity test
 * missed, or a teleport): drawn where the step left it. Not measured yet. */
#define MOA_POOL_MOVE       4096.0
#define MOA_POOL_TURN       (MOA_PI / 2.0)

/* How far an AC may move or turn in one step and still glide. From the
 * battle lane's measured speeds: 83 a step at full ground boost (75 after
 * the size cap), 150 at the fall limit, turning at most 38 a step; a round
 * restart moves an AC by thousands. An AC that moved further was placed: it
 * is drawn where the step left it, floor and pose included; angles that
 * turned further are drawn as the step left them. */
#define MOA_PI              3.14159265358979323846
#define MOA_AC_MOVE         512.0          /* world units */
#define MOA_AC_TURN         (MOA_PI / 2.0) /* a quarter turn */
/* Joints, in model space (a model unit is 1/4 of a world unit: near ACs are
 * drawn with the view matrix >> 2). A joint that jumps further than this in
 * one step belongs to a pose that was cut, not animated. Not measured yet. */
#define MOA_JOINT_MOVE      1024.0
#define MOA_JOINT_TURN      (MOA_PI / 2.0)

static int moa_fr_enabled, moa_fr_in_pass;
static int16_t moa_cam_prev[16], moa_cam_now[16];
static int moa_prev_valid;
static uint16_t moa_prev_tick;
static int moa_cut[2];

/* The pose frame n was drawn with, per AC, and what it belongs to. */
typedef struct MoaPose {
    int ok;
    uint32_t record, joints, model, count;
    uint8_t used[MOA_MAX_JOINTS];
    PSXMotionMatrix m[MOA_MAX_JOINTS];
} MoaPose;

static PSXMotionSet *moa_motion;         /* AC position and angles */
static uint32_t moa_motion_tick;
static MoaPose moa_pose[2];
static int16_t moa_floor_prev[2], moa_floor_now[2];
static int moa_floor_ok[2];
static double moa_pass_t;
static int moa_pose_pending, moa_count_pose;

static PSXMotionSet *moa_pool_motion;    /* object pool records */
static uint32_t moa_pool_tick;

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
           moa_code_is(MOA_VS_SOUND_PLAY, 0x27BDFFB8u, 0xAFB3002Cu) &&
           moa_code_is(MOA_VS_VIEW_DRAW, 0x27BDFFD8u, 0xAFB20020u) &&
           moa_code_is(MOA_VS_POSE, 0x27BDFF98u, 0xAFB10044u) &&
           moa_code_is(MOA_VS_POSE_WALK, 0x27BDFFE0u, 0xAFB10014u) &&
           moa_code_is(MOA_VS_AC_DRAW, 0x27BDFF78u, 0xAFB7007Cu) &&
           moa_code_is(MOA_VS_POOL_STEP, 0x27BDFFE0u, 0x3C02801Du) &&
           moa_code_is(MOA_VS_OBJECTS_DRAW, 0x27BDFF88u, 0x3C02801Au);
}

static uint32_t moa_ac(unsigned i)
{
    return MOA_VS_AC + i * MOA_VS_AC_STRIDE;
}

static int moa_in_ram(uint32_t address)
{
    return address >= 0x80000000u && address < 0x80200000u;
}

/* The pose an AC was last drawn with: the joint matrices the render's pose
 * routine left (state 2 = posed this frame). Nothing writes them between
 * frames: the only other caller of the pose routine is match start. */
static void moa_capture_pose(unsigned i, MoaPose *pose)
{
    const uint32_t ac = moa_ac(i);
    pose->ok = 0;
    if (!psx_mod_read_word(ac)) return;
    const uint32_t record = psx_mod_read_word(ac + 0x68u);
    if (!moa_in_ram(record) || (record & 3u) || psx_mod_read_half(record) != 2u) return;
    const uint32_t joints = psx_mod_read_word(record + 0x20u);
    const uint32_t model = psx_mod_read_word(record + 0x1Cu);
    if (!moa_in_ram(joints) || (joints & 3u) || !moa_in_ram(model)) return;
    const uint32_t count = psx_mod_read_byte(model + 5u);
    if (!count || count > MOA_MAX_JOINTS) return;
    for (uint32_t j = 0; j < count; j++) {
        const uint32_t joint = joints + j * MOA_JOINT_SIZE;
        pose->used[j] = psx_mod_read_half(joint + MOA_JOINT_SLOT) != 0xFFFFu;
        if (pose->used[j]) psx_motion_read_matrix(joint + MOA_JOINT_WORLD, &pose->m[j]);
    }
    pose->record = record;
    pose->joints = joints;
    pose->model = model;
    pose->count = count;
    pose->ok = 1;
}

/* Once per game frame, at the pass point: this step's AC positions and
 * angles (paired with the last step's by the motion set), the shadow's
 * floor height, and the pose frame n was drawn with. */
static void moa_capture_acs(int paired)
{
    if (!moa_motion) moa_motion = psx_motion_set_create(8u);
    if (!moa_motion) {
        moa_pose[0].ok = moa_pose[1].ok = 0;
        moa_floor_ok[0] = moa_floor_ok[1] = 0;
        return;
    }
    if (!paired) psx_motion_invalidate(moa_motion);
    psx_motion_begin(moa_motion, ++moa_motion_tick);
    for (unsigned i = 0; i < 2u; i++) {
        const uint32_t ac = moa_ac(i);
        moa_floor_prev[i] = moa_floor_now[i];
        moa_floor_now[i] = (int16_t)psx_mod_read_half(ac + 0x2Cu);
        if (!psx_mod_read_word(ac)) continue;
        /* A new pose record means a new AC at this address. */
        const uint32_t identity = psx_motion_identity(ac, psx_mod_read_word(ac + 0x68u), 0u);
        (void)psx_motion_track(moa_motion, PSX_MOTION_SVECTOR, ac + 0x08u, identity);
        (void)psx_motion_track(moa_motion, PSX_MOTION_ANGLES, ac + 0x10u, identity);
    }
    const PSXMotionLimits limits = {MOA_AC_MOVE, MOA_AC_TURN};
    (void)psx_motion_prepare(moa_motion, &limits, 0);
    for (unsigned i = 0; i < 2u; i++) {
        const uint32_t ac = moa_ac(i);
        int32_t at[3];
        /* An AC that was placed (or has no history) is drawn as the step
         * left it: position, floor and pose alike. */
        const int glides = psx_motion_blend_at(moa_motion, PSX_MOTION_SVECTOR,
                                               ac + 0x08u, 1.0, at) == 1;
        if (paired && psx_mod_read_word(ac))
            psx_mod_counter_add(glides ? "moa.frame-rate.ac_glide" : "moa.frame-rate.ac_placed", 1);
        const int32_t floor_move = (int32_t)moa_floor_now[i] - moa_floor_prev[i];
        moa_floor_ok[i] = glides && moa_floor_now[i] != MOA_FLOOR_NONE &&
                          moa_floor_prev[i] != MOA_FLOOR_NONE &&
                          floor_move >= -(int32_t)MOA_AC_MOVE && floor_move <= (int32_t)MOA_AC_MOVE;
        moa_capture_pose(i, &moa_pose[i]);
        if (!glides) moa_pose[i].ok = 0;
    }
}

/* Once per game frame, at the pass point: every pool record the draw will
 * draw (in use, kind 0-6), its position and the moving field of its kind.
 * Records are reused at once, so a value pairs with the last step's only
 * when it is the same object: same update routine, owner and kind, and a
 * life one step lower (life + steps unchanged), or no life limit on both.
 * A new object, or a reused record, is drawn where it appears. */
static void moa_capture_pool(int paired)
{
    if (!moa_pool_motion)
        moa_pool_motion = psx_motion_set_create(2u * MOA_VS_POOL_COUNT);
    if (!moa_pool_motion) return;
    if (!paired) psx_motion_invalidate(moa_pool_motion);
    psx_motion_begin(moa_pool_motion, ++moa_pool_tick);
    for (uint32_t r = 0; r < MOA_VS_POOL_COUNT; r++) {
        const uint32_t rec = MOA_VS_POOL + r * MOA_VS_POOL_STRIDE;
        if (psx_mod_read_half(rec + 0x06u) != 0u) continue;
        const uint32_t kind = psx_mod_read_half(rec + 0x10u);
        if (kind > 6u) continue;
        const uint16_t life = psx_mod_read_half(rec + 0x16u);
        const uint32_t age = life == 0xFFFFu ? 0xFFFFFFFFu
                                             : (uint32_t)life + moa_pool_tick;
        const uint32_t identity = psx_motion_identity(
            psx_mod_read_word(rec + 0x18u), psx_mod_read_word(rec + 0x08u),
            psx_motion_identity(kind, age, rec));
        (void)psx_motion_track(moa_pool_motion, PSX_MOTION_SVECTOR, rec, identity);
        switch (kind) {
        case 0u: (void)psx_motion_track(moa_pool_motion, PSX_MOTION_ANGLES, rec + 0xBCu, identity); break;
        case 1u: (void)psx_motion_track(moa_pool_motion, PSX_MOTION_SVECTOR, rec + 0xBCu, identity); break;
        case 3u: (void)psx_motion_track(moa_pool_motion, PSX_MOTION_ANGLES, rec + 0xCAu, identity); break;
        case 4u: (void)psx_motion_track(moa_pool_motion, PSX_MOTION_ANGLES, rec + 0xC0u, identity); break;
        default: break;
        }
    }
    const PSXMotionLimits limits = {MOA_POOL_MOVE, MOA_POOL_TURN};
    PSXMotionStats stats = {0};
    (void)psx_motion_prepare(moa_pool_motion, &limits, &stats);
    if (paired) {
        psx_mod_counter_add("moa.frame-rate.pool_tracked", stats.tracked);
        psx_mod_counter_add("moa.frame-rate.pool_blended", stats.blended);
        psx_mod_counter_add("moa.frame-rate.pool_placed", stats.placed);
        psx_mod_counter_add("moa.frame-rate.pool_unmatched", stats.unmatched);
    }
}

/* Inside a pass, before the first view is drawn: the pass's render has just
 * posed both ACs for this step; blend each joint from the pose frame n was
 * drawn with. A joint that turned or moved past the limits keeps this
 * step's matrix. */
static void moa_view_draw_entry(struct CPUState *cpu, uint32_t address)
{
    (void)cpu;
    (void)address;
    if (!moa_fr_in_pass || !moa_pose_pending) return;
    moa_pose_pending = 0;
    uint32_t blended = 0, placed = 0, unmatched = 0;
    for (unsigned i = 0; i < 2u; i++) {
        const MoaPose *pose = &moa_pose[i];
        if (!pose->ok) continue;
        const uint32_t record = psx_mod_read_word(moa_ac(i) + 0x68u);
        if (record != pose->record || psx_mod_read_half(record) != 2u ||
            psx_mod_read_word(record + 0x20u) != pose->joints ||
            psx_mod_read_word(record + 0x1Cu) != pose->model) {
            unmatched++;
            continue;
        }
        for (uint32_t j = 0; j < pose->count; j++) {
            if (!pose->used[j]) continue;
            const uint32_t at = pose->joints + j * MOA_JOINT_SIZE + MOA_JOINT_WORLD;
            PSXMotionMatrix now, out;
            psx_motion_read_matrix(at, &now);
            double moved = 0.0;
            for (unsigned k = 0; k < 3u; k++) {
                const double d = (double)now.t[k] - (double)pose->m[j].t[k];
                moved += d * d;
            }
            if (moved > MOA_JOINT_MOVE * MOA_JOINT_MOVE ||
                !psx_motion_blend_matrix(&pose->m[j], &now, moa_pass_t, MOA_JOINT_TURN, &out)) {
                placed++;
                continue;
            }
            psx_motion_write_matrix(at, &out, 1);
            blended++;
        }
    }
    /* Counted for the first pass of each frame only (every pass sees the
     * same pair). */
    if (moa_count_pose) {
        moa_count_pose = 0;
        psx_mod_counter_add("moa.frame-rate.joints_blended", blended);
        psx_mod_counter_add("moa.frame-rate.joints_placed", placed);
        psx_mod_counter_add("moa.frame-rate.pose_unmatched", unmatched);
    }
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
    /* ACs: position and angles now, the pose once the render has posed them
     * (moa_view_draw_entry). */
    const double t = (double)alpha_q16 / 65536.0;
    psx_motion_apply(moa_motion, t);
    psx_motion_apply(moa_pool_motion, t);
    for (unsigned i = 0; i < 2u; i++) {
        if (!moa_floor_ok[i]) continue;
        const int32_t from = moa_floor_prev[i];
        const int32_t v = from + (int32_t)(((int64_t)(moa_floor_now[i] - from) *
                                            (int64_t)alpha_q16) >> 16);
        psx_mod_write_half(moa_ac(i) + 0x2Cu, (uint16_t)v);
    }
    moa_pass_t = t;
    moa_pose_pending = 1;
    /* The render starts by switching buffers: start from the other index so
     * it draws into the area frame n is in (the one the next flip shows). */
    psx_mod_write_word(MOA_VS_GFX_INDEX, psx_mod_read_word(MOA_VS_GFX_INDEX) ^ 1u);
    /* Its frame end waits until 715 scanlines have passed since the last
     * flip; make that flip look old enough. */
    psx_mod_write_word(MOA_VS_FRAME_START, (uint32_t)(uint16_t)(moa_timer1(0) - 800u));
    moa_call(&call, MOA_VS_RENDER, cameras, cameras + 16u);
    moa_pose_pending = 0;
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
    const int paired = moa_prev_valid && stepped && index <= 1u;
    moa_capture_acs(paired);
    moa_capture_pool(paired);
    if (paired) {
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
            moa_count_pose = 1;
            const uint32_t kept = psx_mod_render_pass_frame(cpu, &frame, moa_pass, 0);
            moa_fr_in_pass = 0;
            moa_count_pose = 0;
            moa_pose_pending = 0;
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
    moa_pose[0].ok = moa_pose[1].ok = 0;
    moa_floor_ok[0] = moa_floor_ok[1] = 0;
    moa_pose_pending = 0;
    if (moa_motion) psx_motion_invalidate(moa_motion);
    if (moa_pool_motion) psx_motion_invalidate(moa_pool_motion);
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
    (void)psx_mod_register_function_entry_plugin(
        MOA_FR_PLUGIN, MOA_VS_VIEW_DRAW, moa_view_draw_entry);
    (void)psx_mod_register_function_filter_plugin(
        MOA_FR_PLUGIN, MOA_VS_SOUND_QUEUE, moa_sound_filter);
    (void)psx_mod_register_function_filter_plugin(
        MOA_FR_PLUGIN, MOA_VS_SOUND_BEGIN, moa_sound_filter);
    (void)psx_mod_register_function_filter_plugin(
        MOA_FR_PLUGIN, MOA_VS_SOUND_PLAY, moa_sound_filter);
}
