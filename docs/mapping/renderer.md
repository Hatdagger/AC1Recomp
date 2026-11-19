# Master of Arena — Renderer Mapping Plan

Goal: know how every frame of a fight is drawn — where the camera comes from, which routine
draws what, how it is sorted and culled — well enough to (a) switch on the framework's
enhancement features that need a title's draw code mapped, and (b) later draw parts of the game
natively (the custom-engine road).

Disc: USA disc 1 (SLUS-01030 v1.0). Written 2026-11-18 from the repo (master) and the
framework docs in `psxrecomp/docs`. Nothing below is measured yet unless it says where it came
from. Status: lane claimed 2026-11-18; **steps 1 and 2 done for VS (FDAT 209), 2026-11-18** (§1 "Frame
anatomy (VS)", "Camera (VS)"). **First milestone (camera-only render pass) passed 2026-11-18**:
user-tested (the camera moves more smoothly, no visual defects) and merged into master
(`99ae2cf`). Step 3 (draw list): static first cut for VS
2026-11-18 (§1 "Draw list (VS)"), not yet confirmed on screen.

## 1. What we already have

| Thing | Where | Source |
|---|---|---|
| **The scene render already runs in the framework's sandbox** (link engine, FDAT 208): the guest's own view in netplay re-runs the camera and the scene render for player 2 | scene render `0x80067FE4` (called from the main loop at `0x80052914`), camera routine `0x80088988`, camera set `0x80053280` | `moa_link_netplay.c` |
| Double-buffer record (link): +0 buffer index, +4 two 128-byte buffer records (+4 = DRAWENV x / y / w / h), +25,928 frame time | `0x8019D4F0` | `moa_link_netplay.c` |
| Camera smoothing state the camera routine writes (link) | `0x801D1CD8`–`0x801D1D27` (mode word `0x801D1CDC`, `0x801D1CF0` set by the simulation) | `moa_link_netplay.c` |
| Sound upkeep inside the scene render reads the SPU (the sandbox refuses that): skip it during a redraw | `0x80053FE4`, `0x80053808` | `moa_link_netplay.c` (`moa_link_sound_filter`) |
| Camera record, 56 bytes per player: distance, height, smoothing, angles, cockpit-view joints | VS `0x801D6EF8` / `0x801D6F30`, arena `0x801E29C8`; chase `0x800878FC`, cockpit `0x80087B90` | `stat-block.md` "Camera setup" |
| VS main loop: one step per pass, "draw both views" at the end; steps elapsed = match struct +0x12 | loop from `0x80052124`, match struct `0x80097EA0` | `battle-system.md` |
| AC object: position +0x08/+0x0A/+0x0C, facing +0x12, last step's position +0x20…, model data pointer +0x68 `?`, animation player +0x6C, velocity +0xD4… | 376 bytes, P1 / P2 back to back | `battle-system.md` |
| Object pool (bullets, missiles, …): 256 × 228 bytes, update routine at record +0x18 | `0x801D6F6C` (VS) | `battle-system.md` "The object pool" |
| AC models, skeletons, joint animations, textures decoded from the disc (export + software preview) | `acr/` (`acr_models_moa.c`, `acr_motion_moa.c`, `acr_model.c`), `moa_ac_view.c` | repo; Armored-CoreR `docs/research/gen1-models.md` |
| Where the AC textures sit in VRAM; 16 texture sets per `*_T` archive; P1/P2 set swap `0x800C0E7C`; fight engines never load textures | | `boot-loads.md` |
| Stage files: FDAT 5s+1 and 5s+4 (VS), 5s+2 and 5s+4 (arena), s = stage | loads: VS `0x80055AA8` / `0x80055AC8`, arena `0x80056330` / `0x80056350` | `boot-loads.md` §8 |
| HUD sight routine (lock box, aiming guide) | `0x800576E0` | `stat-block.md` "Lock-on" |
| Widescreen is offered with the GTE game mode | `game.toml` `[widescreen] offer = true, gte_game_mode = true` | repo |
| **Frame cadence: at most 22 frames a second.** One logic step per frame; the frame end does `DrawSync(0)`, waits until 715 scanlines (root counter 1, `0x80016724`) have passed since the last frame start, swaps buffers, starts the next OT. No `VSync` wait in the fight loop | frame-end wait `slti 0x2CB`: VS (209) `0x8005C080`, arena (210) `0x8005E2D0`, link (208) `0x8005CED4`, missions (207) `0x8005D2CC`, replay (211) `0x8005C624` | Armored-CoreR `gen1-timing.md` |
| Camera as measured: eye / look-at pairs per player (VS) `0x801A9A88`, eye copies `0x800441D8`. Projection: one view H 320 on 320×240, centre (160, 120); **VS: two side-by-side 160×240 halves, H 160 each, centre row 115** | | Armored-CoreR `gen1-engine-map.md` |
| Rotation-matrix builder in the main program: RotMatrix `0x80029F94`, M = Rx·Ry·Rz (the order the model export uses); a Ry·Rx·Rz version `0x8002A224` that nothing calls | | Armored-CoreR `gen1-models.md` |

### Main program routines the engines call (named 2026-11-18)

The fight engines draw through these (the main program itself calls almost none of them, so
in an engine a `jal` to one of these addresses names the call). Named from PsyQ's debug strings
in `SLUS_010.30` ("DrawOTag(%08x)..." etc.) and, for the GTE ones, from which GTE control
registers they write. Only 51 GTE commands are in the main program: the 3D transform code
lives in the engines.

| Routine | Address | How named |
|---|---|---|
| `VSync(mode)` | `0x8002A714` | VBlank count `0x8003E618`; its wait `0x8002A88C` prints "VSync: timeout" |
| `ResetGraph` / `SetGraphDebug` / `DrawSyncCallback` | `0x8002C058` / `0x8002C2A8` / `0x8002C3B8` | strings |
| `DrawSync(mode)` | `0x8002C4B0` | string |
| `LoadImage` / `MoveImage` | `0x8002C75C` / `0x8002C81C` | strings (LoadImage matches `boot-loads.md`) |
| `ClearOTag` / `ClearOTagR` | `0x8002C8D4` / `0x8002C99C` | strings |
| **`DrawOTag(ot)`** | **`0x8002CAA4`** | string |
| **`PutDrawEnv(env)`** | **`0x8002CB14`** | string |
| `DrawOTagEnv(ot, env)` | `0x8002CBD4` | string |
| **`PutDispEnv(env)`** | **`0x8002CCE0`** | string |
| `SetDefDrawEnv` / `SetDefDispEnv` `?` | `0x80011F94` / `0x80011FF0` | called as (env, 0, 240, 320, 240) by the main program's picture screen `0x80012348`, right before PutDispEnv / PutDrawEnv |
| `InitGeom` | `0x80028DE4` | writes H, OFX/OFY, DQA/DQB, ZSF3/ZSF4 |
| `SetRotMatrix` / `SetTransMatrix` | `0x80029B24` / `0x80029BB4` | 12 / 8 instructions writing GTE R (regs 0–4) / TR (5–7) |
| `SetGeomOffset(x, y)` / **`SetGeomScreen(h)`** | `0x80029D24` / **`0x80029D44`** | GTE OFX/OFY / H (`0x80029D48` is only SetGeomScreen's `jr ra`; corrected 2026-11-18). VS sets them once for both views (see Frame anatomy) |
| `SetFarColor(r, g, b)` | `0x80029D04` | GTE FC (regs 21–23); VS sets black fog |
| `SetDrawEnv(DR_ENV*, DRAWENV*)` `?` | `0x8002D3D8` | builds the 64-byte `DR_ENV` packets VS puts at the head of each view's OT |
| `SetDQA` / `SetDQB` | `0x80029CCC` / `0x80029CD8` | depth cueing (fog) |
| Matrix multiplies (load GTE R, then MVMVA) | `0x80029448`, `0x80029648`, `0x800297A4`, `0x80029904`, `0x80029954`, `0x800290C4`, `0x80029E98`, `0x80029EF0` | not told apart yet |
| VBlank callback | `0x800166A0` | counts VBlanks at `0x80043C74`, then `0x80017340`, then the hook at `0x80043C78` if set |
| VBlanks since a0 / VBlank count | `0x800166FC` / `0x80016710` | read `0x80043C74` |
| Scanlines since a0 (the frame-time wait) | `0x80016724` | `GetRCnt(RCntCNT1)` (`0x8002B600`), 16-bit wrap |
| `DrawSync(0)` then `VSync(0)` | `0x80016678` | |
| `rsin` / `rcos` | `0x80028B64` / `0x80028C34` | 12-bit angle; odd / even in the angle (named 2026-11-18, step 3) |
| `SetLightMatrix` / `SetColorMatrix` / `SetBackColor` | `0x80029B54` / `0x80029B84` / `0x80029CE4` | write GTE L (c8–c12) / LR (c16–c20) / BK (c13–c15) |
| `SetFogNearFar(near, far, h)` `?` | `0x80028CD4` | returns at once when far − near < 100, as PsyQ's does |
| `Square0` / `RotTrans` | `0x80029E2C` / `0x80029F64` | GTE SQR / MVMVA (R · V0 + TR), RotTrans also stores the flag word |
| `StoreImage` | `0x8002C7BC` | string (next to LoadImage) |
| `malloc` / `free` wrappers | `0x80016610` / `0x80016658` | call `0x8002BE54` / `0x8002BF04` |

### Frame anatomy (VS, FDAT 209)

Read from FDAT 209 (disc 1, SLUS-01030) on 2026-11-18. Anything not yet proven by a run is marked `?`.

#### Call order, one frame

The VS main loop (`0x80052124`) runs one logic step, then at its end:

1. `0x80087B90` once per player: camera → 16 bytes each on the stack (P1 `sp+0x10`, P2 `sp+0x20`).
   The current player struct is written to `0x8003EAA0` before each call (P1 `0x80045198`, P2 `0x80045538`).
2. **`0x80067404(camP1, camP2)` = the scene render ("draw both views").** It also runs during the countdown, called with (0, 0) = "keep last camera".
3. `0x8005301C(camP1, camP2)`: copies the eyes to `0x800441D8` / `0x800441EC` (the "eye copies"; sound listener `?`).
4. Logic tick: match struct `0x80097EA0` +0x12 += 1 (capped at 0x4000), then back to the top.

Inside the scene render `0x80067404`:

| Order | Address | What |
|---|---|---|
| a | `0x80067434`–`0x800674C4` | copy the two cameras to `0x801A9A88` (P1) / `0x801A9A9C` (P2), 16 bytes each, stride 0x14 |
| b | `0x80072E70` ×2 | per-player value into `0x801A9A84` / `0x801A9A98` (`?`) |
| c | `0x80066E34`, `0x8005BB40` | `?` (before the swap) |
| d | **`0x8005BFB8` buffer swap** | flips the index, sets the current record, `ClearOTagR` on both OTs, resets the primitive pointer |
| e | `0x8005A72C`, `0x80059624` ×2 (per AC), `0x8005AC68` / `0x8005ACC8` | per-AC work before drawing (`?`) |
| f | 4 × `DR_ENV` (64 bytes each) taken from the primitive buffer | |
| g | **View 1:** current OT = OT A; camera P1 → `0x801A9AB0`; **`0x800670C8`** (per-view draw); `DR_ENV`(view 1 DRAWENV) put at the head of OT A | |
| h | **View 2:** current OT = OT B; camera P2 → `0x801A9AB0`; `0x800670C8`; `DR_ENV`(view 2 DRAWENV) at the head of OT B | |
| i | fade box `0x800632E8` (320×240, when `0x801A8CBB` ≠ 0), timer / text / HUD `0x80065494`, `0x800659DC`, `0x800572B4` into OT B | |
| j | `DR_ENV`(full-screen DRAWENV) at OT B entry 5 (the last few entries draw over the whole screen) and at the head of OT A | |
| k | **chain:** OT A's tail (entry 0) is linked to OT B's head (entry 8191); OT B's tail keeps the end marker | |
| l | **`0x8005C050` frame end** (below) | |
| m | `0x8005A81C`, frame counter `0x801A8C34` += 1 | after the submit |

`0x800670C8` (per-view draw) uses the scratchpad (`0x1F800200`), builds the view matrix (`0x800139F8` ×6, matrix multiply `0x80029904`, `0x80067004`, `SetTransMatrix`), then draws through `0x80066B10`, `0x80061C3C`, `0x80065C44` (step 2 / step 3).

#### Frame end `0x8005C050`

`DrawSync(0)` → `0x800535F4` (sound-slot upkeep `?`, walks 23 × 16-byte records at `0x80044428`) → **wait until 715 scanlines** since the last frame start (`0x8005C074`–`0x8005C084`, `slti 0x2CB` at `0x8005C080`) → `PutDispEnv(R+0x11C)` → `PutDrawEnv(R+0x08)` → **`DrawOTag(OT A + 0x7FFC)`** (one call draws both views, because the OTs are chained) → frame start time = `GetRCnt(RCntCNT1)` into `0x801A8C28` → `0x8005C0F8` (resets a list at `0x801AAF70` `?`) → clears 8 bytes at `0x80045178`.

No `VSync` in the fight loop (one `DrawSync(0)` + `VSync(0)` only at match end, `0x800527A8`).

#### Buffer records

Base `0x801A2568`:

| Offset | What |
|---|---|
| +0x000 | buffer index (0 / 1; −1 before the first frame) |
| +0x004 | record 0 (0x13C bytes) |
| +0x140 | record 1 |
| +0x27C | **current record pointer R** |
| +0x280 | primitive buffer start (`0x8013C560`) |
| +0x284 | pointer to R+0x130 (primitive allocator: +0 start, +4 end, +8 next free) |
| +0x288 | current OT base (switched per view) |

Record layout (R):

| R+ | What | Record 0 | Record 1 |
|---|---|---|---|
| +0x000 | OT A (view 1, P1), 8192 entries | `0x80182560` | `0x8018A560` |
| +0x004 | OT B (view 2, P2), 8192 entries | `0x80192560` | `0x8019A560` |
| +0x008 | DRAWENV view 1 (clears its area, black) | 2, 5, 158×230 | 2, 245, 158×230 |
| +0x064 | DRAWENV view 2 (clears its area, black) | 160, 5, 158×230 | 160, 245, 158×230 |
| +0x0C0 | DRAWENV full screen (no clear, no dither) | 0, 0, 320×240 | 0, 240, 320×240 |
| +0x11C | DISPENV | **0, 240**, 320×240 | **0, 0**, 320×240 |
| +0x130 / +0x134 | primitive buffer start / end (0x21000 = 135,168 bytes) | `0x8013C560`–`0x8015D560` | `0x8015F560`–`0x80180560` |

Unlike the link engine (`0x8019D4F0`: +0 index, +4 two 128-byte records, DRAWENV at +4), VS has a 0x13C-byte record with **two OTs and three DRAWENVs**. Setup: `0x8005C1E8` (envs) and `0x8005C11C` (OT and primitive pointers).

#### Flip mode

Each record draws into one half of VRAM and its DISPENV shows the **other** half. At the frame end, `PutDispEnv` shows the picture the previous frame drew (its `DrawOTag` has just finished with `DrawSync`), then the new OT is submitted into the hidden half. **A submitted frame is first shown at the next frame end's `PutDispEnv`, one flip later** → render-pass flip mode **PENDING**: this is the PsyQ "DISPENV[i] shows the buffer DRAWENV[1−i] draws" loop that `RENDER_PASSES.md` "Flip modes" lists under `PENDING` (a scanline wait in place of `VSync(0)`).

#### The two views

- **One `SetGeomOffset` / `SetGeomScreen` for both views, set once** at setup (`0x8005C1E8`): offset (79, 115), H = 160, `SetFarColor(0, 0, 0)` (fog to black).
- The views are separated by **two OTs**, each starting with a `DR_ENV` that sets that view's DRAWENV (clip rectangle + draw offset (2, 5) or (160, 5)). So the GTE projects both views the same way and the GPU draw offset moves view 2 to the right half. View centre on screen: (81, 120) and (239, 120).
- The views are 158×230 with a 2-pixel gutter at the left and middle and 5 pixels top and bottom (`?` whether anything is drawn in the borders).

#### Render-pass facts

| Item | Value |
|---|---|
| Draw section start | scene render entry `0x80067404` (first words `0x27BDFFA0, 0xAFBF005C, 0xAFBE0058, 0xAFB70054`); the swap is `0x8005BFB8` (`0x27BDFFE8, 0xAFB00010`) |
| **Pass point** | **frame-end entry `0x8005C050`** (`0x27BDFFE8, 0x00002021, 0xAFBF0014, 0x0C00B12C`), called at `0x80067C20` after both OTs are built and chained, before `DrawOTag` |
| Flip mode | PENDING (above) |
| Logic tick | match struct `0x80097EA0` +0x12 (one step per frame) |
| Redraw re-runs | both views (`0x800670C8` ×2) and the HUD, into the record that `0x8005BFB8` picks |
| Camera inputs | `0x801A9A88` / `0x801A9A9C` (copied from the main loop's stack each frame); per view `0x801A9AB0` |
| Device reads | `0x800535F4` in the frame end (sound upkeep `?`): filter it like the link engine's `moa_link_sound_filter` |

### Camera (VS, FDAT 209)

Read from FDAT 209 on 2026-11-18 (step 2). Anything not yet proven by a run is marked `?`.

- **Input, once per step:** the main loop calls the camera routine **`0x80087B90(out)`** for each
  player (`0x8003EAA0` = that player's block; camera record through player block +0x39C; it calls
  the chase camera `0x800878FC`). It writes 16 bytes on the main loop's stack (P1 `sp+0x10`, P2
  `sp+0x20`): **eye x, y, z, (not read by the draw), look-at x, y, z, (not read)**, halves, world
  units. These two are the draw's **only camera input**: the scene render takes them as its
  arguments, `0x80067404(P1, P2)`; (0, 0) keeps the last ones (only on the match-end path).
- **Scene render:** copies them to `0x801A9A88` / `0x801A9A9C` (stride 0x14). Word +0x10 of each
  (`0x801A9A98`, `0x801A9AAC`) = the stage area the eye is in: `0x80072E70(previous area, camera)`
  checks the stage's area boxes (from `0x801BDFD0`: min x / y / z at +0 / +2 / +4, max at +8 / +0xA /
  +0xC, flags +0x30) `?`.
- **Per view, `0x800670C8`** (`0x8003EAA0` = the viewed player; its camera copied to `0x801A9AB0`,
  its area to `0x801A9AAC`):
  - **`0x80014344(eye, look-at, Rx, Ry, R)`** (main program): yaw from the x / z offset, pitch from
    the height difference, **no roll**; Rx → `0x801A9C30`, Ry → scratchpad `0x1F800210`,
    **R = Rx · Ry → `0x801A9AC0`** (MulMatrix0 `0x80029444`).
  - The eye goes into R's translation words (`0x801A9AD4` / `D8` / `DC`); **T = R · (−eye) →
    `0x801A9AE0`** (`0x80029904`, ApplyMatrixLV `?`), for object translations (R · position + T).
  - Scaled copies: R ≫ 2 at `0x801A9AF0`, R ≫ 3 at `0x801A9B10`, three more steps of
    `0x800139F8` from each (`0x801A9B30` / `50` / `70`, and from a copy of R with its eye at
    `0x801A9B90`: `0x801A9BB0` / `D0` / `F0`), Rx ≫ 2 at `0x801A9C10`. Their use is step 3 (model
    scale levels `?`).
  - Then `0x80067004` `?`, `SetTransMatrix(0x80036AC4)` (main-program data `?`), and a list at
    `0x801ADC70` (count `0x801ADF74`) whose positions minus the eye go through the GTE (step 3).
- **Projection:** `SetGeomScreen(160)` and `SetGeomOffset(79, 115)` only at setup (`0x8005C1E8`,
  the only callers in FDAT 209). Both views use them; the DRAWENV offsets (2, 5) / (160, 5) place
  the views. Nothing changes them in a fight: no other calls, and no direct GTE writes to H or
  OFX / OFY anywhere in FDAT 209 (its direct `ctc2` writes are rotation, translation and light
  registers only). The cockpit view only moves the eye.
- **What moves per step (render passes):** the two 16-byte cameras. Everything above is rebuilt from
  them inside the render; smoothing, cockpit view and the camera record are logic-side (the camera
  routine, data lane), so a pass only needs the render's two arguments.

### First milestone: camera-only render pass (VS) — passed, merged

Package `acrecomp.moa.frame-rate` (`mods/preloaded`, off by default, option "Frame rate": display /
60 / 120), plugin `psxrecomp/runtime/src/mod_builtin_moa_frame_rate.c`, on branch
**`renderer-vs-camera-pass`** (pushed 2026-11-18, commits `540e22c` and fix `34a6e64`). Built
2026-11-18 (CI run 44433465501, started by hand: CI builds only master on its own). **User test
2026-11-18** (Windows, plain VS, package on, frame rate "display"): the camera moves more smoothly
than with the package off, no visual defects. Merged into master 2026-11-18 (`99ae2cf`). Not
measured yet (needs a debug-tools build): the counters, `render_pass_stats` and
`PSX_RENDER_PASS_VERIFY`; the camera cut limit (2,048) is still a guess, not set from real motion.

Checked before the test (2026-11-18): compiles with `-Wall -Wextra -Wpedantic`; the engine entry
word and all five code signatures match FDAT 209; engine hooks need no regenerated game C (the VS
AI hook `0x8007DA4C` went in the same way). The pass point is not refused as busy for frame n's
list walk: a `DrawOTag` kick holds the CPU until the walk ends (`dma.c`
`hold_cpu_for_gpu_linked_list`). **Fixed (`34a6e64`):** the main loop counts steps only while
below 0x4000 (`0x800526AC`, about 12 minutes), so the "one step since the last call" check failed
for the rest of a long fight; an unchanged count at 0x4000 now counts as one step (host test: 1,999
of 1,999 frames planned across the cap, 256 before).

| Item | Choice |
|---|---|
| Pass point | **scene render entry** `0x80067404` from the main loop (`$ra` `0x80052698`), before the real render runs: frame n is complete, step n+1's logic is done, and the render has not stepped anything yet, so nothing the render itself steps (effects, counters) is stepped twice. (The frame-end entry `0x8005C050` was rejected: frame n's `DrawOTag` may still be running there, and passes refuse while a GPU walk is in flight.) |
| Rect | current record +0xC0 (full-screen DRAWENV of frame n's area = the other record's DISPENV); flip mode PENDING |
| Pass | cameras blended linearly between the last two steps (a coordinate that jumps more than 2,048 in a step = cut: that camera is not blended), buffer index ^ 1 (so the render's swap draws into frame n's area), frame start = Timer 1 − 800 (so the 715-line wait ends at once), call the render; inside passes `0x800535F4` (queued sounds), `0x80053DD0` (frame-begin sound upkeep) and `0x80053284` (start a sound) are skipped |
| Frame timing | from Timer 1 (count and part-line) and the VBlank clock: VBlanks before the flip, and how long frame n stays up if the next frame takes the usual 715 lines (2 or 3 VBlanks). Unit-tested against a simulated frame clock: 99.96 % exact; the rest are VBlanks that fall between the timer tick and the game's `PutDispEnv` |
| Hooks | `game.toml` `mod_function_entry_funcs`: `0x80067404`, `0x800535F4`, `0x80053DD0`, `0x80053284` (each hook checks the engine entry word and six code signatures first) |
| Counters | `moa.frame-rate.` `enabled`, `frames`, `passes`, `unpaired`, `unavailable` (and by reason: `unavailable_no_presenter` / `_backend` / `_disabled` / `_session` / `_fast_forward` / `_busy` / `_other`), `period_2`, `period_3_plus`, `camera_move_lt64` / `lt256` / `lt1024` / `le2048`, `camera_cut` |
| To check in a run | RENDER_PASSES.md "Porting a title" step 7: `render_pass_stats` (passes, no faults), `render_pass_dump` images between their neighbours, `PSX_RENDER_PASS_VERIFY=1` with `verify_mismatch` 0; the camera-move counters to set the cut limit from real motion. **CI builds are Release, so they have no TCP debug server** (`PSX_DEBUG_TOOLS` off): all of these need a debug-tools build (`-DPSX_DEBUG_TOOLS=ON`, as branch `mapping-debug` makes). First test is by eye |
| Expected look | the camera glides; ACs, shots and effects still step at the game's rate (and sit one step ahead of the camera inside a frame), until steps 4–5 blend them |

