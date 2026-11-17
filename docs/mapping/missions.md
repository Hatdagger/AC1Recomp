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

## Open
- What `0x8003EA29` (from T+1160) does in the engine; what T+8 counts.
- Layout of the per-mission code module: entry points / table at its start.
- Map header fields (80, 19, 4 × 19) and the blocks after it.
- Objectives, win/lose checks, timer, results and rewards; enemy list; script reader
  `0x80091FFC` (stepped at `0x80092E4C`; writes AC +0x6D/+0x7C/+0x7E/+0x84).
- MIS.T per-slot layout and the mail table.
