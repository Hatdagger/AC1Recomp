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
| Compiled stat block | `stat-block.md` | data chat | **The 324-byte block the menu builds at VS START (player block +0x250 … +0x393)**: every field, the compile routine `0x800A7BB4` that fills it, and how the engines read it | every byte named 2026-11-17 (compile, match start, options, camera, lock-on; test list settled from code) |
| Part tables | `parts-tables.md` | data chat | Part tables, weapon details, options on the disc | complete 2026-11-17: weapon detail +0x04 = post-shot hold (read in every engine); only its base length needs a live check |
| Mode launch | `mode-launch.md` | launch chat | Boot → menu → engine loop, the mode byte, resume flags, how Scenario (hub) and VS (menu → VS START → fight) start; goal: starting modes straight from the launcher | static read done 2026-11-17; repo gaps filled; runtime test next |
| GameShark RAM list | `ram-map-gameshark.md` | shared, append-only | Raw code-derived addresses | reference |
| Missions: weapon detail +0x04 | `parts-tables.md` | data chat | Narrow check only: does the missions engine (FDAT 207) read weapon detail +0x04? Not the rest of the missions lane | done 2026-11-17: yes, and so do VS, arena and replay (the earlier "no reader" was wrong); see `parts-tables.md` |
| Menus: scenario hub | `menus-hub.md` | hub chat | The hub ring (FDAT 206 from `0x800552C0`): its 8 entries, tables, input, models and labels, and how to add / remove / replace entries. Not the screens behind the entries (garage, shop, …) | static read done 2026-11-17, icon and caption pictures found |

Not claimed yet (claim before starting): missions data (FDAT 207, apart from the +0x04 check above), save/memory card format,
menus and UI flow (other than mode launch and the hub ring), sound/music, stage data.

Already covered in code, not a mapping lane: **arena opponent rosters.** Arena numbers, opponent
counts (`0x800D5A08`), starts (`0x800D5A14`), names (`0x800D5A20`), the per-arena entries in
`GG/MS/MENU_TIM.T` (names, emblems, 64-byte builds, descriptions, prize money, 36-byte AI records),
beaten flags and rank bytes are all used by `games/master-of-arena/moa_phantasma.c`,
`moa_ai_battle.c` and `moa_arena_run.c`, and by `psxrecomp/runtime/src/mod_builtin_moa_phantasma.c`.
Read those before mapping anything arena-related.

## Handed over

From battle chat to data chat:
- Damage reads player block **+0x272 / +0x273** (compiled +0x22 / +0x23) as the shell / energy damage factors (×/64); default AC = 27 / 32. Confirms Armored-CoreR's note. Code `0x80079500` (VS engine).
- Player block **+0x284 bit 0**: when clear, hit stagger is halved (`0x800796A4`). Meaning still unknown.
- Blade **confirmed in combat** (VS engine): compiled +0x68 = 738 is the attack used by the damage routine
  (246 AP per hit as an energy hit); compiled +0x1C = 2,050 is not used in damage. Blade record +0x30 = attack.
- Blade reads from the stat block (VS engine, static read, `0x80083224` / `0x8007EB48`):
  +0x29C = blade button mask, **+0x2B6 = blade hit record** (flags; +0x2B8 attack),
  +0x26C = charge drain taken from energy +0x16 at swing start, +0x283 = blade wave type
  (0 none, 1–4 jump table at 0x80051154; 4 = no melee sweep `?`), +0x2A8 / +0x2A9 = the
  two model joints of the blade line (hand, tip), +0x298 = a button mask checked after the
  sweep `?`, +0x28E = a button mask that stops the lunge `?`.


