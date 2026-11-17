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
| Battle system | `battle-system.md` | battle chat | Everything else that runs during a fight: player block (outside the stat block), AC object, damage, weapons in flight, movement, AI per frame, match start/end | in progress |
| Compiled stat block | `stat-block.md` | data chat | **The 324-byte block the menu builds at VS START (player block +0x250 … +0x393)**: every field, the compile routine `0x800A7BB4` that fills it, and how the engines read it | compile routine fully read 2026-11-17 |
| Part tables | `parts-tables.md` | data chat | Part tables, weapon details, options on the disc | ~90%, paused |
| GameShark RAM list | `ram-map-gameshark.md` | shared, append-only | Raw code-derived addresses | reference |

Not claimed yet (claim before starting): arena opponent rosters, missions data (FDAT 207), save/memory card format,
menus and UI flow, sound/music, stage data.

## Handed over

Battle chat → data chat: if you find a write into player block +0x250 … +0x393 (the stat
block), note it here instead of mapping it.

Data chat → battle chat:
- Blade: +0x30 of the blade record is attack, +0x2C most likely charge drain (it's what lands
  at compiled +28). Armored-CoreR's notes had them swapped, which may explain the blade damage
  mismatch (683 vs 492). Details in `parts-tables.md`.
- The GameShark weapon slots (player block +0x2E6 on) sit inside the compiled stat block
  (compiled +0x96 on).
- Legs carry a second turn set (+0x3C/+0x3E, two-legged −20%), likely airborne or boosting
  turning; worth spotting when the compiled block is mapped.
- Stat block (`stat-block.md`): AP total ×4 at player block +0x270, radar range +0x26E,
  stealth flag (P77-ST) +0x2D6, usable-weapon count +0x2D7, SP-JAM flag +0x284 bit 3. Weapon
  slot attack is slot +38, impact +40.
