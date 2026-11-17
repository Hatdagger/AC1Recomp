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
- entry 385 = 20 KB of part descriptions ("Head unit with built-in bio sensor." …) `?`.
- **entry 386 = the mail table**, **387–406 = the 20 mail texts**, **407 / 408 = the two mail
  pictures**. See "Mail" below.
Per-slot page layout of the mission entries: see `PHANTASMA-MISSIONS.md` (T+1184 page records).

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
| +0x08 | `0x80090B28`, arg = event number | mission-specific event: event kind 9 of the dispatcher `0x80090934` (see "Event points") |
| +0xA0 … | `0x80078EF4`: `*(module + 0xA0 + 4 × point+0x06)` | **handler per event-point type** (see "Event points") |
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
| 0x101 | engine main loop `0x8005273C`: player block +0x08 & 7 = 1 (outside both inner area boxes, see `battle-system.md` stage boxes), message 157 "Out of area. Combat halted." | **out of area** | — |
| 0x200 | engine `0x800527A0` when player object +0x49 (`0x801E7291`) = 0, message 155 "Defenses damaged. Cannot continue combat." (156 "…Test mode disengaged." when the mission number is 40) | **AC destroyed** | 2 |
| timer | engine main loop `0x80052708` when the mission timer reaches 1 (below) | code and message chosen when the timer was started | — |

**Mission timer** (2026-11-17): **M+0x14** counts down by 1 per engine pass (only while M+0x12 ≥ 16
and the player AC is alive, +0x49 = 255) and stops at 1. At 1 the engine shows message
`0x801F8E70` (byte) and ends with code `0x801F8E72` (u16). Started by **`0x80090FD8(ticks, code,
message)`**, or by event kind 4 (show message *a*, timer = *b* × 22, code 0x100 / 0x200 if *d* = 1 /
0x80 if *d* = 2, message *c*). The HUD clock (`0x8005681C`) shows ticks × 100 ÷ 22 hundredths of a
second, so **22 ticks = one second on the HUD**. Setting M+0x14 = 0 stops the clock.

At the end (`0x80052878`), the engine writes `0x8003EC50` = 1 / 2 / 0 and returns to the menu,
**except** after a successful mission 18 (Enter Factory): it sets player block +0x255 = 19 and
returns 1, so the Final Mission starts straight away.