- Missile interceptor note in `parts-tables.md` says the missile handler is `0x8009907C`;
  measured in VS it is **`0x8008907C`** (pool record +0x18 while a missile flies). AC +0x118
  (interceptor burst) confirmed counting down in the snapshots.

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
- From the match-start routine `0x8008342C` (details in `stat-block.md`, "Match start"):
  player block **+0x04 = pad held, +0x06 = pad last frame** (raw bits: Up 0x10 … □ 0x8000).
  +0x16 = energy capacity, +0x18 = 0. +0x200 = thrust(+0x06) × 2, +0x202 = +0x07 × 200,
  +0x204 = +0x08 × 14, +0x206 = +0x09 × 140, +0x208 = +0x0A × 14, +0x20A = +0x0B × 140,
  +0x20C = turn accel × 5 ÷ 16, +0x20E = turn speed × 15 ÷ 16, +0x216 = friction × 4
  (all "+0x0N" = compiled). +0x210 / +0x212 / +0x214 = selected weapon slot +7 / +8 / +9
  scaled (`0x80082834`, also on weapon change). +0x44 = `0x80091340` + body combination × 20.
  +0x1FC = radar range². △ (next weapon) bumps +0x11; `0x80082834` reads +0x10 as the slot.
- AC object at match start: +0x168 / +0x16A = AP × 4 (stat +0x20), +0x16C = stability × 25 ÷ 4,
  +0x16E = stealth, +0x38 = stat flags +0x34 minus bits 0–1 (SP-JAM bit 3 moves here),
  +0x43 = 5, +0x36 = 1, +0x70 = 2, +0x118 = 0. Fire routine is `0x80080DF4(ac, slot)`.
- **Camera record** (56 bytes; VS `0x801D6EF8` P1 / `0x801D6F30` P2, pointer player block
  +0x39C; arena `0x801E29C8`): +0x0A distance 650, +0x0C height 40, +0x0E smoothing shift 2,
  +0x2C / +0x2E angles, +0x01/+0x02 joints and +0x04 hidden-joint list for the alternate
  camera (stat +0x02 bit 0 = first-person cockpit view, START + △ + □, tested; `0x8007CC70`). Chase camera
  `0x800878FC`, alternate `0x80087B90`. START + ○ + ✕ sets camera +0x08 bit 0: tested, the
  camera stays in place and keeps following the AC (screenshot camera). Details in `stat-block.md`, "Camera setup".
- Player block **+0x210 / +0x212** (from the selected slot +7 / +8) are a turn set that depends
  on the weapon: hand weapons always give 11 / 31, back weapons give values from the legs'
  second turn figure (legs +0x3C) and carried weight (formulas in `stat-block.md`, slot +7/+8).
  Worth checking how the engine uses them next to +0x20C / +0x20E (normal turning).
- **Missile interceptor** `0x800829E4` (VS; arena `0x80084694`): reaction chance stat +0x0F ÷ 128
  per frame, cone ±stat +0x10 × 4, range 5,000, AC +0x118 = 15-frame burst, 1 in 32 shots kill
  the missile. Object list `0x801D6F6C` (256 × 228 bytes), missile handler `0x8009907C`. Details
  in `parts-tables.md`.
- ~~Stat +0x34 bits 0–1: which objects carry +0x38 bits 0–1?~~ Answered by the data chat: they
  are the head's noise canceler / bio sensor, and the sweep `0x80078230` only checks the two
  ACs, which never carry those bits, so no effect in fights. SP-JAM: a missile lock on a jammer
  AC breaks with a 5% chance per frame (`0x800809B0`).
- AC state 34 looks like the two-legged cannon crouch (cannon fire needs it on frame 0,
  `0x80081BF4`); `0x8007E3E0` (turning with player block +0x210 / +0x212) is installed from
  it. Worth confirming in the state machine.
- **Hit record** (6 bytes: flags, attack, impact) is the same for guns (slot +36–41) and the
  blade (stat +0x66–0x6B, so player block +0x2B6 = flags 39, +0x2B8 attack, +0x2BA impact).
  Flags: 0x10 solid / 0x20 energy pick the defence factor; 0x40 (WR-RS7) puts attack into
  target AC +0x3A; 0x80 (WG-FGI-00 flame shell) sets AC +0x3E = damage ÷ 16, +0x3C = 100
  (burn? `0x800796E4`). Impact → knockback level 0–5 = impact × 256 ÷ AC +0x16C (`0x80079AA4`,
  from the AC hit callback `0x80082430`, set as AC +0x64 in both leg inits).
