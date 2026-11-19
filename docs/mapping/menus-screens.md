# Master of Arena — menus and screens

Lane: **Menus and screens** (boot chat, see `README.md`). Disc: USA disc 1 (SLUS-01030 v1.0).
Read statically 2026-11-18 from FDAT 206 (the menu module, loads at `0x80050DD8`; file offset =
address − `0x80050DD8`) and the boot exe. Addresses are measured from the code unless marked `?`.
Related: `menus-hub.md` (the hub ring and its 8 entries), `boot-loads.md` (what each step loads,
LOAD / SAVE, arena launch), `mode-launch.md` (title, VS START).

Goal: know how every screen behind the hub opens, what it loads, how it runs and how it closes,
so a mod can replace a screen with an overlay, reuse the game's windows and sounds, or move a
screen into the launcher.

## 1. How a menu screen works

### 1a. Tasks

Everything on a menu screen is a **task**: a 240-byte record that the menu's frame loop runs
every frame. A screen is one main task plus helper tasks (windows, text lines, sprites, models).

| Field | Meaning |
|---|---|
| +0x00 | in use (1) |
| +0x01 | **kill flag**: set to 1 to delete the task at the end of the frame |
| +0x02 … +0x06 | draw switches `?` (+0x04 and +0x05 both set: the matrix at +0x24 is applied) |
| +0x08 | group (u16, the argument of the create call) |
| +0x0A | **draw kind** (u16): 0 model, 1 `0x8006909C`, 2 `0x800620D4`, 3 / 9 `0x8006EAEC`, 4 `0x8006EE14`, 5 window frame `0x8006E230`, 6 `0x8006F104`, 7 **text** `0x8006F6D4`, 8 `0x8006E5B4`; dispatcher `0x800A7634` |
| +0x0C / +0x0E | kind 0 / 2: model object and model slot. Text: string pointer |
| +0x1C / +0x20 | the group's two primitive buffers (double buffered) |
| +0x24 … +0x43 | matrix (from `0x800CB74C`) |
| +0x6C | the group's draw layer (struct at `0x801B1A88` + 56 × layer) |
| +0x70 | **parent task**: when the parent is gone, the task is deleted too |
| +0x74 | the routine that opened it (its identity; `0x80057470` searches on it) |
| +0x78 | **state**: called every frame with a0 = task. Screens move on by storing the next routine here |
| +0x7C | destructor, called when the task is deleted (`0x80057C24` = none) |
| +0x80 … +0xEF | the task's own data |

| Routine | Does |
|---|---|
| `0x800575B8(group)` | new task in a group (returns the record, or `0x801D0BA0` when the group is full) |
| `0x80057470(open_fn)` | the live task opened by `open_fn`, or `0x801D0BA0`. Screens use it to **wait**: "until the window task is gone" |
| `0x80057554(group)` | the first live task of a group |
| `0x80057B28(group)` | set the kill flag on every task of groups 2 … 20 |
| `0x800579A0()` | delete every task now (destructors run). EXIT GAME uses it |
| `0x80057C2C` | the idle state (does nothing): a task parked here only draws |

**Groups and layers.** The task area `0x801B1CE8` … `0x801D0AF8` is cut into 22 fixed groups.
Each group draws on one of 10 layers. Group 0 holds 20 tasks, groups 1 and 2 hold 1 and 30, all on
layer 0. From there on each layer k (1 … 9) has an odd group with 2 tasks (2k − 1) and an even
group with 30 to 70 tasks (2k). Group 21 (8 tasks) is also on layer 9 and is used for things that
must sit on top ("Now Loading", `0x80080C60`). The game's own habit: a **window goes in the odd
group of a layer and its contents in the even group** (the yes/no prompt opens its window in group
g − 1 and its text in g). Contents seem to be placed relative to their layer's window `?` (the
prompt's text is at (8, 4)). Layer 9 is drawn last, so on top `?`.

**The frame loop** (inside the menu entry `0x800A69C8`, `0x800A7104` …): flip the draw buffer
(`0x801F9DB0`), clear the 10 layers' ordering tables, run every live task's +0x78 state in record
order, delete the tasks with the kill flag or a dead parent (destructor first), draw each layer
and show the frame. The loop ends when `0x801B1CC8` ≠ 0 (start a fight, see
`mode-launch.md`).

