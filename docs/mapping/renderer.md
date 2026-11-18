# Master of Arena — Renderer Mapping Plan

Goal: know how every frame of a fight is drawn — where the camera comes from, which routine
draws what, how it is sorted and culled — well enough to (a) switch on the framework's
enhancement features that need a title's draw code mapped, and (b) later draw parts of the game
natively (the custom-engine road).

Disc: USA disc 1 (SLUS-01030 v1.0). Written 2026-11-18 from the repo (master) and the
framework docs in `psxrecomp/docs`. Nothing below is measured yet unless it says where it came
from. Status: lane claimed 2026-11-18; step 1 started (main program routines named, frame
cadence from Armored-CoreR). Next: read FDAT 209 (needs the `ma-work` disc pack).

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
| `SetGeomOffset(x, y)` / **`SetGeomScreen(h)`** | `0x80029D24` / `0x80029D48` | GTE OFX/OFY / H. Split screen must change these per view |
| `SetDQA` / `SetDQB` | `0x80029CCC` / `0x80029CD8` | depth cueing (fog) |
| Matrix multiplies (load GTE R, then MVMVA) | `0x80029448`, `0x80029648`, `0x800297A4`, `0x80029904`, `0x80029954`, `0x800290C4`, `0x80029E98`, `0x80029EF0` | not told apart yet |
| VBlank callback | `0x800166A0` | counts VBlanks at `0x80043C74`, then `0x80017340`, then the hook at `0x80043C78` if set |
| VBlanks since a0 / VBlank count | `0x800166FC` / `0x80016710` | read `0x80043C74` |
| Scanlines since a0 (the frame-time wait) | `0x80016724` | `GetRCnt(RCntCNT1)` (`0x8002B600`), 16-bit wrap |
| `DrawSync(0)` then `VSync(0)` | `0x80016678` | |

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
   *Deliverable:* a table of these + the render-pass point and flip mode.
2. **Camera.** What the GTE is fed per view: rotation matrix, translation, projection distance
   (H), screen offset; which bytes of the camera record and smoothing state produce them.
   *Test:* a camera-only render pass in VS (in-between camera, everything else still).
   **First milestone:** proves the 60 fps road on this game.
3. **Draw list.** Every routine the scene render calls, named by what it draws: stage, ACs,
   object pool, effects, shadows, HUD, radar, text. *Method:* `tools/gpu_frame_layers.py`
   (`GPU_FRAME_TOOLS.md`) gives one image per guest function from one captured frame.
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
| 209 | split-screen VS | first (§3) |
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