### Draw list (VS, FDAT 209) — static first cut

Read from FDAT 209 and `SLUS_010.30` (code loaded at `0x80011000`) on 2026-11-18 (step 3). What a
routine draws is named from the data it walks and what it calls: **nothing here is confirmed on
screen yet**, `?` marks guesses. Confirm with `tools/gpu_frame_layers.py` (`GPU_FRAME_TOOLS.md`),
which needs a debug-tools build.

Scene render `0x80067404`, one frame, both views:

| When | Routine | What it draws or does |
|---|---|---|
| before the swap | `0x80066E34` | **colour pulse**: 8 colours from `0x80090014` scaled by `rsin`(frame counter `0x801A8C34` × 512) into `0x80099FC0`, a table the mesh code reads: blinking lights `?` |
| before the swap | `0x8005BB40` | **scrolling texture**: when its counter `0x801A8CC2` runs out (reload `0x801A8CC1`), `StoreImage` + `LoadImage` move a VRAM rectangle by a step (settings from `0x801A8CC0`: mode, rectangle, step), using `malloc` / `free`: a stage texture that scrolls `?`. Inside a pass this VRAM write is outside the rect, so it is journaled and rolled back |
| per AC | `0x80059624(AC+0x68, AC+0x0E + 0x80, AC+0x6C, AC+0x76; AC+0x7A, AC+0x6D, AC+0x7C, AC+0x7E)` | **AC pose**: animation player → joint matrices (`RotMatrix`, 13 GTE commands), for each AC whose word +0 is set (`0x801D4BC0`, `0x801D4D38`). Step 4. Pool objects with a skeleton use it too (`0x80061F64`) |
| around it | `0x8005A72C`, `0x8005AC68` / `0x8005ACC8`, `0x8005A81C` (after the submit) | upkeep of the AC model records (AC +0x68: P1 `0x801A2E10`, P2 `0x801A2E38`) `?` |
| per view | `0x80067004` | for the eye's area and its neighbours, `0x80066E74` → `0x8005B96C` fills the list at `0x801ADC70` (≤ 16 × 48 bytes; next free `0x801ADF70`, count `0x801ADF74`) that the per-view draw transforms: per-area lights or effects `?` |
| per view | `0x80066B10` | **that view's HUD** (viewed player `0x8003EAA0`): gauges and numbers `0x80056754` (text `0x80065494`, icons `0x800572B4`, fade box `0x800632E8`), text `0x80057210`, icons at points put through `RotTrans` `0x80057B8C` / `0x80057E8C` / `0x80058208` (radar, markers `?`), text at a projected point `0x800580EC` `?`, sight `0x800576E0` |
| per view | **`0x80061C3C`** | **stage** (below) |
| per view | **`0x80065C44`** | **objects**: ACs, their shadows, stage objects, the object pool (below) |
| after both views | `0x800632E8`, `0x80065494`, `0x800659DC`, `0x800572B4` | fade box (POLY_F4, semi-transparent while fading), text, shared HUD, into OT B over both views |

