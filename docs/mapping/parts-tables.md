# Master of Arena — Parts Tables Mapping

Goal: know every byte of the game's part data, from the shop tables on disc to the stat block
the battle engine reads. Feeds the launcher garage, Raven cards, the Arena Run shop and
Armored-CoreR's importer. Runs in parallel with `battle-system.md`; the two meet at the
**compiled stat block** (bottom of this file).

Disc: USA disc 1 (SLUS-01030) **v1.0**. Everything here is static data in the **menu module**
(FDAT entry 206, loaded at `0x80050DD8`), except the shot-speed tables (VS engine, FDAT 209).
File offset in entry 206 = address − `0x80050DD8`.

Status: ~90%. Every table is located, and every byte has been checked against the real disc
(2026-11-17, USA disc 1, MD5 `4d5774c4…`). Most fields are named and used by working code; a
few small fields are still unnamed (see "Still unknown").

Re-run the check yourself: `python3 tools/moa_parts_dump.py "Disc 1.bin" [legs back …]`, or
`… weapons` for the weapon detail records.

## Where the knowledge lives in code

| File | What it does with the tables |
|---|---|
| `games/master-of-arena/moa_parts.c` | Launcher reads the tables from the player's disc; build checks; PERFORMANCE screen maths |
| `games/master-of-arena/acr/acr_parts_moa.c` | Exports every part to a readable `.toml`, unknown bytes kept as `raw` |
| Armored-CoreR `docs/research/gen1-movement.md`, `gen1-weapons.md`, `gen1-fitting-rules.md` | How each field was proven (traces, live compiled blocks, in-game tests) |

## Part tables (menu module)

Every record: name (18 bytes, `+0x00`), type text (18 bytes, `+0x12`, e.g. "HUMANOID LEGS",
"SNIPER RIFLE"), then little-endian stats. Records named `DUMMY` are empty slots. The record
index is the part's ID in cards, the garage struct and arena opponent records.

| Category | Address | Stride | Count |
|---|---|---|---|
| Head | `0x800CE690` | 60 | 18 |
| Core | `0x800CEAC8` | 56 | 11 |
| Legs | `0x800CED30` | 72 | 40 |
| Arms | `0x800CF870` | 52 | 24 (12+ = weapon arms) |
| Hand weapon | `0x800CFD50` | 44 | 26 (0 = NO WEAPON) |
| Back weapon | `0x800D01C8` | 52 | 60 |
| Blade | `0x800D0DF8` | 52 | 6 |
| Booster | `0x800D0F30` | 48 | 14 |
| FCS | `0x800D11D0` | 96 | 16 |
| Generator | `0x800D17D0` | 48 | 15 |
| Optional parts | `0x800D1AA0` | 44 | 13 (12 sold + SP-ABS_Re, never sold) |

### Fields shared by every category

| Offset | Size | Meaning |
|---|---|---|
| +0x24 | u32 | Price |
| +0x28 | u16 | Weight |
| +0x2A | u16 | Energy drain (legs: while moving; generator: **output**) |

### Per category

