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

Plain 2-player **VS** (split screen), time limit set to unlimited. Both units are under
direct control (no AI, no clock), so one action at a time can be repeated and logged: one
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

1. **AC object (376 bytes)** — AP first (anchored), then position, velocity, facing, heat/energy,
   on-ground/boosting state, lock target. Everything else in combat reads these.
2. **Damage path** — what lowers AP: hit test → damage formula (weapon attack vs shell/energy
   defence) → AP write. Ties the part tables' defence numbers to combat.
3. **Weapons and projectiles** — fire routine, the projectile list (likely another fixed array),
   reload/ammo, missile lock-on using the FCS lock box.
4. **Movement** — ground speed cap (Armored-CoreR uses 75 per axis), boost, jump, turning,
   leg type differences. This is the "feel" Armored-CoreR keeps chasing.
5. **Match start / end** — the call that sets up both ACs from the build and part tables
   (unblocks direct launching from the launcher) and the result record.

## AC object fields

| Offset | Size | Meaning | Proven by | Engine |
|---|---|---|---|---|
| +0x45 | u8 | State: 0 standing, 1 walking, 64 jump take-off, 65 rising, 75 falling, 97 landing | Armored-CoreR (measured) | VS |
| +0x08 / +0x0A / +0x0C | s16 | Position x / y / z (y = height; start: P1 (-3000,-1251,-3000), P2 (5000,-1251,-3000)) | moved with the pad, teleport test | VS |
| +0x12 | u16 | Facing, 4096 = full turn (Left/Right change it) | pad test | VS |
| +0x20 / +0x24 | s16 | Last step's x / z | pad test | VS |
| +0x3A | u16 | Hit stagger, set on hits with flag 0x40 (from the hit's +2, halved unless player block +0x284 bit 0) | damage code 0x800796E0 | VS |
| +0x3C / +0x3E | u16 | Hit flash: +0x3C = 100, +0x3E = damage/4, on hits with flag 0x80 | damage code 0x800796F8 | VS |
| +0x64 | u32 | On-hit callback, called after the AP write | damage code 0x80079748 | VS |
| +0x168 | u16 | **AP × 4** (default AC 33,536 = 8,384 AP) | write trace on a hit | VS |
| +0x16A | u16 | Max AP × 4 | read | VS |

VS engine objects: P1 0x801D4BC0, P2 0x801D4D38 (player block +0x398 points to them).
Arena engine (codes): P1 AP at 0x801D8D70 = an object at 0x801D8C08 if the layout matches.

## Player block fields (0x80045198)

| Offset | Size | Meaning | Proven by |
|---|---|---|---|
| +0x004 / +0x006 | u16 | Button word / previous (read by the AC code) | repo |
| +0x016 | u16 | Booster / energy reserve | code (infinite booster) |
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

## Test harness (this chat)

Debug-tools Linux build from branch `mapping-debug` (draft release, not for players),
run headless with the debug server on port 4370. Pad input via `press` (port 1/2),
RAM via `read_ram`/`write_ram`, writers via `wtrace_add` + `wtrace_dump`. VS from the
title screen: Start, Start, Down, Cross, Cross. Cross confirms; Square fires; Circle
boosts; L1/R1 strafe; L2/R2 look; Left/Right turn.

## Open questions

- Match timer address in VS (to freeze it for unlimited time).

- Blade damage mismatch (683 expected vs 492 seen in Armored-CoreR): data chat says blade record +0x30 is attack and +0x2C charge drain (swapped in the old notes). Re-measure a blade hit in VS.

- Base address of the AC object array in the arena engine, and AP's offset in it.
- Is the player block a copy made at match start, or live? (Test: poke range mid-fight.)
- Mission engine AC object (0x801E73B0 is player AP there): same layout?
- Where the enemy list lives in missions (non-AC enemies).
