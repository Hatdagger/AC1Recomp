# Master of Arena — Compiled Stat Block (324 bytes)

Owner: data chat (see `README.md`). What the battle engines read about each AC.

At VS START the menu module (FDAT 206) runs **`0x800A7BB4`** (`compile(player)`, called from
`0x800B3058`). It reads the garage loadout (`0x80036FB0`), the part tables and the option
mask (`0x8003EA70`), and fills a 324-byte block per player:

    block = 0x800453E8 + player × 928      (= player block 0x80045198 + 0x250)

So **compiled +X = player block +0x250+X**. The block ends at player block +0x393; the AC
object pointer (+0x398) comes right after.

How this was mapped: disassembled the whole routine (`0x800A7BB4`–`0x800AAA84`, ~2,900
instructions) from USA disc 1 v1.0 and followed every write into the block. Formulas checked
earlier against live blocks by Armored-CoreR (`gen1-movement.md`). Status: **every byte the
routine writes is identified**. What's left is naming a few copied values by watching them in a
fight, and the bytes the routine never writes (filled during the fight).

2026-11-17 (second pass): the bytes the compile routine doesn't write are now traced too. Most
come from the **options record** (`0x800B6A80`, see "Options and button masks"); the VS
engine's **match-start routine `0x8008342C`** adjusts a few values and moves some into the AC
object (see "Match start"). The arena engine has an identical copy at `0x800850DC` (FDAT 210).

Part records named below are the menu tables in `parts-tables.md`. "Garage" = loadout bytes:
0 core, 1 booster, 2 head, 3 arms, 4 blade, 5 hand, 6 back L, 7 back R, 8 generator,
9 FCS, 11 legs.

## Header and movement (+0x00 – +0x11)

| Off | Size | Meaning | Source |
|---|---|---|---|
| +0x00 | u16 | **Match time limit in seconds**: 120 or 300 or 0 (no limit), from options record +0x34 (`0x8004B59C`: 0 → 120, 1 → 300, else 0). VS setup (`0x80055C48`) takes the larger of the two players' values × 22 as the time left in steps at `0x80097EB4` (120 → 2,640, matches the battle chat's measured timer); the clock only runs while it's ≥ 2 | options |
| +0x02 | u8 | Flags: bit 0 cleared, bits 1 and 2 set, **bit 3 = legs can fit a booster** (legs +0x42). **Bit 0** is a fight-time flag (set `0x8007CC70`; cleared `0x8007CA88`, `0x80082920`, `0x8008FB68`). **Bit 5** comes from options record +0x35 (default 1; `0x800B6AE4`), meaning unknown. Bit 1 read at `0x80087760`, bit 4 at `0x80087EB8` | |
| +0x03 | u8 | **Human PLUS level** (`0x8003EAC8`) | |
| +0x04 | | no access found in the menus, VS or arena engines: padding | |
| +0x05 | u8 | **Stage number (very likely)**. Set by the menus in both players' blocks (same value, e.g. `0x800C097C` from a table indexed by `0x8003EA78`; `0x8007E0DC` writes 29). At VS load `0x80055930` copies player 1's or player 2's (picked by bit 0 of `0x8003EA94`) to the global `0x80097EA0` | menus |
| +0x06, +0x08 | u8 | Thrust: booster power (9,800 for caterpillars) × 4096 ÷ carried weight ÷ 224, 1–64 | known |
| +0x07, +0x09 | u8 | Thrust × 0.8, 1–50 | known |
| +0x0A, +0x0B | u8 | Thrust × 1.2, 1–64; fixed for legs 21 (26), 22 (23), 23 (19), 24 (26), 26 (24), 27 (6), 28 (7), 30 (13), 31 (11), 32 (13) | known |
| +0x0C | u8 | Turn acceleration (legs +0x38 ÷ weight) | known |
| +0x0D | u8 | Turn speed (legs +0x3A ÷ weight), 0–64 | known |
| +0x0E | u8 | Friction: weight × 8 ÷ 1000, 32–96 | known |
| +0x0F, +0x10 | u8 | **Core bytes +0x34, +0x35**, copied as is | **name by test** |
| +0x11 | | no access found anywhere: padding | |

Carried weight = every part except the generator, capped at legs weight + carry limit.

## Energy, defence, sensors (+0x12 – +0x2A)

