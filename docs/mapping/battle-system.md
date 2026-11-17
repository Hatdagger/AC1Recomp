# Master of Arena — Battle System Mapping

Goal: decode the battle engine (bucket 3 of the system map) well enough to read, change and
eventually replace how an AC moves, fights and takes damage. Status today: ~10%.

Disc: USA disc 1 (SLUS-01030, v1.0). Each mode has its own engine, loaded from
`GG/COM/FDAT.T` over the same address range (from 0x80050DD8), so every finding must say
which engine it was measured in:

| FDAT entry | Engine | Mode byte 0x8003EA2A |
|---|---|---|
| 206 | menus | — |
| 207 | missions | |
| 208 | link-cable versus (netplay) | |
| 209 | **split-screen VS** | 2 |
| 210 | **arena** (also AI Battle, which switches the mode byte from 2 to 3) | 3 |

**Earlier research lives in Armored-CoreR** (`docs/research/gen1-*.md` in
Hatdagger/Armored-CoreR): engine map, movement and energy, weapons/FCS/damage, timing,
models. That is the main source; this file only adds what the recomp side finds and
must not contradict it without a measurement.

## What is already anchored

| Thing | Where | Source |
|---|---|---|
| AC state byte: 0 standing, 1 walking, 64 jump take-off, 65 rising, 75 falling, 97 landing | AC object +0x45 | Armored-CoreR gen1-movement.md (VS engine) |
| Movement values scaled in at match start (rise, boost, turn speeds) | player block +0x200 on, by VS engine 0x8008342C | Armored-CoreR gen1-movement.md |
| Energy rules (drain, refill, red zone, booster lock) | VS engine 0x80082D90; lock = player block +0x18 bit 0 | Armored-CoreR gen1-movement.md |
| Weapon detail tables, FCS records, damage factors | menu tables + VS engine | Armored-CoreR gen1-weapons.md |
| Arena main loop, opponent AI scoring | arena engine 0x80052378 | Armored-CoreR gen1-engine-map.md |
| Player block, 928 bytes × 2 | 0x80045198 / +928 | repo (moa_ai_battle.c) |
| Pointer to the slot's AC object | player block +920 | repo |
| AC object, 376 bytes each, back to back | arena: player AP at 0x801D8D70, opponent at 0x801D8EE8 | codes + repo size |
| Weapon stats in play (ammo, range, lock speed, lock box, missile locks) | inside the compiled stat block, player block +0x250 … +0x393 | codes — **data chat's lane** (`stat-block.md`) |
| Booster / energy reserve | player block +0x016 | codes |
| Opponent AI per frame / init / setup | 0x8008D4AC / 0x8008DC98 / 0x80055768 | repo |
| Camera routine and state | 0x80088988, set 0x80053280 | repo (netplay) |
| Controller sampler, replay flag, match timer | 0x8007F69C, 0x8003EA92, 0x8009EFDC | repo |

See `ram-map-gameshark.md` for the full code-derived list.

## Test setup

Plain 2-player **VS** (split screen). The clock is kept alive by writing 2,600 back into
0x80097EB4 (unlimited time is also a real option in player 1's or 2's VS settings, but the
write is quicker to set up). Both units are under direct control (no AI), so one action at a time can be repeated and logged: one
shot, one boost, one hit. VS runs on its **own engine (FDAT 209)**, not the arena engine: the
arena-only addresses (the AP codes gated on 0x8007BBAE, the AI hooks) do not apply to plain
VS until re-found there. Player blocks (0x80045198) are shared by all engines.
Use AI Battle only later, to check that AI-driven units behave the same.

Before trusting any address, confirm the disc is NTSC-U **v1.0**: a v1.1 code set exists
with the same layout shifted (+0x64 save data, +0x60 battle data).

## Method

1. **Watch a value change, then name the code that changed it.** The runtime's debug tools
   (debug server, write trace, `PSX_READ_WATCH=lo,hi`) report which guest function touches
   an address. One AP drop → the damage routine. One ammo drop → the fire routine.
2. **Poke a value, watch the effect.** Change one byte of the AC object mid-fight in AI Battle
   and note what the AC does. This names fields fast without reading code.