**Stage, `0x80061C3C`.** Starts at the eye's area (`0x801A9AAC`; the 64-byte area records from
`0x801BDFD0` the camera's area check uses) and walks its **neighbour list** (area +0x26 count, +0x28
→ u16 entries: low 9 bits = area number; bits 11–13 name an extra entry after the list whose area,
when its +0x30 bit 8 is set, hides this one `?`; bit 14 is passed on).
- **Distance cut-off:** with area +0x30 bit 12 set, an area is drawn only while
  ((eye − area centre +0x10 / +0x12 / +0x14) ÷ 2)² ≫ 18 < area +0x34 & 0xFFF, i.e. distance below
  about 1,024 × √limit. Bitsets of areas looked at / drawn in this view: `0x801A8C38` / `0x801A8C78`
  (32 areas a word); the object draw only draws things in "drawn" areas.
- **One area, `0x80061700`:** skipped when +0x30 bit 11 is set or the box test `0x80061560(+0x18
  min, +0x20 max)` fails (view cone `?`). Model = table `0x801A2C0C`[area +0x1E], up to two levels
  of detail. Within 1,300 of the area's box the near level is drawn lit (light set `0x801A8D80` +
  (area +0x30 byte 0) × 104, `SetFogNearFar`) by `0x8006CAEC` / `0x8006D8EC`; otherwise the far level
  by `0x80060CB4`, or it is queued on two lists drawn at the end: `0x8005FE60` → `0x800600A0` and
  `0x8006861C` → `0x80067D00` / `0x8006882C`. With +0x30 bit 13 set, the far level is also forced
  outside the box grown by (+0x34 ≫ 8) & 0xFFF0. Both levels use the scaled view matrices from the
  camera step (far `0x801A9B10` family, near `0x801A9B90` family; level = −(area +0x16) & 3 `?`).