| Off | Size | Meaning | Source |
|---|---|---|---|
| +0x12 | u16 | Energy capacity (generator +0x2C); **+12,000 with SP-CND-K** | |
| +0x14 | u16 | Rise drain per step: booster +0x2E ÷ 10 (no booster: legs +0x44 ÷ 10) | known |
| +0x16 | u16 | Refill standing: (output − standing drain) × 64 ÷ 1000, 1–384 | known |
| +0x18 | u16 | Red zone (generator +0x2E) | known |
| +0x1A | u16 | Refill moving, 1–192 | known |
| +0x1C | u16 | Blade +0x2C (charge drain; 0 without a blade) | known |
| +0x1E | u16 | **Radar range**: head +0x38, or a back radar's +0x2E if larger; 5,120 with Human PLUS and no radar | |
| +0x20 | u16 | **Total AP × 4** (head + core + legs + arms) | |
| +0x22 | u8 | Shell damage factor, 1–45 (SP-S/SCR: defence × 1.15) | known |
| +0x23 | u8 | Energy damage factor, 1–45 (SP-E/SCR) | known |
| +0x24 | u8 | Stability, 32–255 (legs +0x40, SP-ABS +800, SP-ABS_Re +1600) | known |
| +0x25 | u8 | **Radar flags**: head +0x3A or back radar +0x30; bit 0 set by SP-MAW (missile indicator); 1 with Human PLUS and no radar | |
| +0x26 | u8 | Head byte +0x33 | **name by test** |
| +0x27 | u8 | **Radar type**: head +0x3B or back radar +0x31; 3 with Human PLUS and no radar | |
| +0x28 | u8 | **Body frame**: 0 two-legged, 1 caterpillar, 2 four-legged (from the body table). Match start turns 0 into **3** when Human PLUS level ≥ 4; frames 0/3 then run init `0x80085688`, frames 1/2 run `0x800875C8` | |
| +0x29 | u8 | **Body combination** (0–71): core × arms group × legs group, see "Body layout" | |
| +0x2A | u8 | Head byte +0x32 | **name by test** |

**Human PLUS level 6 and up** (match start, `0x80083560`): rise drain +0x14 is halved, both
refill rates (+0x16, +0x1A) are doubled, then each refill is capped at the new rise drain.

Back radars (40–51) switch the head's radar off first (+0x1E, +0x25, +0x27 = 0), then take over.

## Part IDs and flags (+0x2B – +0x3D)

| Off | Size | Meaning |
|---|---|---|
| +0x2B | u8 | Core ID |
| +0x2C | u8 | Legs ID |
| +0x2D | u8 | Head ID |
| +0x2E | u8 | Arms ID |
| +0x2F | u8 | Booster ID |
| +0x30 | u8 | Hand weapon ID (0 = none) |
| +0x31 | u8 | Back L ID (rewritten when a P77-ST is fitted) |
| +0x32 | u8 | Back R ID (same) |
| +0x33 | u8 | Blade ID |
| +0x34 | u16 | Flags, starts at 3: bit 0 cleared if head byte +0x34 = 1; bit 1 cleared if head byte +0x35 = 1; **bit 3 = SP-JAM (missile jammer)**. Match start copies the flags minus bits 0–1 to AC object +0x38, then **clears bit 3 here** (the jammer lives in the AC object from then on) |
| +0x36 – +0x3C | 4×u16 | From the body table (650, 40, 100, 2 on every body except combination 1, core 0 + regular arms + caterpillar, which has 0 instead of 40). Match start (`0x8007DC24`) passes all four, +0x3A squared, layout bytes 4–5 (+0x5A, +0x5B) and the layout list (+0x6E) to `0x800876C4`, so likely **body collision sizes** (+0x3A = 100 looks like a radius) — **name by test** |

## Options and button masks (+0x3E – +0x55)

Not stats: the **button layout** from the options record at `0x8004B568` (48 bytes of
masks, then 6 option bytes). `0x800B6A80` copies it into player 1's block; `0x800B6430` swaps
the masks and the header word (+0x00–0x03) between the two players' blocks. `0x800B69F8`
resets the record from the defaults at `0x800D5860`.

Masks use the raw pad word at **player block +0x04** (held; +0x06 = previous frame): Up 0x10,
Right 0x20, Down 0x40, Left 0x80, L2 0x100, R2 0x200, L1 0x400, R1 0x800, △ 0x1000, ○ 0x2000,
✕ 0x4000, □ 0x8000. Uses are from the code where an address is given; the user confirmed the whole default
layout (2026-11-17).

