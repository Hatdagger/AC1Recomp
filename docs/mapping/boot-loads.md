# Master of Arena — what the game loads, step by step

What is read from the disc, what goes into video memory (VRAM) and what state is set at each
step: power-on, title, Scenario (Load Save / New Game) → hub, VS setup, VS START, arena launch,
and the fight engines. Goal: anything that starts a mode directly (launcher, Rogue Arena, AI
Battle, netplay) can do exactly what the game does, and no step is skipped by accident.

Disc: USA disc 1 (SLUS-01030 v1.0) only. Disc 2 is not used (it has no NEW GAME). Read
statically 2026-11-17 from the boot exe and FDAT 206 (menus), 209 (VS), 210 (arena). Measured
from code unless marked `?`. Related: `mode-launch.md` (how each mode is started),
`menus-hub.md` (the hub ring).

## The rules that explain most bugs

1. **Each module load wipes the module area.** The menu clears 0x80050DD8 … 0x801FC898 before
   it loads (arena the same, VS / missions / link to 0x801FE000). Menu work memory (tasks
   0x801B1CE8, model slot table 0x801FBB00, card list 0x801F9DB8, 0x801B1CC8 …) is new every
   time. Only the exe image and its bss survive, 0x80011000 … 0x80050DD8: garage 0x80036FB0,
   owned parts 0x80036FEC, flags 0x8003EAxx, player blocks 0x80045198 / 0x80045538, VS staging,
   settings.
2. **VRAM is never cleared**, and the fight engines **do not load part textures**. The AC's
   textures must already be in VRAM when a fight starts. The menu loads them; the fight uses
   whatever is there. A skipped texture load is not a crash. The AC just draws wrong or not at
   all until some later path loads the textures.
3. **Loads are queued.** Most menu loads go into a disc queue and finish over the next frames.
   The game waits with `0x800589DC` (non-zero = still busy) before it relies on them. A direct
   start that runs the same routines from a hook must let the menu loop drain the queue (or
   wait) before it opens the next screen. **Fast Loading** (the 16-sectors-a-frame drive,
   `mod_builtin_moa_loading.c`) only covers the initial load since 2026-11-18: it goes back to
   the drive's real speed once the first usable screen (hub open `0x800552C0` or title open
   `0x800AAE94`) has opened and the disc queue has stayed empty for 30 frames.
4. **Anything sent to VRAM must sit in main RAM.** LoadImage / StoreImage go by DMA, which
   only reaches main RAM (0x80000000 … 0x801FFFFF). Mod memory from `psx_mod_alloc_guest_memory`
   (0x81000000) folds onto low RAM there, so an upload from it sends whatever code sits at the
   folded address (seen as a garbled emblem, which the game's next SAVE then wrote back to the
   card). Copy the data into a menu heap buffer first (`0x800583A0(bytes)` → buffer,
   `0x800583F8(buffer)` frees it), as LOAD's own card buffer does. In code: `moa_quick_start.c`.
5. **Hooks on menu code run only if listed.** A mod hook on a routine in a disc-loaded module
   (menu, engines) is only called if its address is in `game.toml` `[recompiler]
   mod_function_entry_funcs` (and the code is regenerated). Unlisted, the hook silently never
   fires. Rogue Arena's old X-at-hub hook `0x80055848` was missing for this reason.
6. **A save made outside the game needs its list summary.** The DATA SAVE / LOAD list reads a
   summary before the checksummed range (menu save writer `0x80071ED8` … `0x80071FAC`): +0x184
   AC and pilot names (36 bytes, as at +0x1CC), +0x1A8 u16 sorties, +0x1AC u32 credits. Left
   zero, the list shows the save with no name until the game saves over it. In code:
   `save_summary()` in `moa_card.c`. A new game's state is needed too (§9a).

## Load routines (to read the tables below)