| Category | Offset | Size | Meaning | Proven by |
|---|---|---|---|---|
| Head, core, arms, legs | +0x2C / +0x2E / +0x30 | u16 | AP / shell defence / energy defence | garage, damage formula |
| Head | +0x32–0x35 | 4×u8 | Small values 0–3; same pattern as radars' +0x30 (see unknowns) | disc |
| Head | +0x36 | u16 | Has a radar (1) / none (0) | disc: range is 0 whenever this is 0 |
| Head | +0x38 | u16 | Radar range (PERFORMANCE shows ÷10) | perf maths |
| Head | +0x3A | u16 | Padding (always 0) | disc |
| Arms | +0x32 | u16 | Padding (always 0) | disc |
| Back (radars) | +0x2C / +0x2E | u16 | Is a radar (1) / radar range | disc: set only on RXA/RZ units |
| Back (radars) | +0x30 | 2×u8 | Small values 0–2, same pattern as heads' +0x32 | disc |
| Back | +0x32 | u16 | Padding (always 0) | disc |
| Legs | +0x3C | u16 | Turn acceleration, 2nd set: ×0.8 on two-legged, ×1.0 on four legs / caterpillars | disc |
| Legs | +0x3E | u16 | Turn speed, 2nd set: 51 (two-legged) / 64 (four legs, caterpillars) | disc |
| Legs | +0x42 | u16 | Can fit a booster (1); 0 on every caterpillar | disc |
| Legs | +0x46 | u16 | Padding (always 0) | disc |
| Core | +0x32 | u16 | Arm load limit (arms + hand + blade; weapon arms count alone) | VS START check |
| Core | +0x36 | u16 | Option slots | VS START check |
| Legs | +0x32 | u16 | Carry limit | VS START check |
| Legs | +0x34 | u16 | Energy drain standing | compiled block |
| Legs | +0x36 | u16 | Moving speed (PERFORMANCE ×10) | perf maths |
| Legs | +0x38 | u16 | Turn acceleration (PERFORMANCE "turning" ×10) | compile, perf |
| Legs | +0x3A | u8 | Top turn speed | compile |
| Legs | +0x40 | u16 | Stability | compile, perf |
| Legs | +0x44 | u16 | Rise drain when no booster (caterpillars) | compile |
| Booster | +0x2C | u16 | Power (caterpillars use a flat 9,800) | compile |
| Booster | +0x2E | u16 | Boost drain (÷10 per step) | compile |
| Generator | +0x2C | u16 | Energy capacity | compile |
| Generator | +0x2E | u16 | Red zone | compile |
| Blade | +0x2C | u16 | Most likely **charge drain** (energy per swing), copied to compiled +28 | disc, see open questions |
| Blade | +0x2E | u16 | 39 on every blade | disc |
| Blade | +0x30 | u16 | **Attack** (LS-2001 738 … MOONLIGHT 2,801) | disc, perf maths |
| Blade | +0x32 | u16 | 11–42, rises with attack (about attack ÷ 66) | disc |
| FCS | +0x2C | u8 | Max targets locked at once | weapons doc |
| FCS | +0x2D | u8 | Small value 0–4 (unknown) | disc |
| FCS | +0x5E | u16 | Padding (always 0) | disc |
| FCS | +0x2E + 12k | 6×u16 | Lock class k (0–3): range, box w, box h, inner w, inner h, lock time | in-game lock tests |
| Option | +0x24 / +0x28 | u32 / u16 | Price / slot cost | VS START check |

Hand weapons and boosters/generators use every byte of their records; their combat stats live
in the weapon detail tables.

## Weapon detail tables (24 bytes each)

Hand `0x800D1F64` (by hand ID), back `0x800D21D4` (by back ID), weapon arms `0x800D1DE4`
(by arms ID − 12). Type names (16 bytes each) at `0x800D5488`, indexed by +7.

| Offset | Size | Meaning |
|---|---|---|
| +0x00 | u8 | Kind: 0 gun, 1 missile, 2 cannon |
| +0x01 | u8 | Model ID (picks shot-speed / missile table entry) |
| +0x06 | u8 | Lock class (FCS group; 4 = no lock) |
| +0x07 | u8 | Type name index |
| +0x08 | u16 | Ammo |
| +0x0A | u16 | Energy per shot |
| +0x0C | u16 | Ammo price |
| +0x0E | u16 | Range (capped by the FCS class range) |
| +0x10 | u8 | Missiles locked at once (capped by FCS) |
| +0x11 | u8 | Fire interval (garage RELOAD TIME; ≈118 ms per unit) |
| +0x12 | u8 | Flags: 0x10 solid, 0x20 energy |
| +0x14 | u16 | Attack |
| +0x16 | u8 | Impact |
| +0x13, +0x17 | u8 | Padding (always 0) |

Flag bits seen besides 0x10/0x20: 0x40 on WR-RS7 (special rocket), 0x80 on WG-FGI-00. Radars
and P77-ST have an all-zero record apart from +0x03/+0x04.

Shot speeds (VS engine): guns by model ID range — `0x80091198` (<0x14, 28 B, speed +8),
`0x80092980` (0x14–0x27, 40 B), `0x80092C20` (0x28–0x3B, 36 B), `0x80092824` (0x3C–0x59, 24 B),
`0x800928E4` (0x5A+, 22 B), speed at +0 in the last four. Missiles `0x80093114` (40 B: +0xC
homing push, +0xE top speed). Cannons `0x80091310` (6 B, read from code, not traced).

## Still unknown

Bytes no code reads yet. They're kept in `raw` by acr-import, so nothing is lost.

Checked on the disc; these have values, but no code use found yet. Each needs one poke in VS
or one read of the code that uses it.