| Off | Options record | Default | Use |
|---|---|---|---|
| +0x3E | +0x00 | Up | Forward (with back, L1, R1, ✕ = "moving" check `0x800822EC`) |
| +0x40 | +0x04 | Down | Back |
| +0x42 | +0x08 | Left | **Turn left** (adds to turn speed, `0x8007DC80`) |
| +0x44 | +0x0C | Right | **Turn right** (subtracts) |
| +0x46 | +0x28 | △ | **Next weapon** (new press, needs usable weapons; bumps player block +0x11, `0x80082F88`) |
| +0x48 | +0x24 | ✕ | Boost (read in most movement code) |
| +0x4A | +0x2C | □ | **Fire** (held, `0x80081054`) |
| +0x4C | +0x20 | ○ | **Blade** (new press, needs blade attack +0x68 ≠ 0, `0x80083230`) |
| +0x4E | +0x10 | L1 | Strafe left (read with forward/back by the leg code) |
| +0x50 | +0x18 | L2 | Look up/down; L2 + R2 together reset a timer at player block +0x21C (`0x8007D628`) |
| +0x52 | +0x14 | R1 | Strafe right |
| +0x54 | +0x1C | R2 | Look up/down (pair with L2) |

The other option bytes: +0x30 → `0x8003EA93` (default 1), +0x31 / +0x32 → sound volumes
(defaults 63 / 31), +0x33 (0), **+0x34 → stat +0x00** (time limit), **+0x35 bit 0 → stat
+0x02 bit 5** (default 1).

## Body layout (+0x56 – +0x87)

The body combination (+0x29) is the index of the first matching entry in the combo table
`0x800D4A64` (72 entries of 12 bytes: core ID, pointer to an arms list, pointer to a legs list,
lists end in 255). Cores 3 and 4 (XXA-S0, XXL-D0) are looked up as core 0. The search has no
end check, so every real part combination must match; it does (legs 15, 25, 29 are DUMMY
slots). Groups: arms {0,1,3,6,7,8}, {4,5}, {2}, weapon arms {12–21}; legs {0,3,6,10,11},
{2,5,8,9,13}, {1,4,7,12}, **reverse-joint {14,16–20}**, four legs {21–24,26}, caterpillars
{27,28,30–32}.

The index then picks a 16-byte entry in the body table `0x800D5008` (72 entries, ending where
the weapon name table starts): a layout pointer, the body frame (+0x28) and the four values
copied to +0x36–0x3C. Six layouts, a normal one and a weapon-arms one per frame:

| Layout | Frame | Combinations |
|---|---|---|
| `0x800D4DC4` | 0 two-legged | 0, 4, 5, 7–30, **48–56 (reverse-joint legs)** |
| `0x800D4F5A` | 0 two-legged | 31–39, 66–68 (weapon arms, incl. on reverse-joint legs) |
| `0x800D4DFE` | 1 caterpillar | 1–3, 57–62 |
| `0x800D4FCE` | 1 caterpillar | 69–71 (weapon arms) |
| `0x800D4E72` | 2 four-legged | 6, 40–47 |
| `0x800D4F94` | 2 four-legged | 63–65 (weapon arms) |

So **reverse-joint legs are plain two-legged bodies** in the compiled block: same frame, same
layout, same +0x36–0x3C as normal legs. All entries have 650, 40, 100, 2 except combination 1
(core 0 + regular arms + caterpillar), which has 0 instead of 40.

| Off | Size | Meaning |
|---|---|---|
| +0x56 – +0x65 | 16×u8 | Layout bytes 0–15 (look like model joint numbers) |
| +0x66 | u16 | Blade +0x2E (39 on every blade) |
| +0x68 | u16 | **Blade attack** (blade +0x30) |
| +0x6A | u16 | Blade +0x32 |
| +0x6C | u16 | Bit mask: two bits picked by layout bytes 16 and 17 |
| +0x6E – +0x85 | u8 list | Layout list from layout byte 34, ending in 255 (joints that take hits?) |
| +0x86 | u8 | **Stealth**: 1 when a P77-ST is fitted |
| +0x87 | u8 | **Number of usable weapons**: hand, weapon arms, back weapons other than radars, M118-TD and P77-ST |

