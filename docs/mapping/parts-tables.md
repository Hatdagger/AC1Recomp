# Master of Arena — Parts Tables Mapping

Goal: know every byte of the game's part data, from the shop tables on disc to the stat block
the battle engine reads. Feeds the launcher garage, Raven cards, the Arena Run shop and
Armored-CoreR's importer. Runs in parallel with `battle-system.md`; the two meet at the
**compiled stat block** (bottom of this file).

Disc: USA disc 1 (SLUS-01030) **v1.0**. Everything here is static data in the **menu module**
(FDAT entry 206, loaded at `0x80050DD8`), except the shot-speed tables (VS engine, FDAT 209).
File offset in entry 206 = address − `0x80050DD8`.

Status: ~99%. Every table is located, and every byte has been checked against the real disc
(2026-11-17, USA disc 1, MD5 `4d5774c4…`). Every field is now named from the code that reads
it; one byte (weapon detail +0x04) has no reader in fights or menus (see "Still unknown").

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
| Head | +0x32 | u8 | Arena only: picks a HUD item (stat +0x2A), likely display style | arena code |
| Head | +0x33 | u8 | Never read in fights (stat +0x26) | VS, arena |
| Head | +0x34, +0x35 | u8 | When 1, clear stat +0x34 bit 0 / bit 1: an ignore mask for the collision sweep, so the AC's sweeps also see objects flagged with that bit | VS code |
| Head | +0x36 | u16 | Has a radar (1) / none (0) | disc: range is 0 whenever this is 0 |
| Head | +0x38 | u16 | Radar range (PERFORMANCE shows ÷10) | perf maths |
| Head | +0x3A, +0x3B | u8 | Radar flags / radar display type, the same bytes as back radar +0x30 / +0x31 (compile copies them to stat +0x25 / +0x27). **0 on every head**, so head radars never show missiles and always use display 0 | compile, disc |
| Arms | +0x32 | u16 | Padding (always 0) | disc |
| Back (radars) | +0x2C / +0x2E | u16 | Is a radar (1) / radar range | disc: set only on RXA/RZ units |
| Back (radars) | +0x30 | u8 | **Radar flags** (stat +0x25). Bit 0 = **missiles shown on the radar** (VS `0x8005713C` → `0x8005663C`, which scans the object list for missiles; arena `0x80057AB4`). The same bit SP-MAW sets. Set on RXA-77, RZT-333, RZ-BBP, RZ-Fw2 | VS, arena code |
| Back (radars) | +0x31 | u8 | **Radar display type** (stat +0x27): low 4 bits pick the radar screen graphic, a 12-byte sprite entry at `0x8008FCDC` + type × 12 (VS `0x8005707C`; arena `0x800579F0`). 0 on most, 1 on RZ-A0 / RZ-A1 / RZ-BBP, 2 on RZT-333 / RZ-Fw2; 3 = Human PLUS built-in radar. Bit 0x10 would limit blips to a 90° cone ahead (`0x8005649C`), but no part sets it | VS, arena code |
| Back | +0x32 | u16 | Padding (always 0) | disc |
| Legs | +0x3C | u16 | **Turning with a back weapon selected** (×0.8 of +0x38 on two-legged, ×1.0 on four legs / caterpillars). The compile routine turns it into back-weapon slots +7 and +8 (`0x800A95D4`, `0x800A964C`), same formulas as stat +0x0C / +0x0D; see `stat-block.md` | compile |
| Legs | +0x3E | u16 | 51 (two-legged) / 64 (four legs, caterpillars). **Never read**: back slot +8 reads +0x3C a second time where the matching stat +0x0D reads +0x3A, so +0x3E was probably meant for it (a developer slip) | compile |
| Legs | +0x42 | u16 | Can fit a booster (1); 0 on every caterpillar | disc |
| Legs | +0x46 | u16 | Padding (always 0) | disc |
| Core | +0x32 | u16 | Arm load limit (arms + hand + blade; weapon arms count alone) | VS START check |
| Core | +0x34 | u8 | **Missile interceptor reaction**: chance per frame ÷ 128 (XCA-00 48 = 37.5%, XXL-D0 32 = 25%) | VS code |
| Core | +0x35 | u8 | **Missile interceptor angle**: ±value × 4 (4,096 = 360°): XCL-01 64 = ±22.5°, XCA-00 48 = ±16.9°, XXL-D0 20 = ±7° | VS code |
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
| Blade | +0x2E | u16 | **Hit flags** of the blade's hit record (stat +0x66): 39 = 0x27, of which 0x20 = energy hit (picks the energy defence factor in the damage maths `0x80079500`), same flag byte as weapon detail +0x12. Bits 0x07 have no reader found | VS code |
| Blade | +0x30 | u16 | **Attack** (LS-2001 738 … MOONLIGHT 2,801) | disc, perf maths |
| Blade | +0x32 | u16 | **Impact** (stat +0x6A), 11–42. The hit record's +4, read by the AC hit callback `0x80082430`: 0 = no knockback; otherwise knockback level = impact × 256 ÷ (stability × 25 ÷ 4), capped 0–5 (`0x80079AA4`). Same as a gun's impact (slot +40) | VS code |
| FCS | +0x2C | u8 | Max targets locked at once | weapons doc |
| FCS | +0x2D | u8 | **Lock-on area label** in the garage, a pointer table at `0x800D29BC` (`0x80085ED0`): 0 STANDARD, 1 SIDEWAY, 2 LENGTHWAY, 3 WIDE & SHALLOW, 4 NARROW & DEEP. Text only: the compile routine doesn't copy it, so fights never see it | menu code |
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
| +0x02 | u8 | Kind-dependent, lands in slot +9. **Guns**: firing pattern, bits 0–1 = shots per trigger (0 → 1, 1 → 2, 2 → 4, 3 → one at a time, alternating sides when bits 4–5 are set), bit 2 = always take the fixed-direction path in the fire routine, ignoring the lock (set only on the dispensers WM-X5-AA / WM-X10), bit 3 = weapon-arm aiming (set on every weapon arm). **Cannons**: base recoil, scaled by stability and weight. **Missiles**: ignored (slot +9 comes from the lock time). The whole byte also sets player block +0x214 for the selected weapon |
| +0x03 | u8 | **Firing animation ID** (slot +10): written to AC +0x84 when the weapon fires (guns `0x80080FE4`, missiles `0x800818D0`, cannons `0x80081BC4`); the cannon code compares it with the playing animation's ID. Right back slot gets +1. 0 hand guns, 1/2 missiles, 3/4 cannons, 5/6 rockets, 7 dual weapon arms; 255 = none |
| +0x04 | u8 | Copied to slot +11 (right back slot +1 unless 255); **no reader found** in VS, arena, replay, menus or the main executable. 2/3 on radars and two-slot units, 1/2 on WC-GN230 and WC-IR24, 255 elsewhere; looks like an unused second animation ID |
| +0x05 | u8 | **Aim flags** (slot +12): bit 2 = fire straight ahead, no aiming and no lock box drawn (rockets; fire `0x80080F8C`, HUD `0x80066BEC`). Bits 0–1 have no reader. 1 most, 6 rockets, 3 energy cannons, 0 dispensers / radars. SP-M/AUTO adds bit 0x80 on missiles |
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

