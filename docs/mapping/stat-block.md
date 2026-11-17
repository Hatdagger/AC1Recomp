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

Part records named below are the menu tables in `parts-tables.md`. "Garage" = loadout bytes:
0 core, 1 booster, 2 head, 3 arms, 4 blade, 5 hand, 6 back L, 7 back R, 8 generator,
9 FCS, 11 legs.

## Header and movement (+0x00 – +0x11)

| Off | Size | Meaning | Source |
|---|---|---|---|
| +0x00 | u16 | 120 or 300 or 0, picked by byte `0x8004B59C` (0 → 120, 1 → 300, else 0) | mode setting; **name by test** (a timer?) |
| +0x02 | u8 | Flags: bit 0 cleared, bits 1 and 2 set, **bit 3 = legs can fit a booster** (legs +0x42) | |
| +0x03 | u8 | **Human PLUS level** (`0x8003EAC8`) | |
| +0x04, +0x05 | | not written | |
| +0x06, +0x08 | u8 | Thrust: booster power (9,800 for caterpillars) × 4096 ÷ carried weight ÷ 224, 1–64 | known |
| +0x07, +0x09 | u8 | Thrust × 0.8, 1–50 | known |
| +0x0A, +0x0B | u8 | Thrust × 1.2, 1–64; fixed for legs 21 (26), 22 (23), 23 (19), 24 (26), 26 (24), 27 (6), 28 (7), 30 (13), 31 (11), 32 (13) | known |
| +0x0C | u8 | Turn acceleration (legs +0x38 ÷ weight) | known |
| +0x0D | u8 | Turn speed (legs +0x3A ÷ weight), 0–64 | known |
| +0x0E | u8 | Friction: weight × 8 ÷ 1000, 32–96 | known |
| +0x0F, +0x10 | u8 | **Core bytes +0x34, +0x35**, copied as is | **name by test** |
| +0x11 | | not written | |

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
| +0x28 | u8 | **Body frame**: 0 two-legged, 1 caterpillar, 2 four-legged (from the body table) | |
| +0x29 | u8 | **Body combination** (0–39): core shape × arms shape × legs shape | |
| +0x2A | u8 | Head byte +0x32 | **name by test** |

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
| +0x34 | u16 | Flags, starts at 3: bit 0 cleared if head byte +0x34 = 1; bit 1 cleared if head byte +0x35 = 1; **bit 3 = SP-JAM (missile jammer)** |
| +0x36 – +0x3C | 4×u16 | From the body table (650, 40, 100, 2 on almost every body; caterpillar + regular arms has 0 instead of 40) — **name by test** |

## Body layout (+0x56 – +0x87)

The body combination (+0x29) picks a 16-byte entry at `0x800D5008`, whose first word points
to a layout record (three layouts on the disc: `0x800D4DC4` two-legged, `0x800D4DFE`
caterpillar, `0x800D4E72` four-legged). The combo table at `0x800D4A64` (12-byte entries: core
shape, list of arms, list of legs) decides the combination. Cores 3 and 4 (XXA-S0, XXL-D0)
count as core 0. The shapes in that table: arms {0,1,3,6,7,8}, {4,5}, {2}, weapon arms
{12–21}; legs {0,3,6,10,11}, {2,5,8,9,13}, {1,4,7,12}, four legs {21–26}, caterpillars
{27–32}. Reverse-joint legs (14–20) aren't in it: to check what they get.

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
| +7, +8 | u8 | 64 | constant |
| +9 | u8 | Detail +2 (adjusted for some kinds) | detail |
| +10, +11, +12 | u8 | Detail +3, +4, +5 (+12 can get bit 0x80) | detail |
| +14 | u16 | Ammo (max) | detail +8 |
| +16 | u16 | Ammo (current, starts full) | detail +8 |
| +18 | u16 | Energy per shot / ammo price (depends on kind) | detail +0xA / +0xC |
| +20 | u16 | FCS range for the weapon's lock class | FCS |
| +22 | u16 | Weapon range | detail +0xE |
| +24 – +30 | 4×u16 | Lock box: outer w, h, inner w, h (0 for no-lock weapons) | FCS |
| +32 | u16 | Lock time (scaled for solid weapons, SP-AXL), 1–255 | FCS |
| +34 | u8 | Locks at once: the smaller of FCS max and the weapon's salvo | FCS, detail +0x10 |
| +35 | u8 | Fire interval (energy weapons with SP-EH get faster) | detail +0x11 |
| +36 | u8 | Damage flags (0x10 solid, 0x20 energy) | detail +0x12 |
| +38 | u16 | **Attack** | detail +0x14 |
| +40 | u16 | Impact | detail +0x16 |
| +42 – +61 | | not written at VS START (in-fight state?) | |

The slot code also applies SP-M/AUTO, SP-SAP, SP-E+ and SP-DEtq; the exact maths per option is
the next thing to write down (routine `0x800A8904`–`0x800AA75C`).

## Not written by the compile routine

+0x04–0x05, +0x11, +0x3E–0x55, and slot bytes +42–61 (plus the gaps in the slots). Either the
battle engine fills them at match start (VS engine `0x8008342C` writes player block +0x200 on,
which overlaps this block's start) or they're in-fight state. **Next step**: watch them in a
plain VS fight with the write trace and log which routine writes each.

## To name by a test in VS

Poke one, watch the effect, log the result here:
+0x00 (120/300), +0x0F/+0x10 (core bytes), +0x26, +0x2A (head bytes), +0x34 bits 0–1,
+0x36–0x3C, slot +9 to +12.