- For draw distance (step 7): these cut-offs and the near / far switch are where far geometry is
  dropped or simplified.

**Objects, `0x80065C44`.**
1. **ACs** (`0x801D4BC0`, `0x801D4D38`): if the AC's area (AC +0x16) was drawn and the view-cone test
   `0x8006647C(AC +0x08 position, +0x72, +0x74)` `?` passes: **AC model `0x80062C00`** (facing
   AC +0x0E + 0x80, model record AC +0x68; meshes through `0x8006C6F4`, `0x8005D9A4`, `0x8006A5A8`).
   **Shadow `0x800629E0`** `?` at (x, AC +0x2C, z) when the area at AC +0x30 was drawn (same facing
   and record).
2. **Stage objects:** 128 records of 52 bytes from `0x801D2EF8` (centre +0 / +2 / +4; +6 = −1 off;
   flags +0x1C: bit 15 off, bit 0 = drawn only while the eye is inside the box centre ± signed bytes
   × 256 at +0x22…+0x27, the area-box layout in `battle-system.md`). By flag: bit 1 → `0x80063648`,
   bit 3 → `0x80063004`, bit 5 → `0x80062F34`, otherwise a model through `0x80061F64`.
3. `0x800665BC` (24 GTE commands, view-cone test) `?`.
4. **Object pool** (256 × 228 bytes from `0x801D6F6C`), per kind, after the area bit and the
   view-cone test: `0x80061F64` (models, posed with `0x80059624`), `0x80061484`, `0x800645D8`,
   `0x800651E4`, `0x8006288C`, `0x80064EE4`, `0x80064C68` (sprites and effects `?`). Which kind is
   which: next, with the frame-layer tool or by reading each.

