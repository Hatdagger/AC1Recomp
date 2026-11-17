# Master of Arena — Battle System Mapping

Goal: decode the battle engine (bucket 3 of the system map) well enough to read, change and
eventually replace how an AC moves, fights and takes damage. Status today: ~10%.

Disc: USA disc 1 (SLUS-01030). Arena engine = FDAT entry 210, mission engine = FDAT 207.
Both load from disc over the same address range, so every finding must say which engine.

## What is already anchored

| Thing | Where | Source |
|---|---|---|
| Player block, 928 bytes × 2 | 0x80045198 / +928 | repo (moa_ai_battle.c) |
| Pointer to the slot's AC object | player block +920 | repo |
| AC object, 376 bytes each, back to back | arena: player AP at 0x801D8D70, opponent at 0x801D8EE8 | codes + repo size |
| Weapon stats in play (ammo, range, lock speed, lock box, missile locks) | player block +0x2E6 … +0x376 | codes |
| Booster / energy reserve | player block +0x016 | codes |
| Opponent AI per frame / init / setup | 0x8008D4AC / 0x8008DC98 / 0x80055768 | repo |
| Camera routine and state | 0x80088988, set 0x80053280 | repo (netplay) |
| Controller sampler, replay flag, match timer | 0x8007F69C, 0x8003EA92, 0x8009EFDC | repo |

See `ram-map-gameshark.md` for the full code-derived list.

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
| ? | u16 | AP | codes: 0x801D8D70 player / 0x801D8EE8 opponent; base address of the object still to find | arena |

## Player block fields (0x80045198)

| Offset | Size | Meaning | Proven by |
|---|---|---|---|
| +0x004 / +0x006 | u16 | Button word / previous (read by the AC code) | repo |
| +0x016 | u16 | Booster / energy reserve | code (infinite booster) |
| +0x2E6 / +0x2E8 | u16 | Hand weapon ammo | code |
| +0x2EC / +0x2EE | u16 | Hand weapon range | code |
| +0x2F0 | u16 | Hand weapon lock speed | code |
| +0x2F4 / +0x2F6 | u16 | Hand weapon lock box | code |
| +0x2FA | u16 | Missile locks / autofire | code |
| +0x324 … +0x338 | | Back weapon A (stride 0x3E) | code |
| +0x362 … +0x376 | | Back weapon B | code |
| +0x398 | u32 | Pointer to AC object | repo |

## Open questions

- Base address of the AC object array in the arena engine, and AP's offset in it.
- Is the player block a copy made at match start, or live? (Test: poke range mid-fight.)
- Mission engine AC object (0x801E73B0 is player AP there): same layout?
- Where the enemy list lives in missions (non-AC enemies).