**How a screen closes.** It closes its windows (§1c), waits until `0x80057470(0x80064348)` finds
no closing window, sets its own kill flag (+0x01 = 1) and its helpers' (or kills their groups),
then calls **`0x80056528`** (back to the hub) or its caller's routine. Children go with their
parent through +0x70.

**Token tasks.** Some screens create a tiny idle task only so others can ask "is this page open?"
with `0x80057470`: `0x80068BEC`, `0x80068C3C`, `0x80068C8C`, `0x80068CDC`, `0x80068D2C`,
`0x80068D7C`, `0x80068EBC`, `0x80068FFC` (garage pages), `0x80068DCC` (VS menu), `0x80068F5C`
(Arena Maker). Which token means which page is not read yet `?`.

### 1b. Pad input (`0x801B1CB8`)

Sampled once a frame by `0x800639A4` (exe pad read `0x8001767C`). Both pads in one word: **player 1
in the low 16 bits, player 2 in the high 16 bits**, bits as in `stat-block.md` (Up 0x10, Right
0x20, Down 0x40, Left 0x80, L2 0x100, R2 0x200, L1 0x400, R1 0x800, △ 0x1000, ○ 0x2000, ✕ 0x4000,
□ 0x8000, START 0x08, SELECT 0x01).

| Offset | Meaning |
|---|---|
| +0x00 | latch: bits already reported. Screens OR a button in here after using it, so it is not seen twice |
| +0x04 / +0x06 | player 1 auto-repeat timers (the d-pad and L2 / R2 bits are cleared from the latch when they run out, so held directions repeat) |
| +0x0C / +0x0E | the same for player 2 |
| +0x14 | held now |
| **+0x18** | **pressed this frame** (held and not latched): what every screen reads |
| +0x1C | held last frame |

The sampler also stirs the random seed `0x8003EABC`. A task opened by `0x80068038` can feed its
own "last frame" word (+0x88) while its byte +0xBC is set `?` (looks like scripted input).

### 1c. Windows, text, prompts

| Routine | Makes | Arguments (stack words after a3) |
|---|---|---|
| **`0x80063EF4`** | window (frame + backdrop, opens with an animation), task kind 5, state `0x800642B0` | x, y, w, h; group, then two halfwords stored at +0x88 / +0x8A `?` |
| **`0x80064348`** | the same window **closing** (shrinks, then deletes itself) | the same x, y, w, h; group |
| **`0x80065330`** | one line of text, task kind 7 | colour `?` (+0x14), string, font, x; y, group. Font = the cell size: 16 (8 × 16), 8 (8 × 8), 6, 4. Characters are ASCII from 32, 32 per row of the font picture |
| `0x8006527C` | sprite from a 12-byte record (u, v, page, w, h, palette) | kind `?`, record, x, y; group |
| `0x800647B8` | sprite (other kind) | record, x, y, group |
| **`0x80066C08`** | message box: window with the text, tone 7. When it is dismissed, its window closes, it deletes itself and sets **task's state (+0x78) = next state** | task, next state, string, group; x, y, w, h, 0. Callers centre it themselves (20, 104, 280, 32 is the usual box) |
| **`0x80065858`** | **yes / no prompt** (window 88, 96, 144, 48; tone 7) | question, answer byte pointer, group, start on NO (≠ 0); answer byte = 2 while open, then **0 = yes, 1 = no**. ✕ on YES plays tone 2, NO or ○ tone 0 |
| `0x800B6550` | shop / garage message by number (window 32, 100, 256, 48; texts at `0x800B6658`: CAN NOT BUY, CAN NOT SELL, GENERATOR FIRST, ENERGY SHORT, No parts to sell., Not enough credits., No parts to buy., Pay back your debt first, …) | message number `?` |
| `0x80065D00` / `0x80065DD4` | copy window art blocks around VRAM x 960 (used when a screen builds its frame) | |
| `0x80063B54` | full-screen layer (the hub and detail pages put one behind them) | |

Screens open a window and wait for it with `0x80057470(0x80063EF4)` before drawing into it.

### 1d. Menu sounds

Every menu sound is **`SsUtKeyOn` = exe `0x800227D4`(vab, program, tone, note, fine, vol L, vol R)**
with vab = s16 at `0x801B1A80` (the menu bank, `MENU_VAB` 0), program 0, note 60, volumes 127.
Counted over all 361 calls in FDAT 206:

