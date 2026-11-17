# Missions (Scenario) — missions chat

Lane scope: see `README.md`. Static reads from the USA disc 1 (.bin MD5
`4d5774c4baa65d6eaf281db59ee8e945`). Read `games/master-of-arena/PHANTASMA-MISSIONS.md` too: it
covers the mission screen table and how the Phantasma missions are packed for MoA.

## Where mission data lives on the disc (2026-11-17)

**Correction to earlier notes:** MoA's per-mission code and maps are **not** in MIS.T. They are in
`GG/COM/FDAT.T`.

The missions engine (FDAT 207) loads with `0x80016A58(file, entry, dest)`; file table `0x8004402C`
(12-byte records in RAM, filled at boot). File **2 = FDAT.T** (same file as "common" = entry 205).
Mission slot = byte `0x800A0DB0`.

| Load site | File, entry | Destination | What |
|---|---|---|---|
| `0x800556B8` | file 1 (?), slot | — | textures |
| `0x800556D4` | FDAT 205 | buffer | common (messages etc.) |
| `0x80055874` | FDAT slot × 5 | `0x800A0DD0` | mission code |
| `0x80055894` | FDAT slot × 5 + 4 | buffer | mission map |

FDAT entries 0–99 = 20 missions × 5 entries:
- **5n = code**, 32–55 KB. Starts with pointers into `0x800A....` (it's linked to run at `0x800A0DD0`).
- 5n+1 … 5n+3 = empty.
- **5n+4 = map**, 220–720 KB. Starts with the 84-byte header (80, 19, then 4 × 19 …), then blocks of
  `size word + data`. Each block ends with sum(words) + 0x12345678.

FDAT 100–199 have the same 5-entry layout, but their code is linked near `0x80099xxx` (another
engine). Likely stage data; not this lane (see README).

### FDAT.T / MIS.T header format
`u16 count`, then `count + 1` u16 sector starts (relative to the file), entry k = sectors
`start[k] … start[k+1]`. For FDAT the tools' `extract.py` reads `u16[k+1]`; same thing.

### MIS.T (`GG/MS/MIS.T`, 409 entries, 460 sectors)
Mission screen material, not mission code:
- entry 0 = the mission screen table copied to `0x8019F7A8 + 80` (see `PHANTASMA-MISSIONS.md`).
- entries 1 … ~380, in groups of 5 per slot: briefing text (2 KB, e.g. "Sender: Lana Nielsen /
  Requester: …"), 12 KB pictures, many empty slots.
- entries ~381 … 408: bigger blocks and the mail texts ("Sender: …" per mail).
Exact per-slot layout still to map.

## Menu → engine: how the mission number gets there (2026-11-17, static)

Mission screen table `T = 0x8019F7A8` (menu, FDAT 206). In the mission screen's per-frame code
(around `0x80097584`, runs when its state `+0x4C` = 4):

- **T+1 (signed byte) = slot just accepted**, −1 = none.
  - **T+1 < 0** (`0x80097594`): slot = T+56 list[T+45] (T+45 = cursor row) → T+1. If flags[slot]
    (T+20) bit 0 is clear, set it and add **T+944 u32[slot]** to credits `0x8003EA74` (capped at
    99,999,999). Stock T+944 is 0 for every mission, so it's dormant (advance pay? `?`).
  - **T+1 ≥ 0** (`0x80097660`, the launch): T+0 = slot, T+1 = −1, T+8 += 1 (capped 999; missions
    played?), then
    - **player block +0x255 (`0x800453ED`) = T+1136 [slot]** = engine mission number,
    - `0x8003EA29` = T+1160 [slot],
    - `0x8003EC4C` = T+1040 [slot] (reward),
    then sums several values through `0x8009B488` (not read yet).
- The missions engine reads it back in its init `0x80055628`: **slot byte `0x800A0DB0` =
  player block +0x255**, then loads FDAT slot × 5 (code) and slot × 5 + 4 (map).

So player block +0x255 is the same byte VS uses for the stage (compiled +0x05): in missions it
holds the mission number. Other writers of it in the menu: `0x800ADD50` (VS map select),
`0x8007DBA0` (writes 40 and `0x8003EA29` = 255, then compiles: some non-mission fight `?`).

### Stock per-slot values (MIS.T entry 0, copied to T+80)

Engine number (T+1136) = slot for every stock mission. T+944 = 0 for all.

| Slot | Name | Reward (T+1040) | T+1160 | Window (T+80) |
|---|---|---|---|---|
| 0 | Eliminate Fugitive | 32,000 | 255 | 0–1 |
| 1 | Defend Submarine | 34,000 | 255 | 5–12 |
| 2 | Destroy Berserk MTs | 36,000 | **38** | 1–3 |
| 3 | Destroy Cannon | 33,000 | 255 | 9–12 |
| 4 | Recover Meteorite | 38,000 | 255 | 1–3 |
| 5 | Enemy Fortress | 36,000 | **35** | 5–12 |
| 6 | Assist Marine Lab | 46,000 | 255 | 8–8 |
| 7 | Retrieve Secret Files | 33,000 | 255 | 2–3 |
| 8 | Cavern Invasion | 0 | **52** | 4–4 |
| 9 | Enter Abandoned Mine | 49,000 | 255 | 13–13 |
| 10 | Board Spaceship | 42,000 | 255 | 9–12 |
| 11 | Attack Terrorists | 0 | 255 | 4–4 |
| 12 | Assist MTs | 55,000 | 255 | 17–17 |
| 13 | Assault Transports | 35,000 | 255 | 2–3 |
| 14 | Attack Chrome HQ | 0 | 255 | 4–4 |
| 15 | Destroy Giant Tank | 40,000 | 255 | 9–12 |
| 16 | Eliminate Intruders | 56,000 | 255 | 19–19 |
| 17 | Defend Laboratory | 52,000 | 255 | 15–15 |
| 18 | Enter Factory | 60,000 | 255 | 21–21 |
| 19 | Final Mission | 0 | 255 | — (ff) |
| 20–23 | Dummy | 0 | 255 | — |

T+1160 is 255 except on three missions; guess: an enemy AC to load (arena roster number?) `?`.
Window = (lo, hi) bytes of T+80 [slot]; likely the chapter range the mission is offered in `?`.

## Mission state record `M = 0x800A0DB0` (missions engine)

Set up by the engine init `0x80055628`:

| Offset | Size | Meaning |
|---|---|---|
| +0x00 | u8 | mission slot (= player block +0x255) |
| +0x04 | ptr | mission code module = `0x800A0DD0` |
| +0x08 | ptr | load buffer (`0x800C2D30` at start, advanced as map blocks are read) |
| +0x0C | u32 | **end result** (0 while running; see below) |
| +0x10 | u16 | end countdown: set to 100 frames when the result is first set |
| +0x12, +0x18 | u16 | 0 at start |
| +0x14 | u16 | cleared before some ends (`0x8007E5B0`) `?` |
| +0x16 | u16 | 128 at start `?` |
| +0x1A | u16 | 64 at start `?` |

## Mission code module (FDAT slot × 5, runs at `0x800A0DD0`)

32–55 KB of MIPS code linked to `0x800A0DD0`. It calls straight into the engine (89–103
distinct engine routines per mission; most used: `0x8007C290`, `0x80015138`, `0x8002C0F4`,
`0x80054D24` / `0x80054C04`, `0x8007A1BC`, `0x8007BBAC`, `0x80079508`). It starts with a table:

| Offset | Called from (engine) | When / what |
|---|---|---|
| +0x00 | `0x80091390` ← main loop `0x80052608` | **every frame** (mission logic) |
| +0x04 | `0x80052590` | **once at start** (mission init) |
| +0x08 | `0x80090B28`, arg = event number | mission-specific event from the event dispatcher around `0x80090A30` `?` |
| +0x0C | not found yet | — |
| +0x10 | `0x8007EE6C`, arg = object | per-object callback `?` |
| +0x14 | `0x80055AF4` | after the map blocks are loaded (map setup) |
| +0x18, +0x1C | not found yet | often the same stub (`jr ra`) |
| +0x20 | — | 0 |
| +0x24 … +0x5C | `0x8007ED44`: `*(module + 0x20 + 4 × obj+0x0E)` | **handler per object type**, types 1 … 15 (unused = 0) |
| +0x60 … | `0x80077C94`: `*(module + 0x60 + 4 × section+0x3D)` | **handler per map-section type** (doors, breakable buildings…), see "Map sections" below |

(Corrected 2026-11-17: the type list is a fixed 15 slots, and a second table starts at +0x60.)
Mission 0 has 3 object-type handlers and 2 section handlers; mission 1 (FDAT 5) 6 and 4;
mission 19 (FDAT 95) 1 and 2.

## How a mission ends

`0x80052490(code)`: stores `code` in M+0x0C and starts the 100-frame countdown (only the first
call counts). The codes found:

| Code | Set by | Meaning | Back to menu (`0x8003EC50`) |
|---|---|---|---|
| 0x80 | the mission code (missions 0, 1, 19), event dispatcher | **mission complete** | 0 |
| 0x100 | mission code (mission 1, with message 158), `0x8007E5B4` and `0x80090B0C` after dialog `0x80058B58(149)` returns 1 | **failed** (objective lost, or quit from a confirm dialog `?`) | 1 |
| 0x200 | engine `0x800527A0` when a byte (+73 of a struct) hits 0 | probably **AC destroyed** `?` | 2 |

At the end (`0x80052878`), the engine writes `0x8003EC50` = 1 / 2 / 0 and returns to the menu,
**except** after a successful mission 18 (Enter Factory): it sets player block +0x255 = 19 and
returns 1, so the Final Mission starts straight away.

Mission number **40** is a special case in the engine (message 155 vs 156 at the 0x200 end). The
menu writes 40 at `0x8007DBA0` before compiling the AC, so it's a non-mission use of the missions
engine (garage test? `?`).

## Map file (FDAT slot × 5 + 4)

**Corrected 2026-11-17.** The file is just a run of `u32 size` + data blocks, and the first one is
the 80-byte "header" (`19`, then 19 × `4`): the loader (`0x8005589C` …) treats it as a block too.
The old table mixed the two numberings. Below, "file block" counts the blocks *after* the header
(so file block 0 = the 171,868-byte one in mission 0):

| File block | Handled by | What (mission 0 size) |
|---|---|---|
| header | `0x8005999C(72, data)` | 80 bytes: 19, 19 × 4 `?` |
| 0 / 1 / 2 | `0x8005999C(260 / 128 / 160, data)` | graphics uploads `?` (171,868 / 170,944 / 12) |
| 3 | `0x8005D9EC` | ? (26,372) |
| 4 | pointer kept at `0x801F8E90` | ? (592) |
| 5 | `0x800529F0` (result → M+0x08) | ? (52,988) |
| 6 | copied to `0x801BFAB0`, then `0x80073694` | 1,024 offset pairs turned into pointers, then their data (46,540): a 32 × 32 grid of lists (collision? `?`) |
| **7** | `0x80077A00(data, size)` | **map sections**: 512 × 52-byte records, then a data heap (46,848) — see below |
| 8 | `0x80078F58` | ? (52; trigger zones? `?`) |
| 9 | `0x80078DE8` | ? (16,384, same size in every mission) |
| 10 | `0x8005CB58` | ? (14,336, same in every mission) |
| **11** | `0x800795A8` | **object types**: u16 count (3 in mission 0), then per type `u32 size` + 36 bytes of parameters (→ type entry +4) + model data (appended to a heap) |
| **12** | `0x80079774` | **placement records**: 64 × 40 bytes read (→ record +4 … +0x2B); block is 10,240 bytes |
| 13–18 | loop from `0x80055A08` | **three sound banks**: VAB header (3,616 bytes, starts `pBAV`) + sample data, × 3 |

## Map sections (file block 7, 2026-11-17)

`0x80077A00` copies the 512 records (52 bytes each) to **`0x801CFAB0` with a 64-byte stride**
(section n = `0x801CFAB0 + 64n`) and the data after them to a heap at `0x801D7AB0`. A section whose
+0x06 or +0x1E is −1 is unused (mission 0 uses 0–319). While loading it fills in:

| Offset | Meaning |
|---|---|
| +0x00 … +0x27 | from the file (bounding box at about +0x18 … +0x24; neighbour counts +0x0E / +0x26) |
| +0x28 | pointer to the section's link list (in the heap) |
| +0x2C | **controller** (0 at load; set by the section handler) |
| +0x30, +0x34 | flags / value from the file (+0x30 bit 0x100 = solid? `?` doors clear it while open) |
| +0x38 | pointer to the section's **parameter list** (file byte +0x30 = number of u16s) |
| +0x3C | byte from the file |
| +0x3D | **section handler number** (file byte +0x31; 255 = none) |

After loading, for every used section with a handler, the engine calls the mission module's
**section handler** (module +0x60 + 4 × handler) with the section pointer. The handler allocates a
**controller** with `0x80077D28(extra bytes)` and stores it at section +0x2C:

| Controller offset | Meaning |
|---|---|
| +0x00 | per-frame update (section) |
| +0x04 | **message handler** (section, message, …) |
| +0x08 … +0x0C | movement per frame, applied by `0x80077D8C` (moves the section's box) |
| +0x10 | state (doors: 0 closed, 1 opening, …) |
| +0x11, +0x12 | step counter / number of steps |

### Messages (2026-11-17)

`0x80090C24(target, message, …)` sends a message: if the target is in the object pool it calls the
object's **+0x5C**, if it is a map section it calls the section controller's **+0x04**. Codes seen:

| Message | Sent by | Meaning |
|---|---|---|
| 0 | `0x8007F5E8` every frame to the player AC; AC setup | "think" / per-frame control (player AC: reads the pad) |
| 5, 7 | `0x8007D128`, `0x8007A7FC` / `0x8007AAEC` | ? |
| **0x8000** | `0x80089774` (weapon hit on map geometry) | **section was shot** (hit info passed along) |
| **0x8100** | `0x80090EDC` | **player touches / uses** the thing in front of it |
| **0x8200 + n** | event dispatcher `0x80090B90`, script reader `0x8009276C`, mission code | **command n** to a section (mission doors: 0 = open, 1 = open + unlock, 2 = special close state) |

### Mission 0's two section types

- **Handler 0 (`0x800A2600`) = sliding door** (16 sections). Parameters: byte +6 = "locked"
  message number (0 = unlocked), +8 / +9 = open / close sounds, +0x0A = stays open, **+0x0B = 1:
  automatic** (opens when the player AC is close, `0x800A2130` box test against `0x801E7250`),
  otherwise it opens on message 0x8100. Opening clears section flag 0x100.
- **Handler 1 (`0x800A2A14`) = breakable structure** (section 285). Message 0x8000 subtracts the
  hit's damage from parameter u16 +0 (HP, 3,000 here); at 0 it explodes and `0x800A2708` either
  swaps in a wrecked version (parameter +0x10) or removes the section (`0x80072F18`, which leaves
  +0x06 = −1). Parameter +0x12 is added to one of two counters at `0x801F8E70` +0x24 / +0x28
  (score / damage tally `?`).

## Mission objects (2026-11-17, static)

**Object pool:** 16 objects × 376 bytes from `0x801E7248`. **Slot 0 = the player AC** (AP at
+0x168 = `0x801E73B0`, as the battle chat noted). Slots 1–15 (from `0x801E73C0`) = mission
objects; same layout as the AC object where they overlap (+0x168 AP, +0x16C, +0x6D, +0x88 anim
data…). `0x8007EA24` finds a free slot (+0 = 0); if the placement has flag 0x8000 it may take
over a slot whose placement doesn't. `0x8007EACC(rec)` finds the object spawned from a record.

**Type table:** 44-byte entries at `0x801E65E8 + 44 × type` (type 1 = `0x801E6614`), filled
from file block 11. Entry +0 / +2 / +4 → object +0x72 & +0x28 / +0x74 & +0x2A / +0x1E; entry
+0x28 = animation data pointer → object +0x88.

**Placement records:** 64 × 44 bytes at `0x801E6748`, filled from file block 12. Records 4–63
are checked for spawning, 4 per frame (`0x8007F20C` → `0x8007EEE0`).

| Offset | Meaning |
|---|---|
| +0x00 u8 | 1 = spawned (object exists) |
| +0x01 u8 | object slot |
| +0x02 u16 | copy of +0x1A at load |
| +0x04, +0x06, +0x08 | → object +0x08, +0x0A (−1), +0x0C |
| +0x0A s16 | **object type** (−1 = unused) → object +0x0E; picks the mission's handler and the type entry |
| +0x0C … +0x13 | → object +0x10 … +0x17 (position? `?`) |
| +0x12 u16 | **map section number** (bit in the active-section set at `0x801EA9E8`, see below) |
| +0x14 u16 | flags: bit 0 = spawn by zone, 0x40 = disabled, 0x8000 = priority, bit 5 → object +0x32 |
| +0x16 u16 | **trigger zone** number (52-byte zones at `0x801E49D8`) |
| +0x1A u16 | → object +0x168 (AP), +0x16A, +0x16C |
| +0x22 … +0x2B | → object +0x104 … +0x10D |

**Active sections** (corrected 2026-11-17: not event flags). Each frame `0x8007F20C` clears the
512-bit set at `0x801EA9E8` (`0x8007448C`) and sets (`0x800744A8`) the bit of the map section the
player is in plus its linked sections. Sections = 64-byte records from file block 7 (see "Map sections"), at
`0x801CFAB0` (section number = (ptr − `0x801CFAB0`) ÷ 64; +0x26 = link count, +0x28 = link list).
A second section (pointer `0x801BB58C`) is added when `0x801EAA30` bit 0 is set (camera? `?`).
The same set is also read at `0x800798D4`, `0x80079C2C` and `0x8007D7CC` (drawing / culling `?`).

**Spawn rule** (`0x8007EEE0`): skip if type = −1, already spawned or disabled. Spawn if the
record's map section is active (player in it or next to it), **or** (bit 0) if the player is inside its trigger zone: zone
+0/+2/+4 = centre x / y / z, signed bytes +0x22 … +0x27 × 256 = box limits. Spawning
(`0x8007ECEC`) takes a pool slot, copies the record in (`0x8007EB00`), then calls the mission's
**handler for that type** (module +0x20 + 4 × type), which sets the object up.

So a mission = its object types (models + parameters), up to 60 placements that appear when the
player gets near their map section or walks into a box, and its own code for each type.

**Type handlers set the object's callbacks** at +0x58, +0x5C, +0x60, +0x64, plus +0x36 and
+0x16C. Mission 0: type 1 (`0x800A1680`) sets +0x60 / +0x64 only (hittable, no own movement `?`);
type 2 (`0x800A1EA8`) sets all four (the enemy `?`); type 3 (`0x800A1F50`) sets none (scenery `?`).

**Object callbacks** (2026-11-17). The per-object update `0x8007E6A8(obj)` (from `0x8007F678`)
stores the current object at `0x801EA9CC` and runs, in order:

| Slot | Called | Meaning |
|---|---|---|
| +0x54 | first, no arguments | **movement**; afterwards the object's map section (+0x16) is recomputed from its position (`0x8007438C`) |
| +0x60 | next, (obj) | **per-frame logic** |
| +0x5C | (obj, message, …) via `0x80090C24` | **message handler** (see "Messages") |
| +0x64 | (obj, hit info, `0x8003EA48`, obj+8, damage) | **hit / damage** (as `battle-system.md` says) |
| +0x58 | (obj, state) from AC setup (state = +0x4A) and the script reader (state 0) | **set state / animation** `?` |

Also in that update: while +0x3A > 0 the object gives off an effect each frame (smoke `?`); while
+0x3C > 0 it takes +0x3E damage per frame (AP +0x168, floored at 0) and its +0x64 is called with
damage type 23 (burning `?`).

The player AC's callbacks are set by `0x80086CC0`: +0x58 `0x80084F98`, +0x5C `0x80086840`
(message 0 = read the pad), +0x60 `0x80086CA0`, +0x64 `0x80083C68`.

## Mission 0 (Eliminate Fugitive): how it is won (2026-11-17, static)

The every-frame hook (`0x800A2E78`) does nothing once M+0x0C has an end bit (0x381) or the
player's AP is 0. Otherwise:
- A frame counter at `0x800A87F8` plays opening messages (at frame 10 and 200).
- **Win:** when the **automatic door in section 290 starts opening** (its controller state = 1,
  i.e. the player AC walked up to it), the hook clears bit 7 of player block bytes +0x2E4 /
  +0x322 / +0x360, runs `0x80093844(0x1300, …)`, spawns placement record 5 (`0x801E6824`), runs
  `0x80093844(0x301, …)`, shows message 153 and ends with **0x80** (complete).
- Separately it shows message 119 once, the first time a check involving section 285 (the
  breakable structure) passes `?`; if section 285 is already gone it skips the message.

Nothing in the hook counts kills, so in mission 0 the objective is "reach door 290" as far as the
code is concerned (the fugitive may be what blocks the way `?`). Other missions will differ: each
has its own hook.

## Broken Phantasma missions: what sets them apart (2026-11-17, static, PP USA disc)

Mission #k = Phantasma FDAT 5(k−1) (code, base `0x80097A38`) and 5(k−1)+4 (map). PP's missions
engine is **PP FDAT 202, loaded at `0x800490D0`** (the only overlay that calls `0x80097A38`).

**Ruled out:** PP modules use the same table layout as MoA (hooks, 15 type slots, section
handlers from +0x60); every section handler number and placement type in all 17 PP maps exists
in its module; maps have the same 19 blocks (no header block); every PP code file fits MoA's
module space (MoA engine data starts at `0x800AE5D0`, so the limit is 55,296 bytes = PP #7's
size exactly, same as MoA mission 6).

**The difference:** comparing the engine routines each PP mission calls against the twelve that
work (2–6, 8, 10, 12, 13, 15–17):

| Mission | PP engine routines no working mission calls |
|---|---|
| #1, #9, #14 | `0x8004B80C`, `0x8006B2BC`, `0x80072D94`, `0x80076880`, `0x800886F8` (the same five) |
| #9 also | `0x80015344`, `0x80015968` (exe), `0x80073724` |
| #7 | none (see data below) |
| #11 (wrong view) | `0x8002CC20`, `0x8004D078`, `0x80054F8C`, `0x80083ED0`, `0x8008475C` |

So the converter (`moa_phantasma.c` fn map) is most likely missing or mis-mapping these. Mission
#1 calls two of them straight away: its start hook (+0x04, `0x8009CFE8`) calls `0x80076880` to
put placement record → object slot 1, and its every-frame hook calls `0x800886F8` on frames 0–20.
A bad mapping there breaks the mission on its first frame (matches "black / hang").

MoA counterparts, found by matching instruction patterns (24 of 24 = same code, different
addresses):

| PP routine | What it is | MoA match |
|---|---|---|
| `0x80076880` | copy a placement record into an object (PP objects are 368 bytes, pool `0x801E2DE0`) | `0x8007EB00` (same code, MoA's 376-byte objects) |
| `0x800886F8` | screen fade-in, frames 0–20 | `0x80090FF4` (24/24) |
| `0x80072D94` | ? (object routine) | `0x8007ABF0` (24/24) |
| `0x8006B2BC` | ? (searches map sections) | `0x8007302C` (24/24; runner-up `0x80077940` 23/24) |
| `0x8004B80C` | play sound effect (id, ?, volume L, volume R, …) | `0x80053320` `?` (same arguments; MoA adds a play-once-per-frame check) |
| `0x80015344` / `0x80015968` | exe routines | `0x80014D08` / `0x8001532C` (24/24) |
| `0x80073724` | small object setter | one of `0x8007B5D0` / `…60C` / `…648` / `…684` (four look identical; check which field each sets) |
| `0x8002CC20` | exe | near `0x8002C0F4` (17/24, weak) |
| `0x8004D078` | ? | `0x80054BCC` (24/24) |
| `0x80054F8C` | ? | `0x8005CBD8` (24/24) |
| `0x80083ED0` | ? | `0x8008C5C8` (14/24, weak) |
| `0x8008475C` | ? | `0x8008CD84` (24/24) |

PP data addresses (lui-based) used only by the broken missions; each needs a data-map entry:
- #1, #9, #14 share `0x801A0020`, `0x801A7FAC`, `0x801DD340`, `0x801E0008`, `0x801E000C`,
  `0x801E3190`, `0x801E31B4`, `0x801E31C0`, `0x801E6888` (+ #1: `0x801BE2E8`, `0x801E091C`,
  `0x801E0948`; #9: `0x8008F060`, `0x801A0000`, `0x801E2DEC`; #14: `0x801E00DC`).
- #7: `0x8003BCDC`, `0x801BD9A8`, `0x801BD9E0`, `0x801DFA80`, `0x801E002C`, `0x801E04A0`,
  `0x801E0580`, `0x801E2C00`, `0x801E2F40` (= player object +0x160 in PP's layout).
- #11: `0x801E0630`.

PP layout for those maps: type table `0x801DFC60` (44-byte entries, MoA `0x801E65E8`), object pool
`0x801E2DE0` (368-byte objects, MoA `0x801E7248`, 376 bytes).

Next: compare this list with the converter's function and data maps in the repo (needs GitHub).

## Open
- What `0x8003EA29` (from T+1160) does in the engine; what T+8 counts.
- Mission 0's three type handlers (`0x800A1680`, `0x800A1EA8`, `0x800A1F50`): what they set up.
- Messages 5 and 7; what `0x80093844(0x1300 / 0x301)` runs (cutscene / script `?`).
- Other missions' win checks (kill counts? object AP?): read each mission's +0x00 hook.
- File blocks 0–6, 8–10; the 36-byte type parameters; the 32 × 32 grid in block 6.
- Hooks +0x0C, +0x18, +0x1C; the event numbers passed to +0x08.
- Mission number 40: which menu screen uses it.
- Layout of the per-mission code module: entry points / table at its start.
- Map header fields (80, 19, 4 × 19) and the blocks after it.
- Objectives, win/lose checks, timer, results and rewards; enemy list; script reader
  `0x80091FFC` (stepped at `0x80092E4C`; writes AC +0x6D/+0x7C/+0x7E/+0x84).
- MIS.T per-slot layout and the mail table.