| Routine | What it does |
|---|---|
| `0x80016A58(archive, entry, dest)` | exe: synchronous read of an archive entry |
| `0x800585BC(archive, entry)` | menu: synchronous read into a new heap buffer |
| `0x80057CB0(archive, entry)` | **PICTURE**: sync read, every TIM in the entry → VRAM (`0x80057DD0`), DrawSync, free |
| `0x800586A0(archive, entry)` | **TIM queue**: async read, then the TIMs → VRAM (callback `0x80058A34`) |
| `0x80057F74(slot, entry)` | **MODEL**: async read of MENU_TMD entry into slot (table 0x801FBB00, sizes 0x801D0B28) |
| `0x80058700(n)` | **SOUND**: MENU_VAB entries n and n+1 (bank header + data), callback `0x80058828` |
| `0x80058644(archive, entry, cb)` | async read with your own callback |
| `0x800589DC()` | disc queue busy (exe queue at 0x80043FE8 / 0x80043FEC) |
| `0x8002C75C` / `0x8002C7BC` / `0x8002C81C` | exe LoadImage(rect, src) / StoreImage(rect, dst) / MoveImage(rect, x, y) |
| `0x800575B8(group)` / `0x80057B28(group)` | new menu task / kill every task of a group |
| `0x80057470(open_fn)` | find the live task opened by `open_fn` (0x801D0BA0 = none). "Wait until X is gone" |

Archives (registered at boot, numbers used everywhere): 1 RTIM (loading pictures), 2 FDAT
(code), 4 MENU_TIM, 5 MENU_TMD, 6 MENU_VAB, 7 CORE_T, 8 LEG_T, 9 ARMS_T, 10 HEAD_T, 11 BST_T,
12 BWL_T, 13 BWR_T, 14 WEL_T, 15 WER_T, 16 COMP_T, 17 SPEC_T, 18 GENE_T, 19 MIS.T.

### Where things sit in VRAM (from the TIM headers on the disc)

Sizes are in VRAM units (16-bit words): an 8-bit picture is twice as wide in pixels, a 4-bit one four times.

| What | VRAM |
|---|---|
| **Player's AC textures** (`*_T` archives) | core (704,256) 64×128, legs (704,416) 64×96, head (768,352) 64×32, arms (768,256) 64×96, booster (704,384), back L (768,448), back R (768,480), arm weapon L (768,384) + (320,384), R (768,416); CLUTs on rows 487–494 at x 0 |
| MENU_TIM 0 | (448,0) 64×216, (416,0) 16×64, (368,96), (960,0) 64×255 + CLUTs (0,481), (368,224/225), (960,255) |
| MENU_TIM 1 | hub banner icons (96×48 px) at x 320/368 (y 0–192), (512,0) 64×256, (672,0), (368,192) + CLUTs row 480 / 229–232 |
| MENU_TIM 2 | (640,0) 128×96 16-bit |
| MENU_TIM 3, 875 | full screen 320×240 16-bit at (320,0) (title pictures) |
| MENU_TIM 4 | four 64×256 pages at (576,256), (640,256), (896,256), (960,256), CLUT (320,240) |
| MENU_TIM 6 | (960,0) 64×48 |
| MENU_TIM 7 | (768,0) 160×144 8-bit, (704,0) 64×256, (960,0) 64×240 |

`0x800C0E7C` swaps the player 1 and player 2 texture sets (pages in the 704–831 / 256–511 area,
CLUT rows 486–495) through scratch space at (960,0). VS START and the arena use it.

## 1. Power-on (boot exe)

1. crt0 `0x80011D84`: zero 0x8003EA88 … 0x80050DD8, gp = 0x8003EA28.
2. main `0x80011E40`: **zero all RAM 0x8003EA90 … 0x801FE000**, library setup.
3. `0x800124B0` once: graphics, pads `0x80016C10`, sound `0x80016DB8`, register RTIM, **loading
   picture RTIM 62**, register archives 2–19.
4. Load FDAT 206 → 0x80050DD8 and call the menu `0x800A69C8`.

Initial values from the exe image (not zeroed): credits 0x8003EA74 = 100,000;
**0x8003EA7A = 1** ("go to the title"); 0x8003EA79 = 1; mode 0x8003EA2A = 0.

## 2. Every menu start (`0x800A69C8`, before any screen)

Runs on power-on **and after every fight**:
- Display/draw environments, task list init (`0x800A698C(0x801B1CE8)`), `0x8005803C`,
  `0x80058098`, `0x80058134`, `0x80057ED4`.