3. **Record every finding** in the tables below: address/offset, meaning, how it was proven,
   which engine. Unproven guesses are marked `?`.

## Order of work

1. **AC object (376 bytes)** — first pass done 2026-11-17 (table below). Open: heat, the
   animation records, +0x118, the two extra callbacks. Everything else in combat reads these.
2. **Damage path** — what lowers AP: hit test → damage formula (weapon attack vs shell/energy
   defence) → AP write. Ties the part tables' defence numbers to combat.
3. **Weapons and projectiles** — fire routines, pool layout and update routines done
   2026-11-17 ("Weapons in flight"); missile lock slots, volleys and steering read 2026-11-17
   (static). Open: run-test the missile notes; the second missile spawn call `0x8007BED4`.
4. **Movement** — ground speed cap (Armored-CoreR uses 75 per axis), boost, jump, turning,
   leg type differences. This is the "feel" Armored-CoreR keeps chasing.
5. **Match start / end** — the call that sets up both ACs from the build and part tables
   (unblocks direct launching from the launcher) and the result record.

## AC object fields

Measured 2026-11-17 in VS (FDAT 209) by snapshotting both objects while doing one thing
at a time (stand, walk, turn, strafe, boost, fire, get hit), then naming fields from the code.
"snap" = seen changing in the snapshots; `?` = unproven.

