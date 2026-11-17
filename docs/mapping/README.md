# Mapping — who is doing what

Several chats map Master of Arena at the same time. To avoid double work:

1. **Before starting**, pull the latest master and read this table.
2. **Only work inside your own lane.** If you find something in another lane, add one line to
   that lane's "Handed over" list below; don't edit that lane's doc.
3. **Claim new work here first** (one commit), then start.
4. Commit notes with `[skip ci]`. Rebase on master before pushing; never force-push.

## Lanes

| Lane | Doc | Owner | Covers | Status |
|---|---|---|---|---|
| Battle system | `battle-system.md` | battle chat | Everything that runs during a fight: player block, AC object, **the 324-byte compiled stat block (player block +0x250)**, damage, weapons in flight, movement, AI per frame, match start/end | in progress |
| Static game data | `parts-tables.md`, `arena-roster.md` (next) | data chat | Data stored on the disc and read by the menus: part tables, weapon details, options, **arena opponent rosters** (builds, AI settings, rewards, ranks) | parts ~90%; roster claimed |
| GameShark RAM list | `ram-map-gameshark.md` | shared, append-only | Raw code-derived addresses | reference |

Not claimed yet (claim before starting): missions data (FDAT 207), save/memory card format,
menus and UI flow, sound/music, stage data.

## Handed over

From data chat to battle chat:
- Blade: +0x30 of the blade record is attack, +0x2C most likely charge drain (it's what lands
  at compiled +28). Armored-CoreR's notes had them swapped, which may explain the blade damage
  mismatch (683 vs 492). Details in `parts-tables.md`.
- The GameShark weapon slots (player block +0x2E6 on) sit inside the compiled stat block
  (compiled +0x96 on).
- Legs carry a second turn set (+0x3C/+0x3E, two-legged −20%), likely airborne or boosting
  turning; worth spotting when the compiled block is mapped.