| Tone | Calls | Plays when |
|---|---|---|
| 0 | 45 | back / cancel (○) |
| 1 | 118 | cursor move |
| 2 | 52 | confirm (✕) |
| 6 | 99 | a window opens (follows almost every `0x80063EF4`) |
| 7 | 21 | a question or message box opens (yes / no, shop messages, "No Arena data exists.") |
| 8 | 1 | the hub ring is built again (`0x8005557C`) `?` |
| 9 | 2 | missions and mail page change `?` |
| 10 | 2 | **a part is equipped** (garage `0x80088340`, option parts `0x80092550`) |
| 12 | 4 | assembly slot list cursor (Up / Down in `0x800865B4`) |

The Rogue Arena work found tones 0, 1, 2 and 10 the same way (`moa_ui_sound.c` on its branch).

### 1e. Where each screen's code is in FDAT 206

| Range | What |
|---|---|
| `0x80050DD8` … `0x800544A0` | header, jump tables, strings |
| `0x800544A0` | intro movie |
| `0x800552C0` … `0x800572FF` | hub ring (`menus-hub.md`) |
| `0x80057470` … `0x80058B04` | tasks, disc loads, menu heap (`0x800583A0` / `0x800583F8`), disc check |
| `0x80059554` … `0x8005F8FF` | **System / card screen** |
| `0x800620D4` … `0x8006FFFF` | shared UI: windows, text, sprites, prompts, draw routines |
| `0x80070120` … `0x80073E28` | memory card files (list, SAVE `0x8007155C`, LOAD `0x80073424`) |
| `0x80074DC0` … `0x8007C6AF` | screen reset `0x80074DC0`, **emblem editor** |
| `0x8007C6B0` … `0x800939FF` | **garage and its pages** |
| `0x80093B18` … `0x80098FFF` | missions (missions lane) |
| `0x80099048` … `0x800A1FFF` | **shop** |
| `0x800A2020` … `0x800A69FF` | mail (missions lane) |
| `0x800A69C8` … `0x800A7BB4` | menu entry and frame loop, compile `0x800A7BB4` |
| `0x800AA000` … `0x800ACDFF` | title, NEW GAME / LOAD GAME |
| `0x800ACE14` … `0x800B0CB3` | link-cable versus menu |
| `0x800B0CB4` … `0x800B6A7F` | **VS menu** |
| `0x800B69F8` … `0x800BA5FF` | settings defaults / apply, **Options**, key assign |
| `0x800BA64C` … `0x800C19FF` | **Arena and Ex Arena** |
| `0x800C1A78` … `0x800C54FF` | card files for Arena Maker data and imports, disc check messages |
| `0x800C5580` … `0x800CADFF` | **Arena Maker** |
| `0x800CAEB8` … | movie decoder, then data tables |

## 2. The screens

Each table lists the main task's states in order. "→" = next state. Windows are (x, y, w, h).

### 2a. Garage (`0x8007C6B0(page)`, hub entry 0)

Opened by the hub with page 0, or 6 when back from AC TEST (`0x8003EA7B`). Before that the hub
copies the owned parts to `0x8003EB20` and runs `0x80090FD8` (`menus-hub.md` §3).

| State | Does |
|---|---|
| `0x8007C6B0` | task (group 0), windows (32, 40, 128, 168) menu and (176, 24, 128, 144) side panel, tone 6 |
| `0x8007C8F4` / `0x8007C9E4` | draw the menu (`0x80093808`: ASSEMBLY, OPTIONAL PARTS, PERFORMANCE, CHANGE COLOR, EDIT EMBLEM, NAME ENTRY, AC TEST; the Arena Maker adds RANKER LOGIC, RANKER TEST) |
| **`0x8007CAC8`** | **input**: Up / Down tone 1, ✕ tone 2 → row through the table at `0x80052030` (8 rows, cursor + scroll), ○ → close |
| `0x8007DA68` / `0x8007DBD8` | page closed: windows back, return to the menu |
| `0x80080AA8` | **exit**: waits for the closing windows, kills itself, then by `0x8003EA7D`: 0 → `0x80056528` (hub); 1 … 3 → Arena Maker roads `?` |