| Offset | Size | Meaning | Proven by |
|---|---|---|---|
| +0x00 / +0x04 | u32 | List links (previous / next object; P1: 0x801D4B68 / 0x801D4B10) `?` | read |
| +0x08 / +0x0A / +0x0C | s16 | Position x / y / z (y = height, **negative = up**; start P1 (−3000, −1251, −3000), P2 (5000, −1251, −3000)) | pad + teleport |
| +0x12 | u16 | Facing, 4096 = full turn (0 = +z, 3072 ≈ +x) | pad test |
| +0x20 / +0x22 / +0x24 | s16 | Last step's x / y / z | snap |
| +0x28 / +0x2A | u16 | 100 / 212 (+0x2A = body height, same value as +0x74) `?` | read, `0x8007D2D8` |
| +0x2C | s16 | **Floor height below the AC** (−1250 on the ground; 32767 when nothing found below) | snap (high jump) |
| +0x30 | u16 | Floor polygon / material under the AC (87 on the VS floor, −1 = none) `?` | snap |
| +0x32 | u16 | Bit 2: can't be hit by the blade line test (set at creation from the model) | code `0x80078A38` |
| +0x36 | u16 | Flags. Starts 1. **Bit 0x10 = busy** (set during a blade swing and during hit stagger; blocks a new swing) | code + snap |
| +0x38 | u16 | Stat flags from match start (SP-JAM bit 3) | data chat |
| +0x3A | u16 | Hit stagger, set on hits with flag 0x40 (from the hit's +2, halved unless player block +0x284 bit 0) | `0x800796E0` |
| +0x3C / +0x3E | u16 | Hit flash: +0x3C = 100, +0x3E = damage/4, on hits with flag 0x80 | `0x800796F8` |
| +0x43 | u8 | 5 from match start `?` | data chat |
| +0x45 | u8 | **State** (list below) | snap + code |
| +0x46 | u8 | **Movement class**: 0 still, 2 turning on the spot, 0x10 moving on the ground, **0xC0 airborne**, 0x80 staggered | snap |
| +0x48 | u8 | **Refire cooldown**: the gun fire routine only fires at 0, otherwise counts it down (5 after a rifle shot) | `0x80081034` + snap |
| +0x49 | u8 | 255 during the fight (the match timer only runs while P1's is 255) | timer code |
| +0x4A | u8 | State when the current animation started (the state setter writes both +0x45 and +0x4A) | `0x80080060` |
| +0x58 | u32 | **Animation-finished callback**, called with (object, +0x4A); picks the next state. ACs: `0x8008389C` | `0x8007F088` |
| +0x5C / +0x60 | u32 | Two more callbacks (ACs: `0x80085180`, `0x80085668`) `?` | read |
| +0x64 | u32 | On-hit callback after the AP write. ACs: `0x80082430` (sets the stagger states 0x80 / 0x81) | `0x80079748` |
| +0x68 | u32 | Pointer, 0x801A2E10 for P1 (model data `?`) | read |
| +0x6C … +0x6F | | Animation player (`0x80053F50(obj + 0x6C, obj + 0xD4)` each state change) | `0x8008000C` |
| +0x70 | u8 | 2 from match start `?` | data chat |
| +0x72 | u16 | **Hit cylinder radius** (75) | `0x80078A50` |
| +0x74 | u16 | **Hit cylinder height** (212); centre = y − height/2 | `0x80078A5C`, `0x800832E0` |
| +0x76 | s16 | **Frame counter of the current state** (resets to 0 on every state change; blade hit window = 7–11) | snap + blade code |
| +0x8C … +0xCA | | Animation channel records (start / end frame pairs, e.g. 50/51, 9888/9889) `?` | snap |
| +0xD4 / +0xD6 / +0xD8 | s16 | **Velocity x / y / z** in 1/64 units per step (walk ≈ 960 x; jump −3264 y up, +5056 falling; knock-back from a rifle hit −448 / −64). Not cleared when the AC stops | snap |
| +0x118 | s16 | **Missile interceptor burst counter** (15 at a burst start, keeps counting down below 0; see `parts-tables.md`, "Missile interceptor") | snap + data chat |
| +0x150 | u16 | 15 while rising on the booster; cleared at swing start `?` | snap |
| +0x152 … +0x158 | | Two (id, value) pairs, e.g. (0xA0B4, 140), changed by firing and boost-dashing: sound / effect handles `?` | snap |
| +0x164 | u32 | 6 while rising on the booster; cleared at swing start `?` | snap |
| +0x168 | u16 | **AP × 4** (default AC 33,536 = 8,384 AP) | write trace on a hit |
| +0x16A | u16 | Max AP × 4 | read |
| +0x16C | u16 | Stability × 25 ÷ 4 (default AC 237) | data chat + read |
| +0x16E | u16 | Stealth | data chat |

**States (+0x45)** — seen in snapshots or written by the code:

| Value | State | Source |
|---|---|---|
| 0 | standing | snap |
| 1 | walking | snap |
| 5 | strafing (R1) | snap |
| 9 | turning on the spot (Left) | snap |
| 1 … 8 | ground movement group (jump table at `0x80085730`) | code |
| 0x20 / 0x21 | blade swing, ground / airborne (+0x46 = 0xC0) | blade code |
| 0x22 / 0x23 | blade / action states set by `0x80081B18` and `0x800828BC` `?` | code |
| 0x30 … 0x37 | set by the animation-finished handler `0x8008389C` from the animation byte +0x6C, likely landing / recovery variants `?` | code |
| 64 (0x40) | jump take-off (Cross on the ground) | snap |
| 65 (0x41) | rising on the booster | snap |
| 0x41 … 0x48 | airborne group (jump table at `0x8007FCD8`) | code |
| 74 / 75 (0x4A / 0x4B) | falling | snap + code |
| 96 / 97 (0x60 / 0x61) | landing | code, Armored-CoreR |
| 0x80 / 0x81 | **hit stagger** (set by the on-hit callback `0x80082430`) | snap + code |

State setter: **`0x8007FFF0(object, animation, state)`**: starts the animation, scales the
velocity ×64, writes +0x45 and +0x4A.

VS engine objects: P1 0x801D4BC0, P2 0x801D4D38 (player block +0x398 points to them).
Arena engine (codes): P1 AP at 0x801D8D70 = an object at 0x801D8C08 if the layout matches.

## Player block fields (0x80045198)

| Offset | Size | Meaning | Proven by |
|---|---|---|---|
| +0x000 / +0x002 | u16 | Button word copies (match +0x04 / +0x06 in the snapshots) `?` | snap |
| +0x004 / +0x006 | u16 | Button word / previous (read by the AC code) | repo |
| +0x00A | u16 | 5 after the first jump / landing (set in `0x8008389C`) `?` | snap + code |
| +0x016 | u16 | Booster / energy reserve (28,000 full; 18,844 → 6,963 while boosting) | code + snap |
| +0x030 … +0x03F | | Blade line from the previous swing frame | blade code |
| +0x07C | u32 | **Lock target**: pointer to the locked AC object (P2's 0x801D4D38 when P2 is in front; 0 when lost). Also the blade lunge target | snap + blade code |
| +0x07C … +0x1FB | 8 × 48 | **Lock slots**, one per locked target (+0x7C is slot 0's target, +0xA2 its lock count). Layout in "Missiles: lock slots, volleys and steering" | code `0x80080638` |
| +0x01A | u16 | Locks still free (missile weapon) | code |
| +0x020 / +0x022 | u16 | Missile lock cursor pitch / yaw | code + `stat-block.md` |
| +0x078 | u32 | Lock slot being worked on | code |
| +0x24C | u16 | "HIT" HUD blink: 40 on landing a hit on the other AC, counts down | code `0x8008821C`, `0x8005699C` |
| +0x24E | u16 | Set (37) when this AC is hit, counts down (damage indicator `?`) | snap |
| +0x250 … +0x393 | 324 bytes | Compiled stat block (weapon slots, ammo, range, lock values…) — **data chat's lane, see `stat-block.md`**; writes found here get handed over in README.md | — |
| +0x398 | u32 | Pointer to AC object | repo |

## Damage (VS engine, measured 2026-11-17)

Hit handler `0x800795DC(target object, hit record, a2, a3)`:

1. Target is P1's or P2's AC object → sets the "current player block" pointer
   `0x8003EAA0` to that player's block and calls `0x80079500(hit)`. Any other object
   (non-AC targets) → `0x80079580(armour class, hit)`.
2. `0x80079500`: `raw = hit+2` (attack). Shell hits (hit byte 0 & 0x30 = 0x10) use
   `raw × player block +0x272 >> 6`; energy hits (0x20) use `+0x273`. Then
   **damage = raw × 2 / 3**.
3. AP (object +0x168, in quarters) −= damage × 4, floored at 0 (`0x80079704…0x80079720`).
4. Hit flags: 0x40 → stagger (+0x3A), 0x80 → flash (+0x3C/+0x3E); then the object's
   +0x64 callback.

Non-AC targets (`0x80079580`): energy hits scale by `(8 − class) / 8 + 1`, the class from
bits 8–11 of a value on the target's model; shell hits do full damage.

Check: WG-RF35 (attack 218) on the default AC (shell factor 27): 218 × 27 >> 6 = 91,
× 2 / 3 = **60 AP per hit** — matches the HUD (8,384 → 8,324 → …).

Hit record (`0x801D6F88` for P2's rifle): byte 0 flags (= weapon detail +0x12, 0x17),
+2 attack (218), +4 impact (3) — the weapon detail table values.

## Blade (VS engine, measured 2026-11-17)

The default VS AC carries an LS-2001 blade. It is not in the Triangle weapon cycle; it has
its own button, **Circle**. A swing lunges about 500 units forward along the facing
(facing 0 = +z).

- One blade hit on the default AC = **246 AP**. Same damage routine as guns
  (AP write at `0x80079720`), treated as an **energy** hit: 738 × 32 (player block +0x273)
  >> 6 = 369, × 2 / 3 = **246**. Repeated 4 times, same result each time.
- In RAM: P1 player block **+0x2B8 = 738** (compiled +0x68, blade attack) and
  **+0x26C = 2,050** (compiled +0x1C, charge drain). This confirms the data chat's reading:
  blade record **+0x30 = attack**, +0x2C = charge drain.
- Armored-CoreR's "492 seen" is two blade hits (one swing once landed 492 here too). The
  "683 expected" came from the swapped fields and can be dropped.
- Reach: one hit at 700 units, two hits from 650 in (see "Blade code"), miss at 800.

### Blade code (VS engine, read statically from FDAT 209, 2026-11-17, not yet run-tested)

**Swing start** (inside the AC update, `0x80083224`):
- Needs the blade button (player block +0x29C) newly pressed (+0x04 set, +0x06 clear),
  `0x8008F8B8` false, blade attack +0x2B8 ≠ 0, energy not locked (+0x18 bit 0 clear),
  no swing already running (AC object +0x36 bit 0x10 clear), and energy +0x16 **greater
  than** the charge drain +0x26C. Then +0x16 −= drain.
- Lunge target: `0x80078230` searches 3,000 units ahead within ±512 (±45°) of the facing;
  the found AC goes to player block **+0x7C**. While swinging, the AC turns towards it
  each frame at turn speed +0x20E (`0x8007EB48` start).
- AC object **+0x45 = 0x20** (ground swing) or **0x21** (when +0x46 = 0xC0, likely
  airborne `?`); **+0x36 |= 0x10** (swinging).

**Each swing frame** (`0x8007EB48`, frame counter = AC object **+0x76**):
- Frame 6: stores the blade line (hand → tip, model joints player block +0x2A8 / +0x2A9,
  via `0x80078888`) in player block +0x30 … +0x3F.
- **Frames 7–11 are the hit window** (5 frames): `0x8007E9EC(ac, player block +0x2B6)`.
  The hit record is **player block +0x2B6** (so +0x2B6 = flags, +0x2B8 = attack 738).
- From frame 12 the swing winds down; +0x36 bit 0x10 is cleared at the end
  (`0x8007EFD0`).
- Blade waves (player block +0x283 = 1…4, a jump table at 0x80051154) spawn a
  projectile through `0x8008E664` once per swing (flag player block +0x40).
  +0x283 = 4 skips the melee sweep entirely (wave only `?`).

**Blade sweep** `0x8007E9EC`: each active frame it tests **two** blade lines, the
**halfway** line between last frame's and this frame's position, then this frame's
line (`0x8007E82C` twice), and then saves this frame's line to +0x30. Each test:
1. `0x80071ED4`: stage first (`0x80077EC8`), then both AC objects except the swinger
   (`0x800789DC`, line vs the target's cylinder: radius object +0x72, height +0x74;
   skipped if object **+0x32 bit 2**, a flag set at object creation from the model),
   then other objects (`0x8008CFB8`). The first thing hit → hit dispatcher **`0x8008821C`**
   → (AC) `0x800795DC` damage, plus a spark (`0x8008C164`) and a sound (`0x8008B7B8`).
2. `0x80074FA4`: stage geometry; destructible pieces (owner +0x30 bit 17) also take the hit.

**Why one swing can hit twice (492):** there is **no "already hit this swing" check**.
Each of the up to **10 line tests** (5 frames × 2) that touches the target's cylinder
deals a full hit. Normally the blade arc crosses the target in only one sample → 246.
When two samples overlap the target → 2 hits = 492. Theoretical maximum 10 hits.

**Measured (VS engine, 2026-11-17, harness script `blade4.py`):** both ACs reset to the same spot
before each swing (P2 at (5000, −1251, −3000), P1 straight behind it at distance d, facing it),
default AC vs default AC, write trace on P2's AP:

| Distance | Result (11 swings) |
|---|---|
| 800 | miss |
| 700 | 1 hit, 246 (twice) |
| 650, 600, 550, 500, 400, 300 | **2 hits, 492, every time** |

So **close range = double hit** for the default LS-2001 blade; 3+ hits never seen. The two
AP writes land 3–4 video frames apart, i.e. on **two different swing frames**, not the
halfway + current pair within one frame. (The earlier "from 400 it missed" was P2 knocked out of
place by the previous swing.)

**Hit dispatcher side note:** when one AC hits the other, the attacker's player block
**+0x24C = 40**; it counts down each frame and drives the HUD "HIT" blink (`0x8005699C`).

## Weapons in flight (VS engine, 2026-11-17)

**Fire routines by weapon kind** (slot +0 = kind, slot record = player block +0x2D8 + slot × 62,
selected slot = player block +0x10; picked in the AC update at `0x80083160`):
0 gun → **`0x80080DF4`**, 1 missile → **`0x80081888`**, 2 cannon → **`0x80081B18`** (the cannon one
sets the AC states 0x22 / 0x23, the stop-to-fire stance `?`).

Gun fire `0x80080DF4`: needs AC +0x6D = 255 and **refire cooldown AC +0x48 = 0**, the fire
button (player block +0x29A) or AC +0x47 set, and ammo (slot +16) > 0. Slot +9 bits 0–1 =
shots per trigger. The **model ID (slot +1)** picks the family, its shot table (data chat's
shot-speed tables) and its spawner; every spawner installs a per-frame **update routine**:

| Model ID | Spawner | Update routine (record +0x18) |
|---|---|---|
| 0–19 (bullets; rifle = 0) | `0x8008AC30` (loops for multi-shot; ID 8 = 5 shots, −4 ammo) | **`0x80088674`** (measured on the rifle) |
| 0–19, other branch `?` | `0x8008DEC0` | `0x8008DB18` |
| 20–39 | `0x8008E664` (blade waves use it too) | `0x8008E1B0` |
| 40–69 | `0x8008D8CC` | `0x8008D584` |
| 70–89 | `0x8008CBCC` | `0x8008C948` |
| 90+ | `0x8008D38C` | `0x8008D0A4` |
| missiles (kind 1) | `0x8008B070` | **`0x8008907C`** (measured) |

Every update routine reaches the hit dispatcher `0x8008821C` except the 90+ one (`0x8008D0A4`, not traced).

### The object pool (256 × 228 bytes at 0x801D6F6C)

Shared by shots, missiles and effects (smoke, sparks, explosions have their own update
routines, e.g. `0x80089F6C`, `0x8008A000`). Cleared at match start by `0x800881D0`
(+0x06 = −1 in every record). Measured by dumping the pool while a rifle shot and a missile
were in flight:

| Offset | Size | Meaning |
|---|---|---|
| +0x00 / +0x02 / +0x04 | s16 | Position x / y / z |
| +0x06 | s16 | **−1 = free**, 0 = in use |
| +0x08 | u32 | Owner (the firing AC object) |
| +0x0E | u16 | 83–84 `?` |
| +0x10 / +0x12 | u16 | 1 (2 for some effects) / 255 `?` |
| +0x16 | u16 | **Life left**, counts down each step (rifle 13, missile 60) |
| +0x18 | u32 | **Update routine** (table above) |
| +0x1C … +0x22 | | **Hit record** passed to the dispatcher: +0x1C flags (0x17 for both), +0x1E attack (rifle 218, missile 830), +0x20 impact (3 / 12), +0x22 2 / 94 `?`. This is the 0x801D6F88 record seen in the damage test |
| +0x24 | s16 | Rifle: **speed per step (701)** |
| +0x2A / +0x2C / +0x2E | s16 | Missile: **target position** (P2's x, y − 106, z) |
| +0x38 | u32 | Missile: **target object** (P2's 0x801D4D38) |
| +0xBC / +0xBE / +0xC0 | s16 | Rifle: **position one step earlier** (the hit test runs along this line) |
| +0xC4 … +0xCA | | Rifle: 25850, 250, 4, 30 `?` |

Not done yet: the families 20+ in play (needs a build with those weapons). Missiles: see below.

### Missiles: lock slots, volleys and steering (VS engine, static read 2026-11-17, not run-tested)

Read from FDAT 209 only. Angles: 4,096 = 360°. "Step" = one game step (~22 per second).
Weapon slot offsets are decimal, as in `stat-block.md`. The lock build-up itself (cursor speed
from slot +32, lock time slot +9) is in `stat-block.md`, "Lock-on"; this adds the rest.

**Lock slots**: 8 × 48 bytes from player block +0x7C (+0x7C … +0x1FB), one per locked target.
Offsets from the slot start (slot 0 = player block +0x7C):

| Slot off | Meaning |
|---|---|
| +0x00 | Target object (0 = empty) |
| +0x08 | Lock timer (counts up while the cursor sits on the target) |
| +0x10 / +0x12 / +0x14 | Target position (refreshed from the sight sweep each step) |
| +0x16 | Flags: bit 1 = lockable, bits 3+4 = locked |
| +0x18 / +0x1A | Direction to the target (pitch / yaw) |
| +0x26 | **Locks on this target** (max 6) |

The snapshots' "+0x86 / +0x9A count up" are slot 0 +0x0A / +0x1E: driver still open.

Other player-block fields used (`0x80080638`):
- **+0x1A** = locks still free (slot +34 − locks held).
- **+0x1C** = last missile launched at the current target.
- **+0x20 / +0x22** = cursor pitch / yaw (rests at +0x218 / 0).
- **+0x26** = cursor shown.
- **+0x28 / +0x2A / +0x2C** = cursor point in the world.
- **+0x78** = lock slot being worked on.

**Build-up extras**:
- Candidates come from the sight sweep `0x80078230` with the inner lock box (slot +28 / +30).
- A lock is dropped when its direction leaves the outer box (± slot +24 pitch, ± slot +26 yaw)
  or the target leaves the sweep.
- It stops gaining locks once the locks held reach slot +34 or the current ammo (slot +16).
- Each new lock plays sound 0x2B (0x2C for the last free one), and **the cursor jumps back
  to the centre**. So every extra lock on the same target makes the cursor travel out again.
- While locking, a tick sound 0x2F plays every 4 steps.

**Firing** (`0x80081888`):
- It fires when there is at least one lock and either the fire button is held (pad +0x04 &
  +0x29A), or no locks are free and slot +12 bit 0x80 (SP-M/AUTO) is set.
- AC +0x47 = locks × 4. It counts down 1 per step, and every 4th step a volley goes out from
  side A and side B (`0x800815A4` twice). That uses one lock, and the target gets
  `0x8008F3C8(target, 2, ac)`. After the last volley AC +0x48 = slot +35.
- `0x800815A4` launches each missile:
  - ammo slot +16 goes −1;
  - the start direction is the target direction plus random spread;
  - the start speed is 100 plus the launcher's motion;
  - the spawner is `0x8008B070`.

**Launcher table `0x800912B0`**, 8 bytes per model ID (slot +1):
- +0 = flight kind
- +1 = ignition delay (steps)
- +2 / +3 = missiles per volley from side A / B
- +4 / +6 = random spread (pitch / yaw)

| Model | Kind | Delay | A+B | Spread |
|---|---|---|---|---|
| 0 | 0 | 1 | 1+0 | 300 / 600 |
| 1 | 1 | 1 | 1+0 | 288 / 578 |
| 2 | 0 | 1 | 1+1 | 0 |
| 3 | 1 | 1 | 3+3 | 0 |
| 4 | 2 | 2 | 1+0 | 256 / 512 |
| 5 | 3 (splits) | 1 | 1+0 | 256 / 512 |
| 6 | 3 (splits) | 1 | 1+1 | 0 (+0xC0 pitch) |
| 7 | 0 | 1 | 2+0 | 0 |
| 8 | 0 | 1 | 3+0 | 0 |
| 9 | 5 (never lets go) | 3 | 1+0 | 0 |
| 10 | 4 (aims during delay) | 20 | 1+0 | 0 |
| 11 | 11 (never lets go) | 1 | 1+0 | 0 |

**Missile pool record** (update `0x8008907C`, spawner `0x8008B070`):
- +0x16 = life (flight table +0x02)
- +0x22 / +0x24 / +0x26 = **velocity**
- +0x38 = target
- +0x3C = age
- +0x3E = ignition delay
- +0x40 = flight kind
- +0x42 = flags (bit 0 = target has been in the cone, bit 1 = **gave up**)
- +0xBC / +0xBE = heading (pitch / yaw)

**Flight table `0x80093114`**, 40 bytes per kind:

| Kind | Fuel +0x00 | Life +0x02 | Thrust +0x0C | Top speed +0x0E | Turn/step +0x10 | Cone ± +0x12 |
|---|---|---|---|---|---|---|
| 0, 9 | 50 | 60 | 80 | 350 | 110 (9.7°) | 1024 (90°) |
| 1 | 80 | 100 | 70 | 320 | 100 (8.8°) | 90° |
| 2, 3, 4, 12 | 120 | 160 | 60 | 300 | 90 (7.9°) | 90° |
| 5 | 180 | 200 | 30 | 100 | 50 (4.4°) | 90° |
| 8 | 80 | 100 | 40 | 300 | 45 (4.0°) | 800 (70°) |
| 10 | 80 | 100 | 40 | 320 | 60 (5.3°) | 90° |
| 11 | 250 | 300 | 30 | 100 | 60 | 1536 (135°) |

The rest of each flight record:
- +0x08 / +0x0A = exhaust puff (`0x8008A660`);
- +0x16 / +0x20 / +0x24 = hit effect and blast data.

Kinds 8, 9, 10, 12 are not used by the launcher table. The other spawn call (`0x8007BED4`) may use them `?`.

**Each step**:
1. Before ignition (age < +0x3E): coasts with no thrust, under gravity (0x8003EA48). Kind 4 already
   turns toward the target.
2. At ignition: sound 0x6E, then an exhaust trail. **Kind 3 splits into 4 kind-0 missiles 6 steps
   later** (yaw ±0x1C0, two with pitch −0x200), same target.
3. **Homing** (`0x80088F60` gives the direction to the target):
   - inside the cone (± table +0x12, pitch and yaw) → bit 0 is set;
   - **leaving the cone after having been inside → bit 1 is set, and it gives up for good**. It then flies on
     with a 10% chance per step of a random ±32 wobble. Before it has ever been inside, it keeps turning
     toward the target;
   - kinds 5 and 11 clear bit 1 every step, so they never give up;
   - the heading turns by at most table +0x10 per step (`0x800136E0`).
4. Thrust: velocity += heading × table +0x0C, capped at table +0x0E. Then position += velocity
   and the collision sweep runs (`0x80088BF8` → `0x80074FA4`).
5. Burn-out (age ≥ table +0x00): the nose eases 16 per step toward pitch 0x400 and velocity +0x24
   gains 8 per step, so it drops. Life ends it.
6. When nothing is hit: an exhaust puff, and **every 8 steps the target gets `0x8008F3C8(target, 1, …)`**
   (likely the "missile incoming" signal the interceptor reacts to `?`).
7. On a hit: hit effect `0x8008C164`, the hit dispatcher `0x8008821C`, a blast `0x8008BB78` on
   some targets, then removal (`0x8008B60C`).

Arena engine: the same handler is most likely at `0x8008FFB4` (pattern match only).

## Match timer (VS engine, measured)

- Time left = **game steps** at `0x80097EB4` (u16, timer struct `0x80097EA0` +0x14).
  About **22 steps per second** (2:00 ≈ 2,640). Writing it moves the HUD clock.
- Decremented once per step at `0x800523CC` (called from `0x80052180`), only while byte
  `0x801D4C09` (P1 object +0x49) is 255 and the value is ≥ 2.
- For tests, the harness writes 2,600 back into it to keep the clock from running out.

## Test harness (this chat)

Debug-tools Linux build from branch `mapping-debug` (draft release, not for players),
run headless with the debug server on port 4370. Pad input via `press` (port 1/2),
RAM via `read_ram`/`write_ram`, writers via `wtrace_add` + `wtrace_dump`. VS from the
title screen: Start, Start, Down, Cross, Cross. Cross confirms; Square fires; **Circle
swings the blade; Cross boosts** (user-confirmed; earlier notes had Circle as boost);
Triangle changes weapon; L1/R1 strafe; L2/R2 look; Left/Right turn.

## Rogue Arena fight recorder (arena engine)

`games/master-of-arena/moa_run_record.c` logs every Rogue Arena fight to
`<run file>.rec.csv` (beside the memory card): per fight a header with both AC object
pointers (player block +0x398), a layout check (max AP non-zero, AP ≤ max), raw dumps of
both player blocks and AC objects, then a row whenever any value changed, checked on every
call of the arena controller sampler `0x8007F69C` (t = call number; buttons; per AC: x, y,
z, facing, state +0x45, movement +0x46, AP, energy, locked, refire +0x48). The rows are at
most one per game step (values only change on steps), so t against row count also shows
how often the sampler runs per step and a summary line (air %, boosting %, locked %, blade
swings, shots, range, hits). Its first test log answers "AC object base in the arena
engine" (expected 0x801D8C08 for P1 from the AP code). Later use: shaping a ghost's AI
record from how the player fought.

## Open questions

- Blade double hit: solved (no per-swing hit guard; 2 hits from ≤ 650 units, 1 at 700).
  Open: whether other blades (longer reach, Moonlight) can reach 3+ hits.

- Base address of the AC object array in the arena engine, and AP's offset in it.
- Is the player block a copy made at match start, or live? (Test: poke range mid-fight.)
- Mission engine AC object (0x801E73B0 is player AP there): same layout?
- Where the enemy list lives in missions (non-AC enemies).
