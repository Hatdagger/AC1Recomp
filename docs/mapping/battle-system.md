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
| ? | u16 | AP | codes: 0x801D8D70 player / 0x801D8EE8 opponent; base address of the object still to find | arena |

## Player block fields (0x80045198)

| Offset | Size | Meaning | Proven by |
|---|---|---|---|
| +0x004 / +0x006 | u16 | Button word / previous (read by the AC code) | repo |
| +0x016 | u16 | Booster / energy reserve | code (infinite booster) |
| +0x250 … +0x393 | 324 bytes | Compiled stat block (weapon slots, ammo, range, lock values…) — **data chat's lane, see `stat-block.md`**; writes found here get handed over in README.md | — |
| +0x398 | u32 | Pointer to AC object | repo |

## Open questions

- Blade damage mismatch (683 expected vs 492 seen in Armored-CoreR): data chat says blade record +0x30 is attack and +0x2C charge drain (swapped in the old notes). Re-measure a blade hit in VS.

- Base address of the AC object array in the arena engine, and AP's offset in it.
- Is the player block a copy made at match start, or live? (Test: poke range mid-fight.)
- Mission engine AC object (0x801E73B0 is player AP there): same layout?
- Where the enemy list lives in missions (non-AC enemies).