Flag bits seen besides 0x10/0x20, read by the damage routine (VS `0x800795DC`): 0x40 on WR-RS7
(special rocket) copies the hit's attack into the target's AC +0x3A (the stagger the battle chat
found halved at `0x800796A4`); 0x80 on WG-FGI-00 (FLAME SHELL) sets target AC +0x3E = damage ÷ 16
and +0x3C = 100, likely a burn over time `?`. Blades use the same byte (blade +0x2E). Radars
and P77-ST have an all-zero record apart from +0x03/+0x04.

Shot speeds (VS engine): guns by model ID range — `0x80091198` (<0x14, 28 B, speed +8),
`0x80092980` (0x14–0x27, 40 B), `0x80092C20` (0x28–0x3B, 36 B), `0x80092824` (0x3C–0x59, 24 B),
`0x800928E4` (0x5A+, 22 B), speed at +0 in the last four. Missiles `0x80093114` (40 B: +0xC
homing push, +0xE top speed). Cannons `0x80091310` (6 B, read from code, not traced).

## Still unknown

Only one byte is left without a reader (checked 2026-11-17: VS, arena, VS replay, menus and the
main executable):

| Table | Bytes | What the disc shows | Notes |
|---|---|---|---|
| Weapon detail | +0x04 | 2 radars and two-slot units, 1 WC-GN230 / WC-IR24, 255 elsewhere | Compiled into slot +11 with the same left/right +1 as the firing animation ID (+0x03), so probably a second animation ID that the fight code never uses |

Small leftovers inside named fields: blade +0x2E bits 0x07, weapon detail +0x05 bits 0–1, and
radar type bit 0x10 have no reader either. Everything is kept in `raw` by acr-import.

## Missile interceptor (core +0x34 / +0x35)

VS `0x800829E4` (arena `0x80084694`), every frame for each AC:

1. If `(rand & 127) <` reaction (stat +0x0F), or a burst is running (AC +0x118, odd frames):
   look for a missile.
2. From the interceptor joints (layout bytes 0–1, stat +0x56/+0x57), scan the 256 objects at
   `0x801D6F6C` (228 bytes each) for an enemy missile (handler `0x8008907C`, alive) within
   5,000 on every axis and 5,000 straight-line, and inside ±angle × 4 (stat +0x10) of the AC's
   facing, sideways and up/down. Take the nearest.
3. Found: start a 15-frame burst (AC +0x118 = 15). Each shot has a **1 in 32** chance
   (`rand < 1024`) to be aimed at the missile and **destroy it**; otherwise it fires off with a
   random spread of up to 31 units. Shots spawn through `0x8008A7C0`.

So better cores react more often (+0x34) and cover a wider cone (+0x35); range and hit chance
are fixed. The user suspected this from the garage stats (2026-11-17).

## Room for more optional parts

Checked 2026-11-17 (menu module, FDAT 206), with custom options in mind:

- **The menus are built for 19 option IDs, not 13.** The option list loops (e.g. `0x80091A5C`,
  also around `0x80091DF0`, `0x8009241C`) run IDs 0–18, reading the record at
  `0x800D1AA0 + ID × 44` and shifting the owned/fitted mask one bit per ID. Inventory also has
  19 option bytes (13 + 6 spare, see below).
- **The table itself has only 13 records.** `0x800D1AA0 + 13 × 44 = 0x800D1DE4`, which is where
  the weapon-arms detail table starts, so IDs 13–18 would read weapon data as a name. New
  records need a new home (a recomp hook can point the reads elsewhere).
- **The fitted mask `0x8003EA70` is 16 bits** (only `lh`/`lhu`/`sh` at +0, 27 stores and 55
  loads in the menus; the engines never read it, they get the compiled block). Bits 0–12 are
  used, so **3 free bits (IDs 13–15)** without changing the game. IDs 16–18 need the mask widened.
- Option effects are coded in the compile routine (`stat-block.md`, "Options in the slot code"),
  so a new option needs its own code, which a recomp plugin can run after `0x800A7BB4`.

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