- **TIM queue MENU_TIM 0.**
- **0x80044121 = `0x80058B04()`** = which disc is in the drive (reads `\GG\COM\CD.DAT`, magic
  0xBA; 0 = disc 1, 1 = disc 2, 250+ = drive errors). Disc 1 → SCENARIO opens NEW/LOAD;
  disc 2 → card load at once. We only use disc 1.
- `0x800122F0`, clear screen, `0x80063944`.
- If 0x8003EA7A (first boot / back to title): `0x800B69F8` = **settings defaults**
  (0x8004B568 ← 0x800D5860, 48 bytes; sound flags 0x80044200/02). Always `0x800B6A80` =
  **apply settings** (button layout into player block +0x250…, sound).
- Then the resume checks in `mode-launch.md` §3 (VS menu, link menu, back from mission, back
  from arena, or title).
- Title branch only, after the title opens: 0x8003EA7A = 0, **StoreImage (416,0,16,64) →
  0x8004ACA8 and (368,225,16,1) → 0x8004B6F0**. These hold part of MENU_TIM 0, and the hub open
  and the VS menu put them back.

## 3. Title (`0x800AAE94` and its states)

| State | Loads / does |
|---|---|
| open `0x800AAE94(skip)` | new task. skip = 1 (0x8003EA7F): **TIM queue MENU_TIM 875**, go straight to `0x800AB014` |
| `0x800AAF30` | wait for the disc queue; the first time → logos `0x800AAA8C` |
| `0x800AAA8C` | **TIM queue MENU_TIM 875**, draws the logo screens with their own loop |
| `0x800AAFB4` | intro movie `0x800544A0(n, 1)` (n alternates 0/1 on each attract loop). Inside: TIM queue MENU_TIM 2, `0x80054B54` movie, then `0x800B6A80` again |
| `0x800AB014` | **TIM queue MENU_TIM 3** (title picture) |
| `0x800AB04C` | wait for the disc, title widgets `0x800ACC34` ×2, `0x800ACCF4` |
| `0x800AB0C8` | PRESS START: random seed `0x800A7B80` (0x8003EABC), 0x8003EA79 = 0; idle → TIM queue MENU_TIM 2, back to the movie |
| `0x800AB2FC` → `0x800AB4F0` → `0x800AB500` | SCENARIO / VS rows, then the choice |

**SCENARIO picked (`0x800AB500`, row 176):**
1. 0x8003EA94 = 255, mode = 0.
2. `0x800631FC`: **garage = the scenario starting AC** (parts 0x800CBA0C, options 0x800CBA18,
   owned parts 256 bytes from 0x800CBA1C).
3. `0x800BA854`: arena tables reset (0x8004B5A0 from 0x800D5A08, arena lock 0x8004B686).
4. `0x800C566C`: VS staging area at 0x80045958 / 0x8004A7C4 reset.
5. `0x800B69F8` settings defaults, `0x800B6A80` apply.
6. 0x8003EAC8 … 0x8003EACF = 0 (Human PLUS, Ex Arena, overweight flags).
7. Kill task group 2; **PICTURE MENU_TIM 0, 1, 4** (synchronous, straight to VRAM).
8. Disc 1 → NEW GAME / LOAD GAME menu `0x800AC3AC`.

## 4. Scenario → hub

### 4a. LOAD GAME

1. NEW/LOAD menu, cursor 0 + ✕ (`0x800AC4B4`): kill group 4, dialog, then `0x800AC8E8` →
   card screen `0x80059554(task, callback 0x800AC93C)`. **The card screen loads nothing from the
   disc.** It draws its windows by moving VRAM blocks (`0x80065D00` / `0x80065DD4` at x 960).
2. LOAD `0x80073424` ('N' save): checksum, inline unpack into the exe area, **LoadImage emblem
   (544,384) and its CLUT (256,510)**, then `0x800B6A80`, `0x80099098`, `0x800941F4`,
   `0x80093BC4`, `0x800A2318`, `0x800A2020`, 0x8003EA82 = 1.
