# Master of Arena (USA, SLUS-01030) — RAM map from GameShark / CodeBreaker codes

Source: public NTSC-U CodeBreaker lists (almarsguides.com, cheatcc.com). Japanese-version
codes use different addresses and are ignored except as hints.
Status column: **known** = already in AC1Recomp (master f40066c, re-checked at 602a5b6), **new** = not in the repo yet,
**confirms** = matches something the repo only knew by size/position.

Code types: `80` = 16-bit write, `30` = 8-bit write, `D0` = "only if 16-bit value equals"
(used here to wait for an engine's code to be loaded), `50` = repeat the next write.

## Garage / save state (boot-exe RAM, survives across modes)

| Address | Size | Meaning | Status |
|---|---|---|---|
| 0x80036FEC–0x800370EB | 1 byte × 256 | Owned-part counts, one byte per part in ID order (heads from 36FEC, FCS ~37058, options ~37068, boosters ~3707B, back weapons ~37089 (value 2 = a pair?), hand weapons ~370C6, blades ~370DF) | known (SAVE_OWNED) — per-category offsets **new** |
| 0x8003EA74 | u32 | Credits | known |
| 0x8003EA94 | u16/u32 | Option parts owned (bit mask; JP "all options" = FFFF) | known as save block — meaning **new** |
| 0x8003EAC8 | u8 | Human PLUS level (0x0F written by "max level") | **new** |
| 0x8003EAC9 | u8 | Champion Ex Arena enabled | **new** |
| 0x8003EACA | u8 | Overweight builds allowed | known |
| 0x8003EACB | u8 | Guest Ex Arena enabled | **new** |

## Ex Arena progress (save block 0x8004B5A0, 224 bytes — known as a block, meaning **new**)

One byte per opponent "finished", one byte per arena "complete":

| Arena | Opponent flags | Arena-complete flag |
|---|---|---|
| Humanoid legs | 0x8004B608–B610 (9) | 0x8004B67E |
| Four legs | 0x8004B612–B61A (9) | 0x8004B67F |
| Reverse joint | 0x8004B61C–B624 (9) | 0x8004B680 |
| Caterpillar | 0x8004B626–B62E (9) | 0x8004B681 |
| Champions | 0x8004B630–B63F (16) | — |
| Guests | 0x8004B640–B65D (30) | — |
| FromSoft | 0x8004B65F–B667 (9) | 0x8004B684 |
| Masters | 0x8004B668–B67B (20) | 0x8004B685 |

Implication: the Phantasma tenth arena and any card arena need their own block (the
repo already keeps PP mission progress in a side file — same idea).

## In-fight player block (0x80045198, 928 bytes per player — size **known**, fields **new**)

Offsets from the start of player 1's block. Player 2 = +928.

| Offset | Meaning (from code) |
|---|---|
| +0x016 | Booster / energy reserve ("infinite booster" writes 60000) |
| +0x2E6 / +0x2E8 | Hand weapon ammo (current / max) |
| +0x2EC / +0x2EE | Hand weapon range |
| +0x2F0 | Hand weapon lock-on speed |
| +0x2F4 / +0x2F6 | Hand weapon FCS lock box (width / height) |
| +0x2FA | Missile lock count / autofire flag |
| +0x324 … +0x338 | Back weapon slot A: same fields, +0x3E stride |
| +0x362 … +0x376 | Back weapon slot B: same fields, +0x3E stride |

These are the part-table stats copied in at match start. This is the missing link
"part table → combat": find the code that fills these from the menu tables and the unnamed
part-record bytes get names.

## AC object (376 bytes — size **known**, these **confirm** it)

| Address | Meaning | Mode |
|---|---|---|
| 0x801D8D70 | Player AP (u16) | Arena engine (check: 0x8007BBAE == 0x1040) |
| 0x801D8EE8 | Opponent AP | Arena engine |
| 0x801E73B0 | Player AP | Mission engine (check: 0x8007B7CA == 0x1040) |

0x801D8EE8 − 0x801D8D70 = 0x178 = 376: the two ACs sit back to back in an array, matching
the repo's 376-byte AC object. AP is at the same offset inside each object.

## Not found for USA
Mission timer (JP only: 0x800A0DCC), enemy AP in missions, heat, position/speed.
