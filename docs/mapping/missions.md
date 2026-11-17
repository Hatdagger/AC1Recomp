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

## Open
- Which routine sets the mission slot byte `0x800A0DB0` (menu → engine).
- Layout of the per-mission code module: entry points / table at its start.
- Map header fields (80, 19, 4 × 19) and the blocks after it.
- Objectives, win/lose checks, timer, results and rewards; enemy list; script reader
  `0x80091FFC` (stepped at `0x80092E4C`; writes AC +0x6D/+0x7C/+0x7E/+0x84).
- MIS.T per-slot layout and the mail table.
