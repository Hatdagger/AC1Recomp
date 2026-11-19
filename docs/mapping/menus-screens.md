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
with `0x80057470`, and kill it when the page closes. Matched to the pages they open (2026-11-18,
garage menu input `0x8007CAC8`): `0x80068BEC` ASSEMBLY, `0x80068C3C` OPTIONAL PARTS, `0x80068C8C`
PERFORMANCE, `0x80068CDC` CHANGE COLOR, `0x80068D2C` EDIT EMBLEM, `0x80068D7C` NAME ENTRY (NEW
GAME's name entry too), `0x80068EBC` / `0x80068FFC` the garage menu itself (made when it opens).
The VS AC SETUP and the link menu make the ASSEMBLY, OPTIONAL PARTS and CHANGE COLOR tokens too.
`0x80068DCC` VS menu, `0x80068F5C` Arena Maker.

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
| 11 | (the same 2) | **a part is removed**: NO EQUIP picked in ASSEMBLY, an option taken off. Both calls above pick 10 or 11 when they run, so a count of fixed values missed it |
| 12 | 4 | ASSEMBLY cursor: Up / Down between slots, Left / Right between parts (`0x800865B4`) |

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
| ASSEMBLY | windows (16, 40, 144, 184) + (176, 162, 128, 62), `0x800807E4` → `0x800808A0` | `0x80082BEC` page task → `0x80086260` → `0x800862DC` → **`0x800865B4` slot input** (browse, equip, fix-ups) → on ○ `0x800887E0` → `0x80088924` summary of the AC → `0x8008935C` ✕ / ○ → `0x800897E4` garage menu. Fix-up notices `0x80089514` / `0x80089750`. **Read in depth: §2h** |
| OPTIONAL PARTS | `0x80091204`: windows (16, 64, 144, ?), (16, 202, 288, 24), (168, 158, 128, 40), (168, 34, 128, 120) | `0x80091610` → `0x80091C7C` → **`0x80092020` input** ("Not enough open slots.", tone 10 on fitting; loads the option part's picture: MODEL slot 1 and SPEC_T textures) → back `0x80092794`. **Read in depth: §2j** |
| PERFORMANCE | `0x80092A34`: windows (8, 40, 160, 180) + (176, 162, 128, 62) | `0x80092BB4` stats page (PRICE, OFFENSIVE / DEFENSIVE POINT, STABILITY, MOBILITY, MOVING / TURNING SPEED, BOOST ABILITY, SUPPORT SYSTEM, RADAR RANGE, FCS PERFORMANCE, OVERALL, GRADE) → ○ `0x800936A8`. **Read in depth: §2k** |
| CHANGE COLOR | `0x800809C8`: window (32, 40, 136, 48) | `0x8008A254` (CHANGE PATTERN, EDIT COLOR) → `0x8008A32C` input. Patterns: `0x8008A748` → `0x8008DCC8` (CROSS STRIPES … CUSTOM). Colour schemes list `0x8008A910` → `0x8008AC78`. Edit colour: `0x8008B110` (GENERAL, HEAD, CORE, ARMS, LEGS, DEFAULT) → `0x8008B304` → editor `0x8008B900` / `0x8008B9BC` (windows (16, 40, 144, 128) + (20, 177, 134, 24)) → `0x8008BDF0` → close `0x8008CDF4`. **Read in depth: §2l** |
| EDIT EMBLEM | `0x80074DFC` (from `0x8007ED74`) | emblem editor: drawing `0x80074FD4`; ○ opens the menu window (80, 50, 104, 144) (the first read called it the canvas), menu `0x800751AC` (ZOOM, UNDO, CLEAR, SAMPLE, APPLY, CANCEL) → input `0x80075354`; samples `0x80076064` (PICTURE MENU_TIM 8) → "Load the sample?" `0x8007646C`; `0x800779A0` puts the canvas into VRAM (LoadImage); done `0x80077204`. **Read in depth: §2m** |
| NAME ENTRY | `0x8007EEE8`: window (32, 40, 128, 48) | `0x8007F038` (AC NAME, PILOT NAME) → `0x8007F0CC` → windows (24, 40, 288, 136) + (24, 178, 144, 48) → name entry `0x8008E8D8` |
| RANKER LOGIC | `0x8007F6C4` | `0x8007F8A8` sliders ("SHORT … LONG") → `0x800800DC` input → close `0x800806B0` |
| AC TEST | build check `0x8007E164`, "Begin test?" | **`0x8007DB44`**: stage 40, music 255, compile, `0x80067000` starts it, `0x8003EA7B` = 1 so the garage reopens on page 6 afterwards. Stage 40 is one of the two that use loading picture 62 in the missions road (`mode-launch.md` §2), so the test seems to run in the missions engine `?` |
| RANKER TEST | the same checks | **`0x8007DE4C`**: the ranker's build (`0x800C6480`) and the opponent (`0x800C5C3C`) compiled, texture sets swapped, mode 3 (arena engine), stage 29, `0x8003EA7D` = 5 so the hub reopens on the Arena Maker icon (`mode-launch.md` §6) |

"Now Loading" while part models load: `0x80080C60` (group 21, text `0x80080D90`).

### 2b. Shop (`0x800990AC()`, hub entry 1)

States below; what it lists, prices, refusals and loads: **§2i**.

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

### 2h. ASSEMBLY in depth (2026-11-18)

The garage's ASSEMBLY page, which the VS AC SETUP also runs (§2f). The rules match `moa_parts.c`
(`moa_build_fit`, `moa_build_check`) and `acr/acr_gen1_fit.c`, which apply them to whole builds.
This section adds how the page applies them one swap at a time, what it shows, and what it loads.

**The AC it edits.** 12 bytes at `0x80036FB0`, in the save's order: core, booster, head, arms,
blade, hand weapon, back L, back R, generator, FCS, (unused), legs. 255 = empty. Option parts are
bits of the u16 at `0x8003EA70`. Owned parts are counts (`0x80036FEC`, numbering in `moa_card.c`
`moa_owned_index`). The page reads a copy at `0x8003EB20`, made when it opens. Equipping never
changes a count, because an equipped part is one of the owned ones. The same back weapon on both
sides needs 2 owned.

**Rows** (row → kind `0x800CD618`; kind table `0x800D4544`, 16 bytes per kind: part count, MENU_TMD
start, owned-bit mask, framing table; kind order in §2j): 0 head, 1 core, 2 arms, 3 legs, 4 generator, 5 FCS, 6 options, 7 booster,
8 back L, 9 back R, 10 blade, 11 hand weapon. The cursor skips a row when no part of its kind is
owned. It always skips the options row, because the page clears that mask when it opens (options
have their own page). It also skips the back row that a two-slot unit blocks.

**Controls** (state `0x800865B4`):

| Button | Does |
|---|---|
| Up / Down | next row (tone 12) |
| Left / Right | previous / next owned part of the row's kind, in id order (`0x80089E4C(task, 'P' / 'N')`). After the last one comes NO EQUIP (every row but the core), then it wraps around. Nothing is fitted yet: the AC on screen just wears the part (tone 12) |
| R1 or R2 held + Left / Right | jump to the previous / next group: legs humanoid / reverse joint / four legs / caterpillar (ids from 0, 14, 21, 27), arms regular / weapon (from 12), back missiles / rockets / cannons and guns / radars / two-slot units (from 0, 18, 28, 40, 52) |
| L1 or L2 held + Left / Right | the part preview's slide value is ±360 or ±540 instead of ±180 (faster? `?`) |
| △ | show or hide the hovered part's full spec page (`0x80083E7C`, task `0x80093B18`) |
| ✕ | fit the hovered part (checks and fix-ups below) |
| ○ | leave (tone 0). The equipped part's textures go back, and a summary of the whole AC follows (`0x800887E0` → `0x80088924`). ✕ or ○ there (`0x8008935C`) returns to the garage menu |

**The numbers show the AC as it would be with the hovered part** (`0x8008284C`). It copies the
garage, puts the hovered part in, applies the fit rules, runs the stat totals `0x8007E730` and
puts the garage back. The totals block is `0x8004B710`:

| Offset | Value |
|---|---|
| +0x00 | energy left = generator output − drain |
| +0x04 / +0x08 / +0x0C | AP / weight / price totals |
| +0x10 | leg load left = legs' max − weight of everything but the legs |
| +0x14 | arm load left = core's max − arms, plus the hand weapon and blade on regular arms |
| +0x18 / +0x1A / +0x1C (u16) | output, legs' max, core's max |

`0x80083AA8` draws them as EP, LEGS WP and CORE WP ("left / max"), AP and WEIGHT. **A negative
"left" is drawn in red** (the text task's +0x16 set to 13). A back weapon on both sides with only
one owned is counted once. While the hovered part can't be fitted, a warning mark shows (sprite
`0x800CD82C` at (20, 48), set by `0x800862DC`).

**✕ refused** (a message box, nothing changes), checked in this order:

| When | Message |
|---|---|
| the hovered part is the one fitted | "This part is already equipped." (NO EQUIP on an empty slot: "No part to cancel.") |
| blade or hand weapon row, no arms | "Can't equip without an ARM part." |
| blade or hand weapon row, weapon arms (id 12 and up) | "Can't equip with ARM type weapon." |
| caterpillar legs (27 and up): the booster row, or a back row with radar 40, 41, 42, 44 or 46 | "Can't equip with a caterpillar." |
| hand weapon row: WG-1-KARASAWA (15) on legs that can't hold it (5, 21–24, 26 and the caterpillars, table `0x80052460`) | "Can't equip with current legs." |
| a back row with ANKS-1A46J arms (5): a back weapon (below 40) or WX-S800-GF (53) | "Unable to equip." |

**✕ accepted.** The part goes into the garage byte, and the preview model is copied into the AC's
model slot (`0x800860D8`, no disc read). Then come the fix-ups below. If none applies, tone 10
plays (11 for NO EQUIP). A fix-up shows its message instead, and the caterpillar ones chain
through `0x80089514` / `0x80089750`:

| Fitted | The game then |
|---|---|
| arms: NO EQUIP or weapon arms | removes the blade and hand weapon: "Unequipped the ARM weapon." |
| arms: regular, in place of weapon arms | puts back the last blade and hand weapon of this visit (the ones the AC had when the page opened, or fitted since), the hand weapon only if the legs can hold it. No message |
| arms: ANKS-1A46J | removes back weapons (below 40) and WX-S800-GF: "Unequipped the BACK weapons." or "Unequipped the left / right BACK weapon." |
| legs: a caterpillar | removes the booster ("Unequipped the booster."), radars 40, 41, 42, 44, 46 ("Unequipped the radar.") and the KARASAWA ("Unequipped the right ARM weapon.") |
| legs: not a caterpillar, in place of one | fits the first owned booster. No message |
| legs that can't hold the KARASAWA (table `0x80052820`) | removes it: "Unequipped the right ARM weapon." |
| back: a two-slot unit (52 and up) | empties the other back slot: "Unequipped the left / right BACK weapon." |
| back: the weapon the other side carries, with only one owned | takes it off the other side: "Moved the BACK weapons." |
| core | fits again the options the AC had when the page opened, in bit order. The first one that doesn't fit the core's slots (core +54, option +40) is removed, and so is every option after it. No message. Going back to a bigger core during the same visit brings them back |

All five copies of the KARASAWA-and-legs table (`0x800522A0`, `0x80052358`, `0x80052460`,
`0x800527B0`, `0x80052820`) list the same legs.

**On opening** (`0x800807E4` → `0x800808A0` → `0x80082BEC`), the page copies the owned table,
builds the masks (`0x80090FD8`) and runs `0x80083130`. That fills an empty core, head, arms, legs,
generator, FCS or booster with the first owned part of its kind, and applies the same rules. If
anything changed, it shows "Necessary parts equipped.". The hand weapon and blade are remembered
at this point, and again whenever one is fitted (task +0x8A / +0x8B), for the arms rule above.

**Loads while browsing.** The AC's part models sit in MODEL slots 3–14. The hub open loads them
(`0x80081E48`): 3 head, 4 core, 5 arms, 6 legs, 7 blade, 8 hand weapon, 9 back L, 10 back R,
11 generator, 12 FCS, 14 booster. The MENU_TMD entry is the kind's start + the part id: core 3,
legs 14, arms 54, head 78, booster 96, back 110, blade 170, hand weapon 176, FCS 202, options 218,
generator 237. Entry 2 is shown for NO EQUIP `?`. A hovered part's model is read into preview
slot 1 or 2, taking turns. **Its textures are queued straight into the AC's own texture pages
in VRAM** (the entries of `0x800822B0`, `boot-loads.md` §4d), so the AC on screen wears them.
Moving to another row, or ○, queues the fitted part's textures again (for an empty slot, the
first owned part's). When NO EQUIP comes up on a required row, the first owned part of that kind
is also loaded into MODEL slot 17 and its textures queued, as a stand-in `?`. Leaving loads
nothing else, and the hub reloads the models when it opens.

**The sortie check `0x8007E164`** is the only check outside the ASSEMBLY page (callers: the
garage menu, VS START for each player, the link menu, the arena's challenge). It fails on a
missing head, core, arms, generator, FCS or legs, or a missing
booster on legs that aren't caterpillars. It also fails when energy left is below 0, the arm load
is over the core's max, or the carried weight is over the legs' max, **unless `0x8003EACA` ≠ 0**
(Human Plus limits waived). Then it passes and writes "Illegal machine." into the caller's text.
With Human Plus on, it first writes a full-width "Rebel>" there. It checks neither option slots
nor the fit rules above, which only the garage enforces.

### 2i. Shop in depth (2026-11-18)

**Owned and stock.** Every part has an owned count (`0x80036FEC`) and a stock count
(`0x800370EC`). Both tables are 256 bytes in the owned-table order (`moa_card.c`
`moa_owned_index`): head 0, core 18, arms 29, legs 53, generator 93, FCS 108, options 124,
boosters 143, back weapons 157, arm weapons 217 (hand weapons, then the 6 blades from 243). The
shop's 10 kinds follow the same order (HEAD … BACK WEAPON, ARM WEAPON; names at `0x800530D8`).
In a new game the stock is 1 for most parts and 2 for back weapons (one per side). It is 0 for
parts not on sale yet.

**The lists** (`0x80099734` BUY, `0x80099990` SELL, work area `0x8019FFE0`: kinds +0x13C, ids
+0x23C, count +0x5C, per-kind counts +0xAC, per-kind starts +0xB6):
- BUY lists every part with stock ≠ 0, in that order, picked through the kinds window first.
- SELL lists every part with owned ≠ 0 in one list, without a kinds window. Fitted parts are in it.

**Prices.** `0x8009B488(kind, id)` = the part table's u32 at +36, for buying and selling alike: a
sale pays the full price back. Credits stop at −999,999 when buying and 99,999,999 when selling.

**A deal** (`0x800A01B4`, after the "Buy Part?" / "Sell Part?" box (96, 100, 144, 48)): buying
adds 1 to owned and takes 1 from stock, selling does the reverse. A count of 127 is never
changed (unlimited `?`). Credits move by the price.

**Refusals** (`0x800B6550`; the caller puts the message number in `0x801FBEC1` / `0x801FBEC2`):

| When | Message |
|---|---|
| BUY, a kind with nothing on sale | "No parts to buy." |
| BUY, credits below the price (so nothing can be bought in debt) | "Not enough credits." |
| SELL, nothing owned | "No parts to sell." |
| SELL with credits below 0, for a head, core, arms, legs, generator, FCS or booster: the part is fitted, or the AC is incomplete (`0x80096288`), over the legs' weight limit (`0x8009632C`) or short of energy (`0x800965B8`) | "Pay back your debt first" |

With credits at 0 or more, SELL checks nothing, and weapons and options can always be sold. So a
fitted part can be sold.

**Leaving** (`0x8009A26C`). `0x80099EAC` empties every garage slot whose part now has an owned
count of 0. It also empties back R when both sides hold the same weapon and fewer than 2 are
owned. Options are not checked `?`. Then `0x80063634` puts the AC's textures back (below), and
the hub opens.

**The 3D preview and the AC's textures.** Before the shop opens, the hub runs `0x8006354C`,
which copies:
- the AC's texture pages (704, 256, 128 × 256) to (704, 0)
- the emblem (544, 384) to (560, 384)
- the colour rows 486–494 to 495–503
- (added 2026-11-18) the option-picture area (384, 256) 64 × 64 to (512, 256), and its colour row
  482 to row 511. That area normally parks a two-slot back unit's texture (`boot-loads.md` §4c),
  and an option part's SPEC_T texture lands on it (§2j)

The preview (`0x8009CD38`) loads the hovered part into MODEL slot 29 and queues its textures,
which land on the AC's own pages. On leaving, `0x80063634` copies all of them back. The shop kind →
garage kind table and the MENU_TMD starts are at `0x800531D4`; each part's framing numbers come
from the kind table's fourth field (`0x800D4544` +8, 16 bytes per part; the option page reads it the
same way, §2j).

**Controls.** △ in the part list shows or hides both the spec page (`0x8009CF4C`, every spec
line) and the part's shop text (`0x8009FCA4`, MIS.T 385 at `0x801A0390`). ○ goes back (tone 0).
The list's names and prices are drawn into VRAM (x 896 and x 1004, by `0x80064CC0`) and shown
as sprites.

The same message routine and the same three checks serve `0x800967C4` in the missions code
(messages "Your equipment is incomplete.", "You exceed the weight limit.", "You don't have
enough energy."), probably the mission sortie `?`. The shop never picks the big picture messages
(CAN NOT BUY, CAN NOT SELL, GENERATOR FIRST, ENERGY SHORT) `?`.

### 2j. OPTIONAL PARTS in depth (2026-11-18)

The garage's option page. The VS AC SETUP runs the same page (§2f) behind its own copy of the opener.

**What it edits.** The fitted options are the bits of the u16 at `0x8003EA70` (bit n = option n).
Owned options are the bits of `0x8004B558`, built from the owned counts by `0x80090FD8` (one
mask per kind, see below). Options are 13 records of 44 bytes at `0x800D1AA0`:

| Offset | Field |
|---|---|
| +0x00 | name ("SP-MAW") |
| +0x12 | type text ("RADAR OPTION", "MISSILE JAMMER", …), shown in the side panel |
| +0x24 | price (u32) |
| +0x28 | slots it takes |

Descriptions are a separate pointer table at `0x800CDE8C` (13 texts: "Adds Missile indicator to
Radar." … "Decreases energy used.", and "New SP-ABS" for SP-ABS_Re). The page code loops over 19
ids, but ids 13–18 have no record and no description (`parts-tables.md`, "Room for more optional
parts").

**The kind table** `0x800D4544` (16 bytes per kind: part count, MENU_TMD start, owned mask, framing
table, and at +0x0C one more table not read here `?`) is in this order: 0 core, 1 legs, 2 arms, 3 blade, 4 head, 5 booster, 6 back weapon, 7 hand
weapon, 8 FCS, **9 options** (19, MENU_TMD 218, mask `0x8004B558`, framing `0x800D375C`), 10
generator. The owned masks are listed per shop kind by `0x80090FD8` (pointers `0x800CDE44`, counts
`0x800CDE6C`); it builds the arm weapons as one 32-bit list and then splits it into hand weapons
(`0x8004B528`, 26 bits) and blades (`0x8004B520`, 6 bits).

**Opening** (garage menu row 1, in `0x8007CAC8`):
1. Copies the owned counts to `0x8003EB20` and rebuilds the masks (`0x80090FD8`).
2. No option owned: "No OPTIONAL parts available." (box 20, 104, 280, 32), back to the menu.
3. In the Arena Maker's garage (`0x8003EA7D` = 1 or 2): tone 7 and nothing opens.
4. Otherwise: page token `0x80068C3C`; **backs up VRAM (384, 256) 64 × 64 to (512, 256) and CLUT
   row 482 to row 511** (that area parks a two-slot back unit's texture, `boot-loads.md` §4a / §4c,
   and the option picture is about to land on it); MODEL slot 13 ← MENU_TMD 218 + the first owned
   option; TIM queue SPEC_T entry = that option; closes the garage menu windows.
5. `0x80091204` → `0x80091504` open four windows (tone 6 each): list (16, 64, 144, rows × 24) with
   rows = owned options up to 5, description line (16, 202, 288, 24), slots box (168, 158, 128, 40),
   picture box (168, 34, 128, 120). Then the page task `0x80091610` (group 0) runs and the garage
   task idles. `0x80091C7C` waits for the disc, adds the cursor bar (`0x80091558`, group 4, follows
   row × 24) and the model, then the input `0x80092020`.

The VS AC SETUP's copy of the opener (`0x800B4C18` … `0x800B4F18`) sizes the list window for up to
6 rows; the page itself always shows 5 `?`.

**What it shows.**
- **List** (rebuilt every frame by `0x80091B78` → `0x800919F8`): the owned options in id order,
  5 rows of 24 pixels, scrolling. Each row is the option's name (font 16, x 36). A fitted option
  gets a marker sprite (`0x800CD970`) and text colour 21 (others 16). Arrow sprites at (14, 48) and
  (14, 184) show when there are more rows above or below.
- **Slots box** (`0x80091DB0`, group 8): the type text, then "n SLOT REQUIRED" / "n SLOTS
  REQUIRED" (the option's +0x28) and "n SLOT AVAILABLE" / "n SLOTS AVAILABLE". Available = the
  core's slots (core record +0x36, core table `0x800CEAC8`, 56 bytes per core) minus the slots of
  every fitted option.
- **Description line** (`0x80091FB8`, group 10): the text from `0x800CDE8C` (font 6, at 4, 9).
- **Picture box: the option's 3D model**, turning. On each cursor move (`0x80091BF0`) the page stops
  a read still in flight (`0x80058864`), then loads MENU_TMD 218 + id into MODEL slot 1 and SPEC_T
  entry id; once the disc is idle the model moves to **MODEL slot 13** (the AC's option slot; the
  hub loads nothing there) and a model task (`0x800927E8`, group 6) shows it. Framing: kind table
  +8 → 16 bytes per option (+0x0C scale, +0x04 depth `?`). **SPEC_T textures are 128 × 128 pixels,
  8-bit, at VRAM (384, 256) with their CLUT at (320, 482)** (entries 0–12; 13 and up are empty).

**Controls** (`0x80092020`):

| Button | Does |
|---|---|
| Up / Down | move the cursor, scrolling at the top or bottom (tone 1). This page sets `0x801B1CDC` = 2, which halves the pad auto-repeat times (3 / 6 frames instead of 6 / 12) |
| ✕ on a fitted option | removes it (tone 11) |
| ✕ on an option not fitted | fits it if available slots ≥ its slots (tone 10), else "Not enough open slots." (box 20, 104, 280, 32), nothing changes |
| ○ | leave (tone 0) |

Nothing else is checked: not SP-ABS and SP-ABS_Re together (SP-ABS_Re is never sold), not Human
Plus, not the owned count (only owned options are listed). The core's slots are only enforced
here and by ASSEMBLY's core fix-up (§2h); the sortie check doesn't look at options.

**Leaving** (○): kills the token and the arrows, stops any read, **puts VRAM (512, 256) back over
(384, 256) and row 511 back over row 482**, kills groups 3–10, frees the model buffers of slots
13, 1 and 2, closes the four windows, sets `0x801B1CDC` = 2, and returns to the garage menu
(`0x80092794` → `0x8007C8F4`). Only the top 64 × 64 of the picture area is restored; the lower half
(384, 320) keeps the last option picture `?`.

The shop does the same backup and restore of (384, 256) and row 482 (`0x8006354C` /
`0x80063634`, besides the three copies listed in §2i).

**For the launcher:** the game's option model is MENU_TMD 218 + option bit and its texture is SPEC_T
entry = option bit, so the launcher's `models/menu/option/NN` (exported from base 218 in
`acr/acr_models_moa.c`) is numbered by option bit. The "ASSUMPTION to check" in the Extra Garage
plan holds.

### 2k. PERFORMANCE and GRADE in depth (2026-11-18)

A display page: it works everything out once, shows it, and closes. Nothing is loaded from the
disc and nothing in the AC changes. The launcher already copies its maths (`moa_parts.c`
`moa_performance`); the differences are listed at the end.

**Opening** (garage menu row 2): it needs a head, arms, legs, FCS, generator and, unless the legs
are caterpillars, a booster. Otherwise "Your equipment is incomplete." (box 20, 104, 280, 32).
Weapons are optional. Then token `0x80068C8C`, windows (8, 40, 160, 180) and (176, 162, 128, 62)
(tone 6), the page task `0x80092B44` → **`0x80092BB4`** (all the maths and text) → `0x800936A8`,
which waits for **✕ or ○** (tone 0), closes both windows and returns to the garage menu
(`0x8009379C`).

`0x80092BB4` runs the stat totals `0x8007E730` (`0x8004B710`, §2h) and **the compile
`0x800A7BB4(0)`**, only to read the radar range. That overwrites player 1's compiled block
(`0x800453E8`), which VS START compiles again anyway.

**Layout.** Big window: the AC name (`0x800371EC`) at the top, then 11 rows 12 pixels apart from
y 24 (font 6; labels at x 8, sub-items at x 20; values at x 110, 6 digits, so at most 999,999),
then GRADE at (8, 162) with its word at (98, 162). Small window: AP, WEIGHT, PRICE (font 16; AP 6
digits, weight and price 8).

| Row | Value |
|---|---|
| AP | totals +0x04 (head + core + arms + legs AP) |
| WEIGHT | totals +0x08 |
| PRICE | totals +0x0C **plus the price of every fitted option** (the totals leave options out; ids 0–12) |
| OFFENSIVE POINT | attack of: the weapon arms (weapon-arms detail +0x14) **or** the hand weapon (detail +0x14) + the blade (+0x30); plus back L and back R (detail +0x14) |
| DEFENSIVE POINT | shell defence of head + core + arms + legs (+0x2E) and energy defence (+0x30) added together; SP-S/SCR makes the shell part ×1.15, SP-E/SCR the energy part ×1.15 |
| STABILITY | legs +0x40, + 800 with SP-SAP, + 1,600 with SP-ABS_Re or else + 800 with SP-ABS |
| MOBILITY | moving + turning + boost (below) |
| · MOVING SPEED | legs +0x36 × 10 |
| · TURNING SPEED | legs +0x38 × 10 |
| · BOOST ABILITY | (booster +0x2C ÷ weight) × 1,000, the division done first in whole numbers; caterpillars use 9,800 in place of the booster |
| SUPPORT SYSTEM | radar + FCS (below) |
| · RADAR RANGE | **compiled radar range (+0x1E) ÷ 10**: the head's range, or with a back radar (40–51) the best back radar's range (the head's is ignored then), or 5,120 with Human Plus on and no radar at all (`stat-block.md` +0x1E) |
| · FCS PERFORMANCE | FCS +0x2E ÷ 10 + (the four lock boxes' widths, +0x30 + 12k) ÷ 10 + (their heights, +0x32 + 12k) ÷ 10 + max lock (+0x2C) × 100 − the first lock class's lock value (+0x38) × 10 |
| OVERALL | the sum of the 9 plain rows: offensive + defensive + stability + moving + turning + boost + radar + FCS |
| **GRADE** | from OVERALL: **25,000 and up SUPERFINE; 20,000–24,999 GREAT; 15,000–19,999 FINE; 10,000–14,999 OK; below 10,000 POOR** (words at `0x80052EDC` …) |

The launcher's own example (`moa_parts.h`): its check build comes to OVERALL 12,641, so OK.

**Differences in the launcher's `moa_performance`:**
- RADAR RANGE: it shows 0 when a back radar is fitted; the game shows the back radar's range ÷ 10.
  With Human Plus on and no radar the game shows 512; the launcher shows the head's figure. Both
  change SUPPORT SYSTEM, OVERALL and so the GRADE.
- PRICE: the game also adds SP-ABS_Re's price (option 12, never sold, so only on edited saves).
- It has no GRADE yet; the thresholds above are all it needs.

### 2l. CHANGE COLOR in depth (2026-11-18)

**What it edits** (all of it goes into the save, `boot-loads.md` §4e):
- **`0x8003EA72` = pattern** (0–15) and **`0x8003EA73` = colour scheme** (0–3) of a pattern.
- **`0x80036FBC` = the custom paint, 48 bytes**: 4 parts in the order **head, core, arms, legs**, each
  4 colours in the order **BASE, OPTIONAL, DETAIL, JOINT**, each R, G, B. The editor allows 0–64
  per channel.

**How the colours reach the AC.** Only the head, core, arms and legs are painted. Each has a 256-colour
palette row in VRAM: **core 486, legs 488, arms 489, head 490** (row 487 is not touched).
- Pattern 0 (CUSTOM): the textures are the plain ones (CORE_T entry = core id, …) and
  **`0x80082530`** builds the four rows from the paint bytes, then LoadImage (rows from `0x800CD9F8`).
- Patterns 1–15: the textures are the pattern's own versions: **HEAD_T head + 18v, CORE_T core + 11v,
  ARMS_T arms + 24v, LEG_T legs + 40v** (v = pattern; `0x8008E70C`), and the colours are a ready-made
  set, **MENU_TIM 26 + 4v + scheme** (entries 30–89: a TIM whose CLUT is row 486 and whose "picture"
  is rows 488–490). The paint bytes are kept, so going back to CUSTOM brings them back.

**The ramp `0x80082530` builds** (one row = 4 colours × 64 entries): colour c fills entries 64c …
64c + 63, as 32 shades from bright to dark, each written twice (entries 64c + 2k and 64c + 2k + 1).
For each channel value p:
- p = 0–31: the shades run from p down towards 1.
- **p = 32–64: they run from 31 (full) down towards p − 30**, so values above 31 make a brighter
  colour with lighter shadows; 61 and up stay at full brightness all the way.
- Steps are equal, worked out in 1/256 units ((start − end) ÷ 32), stopping at the end value; the
  5-bit result is packed as PSX colour R | G << 5 | B << 10.

**Opening** (garage menu row 3): needs head, arms and legs, else "Your equipment is incomplete.".
Token `0x80068CDC`, window (32, 40, 136, 48), menu CHANGE PATTERN / EDIT COLOR (`0x8008A254`,
input `0x8008A32C`: Up / Down tone 1, ✕ tone 2, ○ tone 0 back to the garage menu). The menu reacts
to ✕ only on a new press, not a held one.
- **CHANGE PATTERN** → `0x8008A748` (window 32, 40, 136, 168) → the pattern list `0x8008DCC8`.
- **EDIT COLOR** with pattern 0 → `0x8008A82C` (window 32, 40, 136, 144) → the part menu
  `0x8008B110`; with any other pattern → `0x8008A910` (window 32, 40, 136, 128) → the scheme list
  `0x8008A9F4`.

The VS AC SETUP has CHANGE PATTERN and EDIT COLOR as two entries of its own and makes the same
pattern-0 choice for EDIT COLOR (`0x800B49BC`).

**Pattern list** (`0x8008DCC8`, input `0x8008DFA4`): 16 rows, 7 shown at a time (names rendered
into VRAM x 960 by `0x80064CC0`, names table `0x800CD66C`); the cursor is the pattern number:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| CUSTOM | WOODLAND | HOARFROST | NIGHT SHIFT | DESERT PATTERN | DUCK HUNTER | LEAF PATTERN | REDDISH DUST |
| **8** | **9** | **10** | **11** | **12** | **13** | **14** | **15** |
| DECORATOR | GLEAM SASH | TIGER STRIPE | STAINED BRONZE | OUTER BUSH | SERPENT SCALE | TILE PATTERN | CROSS STRIPES |

Up / Down tone 1 (no wrap). ✕ (tone 2) on a different pattern: writes `0x8003EA72`, queues the four
body textures (and the scheme's colours), shows "Loading the sample." (window 16, 120, 280, 32),
waits for the disc, builds the custom palette when the new pattern is 0, and stays in the list. **The
scheme number is not reset**, so the new pattern opens with the same scheme slot. ✕ on the current
pattern does nothing. ○ (tone 0) goes back to the menu.

**Scheme list** (`0x8008A9F4`, input `0x8008AC78`; names table `0x800CD6AC`, 16 bytes per pattern
1–15): the pattern's name on top and 4 rows, **STANDARD and three named schemes** (WOODLAND:
STANDARD, BLOOD VESSEL, GREEN MONSTER, BRIGHT LAZULI … CROSS STRIPES: STANDARD, PURPLE MESH, PALACE
TAPESTRY, GREEN CHURCH; 45 names in all). Up / Down wrap around (tone 1). ✕ (tone 2) writes
`0x8003EA73` and queues MENU_TIM 26 + 4v + scheme straight away, staying in the list. ○ back.

**Part menu** (`0x8008B110`, input `0x8008B304`): GENERAL, HEAD, CORE, ARMS, LEGS, DEFAULT (wraps).
- GENERAL … LEGS → the colour editor for that part.
- **DEFAULT** copies the default paint (`0x8005295C`) over all 48 bytes at once, **with no question**,
  and rebuilds the palettes. The default gives every part BASE (32, 36, 28), OPTIONAL (16, 20, 12),
  DETAIL (18, 24, 12), JOINT (12, 18, 12).
- In the Arena Maker's garage (`0x8003EA7D` = 1 or 2) ✕ only plays tone 7.

**Colour editor** (`0x8008B900` → `0x8008B9BC`, windows (16, 40, 144, 128) and the help line
(20, 177, 134, 24); input `0x8008BDF0`). It shows the part (GENERAL, HEAD, …), the colour (BASE COLOR,
OPTIONAL COLOR, DETAIL COLOR, JOINT COLOR), three bars R, G, B and the values as six digits. It edits
the paint bytes in place and rebuilds the palettes every frame, so the AC on screen changes as you go.
On opening it copies the 48 bytes to `0x801D14D0`.

| Button | Does |
|---|---|
| Up / Down | pick R, G or B (wraps) |
| Left / Right | −1 / +1, or −2 / +2 while L1 or R1 is held (Right on G: only R1); stops at 0 and 64 |
| △ | next colour: BASE → OPTIONAL → DETAIL → JOINT → BASE |
| ✕ | keep (tone 2) and close |
| ○ | **cancel** (tone 0): puts back the part's 12 bytes (GENERAL: all 48) from the copy, rebuilds, closes |

**GENERAL** edits the core's colours and copies them over the head, arms and legs after every change
(and ○ restores all four).

**For the launcher and previews:** the launcher's renders use `acr_colour_ramp` (`acr/acr_model.c`),
which fades every colour to black and clips values above 31. The game's ramp above keeps lighter
shadows for values 32–64 (the default BASE colour uses them: its green runs 31 → 6 in the game,
31 → 1 in the launcher). The paint layout (`moa_ac_view.c` `paint_section`: head 0, core 1, arms 2,
legs 3) and the palette rows (`acr_models_moa.c`) already match the game.

### 2m. EDIT EMBLEM in depth (2026-11-18)

**The emblem** is 64 × 64 pixels with 16 colours (4-bit); colour 0 is see-through. **It lives only in
VRAM**: pixels at (544, 384), 16 × 64 VRAM units, and its 16 colours at (256, 510). No RAM copy is
kept between screens: LOAD writes it there from the save (+1152, 2,048 bytes, and +3200, 32 bytes,
`boot-loads.md` §4a), the editor's APPLY writes it there, and the shop parks a copy at (560, 384)
while it is open (§2i).

**Opening** (garage menu row 4, no checks): token `0x80068D2C`, the garage windows close, then
`0x8007ED74` sets up the screen (`0x80011F94` twice `?`) and opens the editor task `0x80074DFC`
(group 0, returns to `0x8007EEAC`). The editor:
- takes a 2,048-byte heap buffer for the picture (task +0x8C);
- copies the emblem to a **working copy at VRAM (922, 0)** and an **undo copy at (922, 64)**, and its
  colours to (944, 0);
- reads the picture into the buffer and the colours into an RGB table (`0x8019EA08`, 4 bytes per
  colour) (`0x80077EAC`); colour 0 is made see-through, and a black in any other slot gets the PSX
  "solid" bit so it is drawn;
- sets `0x801B1CDC` = 2 (fast pad repeat, as on the option page).

Every frame `0x800779A0` uploads the buffer to (922, 0) and the colours to (944, 0); the canvas on
screen shows that working copy.

**The canvas** covers x 172–299, y 36–163 on screen (128 × 128): the whole emblem at ×2, or part of
it at ×4 / ×8. The cursor moves one emblem pixel per step on the canvas and 8 screen pixels per
step outside it.

**Drawing controls** (state `0x80074FD4`, tools `0x80079080`):

| Button | Does |
|---|---|
| D-pad | move the cursor |
| ✕ | use the tool (below). Outside the canvas while zoomed: scroll the view |
| L1 / R1 | previous / next tool (7, wrapping) |
| L2 / R2 | previous / next colour (16, wrapping) |
| △ on the canvas | take the colour under the cursor |
| SELECT | zoom ×2 → ×4 → ×8 → ×2, centred on the cursor |
| □ | edit the current colour (below) |
| ○ | the menu (below) |
| SELECT + L1 + R1 | copies the working emblem to VRAM (416, 0) and its colours to (368, 225), a menu picture area the hub reloads anyway: a leftover test copy `?` |

**Tools** (task +0x90; there are no names in the code, only icons; the shapes are told apart by
how they draw):

| # | Tool | Code |
|---|---|---|
| 0 | dot: draws while ✕ is held | `0x80079B70` |
| 1 | line | `0x8007A7BC` |
| 2 | box | `0x8007AAD0` |
| 3 | filled box | `0x8007ADD8` |
| 4 | circle | `0x8007A338` |
| 5 | fill (flood fill, with a work buffer from the heap) | `0x80079E14` → `0x8007B094` |
| 6 | shift: the d-pad moves the whole picture one pixel up, right, down or left (wrapping `?`) | `0x80079F4C` |

The shape tools (1–4, `0x80079BE0`) take a first point on ✕ and draw on the second ✕; ○ drops the
first point. **The undo point** (`0x8007B3C0`: (922, 0) → (922, 64)) is saved on opening, on CLEAR,
before each line, box, circle or fill, and when the dot tool is picked. So UNDO takes back the last
shape or fill, or every dot drawn since the dot tool was picked; shifts are not undo points.

**Colour editing** (□, state `0x80077270`): L2 / R2 change which colour, Up / Down pick R, G or B,
Left / Right step the channel by 8 (32 steps, 0–248). ✕ keeps the colour, ○ puts it back as it was
when picked, and □, ✕ or ○ return to drawing. Colours are edited in the RGB table and appear on the
canvas at once.

**Menu** (○, window (80, 50, 104, 144), input `0x80075354`): ○ returns to drawing.

| Entry | Does |
|---|---|
| ZOOM | small picker (188, 50, …) for ×2, ×4, ×8 |
| UNDO | one step back for the picture (not the colours): the undo copy (922, 64) back into the picture (`0x8007B3F4`) |
| CLEAR | the whole picture to colour 0, no question (undoable) |
| SAMPLE | the sample picker, window (16, 48, 284, 144). **32 samples**: two sheets, PICTURE MENU_TIM 8 and 9 (each a 256 × 256 4-bit picture at VRAM (960, 0) holding a 4 × 4 grid of emblems; sample n = column + 4 × row has its own 16 colours in CLUT row (336, 240 + n); rectangles `0x800CD3DC` / `0x800CD45C`). D-pad moves (4 across, the rows scroll), L1 / R1 change sheet, ✕ asks "Load the sample?" and copies the sample and its colours into the picture **and the undo copy**, so UNDO can't bring the old picture back. In the Arena Maker's garage only tone 7 plays |
| APPLY | "Change the emblem?" (cursor starts on NO). Yes: **MoveImage (922, 0) → (544, 384) and (944, 0) → (256, 510)**, then leave |
| CANCEL | "Delete changes to the emblem?" (starts on NO). Yes: leave without copying |

Leaving (`0x80077204`) frees the picture buffer and returns to the garage menu.

**For the launcher, overlays and direct starts:** an emblem is just those two VRAM blocks. Writing
a new one means LoadImage of 2,048 bytes to (544, 384) and 32 bytes to (256, 510), as LOAD does;
the next DATA SAVE stores it. The Extra Garage's EMBLEM import only has to produce 16 colours with
colour 0 see-through. Any mod that uses VRAM around (922, 0)–(959, 127) or (960, 0) while the editor
is open would clash with it.

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
- **An assembly of your own** (overlay or launcher) that should behave like the game's: write
  the garage bytes, apply the fix-ups in §2h (they differ from "refuse anything illegal": the game
  removes, moves or restores parts and says so), then refresh the AC as LOAD's finish does
  (`boot-loads.md` §4a). The game's preview numbers are `0x8007E730` on a temporary copy of the
  garage. Its textures go straight into the AC's VRAM pages, so a mod that previews parts the
  same way must put the fitted part's textures back afterwards.
- **A shop of your own:** owned and stock counts, price = the part table's +36 both ways, and
  the leaving step that unfits sold parts (§2i). While the game's shop is open, VRAM (704, 0)
  holds the backup of the AC's textures, so nothing else should use that area then.
- **Options of your own** (§2j): the fitted bits are `0x8003EA70`; the only rule the game applies
  is "the option's slots ≤ the core's slots left". Its picture is MENU_TMD 218 + bit and SPEC_T
  entry = bit, landing on VRAM (384, 256), which must be put back afterwards (a two-slot back
  unit's texture parks there).
- **PERFORMANCE in the launcher** (§2k): copy the table, use the compiled radar range (back radars,
  Human Plus), add option prices, and the GRADE is OVERALL against 10,000 / 15,000 / 20,000 / 25,000.
- **Paint** (§2l): pattern `0x8003EA72`, scheme `0x8003EA73`, 48 paint bytes at `0x80036FBC`; after
  changing them, either queue the pattern's textures and colours as `0x8008E70C` does, or for
  pattern 0 run the ramp `0x80082530` (or LoadImage rows 486 / 488 / 489 / 490 built the same way).
  A preview drawn outside the game should use the game's ramp (§2l) to match.
- **Emblem** (§2m): two VRAM blocks, (544, 384) and (256, 510). A launcher import that writes them
  (as LOAD does) gives the game the new emblem; the next save keeps it.

## 4. Open

- ~~Which token task marks which garage page~~ (§1a, 2026-11-18).
- The sprite routine's first argument; the text routine's colour value (ASSEMBLY turns a number
  red by setting the text task's +0x16 to 13 after creating it, §2h).
- Whether contents are really placed relative to their layer's window, and the layer order.
- Tones 8 and 9 (one or two uses each).
- The garage exit's `0x8003EA7D` values 1 … 3 (Arena Maker roads).
- ~~The detail key in the shop's part list~~: △ (§2i, 2026-11-18).
- Shop (§2i): what stock and owned 127 mean, whether a sold option stays fitted, the preview's
  framing numbers, where new stock comes from (missions lane `?`).
- ASSEMBLY (§2h): what the preview slide value (±180 / 360 / 540) changes, MENU_TMD entry 2 and
  MODEL slot 17 (NO EQUIP stand-ins).
- OPTIONAL PARTS (§2j): the VS copy of the opener sizes the list window for 6 rows while the page
  shows 5 (check in game); the kind table's +0x0C table; the framing record's other fields.
- EDIT EMBLEM (§2m): the tool names (only icons; the shapes were told apart by their code), whether
  SHIFT wraps around, what `0x80011F94` changes on entry, and the SELECT + L1 + R1 copy.
- CHANGE COLOR (§2l): which parts of a texture use each of the four colours (the palette order is
  BASE, OPTIONAL, DETAIL, JOINT; what is painted with which is up to each texture).