| Table | Bytes | What the disc shows | Best guess |
|---|---|---|---|
| Head | +0x32–0x35 | four bytes, each 0–3 | Head system features (AC1 heads list map, noise canceler, bio sensor…) |
| Back radars | +0x30–0x31 | two bytes, 0–2, same style as the head bytes | Radar features (missile/AC display?) |
| Core | +0x34–0x35 | two bytes: XCA-00 48/48, XCL-01 48/64, XCH-01 48/32, XXA-S0 50/50, XXL-D0 32/20 | Anti-missile / interceptor figures? |
| Legs | +0x3C, +0x3E | second turn set (two-legged lose 20%) | Turning in the air or while boosting; four legs hover, caterpillars don't jump |
| Legs | +0x3A | 64 on every leg | Code reads it as top turn speed; the real limit may come from +0x3E |
| Blade | +0x2E, +0x32 | 39 always; 11–42 with attack | Reach or lunge figures |
| FCS | +0x2D | 0–4 per FCS | Lock-on display or targeting style |
| Weapon detail | +0x02 | rockets 64, missiles 4, cannons 0–64, weapon arms 9–10 | Recoil (SP-SAP "decreases cannon recoil" acts on weapon records) |
| Weapon detail | +0x03 | hand 0, missile 1, cannon 3, rocket 5, radar/two-slot 255, weapon arms 0/7 | Firing group / muzzle point |
| Weapon detail | +0x04 | 255 most; 2 radars and two-slot units; 1 grenade launcher, WC-IR24 | Slot use (two-slot = 2)? |
| Weapon detail | +0x05 | 1 most; rockets 6; energy cannons 3; dispensers, radars 0 | Aim mode (rockets fire straight, no lock) |

## Open questions

- **Blade +0x2C vs +0x30 (mostly settled).** The disc shows +0x30 running from 738 (LS-2001)
  up to 2,801 (LS-99-MOONLIGHT, the strongest blade), and +0x2C from 2,050 down to 810. So
  +0x30 is attack and +0x2C is most likely charge drain. That matches acr-import and the
  launcher. Armored-CoreR's `gen1-weapons.md` ("attack 2,050") and `gen1-movement.md`
  (compiled +28 "blade power") have these swapped. That may also explain its blade damage
  mismatch (683 vs 492). Confirm with the garage's ATTACK figure for the LS-2001.
- **Blade damage:** `battle-system.md` now treats split-screen VS as its own engine (FDAT 209),
  where Armored-CoreR's damage maths came from. The blade's damage there should be rechecked
  with +0x30 as attack.
- Does Project Phantasma's disc use the same layout? (Needed if its parts ever get imported.)

## Settled against the GameShark map (`ram-map-gameshark.md`)

- **Inventory = one byte per table slot, in table order.** Heads 18 slots from
  `0x80036FEC`, cores 11, arms 24, legs 40, generators 15, FCS 16, options (13 + 6 spare),
  boosters 14, back 60, hand 26, blades 6. Each range ends exactly where the next starts. The
  byte index inside a range is the part ID. The "unexplained gaps / possible cut parts" in the
  RAM map are the tables' `DUMMY` slots: no hidden parts there. GameShark's "right arm"
  range starts with three back units (P77-ST and two empty back slots); the hand weapons start at `0x800370C5`.
- **Equipped loadout `0x80036FB0`**: core, booster, head, arms, blade, hand, back L, back R,
  generator, FCS, *unused*, legs. The GameShark "unknown" slot is the unused byte. Note that the
  order differs from the 13-byte card record (`moa_record_cat`).
- **Weapon slots** in the GameShark codes are inside the compiled stat block (below).

## Bridge to the battle system: the compiled stat block

At VS START the menu's compile routine (`0x800A7BB4`, called from `0x800B3058`) turns the
build into a **324-byte block at player block +0x250** (`0x800453E8` for player 1, the
2nd player +928). The battle engine reads stats from here, not from the part tables.

That block runs from player block +0x250 to +0x393. So the GameShark "live weapon slots" in
`battle-system.md` (+0x2E6 on) sit **inside** it (compiled +0x96 on). The AC object pointer
at +0x398 comes right after. Mapped so far:

| Compiled offset | Meaning | From |
|---|---|---|
| +6, +8 | Thrust (booster power ÷ carried weight) | booster, legs |
| +7, +9 | Thrust × 0.8 | |
| +10, +11 | Thrust × 1.2, or a fixed value for some legs | |
| +12 | Turn acceleration | legs +0x38 |
| +13 | Turn speed | legs +0x3A |
| +14 | Friction | carried weight |
| +18 | Energy capacity | generator +0x2C |
| +20 | Rise drain per step | booster +0x2E (or legs +0x44) |
| +22 | Refill standing | output − standing drain |
| +24 | Red zone | generator +0x2E |
| +26 | Refill moving | output − moving drain |
| +28 | Blade charge drain (was called "blade power") | blade +0x2C |
| +0x22 / +0x23 | Shell / energy damage factor | body defences |
| +36 | Stability | legs +0x40, balancers |
| +0x96 on | Weapon slots (ammo, range, lock…) | weapon detail tables |

Formulas: Armored-CoreR `docs/research/gen1-movement.md` and `gen1-weapons.md`. The C port
(`src/gen1/acr_gen1_compile.c`) matches the game on 600 random builds.

The rest of the 324 bytes belong to the battle lane (`battle-system.md`); see `README.md`
for who maps what.