## Weapon slots (+0x88, 62 bytes each, 3 slots)

Slot n at **+0x88 + 62 × n** (player block +0x2D8 + 62 × n): slot 0 is the weapon arms or
the hand weapon (weapon arms empty the hands), then the usable back weapons. The GameShark "live weapon slots" are these.

| Slot off | Size | Meaning | From |
|---|---|---|---|
| +0 | u8 | Kind (0 gun, 1 missile, 2 cannon) | detail +0 |
| +1 | u8 | Model ID (shot-speed table) | detail +1 |
| +2 – +6 | u8 | Muzzle joints for this slot | layout bytes 18–33 |
| +7, +8 | u8 | 64. For the **selected** weapon, `0x80082834` (match start and every weapon change) sets player block +0x210 = +7 × 5 ÷ 32 + 1 and +0x212 = +8 × 15 ÷ 32 + 1 | constant |
| +9 | u8 | By kind (see "Options in the slot code"): guns detail +2; missiles from the FCS lock time; cannons recoil from stability and weight, 1–64. Bits 0–1 = fire mode in the fire routine `0x80080DF4` (1 → 2, 2 → 4, 3 → checks bits 4–5); the whole byte also sets player block +0x214 = +9 × 75 ÷ 32 + 1 for the selected weapon | detail |
| +10, +11, +12 | u8 | Detail +3, +4, +5; +12 bit 0x80 = **SP-M/AUTO** on a missile | detail |
| +14 | u16 | Ammo (max) | detail +8 |
| +16 | u16 | Ammo (current, starts full; match start copies +14 into it again for every usable slot) | detail +8 |
| +18 | u16 | Solid weapons: ammo price (detail +0xC). Others: energy per shot (detail +0xA), × 3 ÷ 4 with **SP-DEtq** | detail +0xA / +0xC |
| +20 | u16 | Lock range: the smaller of the FCS class range and the weapon range | FCS, detail +0xE |
| +22 | u16 | Weapon range | detail +0xE |
| +24 – +30 | 4×u16 | Lock box: outer w, h, inner w, h (0 for no-lock weapons) | FCS |
| +32 | u16 | Lock time: FCS class lock time; with **SP-AXL** × 1.15 for solid weapons, × 0.85 for the rest, 1–255; 1 for no-lock weapons | FCS |
| +34 | u8 | Locks at once: the smaller of FCS max and the weapon's salvo | FCS, detail +0x10 |
| +35 | u8 | Fire interval (energy weapons with SP-EH get faster) | detail +0x11 |
| +36 | u8 | Damage flags (0x10 solid, 0x20 energy) | detail +0x12 |
| +37 | u8 | Detail +0x13, copied as is | detail |
| +38 | u16 | **Attack**; × 1.15 with **SP-E+** on energy weapons | detail +0x14 |
| +40 | u16 | Impact | detail +0x16 |
| +42 – +57 | 16 chars | **Weapon type name for the HUD**, right-aligned and space-padded, copied with unaligned stores from the 33-name table at `0x800D5488` (16 bytes each) by detail +7: 0 MACHINE GUN, 1 GATLING GUN, 2 CANNON, 3 DUAL MISSILE, 4 LASER CANNON, 5 PLASMA CANNON, 6 RIFLE, 7 HAND GUN, 8 SNIPER RIFLE, 9 FLAMETHROWER, 10 BAZOOKA, 11 PULSE RIFLE, 12 PLASMA RIFLE, 13 SMALL MISSILE, 14 MISSILE, 15 LARGE MISSILE, 16 MULTI MISSILE, 17 TRIPLE MISSILE, 18 BOMB DISPENCER, 19 SMALL ROCKET, 20 ROCKET, 21 LARGE ROCKET, 22 CHAIN GUN, 23 SLUG GUN, 24 LINEAR GUN, 25 GRENADE LAUNCHER, 26 PULSE CANNON, 27 LASER RIFLE, 28 SPECIAL MISSILE, 29 PLASMA DISPENSER, 30 SPECIAL ROCKET, 31 MINE DISPENSER, 32 FLAME SHELL | detail +7 |
| +58 – +61 | | not written (no access found): padding | |

Earlier passes missed +42–57 because they're written with `swl`/`swr` (unaligned
stores); scans for written bytes must include those.

## Options in the slot code (`0x800A8904`–`0x800AA75C`)