3. **The card screen's finish `0x8005CE68`** (runs only when LOAD succeeds):
   - owned parts 0x80036FEC → **0x8003EB20** (256 bytes)
   - `0x80090FD8` part lists, `0x80083130` defaults for empty required slots (both in §4d)
   - opened from the title: `0x80067244(1)` + **`0x800822B0` = queue the AC's part textures**
     (from the hub's DATA screen: `0x80081E20` garage rebuild as well)
   - **wait for the disc queue**
   - back weapon id ≥ 52: MoveImage (384,256,64,64) → (768,448), (320,482,256,1) → (0,493) /
     (0,494)
4. Callback `0x800AC9E8`: **open the hub** `0x800552C0`, delete the card-screen task.

### 4b. NEW GAME

1. NEW/LOAD menu, cursor 1 + ✕: **`0x800822B0` (AC textures)** + **TIM queue MENU_TIM 7**,
   kill group 4 → `0x800AC820` → `0x800ACA0C`.
2. `0x800ACA0C`: credits = 100,000, `0x80068A58`, `0x80064918`, `0x80068D7C`, name entry
   `0x8008E8D8` (callback `0x800ACA88`).
3. `0x800ACB10`: **open the hub**, then `0x800941F4`, `0x800A2318`, `0x80099098`, `0x800A20D8`.

### 4c. The hub opening (`0x800552C0`, every way in)

- New task (open `0x800552C0`, first state `0x800554F4`). Hub entry +0xBE: 5 or 6 after an
  arena fight (Ex Arena when 0x8003EAD4 ≠ 0), 7 when 0x8003EA7D = 5, else 0.
- **LoadImage 0x8004ACA8 → (416,0,16,64), 0x8004B6F0 → (368,225)**: the blocks the menu saved
  after the title.
- `0x80063B54` full-screen layer, `0x80056564` (stored at +0x80).
- Back weapon id ≥ 52: MoveImage (768,448,64,64) → (384,256), (0,493,256,1) → (320,482).
- `0x80068A58`, then the hub ring task `0x80056DBC`:
  - **SOUND MENU_VAB 2 + 3** (`0x80064F24(2)`)
  - **TIM queue MENU_TIM 1 and 4**, **MODEL slot 15 ← MENU_TMD 1**
  - waits for the disc, then `0x80081E48` = **AC part models** into slots 3–14 (from the
    garage, 0x80036FB0), **MODEL slot 16 ← MENU_TMD 2**, waits again.
- **The hub never loads the AC's textures.** Whatever ran before it must have queued
  `0x800822B0` (LOAD finish, NEW GAME, or the arena return path).

### 4d. The AC helpers in the finish (2026-11-18)

None of them changes a full, legal AC, so a real save (or a launcher save with a legal build)
comes through untouched.
- **`0x80090FD8` = owned-part bitmasks.** For each part kind (mask pointers `0x800CDE44`, part
  counts `0x800CDE6C`), bit i is set when owned copy 0x8003EB20 holds part i of that kind
  (id from `0x800996AC(kind, i)`). Masks at 0x8004B508 … 0x8004B560 (8 bytes each). The garage
  and shop lists read them.
- **`0x8008859C(kind)`** = the first owned part of a kind (lowest set bit), 255 if none.
- **`0x80083130` = fill empty slots.** Each garage slot that is 255 gets `0x8008859C` of its
  kind. Then the combination rules, run on every AC: tank legs (id ≥ 27) → booster 255, and a
  back weapon 40–42 / 44 / 46 on either side is removed `?` (meaning not read); hand slot 15
  checked against the legs through a jump table (0x80052358) `?`. A build the game itself made
  never breaks these, so nothing changes.
- **`0x80067244(flag)` = stand-ins for empty slots.** Only for slots that are 255: MODEL
  queue of the first owned part of that kind (MENU_TMD entry + per-kind offset 3, 14, 54, 78,
  96, 202, 237), TIM queue of its textures (same entries as §7, texture set from
  **0x8003EA72**, the player's own byte 14), then `0x80083130` when flag ≠ 0. A full AC loads
  nothing here; its models and textures come from the hub open and `0x800822B0`.

## 5. VS setup (`0x800AB500` row 192 → `0x800B0CB4`)

- Row: `0x80074DC0` (display env 0x800CD3B4), `0x800633A4` = **garage = the VS default AC**
  (0x800CBC1C tables, all parts owned), 0x8003EAC8…CF = 0, kill group 2, PICTURE MENU_TIM (entry `?`, likely 0 as on the SCENARIO
  row), mode = 2.
- `0x800B0CB4`: about 20 MoveImage calls build the VS screen from blocks already in VRAM
  (x 0x380 / 0x2C0 and the CLUT rows 0x1E7 / 0x1F0), **TIM queue MENU_TIM 6**, first visit
  copies the garage to both players (see `mode-launch.md` §5).
- First state `0x800B0FE4`: StoreImage (`0x800637A8`) → 0x8004B6F0 / 0x8004ACA8, **SOUND
  MENU_VAB 2 + 3**, **TIM queue MENU_TIM 1 and 4**, then (`0x800B15D8`) **MODEL slots 15 / 16
  ← MENU_TMD 1 / 2**, then **TIM queue MENU_TIM (stage byte − 10)** = the **stage preview
  picture** of the chosen map (stage byte = `0x800D5800[map − 1]`, map = 0x8003EA81; entries
  10–13, 20, 23–27; each 104×100 px 8-bit at (896,0), CLUT row 506).
- Each player's AC (AC SETUP / DATA LOAD) loads its own models and textures. AI Battle's
  `moa_load_unit` already replays this with `0x80081E20` and the per-player file tables
  0x8004AB60 / 0x8004A9E0.

## 6. VS START (`0x800B2D7C` → `0x800B2E90` → `0x800B2F9C` → `0x800B348C`)

**No disc reads.** Per player: extras → 0x8003EAC8, build check `0x8007E164`, options →
0x8003EA70, **compile `0x800A7BB4(p)`**, **`0x800C0E7C` texture-set swap**, window moves,
weapon blocks MoveImage → (768,448), (0,493), (0,494). Then stage (player block +0x255), music
0x8003EA29, 0x8003EA94 = 0, **0x801B1CC8 = 1** → the exe loads FDAT 209.

## 7. Arena launch (menu)

- **Opponent's textures (2026-11-18):** state `0x800C028C` waits for the disc, swaps the
  texture sets `0x800C0E7C` (the player's set moves to the player 2 area), then
  **`0x800C0D14(records, n)`** queues the opponent's textures from its 64-byte arena record
  (records from MENU_TIM, n = arena list row +8), then sets state `0x800C030C`. TIM queue per part,
  v = record byte 14:
  | Archive | Entry |
  |---|---|
  | CORE_T (7) | +0 + 11v |
  | HEAD_T (10) | +1 + 18v |
  | ARMS_T (9) | +2 + 24v |
  | LEG_T (8) | +3 + 40v |
  | WEL_T (14) / WER_T (15) / BWL_T (12) / BWR_T (13) / BST_T (11) | +5 / +4 / +6 / +7 / +12, skipped if 255 |

  Each `*_T` archive holds 16 complete texture sets (CORE_T 176 = 11 × 16, HEAD_T 288 = 18 × 16,
  ARMS_T 24 × 16, LEG_T 40 × 16). **v = 0 (all but 12 opponent records)**: set 0, colours built from
  the record's paint at +16 (`0x800C14AC`, LoadImage into the AC CLUT rows). **v ≠ 0 (12
  opponents, v 1–15)**: a pre-painted set, colours = **MENU_TIM 26 + 4v + record byte 15**
  (CLUT row 486 + rows 488–490).
- Pre-fight `0x800C030C` / `0x800C0AD8`: wait for the disc, compile `0x800A7BB4`, swap
  `0x800C0E7C` again (the opponent's set goes to the player 2 area, the player's comes back).
- Launch `0x800C0878`: kill group 2, `0x80093C20`, `0x800A20D8`, `0x80093BC4`, 0x8004AC50,
  **0x8003EA7C = 1**, arena 0x8003EAD4 / opponent 0x8003EAD5, frees three heap buffers, stage
  from 0x800D59F0, music, **0x801B1CC8 = 1** → FDAT 210.

## 8. The fight engines (FDAT 209 VS, FDAT 210 arena)

Both load the same way (VS addresses first, arena second):

| Load | VS (209) | Arena (210) |
|---|---|---|
| Body archive: registers **archive 0 = `P0\PAnn.T`** (nn from player block +0x279, the body combination) and reads entry 1 | `0x800552B8` | `0x80055AF8` |
| AC part models from that archive, one per part (arena: entry = part + a per-slot offset 2, 15, 57, 127, 81, 99, 155, 215; VS loader not read in detail) | `0x80058548` | `0x8005A76C`, from `0x80055B54` … `0x80055D9C` |
| Loading pictures RTIM 61 and RTIM [stage picture byte] | `0x80055990` / `0x800559A8` | `0x80056218` / `0x80056230` |
| Shared battle data **FDAT 205** | `0x800559C4` | `0x8005624C` |
| Stage: FDAT 5s+1 and 5s+4 (VS) / 5s+2 and 5s+4 (arena), s = stage | `0x80055AA8` / `0x80055AC8` | `0x80056330` / `0x80056350` |

**No `*_T` texture archive is read by either engine.** The AC (and the opponent) are drawn
with the textures the menu left in VRAM, which is rule 2.

## 9. Direct starts: what each must replay

| Direct start | Must not skip |
|---|---|
| Scenario via Load Save (Rogue Arena) | §3 SCENARIO steps 1–7; LOAD unpack + emblem + six calls; **§4a step 3 (card screen's finish), above all `0x800822B0`**; then the hub open, letting the menu loop drain the queue |
| Scenario via New Game | §3; §4b step 1 (`0x800822B0` + MENU_TIM 7); §4b steps 2–3 |
| VS without menus | §5 (the VS menu must have opened once: the StoreImage blocks, sound bank, MENU_TIM 1/4/6, model slots); each player's AC models + textures (`moa_load_unit`); §6 |
| Arena fight | the player's textures in VRAM (rule 2); the opponent's set: swap, `0x800C0D14`, wait, swap (§7) |

In code: `games/master-of-arena/moa_quick_start.c` replays the Load Save and New Game rows for
the launcher's START GAME menu (LOAD GAME / NEW GAME / LAUNCH GAME with FULL BOOT).

## 9a. What a save must hold besides the AC (2026-11-17)

LOAD copies the save into RAM unchanged, so a save made outside the game must carry a new
game's state, not zeros:
- **+3232, 44 bytes → 0x8004AC50** (mission progress). After the unpack, `0x800941F4` resets
  the mission screen's table T = 0x8019F7A8 and `0x80093BC4` copies 0x8004AC50 back over
  T+0…T+43. **T+1 = the slot whose briefing reopens at once (-1 = none).** Zero there means
  "slot 0", so MISSIONS skips the list start (and the mod's mission source picker). New game:
  FF FF 00 00 01 (T+0 = -1, T+1 = -1, chapter 0, T+4 = 1).
- **+3553 → 0x8004B67D = 0x8004B5A0 + 221**: opponent counts of the 9 arenas (from
  0x800D5A08: 50 10 10 10 10 16 31 10 20), **+3562 → 0x8004B686 = arena lock (2)**. Zero =
  empty arenas. +3607 → 0x8004B6AB: the rest of the arena state.
- +896: shop stock (1 = on sale, cleared when bought).
The full list is `games/master-of-arena/moa_arena_run_save.inc` (read from a fresh NEW GAME
save). `moa_card_to_save` starts from it since branch `load-game-fixes`.

## 10. Open

- ~~Hub background black after the direct LOAD GAME~~ fixed 2026-11-18 (merge of
  `hub-background-direct`, user-tested): the StoreImage from VRAM left the hub's RAM copies
  0x8004ACA8 / 0x8004B6F0 black in the recomp, so `moa_quick_start.c` now writes MENU_TIM 0's
  tile (VRAM (416,0) 16×64, 4-bit) and its 16 colours ((368,225)) from the disc, as Rogue
  Arena does with its belt tiles. Mission picker fix (§9a) user-tested OK 2026-11-18.