Checks on ✕ (messages through `0x80066C08`): " You don't have a CORE part.", "Your equipment is
incomplete.", "AC equipment incomplete.", "No OPTIONAL parts available.", "Begin test?" (yes / no).

| Page | Opens with | States |
|---|---|---|
| ASSEMBLY | windows (16, 40, 144, 184) + (176, 162, 128, 62), `0x800807FC` → `0x800808A0` | `0x80082BEC` slot list (LEGS WP, CORE WP, WEIGHT, NAME, "Necessary parts equipped.") → `0x80086260` → `0x800862DC` → **`0x800865B4` slot input** (equip / unequip messages, tone 12 cursor, part textures queued) → `0x800887E0` part list window (16, 40, 144, 128) → `0x80088924` part list → `0x80089364` close list. Unequip notices `0x80089514` / `0x80089750`. Back: `0x800897E4` |
| OPTIONAL PARTS | `0x80091204`: windows (16, 64, 144, ?), (16, 202, 288, 24), (168, 158, 128, 40), (168, 34, 128, 120) | `0x80091610` → `0x80091C7C` → **`0x80092020` input** ("Not enough open slots.", tone 10 on fitting; loads the option part's picture: MODEL slot 1 and SPEC_T textures) → back `0x80092794` |
| PERFORMANCE | `0x80092A34`: windows (8, 40, 160, 180) + (176, 162, 128, 62) | `0x80092BB4` stats page (PRICE, OFFENSIVE / DEFENSIVE POINT, STABILITY, MOBILITY, MOVING / TURNING SPEED, BOOST ABILITY, SUPPORT SYSTEM, RADAR RANGE, FCS PERFORMANCE, OVERALL, GRADE) → ○ `0x800936A8` |
| CHANGE COLOR | `0x800809C8`: window (32, 40, 136, 48) | `0x8008A254` (CHANGE PATTERN, EDIT COLOR) → `0x8008A32C` input. Patterns: `0x8008A748` → `0x8008DCC8` (CROSS STRIPES … CUSTOM). Colour schemes list `0x8008A910` → `0x8008AC78`. Edit colour: `0x8008B110` (GENERAL, HEAD, CORE, ARMS, LEGS, DEFAULT) → `0x8008B304` → editor `0x8008B900` / `0x8008B9BC` (windows (16, 40, 144, 128) + (20, 177, 134, 24)) → `0x8008BDF0` → close `0x8008CDF4` |
| EDIT EMBLEM | `0x80074DFC` (from `0x8007ED74`) | emblem editor: canvas window (80, 50, 104, 144), menu `0x800751AC` (ZOOM, UNDO, CLEAR, SAMPLE, APPLY, CANCEL) → input `0x80075354`; samples `0x80076064` (PICTURE MENU_TIM 8) → "Load the sample?" `0x8007646C`; `0x800779A0` puts the canvas into VRAM (LoadImage); done `0x80077204` |
| NAME ENTRY | `0x8007EEE8`: window (32, 40, 128, 48) | `0x8007F038` (AC NAME, PILOT NAME) → `0x8007F0CC` → windows (24, 40, 288, 136) + (24, 178, 144, 48) → name entry `0x8008E8D8` |
| RANKER LOGIC | `0x8007F6C4` | `0x8007F8A8` sliders ("SHORT … LONG") → `0x800800DC` input → close `0x800806B0` |
| AC TEST | build check `0x8007E164`, "Begin test?" | **`0x8007DB44`**: stage 40, music 255, compile, `0x80067000` starts it, `0x8003EA7B` = 1 so the garage reopens on page 6 afterwards. Stage 40 is one of the two that use loading picture 62 in the missions road (`mode-launch.md` §2), so the test seems to run in the missions engine `?` |
| RANKER TEST | the same checks | **`0x8007DE4C`**: the ranker's build (`0x800C6480`) and the opponent (`0x800C5C3C`) compiled, texture sets swapped, mode 3 (arena engine), stage 29, `0x8003EA7D` = 5 so the hub reopens on the Arena Maker icon (`mode-launch.md` §6) |

"Now Loading" while part models load: `0x80080C60` (group 21, text `0x80080D90`).

### 2b. Shop (`0x800990AC()`, hub entry 1)

Before it opens, the hub runs `0x8006354C` and **`0x80099048`**: it waits for the disc and reads
**`MIS.T` entry 385 = the part descriptions** (256 lines of 76 bytes, e.g. "Head unit with
built-in bio sensor.") into `0x801A0390` (callback `0x80098FF4`, flag `0x801A0000` = 1 while
loading). Work area `0x8019FFE0`.

| State | Does |
|---|---|
| `0x800990AC` | task (group 0), destructor `0x8009A314`; builds the frame by MoveImage from x 960 |
| `0x80099498` | window (4, 80, 112, 48) = BUY / SELL menu, tone 6 |
| `0x80099524` | menu drawn; CREDITS window (4, 196, 144, 24) via `0x800A0D54` → `0x800A0E0C` |
| **`0x80099BEC`** | **input**: ✕ → part kind list (or a `0x800B6550` message such as "No parts to sell."); ○ → tone 0, kill groups 16 and 4, close windows → `0x8009A26C` |
| `0x8009A26C` | **exit**: wait for the disc, stock bookkeeping `0x80099EAC`, `0x80063634`, then `0x80056528` |
| `0x8009A31C` … `0x8009A738` | part kinds: window (44, 92, 112, 96), list `0x8009A3FC`, input `0x8009A738` |
| `0x8009AB48` … `0x8009BD6C` | part list: window (140, 24, 176, ?), list `0x8009B750`, **input `0x8009BD6C`** (✕ buy / sell prompt; a key opens the detail page `?`) |
| `0x8009C4EC` … `0x8009CED4` | side panel (168, 104, 144, 112) with the **3D part preview**: `0x8009CD38` loads the part into **MODEL slot 29** and queues its textures |
| `0x8009CF4C` → `0x8009FBB8` | detail page on a full-screen layer: every spec line (`0x8009D148`, "INFINITY" for endless ammo) |
| `0x8009FFE8` → `0x800A003C` → `0x800A00C8` → `0x800A01B4` | "Buy Part?" / "Sell Part?" window (96, 100, 144, 48), tone 7; `0x800A01B4` does the deal: owned count at `0x80036FEC` + part id up or down, credits `0x8003EA74` down or up |

### 2c. System / card screen (`0x80059554(task, callback)`)

The same screen serves the hub's System entry (callback 0) and the title's LOAD GAME (callback
`0x800AC93C`, `boot-loads.md` §4a). It loads nothing from the disc.

| State | Does |
|---|---|
| `0x80059554` | task (group 0), report window (?, 40, 128, 144), tone 6 |
| `0x800597E4` | menu window (160, 32, 144, 160): **DATA SAVE, DATA LOAD, EMBLEM SAVE, EMBLEM LOAD, OPTION, EXIT GAME** (names at `0x800CB844`); the left window shows the reports (`0x80059A70`: CREDITS, MISSION REPORT with Sorties / Success / Failure / Overall, ARENA REPORT 1 / 2 with Challenge / Lose) |
| **`0x8005A280`** | **input**: Up / Down tone 1, ✕ → row, ○ → close (`0x8005DA2C` → `0x80056528`, or the callback) |
| `0x8005A90C` → `0x8005A99C` | SLOT 1 / SLOT 2 window (96, 96, 128, 48) |
| `0x8005AAC4` | card checks ("Check failed.", "Memory card is not formatted.", "No Memory card present.", "Memory Card was changed.") |
| `0x8005B2B0` → `0x8005B348` | "Searching for saved data." window (12, 104, 296, 32) |
| `0x8005B684` → `0x8005B7C0` → `0x8005BF38` | the save list (windows (16, 40, 96, 24), (16, 64, 288, 24), (16, 88, 288, ?); "NO. AC NAME SORTIES CREDITS"), list input with yes / no ("… overwrite?", "… create new data?", "Load … saved data?") |
| `0x8005C820` → `0x8005C94C` | save (`0x8007155C`), "Save completed." / "Save failed." |
| `0x8005CD34` → `0x8005CE68` | load (`0x80073424` + the finish, `boot-loads.md` §4a), "Load completed." |
| `0x8005DD48` … `0x8005EAB4` | EMBLEM SAVE / LOAD (" Save Emblem?") |
| `0x8005EC98` → `0x800B6D48` | OPTION → the Options screen (§2g) |
| `0x8005A6C0` | EXIT GAME? yes → TIM queue MENU_TIM 5, delete all tasks (`0x800579A0`), menu setup again, PICTURE MENU_TIM 0, title `0x800AAE94` |
| `0x8005F140` … `0x8005F674` | ARENA SAVE / ARENA LOAD (Arena Maker data: "NO. ARENA NAME RANKER") |

### 2d. Arena and Ex Arena (`0x800BA64C(n)`, hub entries 5 and 6)

n = 0 opens arena 0 with the lock `0x8004B686`; n = 1 first shows the Ex Arena picker. Much of
this screen is already used by Rogue Arena (`moa_arena_run_boot.c` on its branch); the routines
below match its notes.

| State | Does |
|---|---|
| `0x800BACE8` → `0x800BB12C` → `0x800BB1F4` | **Ex Arena picker**: windows (52, 80, 208, 96) + (24, 192, 272, 24), names `0x800BB730` ("HUMANOID LEGS" …, table `0x800D5A20`), descriptions `0x800BB8AC`; ✕ → arena, ○ → `0x800BB660` → hub. "No Arena data exists." |
| `0x800BB93C(arena, lock)` | **arena screen**: task (group 0), four heap buffers (names, rewards, builds, AI), header `0x800BA9B8` (RANK, PILOT, CHALLENGE, "Reference Only") |
| `0x800BBDC8` → `0x800BBEA8` | `0x800BBEA8` books a fight that just ended (`0x8003EA7C`), then → list |
| `0x800BC420` | prize money window ("C"), then `0x800BC514` prize picture window (110, 64, 100, 100, TIM queue MENU_TIM) and `0x800BC690` " Special parts obtained " |
| **`0x800BD044`** | **list input**: Up / Down tone 1, ✕ → opponent panel (per-opponent pictures through `0x800BDE88` → TIM queue MENU_TIM), L1 / R1 page flip (arena 0 only), ○ → `0x800BDAA8` → hub |
| `0x800BDB4C` → `0x800BDC54` | **opponent panel**: windows (24, 80, 144, 32), (128, 168, 176, 48), (176, 16, 128, 144), (24, 120, 96, 96) |
| **`0x800BED64`** | **panel input**: ✕ → checks ("You may not enter this Arena now.", "Arena license suspended.", "Requires a specific leg type.", "Unable to challenge at this time.", "Your equipment is incomplete."), ○ → back |
| `0x800BDEE0` … `0x800BE340` | the opponent's detail view (RANK, PILOT windows) |
| `0x800BF43C` | "Opponent unavailable, default win." / "Opponent is unable to compete." |
| `0x800C00B4` → `0x800C0120` → `0x800C028C` → `0x800C030C` → `0x800C0878` | "Data loading" (32, 108, 256, 24), opponent textures, compile, launch (`boot-loads.md` §7) |

### 2e. Arena Maker (`0x800C5580()`, hub entry 7)

Checks the disc first (`0x800C5514`, `0x80058B04`; "Insert DISC 1," … messages `0x800C4E74` …
`0x800C5498`).

| State | Does |
|---|---|
| `0x800C71DC` | ranker list (RANK, PILOT), task (group 0) |
| `0x800C7748` → **`0x800C7B20`** | list input (tone 1 / 2) |
| `0x800C97DC` → `0x800C98FC` → **`0x800C9D48`** | EDIT menu, windows (16, 68, 72, 96) / (16, 68, 120, 144) |
| `0x800C8F58` | edit the ranker's AC: models `0x800C6B78` (MODEL slots 3 … 14, MENU_TMD entry + per-kind offsets 3, 14, 54, 78, 96, 202, 237) and textures `0x800C6E4C`, then the garage `0x8007C6B0` |
| `0x800C8E2C` | RANKER TEST road (`0x800C5C3C`, then the hub) |
| `0x800CA4C8` | card screen for ARENA SAVE / LOAD |
| `0x800CA7BC` → `0x800CA87C` | ARENA NAME entry (`0x8008E8D8`) |
| `0x800C55D4` | exit → `0x80056528` |

### 2f. VS menu (`0x800B0CB4`)

Loads are in `boot-loads.md` §5. The states after them:

| State | Does |
|---|---|
| `0x800B1658` … `0x800B1CA4` | each player's AC: stand-ins `0x80067244`, slot rules `0x80083130`, models `0x80081E48`, textures `0x80082014` |
| `0x800B1E98` | draw the menu (`0x800B1EF0`: VS START, 1P AC SETUP, 2P AC SETUP, SELECT MAP, and the win / LOSE counts `?`) |
| **`0x800B22D8`** | **input**: VS START → `0x800B2D7C` (`mode-launch.md` §5), AC SETUP → `0x800B3C10`, SELECT MAP → `0x800B38C0` (stage preview reloaded by `0x800B3BA4`), ○ → "Exit VS Mode?" → `0x800B5AEC` (back to the title) |
| `0x800B3C10` → **`0x800B3DDC`** | AC SETUP menu (`0x800B3C6C`: ASSEMBLY, OPTIONAL PARTS, CHANGE PATTERN, EDIT COLOR, SYSTEM) |
| `0x800B56B4` → `0x800B5798` | ASSEMBLY summary (NAME, TYPE, E.C., ARMOR POINT, WEIGHT, LEFT …), then the **garage's own assembly page** `0x80082BEC` |
| `0x800B4C18` … `0x800B4F18` | OPTIONAL PARTS → the garage's page `0x80091610` |
| `0x800B4A68` / `0x800B48E4` | CHANGE PATTERN / EDIT COLOR → the garage's `0x8008DCC8` / `0x8008B110` |
| `0x800B4F78` → `0x800B4FF0` | SYSTEM: DATA LOAD (`0x800B53C8` → card screen) and OPTION (`0x800B541C` → §2g) |

So the garage pages are shared: the VS AC SETUP runs the same assembly, option, pattern and colour
code as the Scenario garage.

### 2g. Options (`0x800B6D48`)

| State | Does |
|---|---|
| `0x800B6F4C` | rows MASTER VOLUME, BGM VOLUME, TEXT MESSAGE, VS STAGE, KEY ASSIGN, VS TIME LIMIT, VIBRATION; windows (24, 64, 136, 120) + (16, 200, 288, 24) |
| **`0x800B7388`** | input; ○ applies the settings (`0x800B6A80`) and closes (`0x800B7910` → `0x800B79BC`) |
| `0x800B7A34` → `0x800B7ABC` | ON / OFF picker (168, 64, 136, 48) |
| `0x800B7E70` → `0x800B7EF4` | VS STAGE: SELECT, RANDOM, TURN (168, 64, 136, 72) |
| `0x800B8298` | volume slider (168, ?, 80, 16) |
| `0x800B8880` → `0x800B8960` → `0x800B9074` | KEY ASSIGN: 12 actions (GO FORWARD … FIRE WEAPON), DEFAULT, EXIT; "Overlapping key assignments." |
| `0x800B9E2C` → `0x800B9EB0` | VS TIME LIMIT (168, 112, 136, 72) |

Settings live at `0x8004B568` (defaults from `0x800D5860`, `boot-loads.md` §2).

## 3. What this means for overlays and the launcher

- **Replacing a screen:** hook the hub's pick (`menus-hub.md` §5), run your own screen, and call
  `0x80056528` when it closes. While a game screen is open, its main task's +0x78 tells you exactly
  which state it is in (the tables above).
- **Locking the game's input** under an overlay: the screens only read `0x801B1CB8` +0x18, so
  clearing it (or latching every bit in +0x00) after the sampler `0x800639A4` stops them `?` (not
  tried).
- **Using the game's own look from a mod:** a window is `0x80063EF4` + tone 6, closed by
  `0x80064348`; text is `0x80065330`; a question is `0x80065858` (answer 0 yes, 1 no). Put the
  window in an odd group and its contents in the even group above it.
- **Shop texts:** every part's in-game description is `MIS.T` entry 385, 76 bytes per part, which
  the spec comparison idea for Rogue Arena prizes can show. The shop's 3D part preview (MODEL slot
  29, `0x8009CD38`) shows how the game loads one part's model and textures on its own.
- **One code path for both garages:** fixes or overlays on the assembly / option / colour pages
  reach the VS AC SETUP too.

## 4. Open

- Which token task marks which garage page (§1a).
- The sprite routine's first argument; the text routine's colour value.
- Whether contents are really placed relative to their layer's window, and the layer order.
- Tones 8 and 9 (one or two uses each).
- The garage exit's `0x8003EA7D` values 1 … 3 (Arena Maker roads).
- The detail key in the shop's part list.