For render passes (steps 4–5): what moves per step is the AC pose (`0x80059624`'s joint
matrices), AC position and facing (+0x08…, +0x0E), shadow height (+0x2C), stage objects and pool
records. The colour pulse and the texture scroll step with the render's own frame counter, so a
pass already shows the next frame's value (harmless).

## 2. What the mapping switches on (the payoff)

Each row is a framework feature that already exists and is waiting on facts only we can supply.

| Feature (framework doc) | What we must find | What the player sees |
|---|---|---|
| **Render passes** (`RENDER_PASSES.md`; worked example V8:2's `v82_frame_rate_plugin.cpp`) | the draw section start/stop PCs, the pass point, the flip mode (DISPENV / DRAWENV use), the logic tick, everything the draw reads that moves: camera, AC positions / facing / joint poses, object pool, particles; how far each moves per step | Real in-between frames: 60 / 120 fps on screen while the game logic keeps its own speed. Unlike the generic "smooth motion" setting, nothing is guessed |
| **Native-wide projection correction** (`NATIVE-WIDE-PROJECTION.md`) | nothing; one call from our plugin (`psx_mod_set_native_wide_projection_correction(1)`), then a test | Widescreen without folded or stretched polygons at the screen edges; faces crossing the camera are clipped properly |
| **Game culling at the 4:3 edge** (`WIDESCREEN.md`) | the screen-edge / view-cone tests in the stage, AC and object draw routines | Widescreen with nothing missing at the sides |
| **Draw-distance clamps** (`[[draw_distance.clamp]]`, `ENHANCEMENTS.md` DD1) | the ordering table (OT) size and its range guards; any distance cut-offs before that | Far geometry kept instead of popping in. **Catch:** the framework supports main-executable code only, and our draw code lives in the engines (FDAT overlays): needs either a framework change or our own patch of each engine |
| **HD textures** (`HD_TEXTURE_PACKS.md`, texture banks in `RETAINED-TERRAIN-EXPERIMENT.md`) | which VRAM page holds what during a fight (mostly known, see §1), how paint / pre-painted sets change the pixels | Dependable texture packs per part and stage; emblems and paint at higher resolution |
| **Native assets** (immutable texture banks + trusted packet arenas, `RETAINED-TERRAIN-EXPERIMENT.md`) | where each AC part's primitives are built (so ours can go in their place) | Higher-detail or brand-new parts drawn through the game's own pipeline: first step toward the custom engine |
| **Stereo / VR, spectator views** (`STEREO_RENDERING.md`, local view) | the same draw boundary as render passes | Free cameras for AI Battle spectating, photo mode; VR later if wanted |

Available today without any mapping: the generic smooth motion / frame blending and the render
thread (`FRAME_GENERATION.md`, `FRAME_RATE.md`; already in the launcher's settings), PGXP, and the
framework's bloom and post shaders (`POST_EFFECTS.md`; config settings).

## 3. Work order

VS (FDAT 209) first: it is the battle lane's test setup and both units are under direct control.
Then the other engines look for the same routines (§4).

1. **Frame anatomy.** Known so far: the cadence and the frame-end wait (§1), the library
   routines (§1). Still to read in FDAT 209, from the VS main loop's "draw both views": the scene render (the link
   engine's is `0x80067FE4`; find VS's), the buffer record (link `0x8019D4F0`), DRAWENV / DISPENV,
   where `DrawOTag` and `PutDispEnv` run, the VSync wait, how many VBlanks one step takes, the OT
   address and size, the primitive buffer. How split-screen draws two views (two OTs or one?).
   *Deliverable:* a table of these + the render-pass point and flip mode. **Done for VS** (§1 "Frame anatomy (VS)").
2. **Camera.** What the GTE is fed per view: rotation matrix, translation, projection distance
   (H), screen offset; which bytes of the camera record and smoothing state produce them.
   *Test:* a camera-only render pass in VS (in-between camera, everything else still).
   **Mapping done for VS; first milestone passed 2026-11-18** (user test, merged; §1 "Camera
   (VS)", "First milestone").
   **First milestone:** proves the 60 fps road on this game.
3. **Draw list.** Every routine the scene render calls, named by what it draws: stage, ACs,
   object pool, effects, shadows, HUD, radar, text. *Method:* `tools/gpu_frame_layers.py`
   (`GPU_FRAME_TOOLS.md`) gives one image per guest function from one captured frame.
   **Static first cut done for VS** (§1 "Draw list (VS)"); still to do: which pool kind is which
   routine, and the on-screen check with the frame-layer tool (debug-tools build).
4. **ACs in battle.** How AC +0x68 (model data) and the animation player (+0x6C) become joint
   matrices each frame, and where those land; tie them to the decoded skeletons (`acr_model.h`).
   Paint and the pre-painted texture sets (`0x800C0D14`, byte 14).
5. **Objects and effects.** Per pool kind: how it is drawn, its blend mode, any particle lists
   outside the pool. Moving values and their identity, for the render-pass blending.
6. **Stage.** The geometry format in FDAT 5s+1 / 5s+4, how it is split up, what decides which
   pieces are drawn. (Collision stays with the battle lane: AC +0x2C / +0x30.)
7. **Culling and sorting.** OT depth formula and range guards, screen-edge tests, distance
   cut-offs, object limits. Feeds widescreen and draw distance.
   **Second milestone:** full render passes in VS (ACs, objects, camera), widescreen with no gaps.
8. **HUD.** Sight `0x800576E0`, gauges, radar, text: positions and anchors, so widescreen can
   pin them to the screen edges and new HUD pieces can be added.
9. **Other engines** (§4), then the menus (hub ring models, garage AC) last.

## 4. Engines

The draw code is in each fight engine (loaded at `0x80050DD8`), so each has its own copy at its
own addresses. Find each routine by its first instructions (as `moa_link_netplay.c` checks
`0x27BDFF98, 0xAFB20060` for the link render) and list the addresses per engine.

| FDAT | Engine | Status |
|---|---|---|
| 209 | split-screen VS | first (§3); frame anatomy done (scene render `0x80067404`, swap `0x8005BFB8`, pass point `0x8005C050`) |
| 210 | arena (also AI Battle) | second: the mode most players see |
| 208 | link versus (netplay) | scene render, camera and buffer record known (§1) |
| 207 | missions | after arena; per-mission code (FDAT 0–99) may add its own draws |
| 211 | VS replay | last |

## 5. Lanes

Lane claimed in `README.md` 2026-11-18. Borders:
- Battle lane owns what the AC object fields *mean* in play; this lane reads them for drawing.
- Data lane owns the camera record's setup fields (`stat-block.md`); this lane owns how the
  camera turns into a view.
- Boot lane owns what is loaded into VRAM; this lane owns how VRAM is used while drawing.
- Stage *geometry for drawing* is claimed here; stage collision and area boxes stay with battle.
- Mission maps (FDAT 0–99) stay with the missions lane. If they turn out to use the stage
  geometry format, note it in its "Handed over" list.

## 6. Watch out for

- **The frame period is not a whole number of VBlanks.** 715 scanlines is about 2.72 NTSC fields,
  so stock frames stay on screen for 3, 3, 2, 3… VBlanks (uneven), and heavy frames run longer.
  The render-pass planner counts whole VBlanks per frame: the plugin must work out each frame's
  period from the flips it sees. In-between frames would lift ~22 fps to 60+ and even out that judder.
- **Performance:** render passes re-run the game's draw code, which costs CPU. The Steam Deck
  already struggles (15–17 fps in the AppImage), so in-between frames may be a desktop feature
  until the Deck runs the stock game at full speed.
- **Netplay:** passes are off in netplay unless the title opts in after a verified boundary; the
  link engine's guest view already uses that same sandbox, which helps.
- **Fast-forward:** passes are off while fast-forwarding (AI Battle's setup); fine, nothing is shown.
- **Device reads in the draw section** (sound upkeep) abort a pass: filter them as the link
  engine does.
- **Split screen** draws two views per step: a pass must redraw both.