The same code runs for each slot (weapon arms, hand, back L, back R; the back copies share
one tail). Option bits are the option IDs in the mask `0x8003EA70`: 0 SP-MAW, 1 SP-JAM,
2 SP-M/AUTO, 3 SP-ABS, 4 SP-SAP, 5 SP-CND-K, 6 SP-AXL, 7 SP-S/SCR, 8 SP-E/SCR, 9 SP-EH,
10 SP-E+, 11 SP-DEtq, 12 SP-ABS_Re. "Solid" / "energy" = detail +0x12 bit 0x10 / 0x20.

| Option | Slot field | Maths |
|---|---|---|
| SP-AXL (bit 6) | +32 lock time | FCS class lock time × 115 ÷ 100 for **solid** weapons, × 85 ÷ 100 for all others, clamped 1–255. Without it: the FCS value as is. Lock class 4 (no lock) always gets 1. Note the trade-off: solid weapons lock *slower* with SP-AXL if lower = faster; **check in game** |
| SP-M/AUTO (bit 2) | +12 | Missiles (kind 1) get bit 0x80 |
| SP-SAP (bit 4) | +9 (cannons) | Recoil = detail +2 × ⌊4,169,728 ÷ S⌋ × ⌊32,768,000 ÷ W⌋ ÷ 2²⁴, clamped 1–64 (about detail +2 × 8,144,000 ÷ (S × W)). S = legs stability (+0x40), **+800 with SP-SAP**; W = carried weight. So SP-SAP and heavier ACs both cut recoil |
| SP-DEtq (bit 11) | +18 | Non-solid weapons: energy per shot × 3 ÷ 4 |
| SP-EH (bit 9) | +35 | Energy weapons: fire interval × 100 ÷ 120 (= × 5 ÷ 6, faster) |
| SP-E+ (bit 10) | +38 | Energy weapons: attack × 115 ÷ 100 |

Without options, slot +9 by kind: guns (0) = detail +2; missiles (1) = (256 − FCS class lock
time) ÷ 8, 0 → 1, capped at 64; cannons (2) = the recoil formula above.

## Match start (VS engine `0x8008342C`, arena copy `0x800850DC`)

Runs once per player with the AC object in a0; player block pointer from `0x8003EAA0`.
Order:

1. Clears player block +0x10, +0x11; `0x80082834` sets the selected-weapon values (slot +7/+8/+9
   above); `0x8007DC24` sets up the body collision from +0x36–0x3C.
2. Derives battle values from the block into player block +0x200–0x216, +0x16, +0x44, +0x1FC
   (handed to the battle chat in `README.md`).
3. Human PLUS ≥ 6: energy adjustment (see "Energy").
4. Refills current ammo (+16) from max (+14) for each usable slot (count +0x87).
5. AC object: +0x168 / +0x16A = stat +0x20 (AP × 4, current and max), +0x16C = stability
   (+0x24) × 25 ÷ 4, +0x16E = stealth (+0x86), +0x38 = flags +0x34 minus bits 0–1; then stat
   +0x34 bit 3 cleared. Also AC +0x43 = 5, +0x36 = 1, +0x70 = 2, +0x118 = 0.
6. Body frame: two-legged + Human PLUS ≥ 4 → +0x28 = 3; leg init `0x80085688` (frames 0, 3)
   or `0x800875C8` (1, 2); then model setup `0x80059624`.

Writes into the block during the fight (outside match start), found by a code scan:
+0x02 bit 0 (see header); radar range +0x1E is set to 5,000 for one draw call and then to 0 at
`0x800571BC` when player 1's block +0x08 bit 0 is set (purpose unknown, looks like a display
mode).

## To name by a test in VS

Poke one, watch the effect, log the result here:
+0x02 bit 5 (options +0x35), +0x05 (stage?), +0x0F/+0x10 (core bytes),
+0x26, +0x2A (head bytes), +0x34 bits 0–1, +0x36–0x3C (collision?), body frame 3 (Human
PLUS ≥ 4), slot +7/+8/+9 → player block +0x210–0x214, slot +10 to +12. Watch slot +58–61 and
+0x04/+0x11 for any write to confirm they're padding.

## Next steps

1. Read `0x800876C4` (collision setup) to name +0x36–0x3C without a test.
2. Check in game which way SP-AXL moves lock time for solid weapons.
3. The tests above (needs the user's PC).