Mission number **40** is a special case in the engine (message 155 vs 156 at the 0x200 end). The
menu writes 40 at `0x8007DBA0` before compiling the AC, so it's a non-mission use of the missions
engine. The message texts settle what it is: **test mode** (152 "Combat system / Engaging test
mode.", 154 "Target demolished. Test mode disengaged.", 156 "Defenses damaged. Test mode
disengaged.").

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
| +0x00 u8 | 1 = spawned (object exists), **3 = destroyed** (never spawns again; set by `0x8007EE04`, see "How each mission is won") |
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

## How each mission is won (2026-11-17, static, all 20 every-frame hooks read)

### Shared pieces

- **Destroying an object** = `0x8007EE04(obj)`: if obj +0x36 bit 1 is set it adds placement AP
  (+0x1A) ÷ 200 to `0x8003EC54` (destruction bonus tally `?`), calls the mission's hook **+0x10**
  (stub `jr ra` in all 20 MoA missions), frees the object and sets **placement +0x00 = 3** unless the
  placement has **flag 0x10** (respawning, e.g. a production line). Mission code tests "is
  placement N destroyed?" as `byte 0x801E6748 + 44N == 3`.
- **"Everything destroyed"** checks loop over placements 4–63 and count those with type ≠ −1 and
  +0x00 ≠ 3. Placements not yet spawned and disabled ones (flag 0x40) count too, so every placed
  enemy must die; a placement with flag 0x10 can never reach 3 and blocks the check until the mission
  clears that flag.
- Every hook starts by doing nothing once M+0x0C has an end bit (0x381) or player AP
  (`0x801E73B0`) is 0. "Complete" is always end code 0x80 (usually after message 153 "Battle
  over."), "failed" 0x100 (message 158 "Mission failed.").
- A **box test** some missions use for "reach the exit": player object +0x08/+0x0A/+0x0C against a
  min/max box the init hook builds from an event point (centre +0/+2/+4, signed bytes +0x22 … +0x27
  × 256, the same rule as spawn zones). So for those points +0x22 … +0x27 are box limits, not an
  event (answers part of the "reconcile" open item: it depends on how the point is used).
- **Message numbers** are global (FDAT 205, see "Message texts" below), so each mission's lines
  name who is who.

### Per mission

Placement = the record number (0x801E6748 + 44 × N), AP = its +0x1A. Sections = map sections.

| Slot | Mission | Win (end 0x80) | Lose (other than AC destroyed / out of area) |
|---|---|---|---|
| 0 | Eliminate Fugitive | walk up to the automatic door in **section 290** (see above) | — |
| 1 | Defend Submarine | destroy the three attackers (**placements 10, 11, 12**, AP 8,000 each; msg 23 mentions a battle cruiser); a 660-frame wait follows (msgs 26 "Repairs are almost complete", 27 "…We will be submerging now"), then **stand on the submarine** (sections 56/55/54, on the deck within 10 units of floor height) | **fall into the sea** (player object +0x0A > 256; msg 25 warns, then 158); **section 56 destroyed** (submarine hull, handler 3, HP 30,000; only the first of the three sections is checked `?`) |
| 2 | Destroy Berserk MTs | **every placement destroyed**. Placements 9–12 respawn (flag 0x10) until **section 143** (breakable, HP 3,000) is destroyed: its event (kind 9, *b* = 0) runs hook +0x08, which clears their flag 0x10 and removes point 3 (msg 28 "The production line has been stopped"). Gate locks: sections 69 / 70 (HP 2,000) unlock gates 141 / 72 (msg 160) | — |
| 3 | Destroy Cannon | touch the **four bomb points** (points 8–11; each fires hook +0x08 with 0–3, counter `0x800AB828`); at 4: msg 30, **Toll-Keeper** (placement 5, AP 13,000) appears (script 1, msgs 98, 31, 32); destroy him → timer stops, msg 33 "We're coming in to retrieve you", placement 7 spawns, script 0x102 | **5-minute timer** (6,600 ticks, started when the intro ends): time out = 0x100. The module also has a "cannon fires" sequence (screen flash, script 0x100, msg 158) for the last 10 ticks, gated on byte `0x800AB894`, which the module never sets itself `?` |
| 4 | Recover Meteorite | destroy the five units (**placements 5–9**, AP 1,600; msg 6); then msg 94, **Regulon** (placement 4, AP 15,000) appears (script 1, msgs 7/8); destroy him → msg 9 "Well done" | — |
| 5 | Enemy Fortress | destroy the defenders: when fewer than 10 placements are left, **8 reinforcements** spawn (placements 15–22, table `0x800AA2B0`); when only one is left, msg 20 "The enemy has hired a Raven… Stalker" and script 0 bring in **Stalker** (placement 5, AP 15,000); destroy him | — |
| 6 | Assist Marine Lab | once section 132 opens and flag `0x800AD838` = 1: Fritz's unit (placement 5) leaves, **Barrel** (placement 4, AP 5,000) and **placement 6** (AP 18,000) spawn (scripts 0x1301, 0x202, msg 37). **Destroy placement 6.** If Barrel is still alive then, a scripted ending (script 0x1304) plays first; Barrel dying or falling (object +0x0A > 30,000) shows msg 123 | — |
| 7 | Retrieve Secret Files | destroy **placement 5** (AP 3,000). Placement 12 spawns with msg 38 "The airport should be just ahead. Destroy him before he escapes", then msgs 39/40 name **Lucky Hopper**; placement 4 (AP 13,000) destroyed only plays a sound. Which of 4/5 is Lucky Hopper `?` | hook +0x08 event 0: script 0x900, msg 158, 0x100 (he escapes `?`; what fires event 0 not found yet) |
| 8 | Cavern Invasion | destroy **Evil Kiss** (placement 4, AP 18,000; msgs 41–43) → msg 44 "Evacuate immediately!" → **reach the exit box** (event point 4: x −4,768 … −2,976, y −1,780 … 12, z −9,536 … −7,232) | — |
| 9 | Enter Abandoned Mine | no end call in the module. Destroying **The Wolf** (placement 4, AP 18,000; msgs 45–47) unlocks door section 169 (clears its locked-message byte); when that door opens, msg 48 "Your target is that container. Retrieve it."; **touch the container** = event point 1 (kind 5, message 153, no part) | — |
| 10 | Board Spaceship | **reach the exit box** (event point 0); the pad is then ignored for 50 frames, script 0x200. **William** (placement 5, AP 10,000; msgs 49–51) is optional | — |
| 11 | Attack Terrorists | destroy **Goldy and Super Bow** (placements 4 and 5, AP 13,000 each, spawned at start with script; msgs 55–58) | — |
| 12 | Assist MTs | destroy the six enemies (**placements 4–9**, AP 5,000, list `0x800AA0D8`) | **all three allies** (placements 10, 11, 12, list `0x800AA0F0`) destroyed: each death shows msg 102/103/104, then msg 59 "My squad has been destroyed", 158 |
| 13 | Assault Transports | **every placement destroyed**; when fewer than 8 are left, msg 93 and 4 reinforcements (placements 11–14) | hook +0x08 event 0 → 0x100 (a transport gets away `?`; trigger not found yet) |
| 14 | Attack Chrome HQ | destroy **placement 4** (AP 19,000) | **3-minute timer** (3,960 ticks; msg 60 "Time limit 3 minutes", 61): at 3 ticks left msg 62 "Time is up. Retreat!", script, 158 |
| 15 | Destroy Giant Tank | **every placement destroyed** (placements 4–10) | — |
| 16 | Eliminate Intruders | when door section 24 opens, **Nine-Ball** (placement 30, AP 15,000) spawns (script 0x1200; msgs 64–66); **destroy Nine-Ball** → msgs 69 "all enemy units have retreated", 70. The other intruders are optional | — |
| 17 | Defend Laboratory | three waves of two MTs (placements **4+5, 6+7, 8+9**, AP 5,000; next wave when both are destroyed, gate messages 73/71/72/74); then **Hustler One** (placement 10, AP 18,000; msg 75 when first hit); **bring his AP to 0** → he's kept alive (+0x49 = 255), script 0x800, complete | — |
| 18 | Enter Factory | when door section 68 opens, **placements 4 and 5** (AP 17,000 each) spawn (script 0x800; msgs 107–110 "???"); destroy both → msg 114, doors 92/93 get command 0x8201 (open + unlock); **door section 96 opens** → 64-frame fade → 0x80 (no message), then the engine starts the Final Mission | — |
| 19 | Final Mission | intro states 0–5 (placement 4 spawns, scripts 0x200–0x202, 0x803); then **bring placement 4's AP to 0** (AP 16,800) → 40-frame fade → 0x80 | — |

Timers: only missions 3 and 14 start one. Kill counts are never numbers in the code: missions
check named placements (by +0x00 = 3) or "all placements gone". Protect targets: mission 1 (the
submarine section), mission 12 (three allied MTs).

### Message texts (FDAT 205)

`0x80058B1C(0, 0 | 200, n)` shows message *n*; the text table is copied from **FDAT 205 block 4, 5
or 6** (picked by a byte at +634 of the struct in `s3` at `0x80055744` `?`) to `0x800AE688`:
u32 offset of a u16 table, entry *n* = offset of message *n* from the block start. The three blocks
hold the same 250 messages except the COM lines (153 reads "Battle over." / "Target of operation
cleared." / "TARGET ACHIEVED."). Encoding: A–Z = 0xAA–0xC3, a–z = 0xC4–0xDD, space and digits as
ASCII, 0x80 + n = digit n in text, 0x8B `-`, 0x8E `.`, 0x8F `,`, 0x90 `?`, 0x91 `!`, 0x98 `:`,
0x9E `'`, 0x0A new line, 0xF0 0x81 … 0xF0 0x82 around the speaker's name; a leading code such as
`M5B32N0X93` or `H21` sets layout / voice / portrait `?`. Used: 3–76, 92–123, 147–166.

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

**Checked against the converter (2026-11-17):** the routine mapping is fine: every routine
above is mapped to the MoA match listed below. The bugs are elsewhere (found by rebuilding the
exact pack from the recipe, sha256 matches, and comparing it with Phantasma's code):

1. **Missions 1, 9, 14 (start-up hang):** the start hook spawns placement record 4 into object
   slot 1 with `addiu a1, s0, -11456` (record pointer = object pointer − 0x2CC0). The base
   (object pool) was moved, the offset was not, so MoA reads a "record" at `0x801E4700`, gets a
   junk type and jumps through a junk handler (the spin at `0x80098DA0`). Should be −0xB48.
2. **16 of 17 missions:** a shared routine that walks objects 1–15 and calls their +0x5C with
   message 8 (area hit `?`) still steps **368 bytes** (Phantasma's object size) instead of 376,
   so from the second object it calls through the wrong field.
3. **More moved-base offsets:** type table ↔ placement records (PP gap 0x580, MoA 0x160), object
   pool ↔ type table (PP 0x3180, MoA 0xC60), and trigger zones read at Phantasma's address
   `0x801D92B8` (MoA `0x801E4A40`) in #7, #12, #15.
4. **#4:** three calls left at Phantasma addresses (`0x800875F8`, `0x80087870`, `0x80087A34`);
   `0x80087A34` = MoA `0x80090658` (24/24); the other two have no clear MoA match yet.

Fixes: branch `phantasma-mission-fixes`, 78 new `k_ppm_fixes` entries (one immediate or call
target per word). Re-scanning the fixed pack finds none of these left. Not yet tested in game.

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

## Event points and hidden parts (2026-11-17, static)

**Map file block 9 = 128 event points** (16,384 bytes = 128 × 32). The loader `0x80078DE8` turns
each 32-byte record into a 52-byte point at **`0x801E49D8 + 52n`** (the same records the spawn rule
uses as trigger zones):

| Point offset | From file | Meaning |
|---|---|---|
| +0x00 … +0x05 | +0 … +5 | position x / y / z |
| +0x06 | +6 | **point type** (−1 = none / removed); picks the handler module +0xA0 + 4 × type |
| +0x08 … +0x0F | +8 … +15 | ? |
| +0x14 | — | controller (allocated by the handler, `0x8007927C`) |
| +0x1C … +0x21 | — | from block 8's 24-byte record for this type (`0x801E46D8 + 24 × type`); +0x1C = 0xC000 when the type is −1 |
| **+0x22 … +0x27** | +16 … +21 | **event 1**: kind, arg byte, arg u16 (+0x24), arg byte, arg byte |
| +0x28 | +22 | 0 = repeatable, else one-shot; 2 = event 2 is used too `?` |
| **+0x2A … +0x2F** | +24 … +29 | **event 2**, same layout |

**Event dispatcher `0x80090934(kind, a, b, c, d)`** (called only from mission code; a = byte +0x23,
b = u16 +0x24, c = byte +0x26, d = byte +0x27):

| Kind | Does |
|---|---|
| 1 | show message *c*, then unlock *a* map sections from *b* (clears their parameter byte +6) |
| 2 | run script `0x80093844(a)` |
| 3 | show message *a* |
| 4 | show message *a*, then `0x80090FD8` (camera / view? `?`) |
| 5 | show message *a*, **give part *b***, end the mission **complete** (0x80) |
| 6 | show message *a*, **give part *b***, end the mission with 0x100 |
| **7** | show message *a*, **give part *b*** (the hidden-part pickup) |
| 8 | "quit?" dialog (message 149); yes → `0x8003EA95` = 1, end with 0x100 |
| 9 | call the mission's hook +0x08 with *b* |
| 10 | show message *c*, send command 0x8200 + *d* to *a* sections from *b* |

**Give part = `0x8009090C(index)`**: inventory byte `0x80036FEC`[index] += 1 (index 0 = nothing).
Index = the global inventory index (heads 0–17, cores 18–28, arms 29–52, legs 53–92, generators
93–107, FCS 108–123, options 124–142, boosters 143–156, back 157–216, hand 217–242, blades 243–248;
see `parts-tables.md`).

**Hidden parts (MoA, all found in the maps):** every map's block 9 searched for kinds 5/6/7 with a part:

| Mission (slot) | Point | Type | Part | Message |
|---|---|---|---|---|
| Destroy Berserk MTs (2) | 4 | 3 | **HD-HELM** (12) | 165 |
| Cavern Invasion (8) | 6 | 0 | **WX-ED2** (212) | 164 |
| Assist MTs (12) | 0 | 4 | **P/CV** (118) | 166 |

These match the GameFAQs part guide (Vesperas) exactly. Missions 9 and 18 carry the same item code but
place no part.

**A hidden part only appears if you don't have it and the shop doesn't sell it.** The item point's
setup (mission 2: type handler `0x800A326C` → check `0x800A3228`, same code in 8, 9, 12, 18) removes
the point (+0x06 = −1) if either event is kind 7 and **owned `0x80036FEC`[part] ≠ 0 or shop stock
`0x800370EC`[part] ≠ 0** (save +640 / +896). So a part added to the shop can no longer be found in its
mission. Otherwise the handler sets the point's message handler (mission 2: `0x800A30E8`): message 0
(touched / used `?`) fires event 1, then removes the point if +0x28 ≠ 0.

**Other games, from the GameFAQs guides (Vesperas), to check when their missions are in:**
- Armored Core: hidden in missions WM-S60/4 (Secret Factory Recon), QX-AF (Exterminate Organisms),
  HD-X1487 (Guard Freight Train), WG-1-KARASAWA (Destroy Fuel Depot), GBG-XR (Destroy Plus Escapee),
  WG-FG99 (Destroy Intruders), WR-L24 (Eliminate Strikers), RZT-333 (Guard Wharf Warehouse; taking it
  ends the contract), WX-S800-GF (Retake Air Cleaner), LS-99-MOONLIGHT (Kill "Struggle" leader), B-T001
  (Guard Factory Entrance); mission rewards LN-SSVT (Remove Gun Emplacement), SP-SAP (Prototype MT Test
  2), SP-AXL (Stop Gang "Dark Soul"); shop line-up upgrades add LN-D-8000R, LN-3001, LFH-X5X,
  LC-MOS4545, then WM-L201, WC-LN350, WC-XC8000. In MoA all of these are in the shop from the start,
  which (rule above) would hide them in their missions.
- Project Phantasma: the guide lists no hidden mission parts; its extra parts are arena prizes.

## Mail (2026-11-17, static)

Mail code is in the menu module (FDAT 206), `0x800A1F00` … `0x800A5100`. Hub entry 4 opens it
(`0x800A273C`, see `menus-hub.md`).

### Mail block `S = 0x801A4F98`

| Offset | Size | Meaning |
|---|---|---|
| +0x00 | 32 × u8 | **per-mail flags**: bit 0 = received, bit 1 = opened (set at `0x800A3178`), bits 2–7 = arrival order (the count at +0x60 when it arrived) |
| +0x20 | 32 × 2 bytes | **trigger table**: (kind, value) per mail, see below |
| +0x60 | u8 | number of mails received so far |
| +0x61 | u8 | cleared at new game `?` |
| +0x64 | u8 | table load in progress |
| +0x65 | s8 | selected row of the mail list |
| +0x66 | u8 | mail screen state `?` |
| +0x70 | 32 × s8 | mail list, newest first (−1 = empty), rebuilt by `0x800A26B8` + `0x800A26E4` |
| +0x90 | 2,112 bytes | copy of MIS.T entry 386 (first 0x840 bytes): +0x90 table, **+0xD0 subjects**, **+0x4D0 page records** |
| +0x8D0 | ptr | screen work area |

**Saved with the game:** S+0x00 … +0x63 (100 bytes, so the trigger table too) = RAM `0x8004B688` =
save +3,572. Copies: S → `0x8004B688` = `0x800A1F68`; back = `0x800A2020` (also `0x800A20D8`
/ `0x800A2190`).
- **New game** (`0x80072E28`, `0x80073364`): `0x800A2318` clears flags and count and loads the table
  from **MIS.T 386** (file 19, async load `0x800A22C8`, callback `0x800A2248` copies 528 words to
  S+0x90 and the 32 pairs to S+0x20), then `0x800A1F68` stores it.
- **Load game** (`0x80073BCC`, `0x8007443C`): `0x800A2318` (disc table), then `0x800A2020` puts the
  **save's** block back over it. So a loaded game uses the trigger table **stored in its save**,
  not the disc's.

### MIS.T entry 386 layout (4,096 bytes)

| Offset | What |
|---|---|
| 0x000 | 32 × (kind, value) trigger pairs; unused = (0xFF, 0) |
| 0x040 | 32 × 32-byte subjects (ASCII, `>` ends); 20 real, 12 "ダミー" (dummy) placeholders |
| 0x440 | 32 × 32-byte page records: **u16[8] pictures**, then **u16[8] text pages**, each = MIS.T entry − 387, −1 = none |
| 0x840 | zero; last word = checksum |

Each mail can have up to 8 text pages and 8 pictures. The screen loads them from MIS.T (file 19)
as entry 387 + n (`0x800A3D68`, `0x800A4764`, …).

### When a mail arrives

`0x800A24FC` delivers mail (called when the mail screen opens, `0x800A2B84`, and at `0x800AC2D4`):
a mail that isn't received yet gets bit 0, its order number and +0x60 goes up. Which ones:

- **Story mode (T+4 = 1)**, by kind:

| Kind | Arrives when |
|---|---|
| 0 | **chapter** T+2 ≥ value |
| 1 | mission slot *value* **completed** (T+20[value] bit 1) |
| 2 | mission slot *value* **completed or failed** (T+20[value] bit 1 or bit 2) |
| 3 and up, 0xFF | never |

- **T+4 = 0** (not story `?`): every mail of kind 0–3 arrives at once.

`0x800A23F4` is the same test without delivering ("any new mail due?"); the hub uses it for "You have
new mail." (`0x800557AC`) and for the Mail entry (`0x8005594C`). `0x800A1F00` = "has any mail".

**Stock table:** all 20 mails are kind 0 (chapter):

| Mail | Chapter | Sender | Subject | Text | Picture |
|---|---|---|---|---|---|
| 0 | 0 | Lana Nielsen | Request | 387 | |
| 1 | 1 | Lana Nielsen | Arena | 388 | |
| 2 | 4 | Lana Nielsen | Sponsor | 389 | |
| 3 | 5 | PROGTECH | Arena Registration | 390 | 407 |
| 4 | 5 | Lana Nielsen | PROGTECH | 391 | |
| 5 | 8 | PROGTECH | Emergency | 392 | |
| 6 | 9 | Elan Cubis | Mysterious MT | 393 | |
| 7 | 9 | Lana Nielsen | Warning | 394 | |
| 8 | 13 | Lana Nielsen | PROGTECH Request | 395 | |
| 9 | 14 | PROGTECH | Promotion | 396 | 408 |
| 10 | 15 | PROGTECH | Destruction Request | 397 | |
| 11 | 16 | Elan Cubis | Nine-Ball | 398 | |
| 12 | 16 | Lana Nielsen | Support Cut Off | 399 | |
| 13 | 17 | PROGTECH | MT Rescue Operation | 400 | |
| 14 | 18 | Elan Cubis | Data Manipulation | 401 | |
| 15 | 19 | Elan Cubis | Connections | 402 | |
| 16 | 19 | PROGTECH | HQ Protection Request | 403 | |
| 17 | 20 | Elan Cubis | RAVENS' NEST | 404 | |
| 18 | 21 | Elan Cubis | RAVENS' NEST Contact | 405 | |
| 19 | 21 | Lana Nielsen | The Promise | 406 | |

### Mission results and chapters (`0x80093CD8`, run on return from a mission)

Mission table T (`0x8019F7A8`): **T+20[slot] bit 1 = completed, bit 2 = failed** (bit 0 = accepted);
T+0x0C successes, T+0x10 failures (each capped at 999). When 19 of slots 0–19 are completed,
`0x8003EACB` = 1 (guest arena unlocked). On a **story-mode success**: chapter T+2 += 1 and points
T+5 += 4; reaching chapter 5 or 14 also gives 50,000 credits and one part (owned-part count
+247 / +241). Then, on any return, chapters jump or wait:
- chapter 5–7 and points ≥ 32 → chapter 8; chapter 9–12 and points ≥ 54 → 13;
- chapter 14 / 16 / 18 / 20 move on to 15 / 17 / 19 / 21 only when byte `0x8004B67D` = 10 / 7 / 4 / 2
  (main-arena rank `?`);
- at chapter 5 that byte is capped at 30, at chapter 14 at 15 (both also clear `0x8004B686`);
- arena lock `0x8004B686` = 0 from chapter 5, 4 at chapters 8, 13, 15, 17, 19, 21.

So chapter-timed (kind 0) mails depend on mission wins **and** arena ranks.

### What this means for per-group mail (mission source picker)

- "Mail after mission N" already exists: **kind 1** (completed) / **kind 2** (played). It is a data
  change in MIS.T 386, no code needed, for MoA's own slots 0–19.
- Saves carry their own table: a mod that changes triggers must also rewrite S+0x20 … +0x5F after
  `0x800A2020` on load (leaving the flags and +0x60 alone), or old saves keep the stock timing.
- There are **32 mail slots, 20 used**: 12 free without enlarging the saved block.
- Phantasma missions all run as slot 20, so T+20 can't tell them apart: a plugin should give their
  mail a kind the game never delivers (4 … 0x7F) and deliver it itself (set bit 0 | (+0x60 << 2),
  then +0x60 += 1), with its text and pictures from the Phantasma MIS.T copy (entry 410+k).

## Open
- What `0x8003EA29` (from T+1160) does in the engine; what T+8 counts.
- Mission 0's three type handlers (`0x800A1680`, `0x800A1EA8`, `0x800A1F50`): what they set up.
- Messages 5 and 7; what `0x80093844(0x1300 / 0x301)` runs (cutscene / script `?`).
- ~~Other missions' win checks~~: done, see "How each mission is won". Left from it: what fires
  event 0 in missions 7 and 13 (the fail), who sets mission 3's `0x800AB894` and mission 6's
  `0x800AD838`, and which placement is Lucky Hopper in mission 7.
- File blocks 0–6, 8–10; the 36-byte type parameters; the 32 × 32 grid in block 6.
- Hooks +0x0C, +0x18, +0x1C; the event numbers passed to +0x08.
- Mission number 40: which menu screen uses it.
- Layout of the per-mission code module: entry points / table at its start.
- Map header fields (80, 19, 4 × 19) and the blocks after it.
- Objectives, win/lose checks, timer, results and rewards; enemy list; script reader
  `0x80091FFC` (stepped at `0x80092E4C`; writes AC +0x6D/+0x7C/+0x7E/+0x84).
- Event points: the other point types (0–2, 4, 5) and the block 8 per-type records.
- Reconcile: the spawn rule reads point +0x22 … +0x27 as box limits, item points use the same bytes as event 1 (per type?).
- Mail: mission mode T+4 = 0 (what it is; it hands out every mail of kinds 0–3 at once); what
  the second delivery call (`0x800AC2D4`) belongs to; Phantasma's own mail table.
