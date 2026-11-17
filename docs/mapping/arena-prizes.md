# Master of Arena — arena prizes (money and parts)

Lane: **arena prizes** (rogue chat). Disc: USA disc 1 (SLUS-01030). Read statically from FDAT 206
(menu module, loads at `0x80050DD8`) and `GG/MS/MENU_TIM.T`, 2026-11-17. Addresses are measured
from the code unless marked `?`.

## 1. The reward record

Every arena has one whole-arena **rewards entry** in `MENU_TIM.T` (entry `140 + 4·start + 4·arena
+ 4·count + 2`), 8 bytes per opponent, sealed like the other arena entries (sum of words +
`0x12345678` in the last 4 bytes). The arena screen keeps a pointer to it at **state + 0xB8**
(= state + 0x80 + 0x38).

| Offset | Size | Meaning |
|---|---|---|
| +0 | u32 | prize money (credits) |
| +4 | u16 | **prize part id** (0–22), `0xFFFF` = none |
| +6 | u16 | not read (`0xFFFF`, or 0 where +4 is set) |

## 2. Money (`0x800BC420`, the payout state)

Opponent = byte `0x8003EAD5`. Credits `0x8003EA74` += record[opponent].+0, clamped to
99,999,999 (`0x05F5E0FF`) and −9,999,999. Money 0 skips the "C" prize window; otherwise it is
shown (`0x800661F0`) and the next state is the part check below.

## 3. Part prize (`0x800BC514` → `0x800BC690`)

**Which prize** (`0x800BC514`), into state + 0xEC (half):
- Arena **5 Champion**: only once all 15 flags (`0x8004B630 … +14`, bit 0) are set, then
  **record 0's** +4; otherwise none.
- Arena **6 Guest**: the same with 30 flags at `0x8004B640`.
- Every other arena: **record[opponent].+4**.

**Picture**: `0x800586A0(4, 844 + prize)` loads `MENU_TIM.T` entry **844 + id** (one 12 KB image per
prize), then the part window `0x80063EF4` and a sound.

**Grant** (`0x800BC690`, once the picture is in): owned index = byte table **`0x800D5C9C`[id]**
(23 bytes, menu module); owned count `0x80036FEC`[index] += 1, **+1 more for back weapons**
(index 157–208, a pair). The shop stock is not touched. Then the message box `0x800647B8`
(text `0x800D59E4`).

## 4. The 23 prizes (table `0x800D5C9C`)

| Id | Index | Part | | Id | Index | Part |
|---|---|---|---|---|---|---|
| 0 | 247 | LS-1000W (blade) | | 12 | 62 | LN-S3 (legs) |
| 1 | 241 | WG-RF/E | | 13 | 73 | LB-H230 (legs) |
| 2 | 13 | HD-H10 | | 14 | 77 | LF-TR-0 (legs) |
| 3 | 14 | HD-4004 | | 15 | 85 | LC-HTP-H5 (legs) |
| 4 | 37 | AN-891-S (arms) | | 16 | 180 | WR-RS7 (back) |
| 5 | 150 | B-PT000 (booster) | | 17 | 181 | WRR-10 (back) |
| 6 | 239 | WG-XW11 | | 18 | 11 | HD-12-RADAR |
| 7 | 238 | WG-HG770 | | 19 | 50 | AW-R/4 (weapon arms) |
| 8 | 242 | WG-MG500/E | | 20 | 240 | WG-FGI-00 |
| 9 | 173 | WM-MVG812 (back) | | 21 | 214 | P77-ST (back) |
| 10 | 174 | WM-X15-EX (back) | | 22 | 136 | index 124–142 group `?` (not in the ten part tables) |
| 11 | 213 | WX-C/4 (back) | | | | |

## 5. Who gives them (disc)

| Arena | Opponent (entry) → prize id |
|---|---|
| 0 main / sub | 2 → 4, 4 → 5, 8 → 10, 11 → 11 (ranked); 24 → 3, 29 → 6, 30 → 9, 31 → 7, 32 → 8, 38 → 2 (sub) |
| 1 Humanoid / 2 Four Legs / 3 Reverse / 4 Caterpillar | entry 0 (rank 1) → 12 / 14 / 13 / 15 |
| 5 Champion | record 0 → 22 (all 15 beaten) |
| 6 Guest | record 0 → 17 (all 30 beaten) |
| 7 FromSoft | entry 0 → 16 |
| 8 Master | 0 → 21, 2 → 18, 7 → 20, 16 → 19 |

## 6. What this means for mods

- Only these 23 parts can be a prize as the game stands, because the id goes through a 23-byte table
  and a picture entry `844 + id`. To give **any** part: hook the `lbu a3,0(v1)` at **`0x800BC6DC`**
  (a3 = owned index; take it from a mod table for ids ≥ 23) and the picture load at **`0x800BC610`**
  (`jal 0x800586A0`, a1 = 844 + id: point it at an existing prize picture or skip the picture).
- The arena run (`moa_arena_run_boot.c`) currently writes `0xFF` over +4…+7 for every arena 0
  opponent, so a run never gives parts. Phantasma's arena (`moa_phantasma.c`) writes money only.
- `?` Whether a refight pays the part again: the payout runs after the result routine; not checked.
