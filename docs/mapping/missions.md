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
| +0x24 … | `0x8007ED44`: `*(module + 0x20 + 4 × obj+0x0E)` | **handler per object type** (types 1, 2, …), list ends with 0 |

Mission 0 has 3 object-type handlers, mission 1 (FDAT 5) 6, mission 19 (FDAT 95) 1.
These are likely the mission's own objects (MTs, tanks, targets…): next thing to read.

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

## Open
- What `0x8003EA29` (from T+1160) does in the engine; what T+8 counts.
- The object-type handlers (+0x24 …) and the object record (+0x0E = type): the mission's enemies.
- Hooks +0x0C, +0x18, +0x1C; the event numbers passed to +0x08.
- Mission number 40: which menu screen uses it.
- Layout of the per-mission code module: entry points / table at its start.
- Map header fields (80, 19, 4 × 19) and the blocks after it.
- Objectives, win/lose checks, timer, results and rewards; enemy list; script reader
  `0x80091FFC` (stepped at `0x80092E4C`; writes AC +0x6D/+0x7C/+0x7E/+0x84).
- MIS.T per-slot layout and the mail table.