- Slot **+10 = firing animation ID** → AC +0x84 at every shot (gun `0x80080FE4`, missile
  `0x800818D0`, cannon `0x80081BC4`, which compares it with AC +0x68 → +0x0E). Fire routines per
  kind: gun `0x80080DF4`, missile `0x80081888`, cannon `0x80081B18` (dispatch `0x80083160`).
- Gun slot +9 bits: 0–1 shots per trigger (1 → 2, 2 → 4), 3 + bits 4–5 → alternate sides
  (AC +0x47 bit 0), bit 2 = fixed-direction path (dispensers), bit 3 = weapon-arm aim
  (passed to `0x800800E4`, changes AC +0x7E–0x82 when locked). Slot +12 bit 2 = no aim (rockets).
- Lock-on read for SP-AXL (`stat-block.md`, "Lock-on"): lock records 8 × 48 bytes at player
  block +0x7C; guns/cannons lock in `0x800800E4` (counter vs distance × slot +32), missiles in
  `0x80080638` (cursor at player block +0x20/+0x22, then counter vs slot +9). Slot +12 bits 0/1
  = draw lock box / aiming guide in the HUD sight routine `0x800576E0`.
- **AC +0x6D = post-shot hold** (2026-11-17, all engines). Gun / cannon fire copies slot +11
  (weapon detail +0x04) into it and zeroes AC +0x7C (VS `0x80081518` / `0x800822A8`). While
  it's ≠ 255: no gun or cannon fires, the cannon cooldown +0x48 pauses, and each frame `0x80080DB8`
  masks AC +0x164 with player block +0x2BC (stat +0x6C) and calls `0x80054DE4(AC + 0x6C)`. That
  call clears it to 255 once +0x7C ≥ (u16 at anim data `*(AC+0x88)` + `*(*(AC+0x88)+8)`) + the byte.
  The anim data's +0x0C table is indexed by animation ID (`0x80054A2C`).
  **Missions only:** a script reader also writes +0x6D, +0x7C, +0x7E and +0x84 from a byte stream
  (`0x80091FFC`, stepped at `0x80092E4C`), and `0x80067048` passes +0x6C/+0x6D to `0x8005A890`.
  Probably cutscene/scripted moves (missions lane).

Launch chat → battle chat:
- FDAT **211 is a fifth engine, the VS replay** (entry 0x80051FA4). When replay is picked the
  VS engine sets 0x8003EA92 = 1 and returns 2 (`0x80052290`); the exe then loads 211, keeping
  RAM above 0x801F8000 (the replay buffer). Engine table: 207 missions, 208 link, 209 VS,
  210 arena, 211 VS replay. Details in `mode-launch.md`.

Hub chat → launch chat:
- **The game's LOAD = `0x80073424(port, file, flag)`**: reads 0xE80 bytes of the card file into a
  buffer, checksum `0x80070F70(buf + 560)` (byte sum of 3,044 bytes) == word buf + 512, then
  **`0x80072E54(buf)` unpacks it into RAM**, LoadImage `0x8002C75C(0x800CC070, buf + 1080)` and
  `(0x800CC078, buf + 3128)` (emblem picture and palette), DrawSync, then `0x800B6A80`,
  `0x80099098`, `0x800941F4`, `0x80093BC4`, `0x800A2318`, `0x800A2020`. That answers §7's
  "Scenario true skip": no need to map the save → RAM copy by hand. Used by the Rogue Arena
  direct boot (branch `rogue-direct`, `moa_arena_run_boot.c`).

Launch chat → data chat:
- Stage (compiled +0x05 = player block +0x255) at VS START = byte `0x800D5800[map − 1]`,
  map = 0x8003EA81 (1–10), written by `0x800B348C`. VS START compiles P1 then P2 in
  `0x800B2F9C` after loading each player's parts / extras / options into the garage.
