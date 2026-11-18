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
| Boot and loads | `boot-loads.md` | boot chat | What is read from the disc, what goes into VRAM and what state is set at each step: power-on, every menu start, title, Scenario (Load Save / New Game) → hub, VS setup, VS START, arena launch, the fight engines' loads; what a direct start must replay | static read done 2026-11-17 (disc 1); open items in its §10 |
| GameShark RAM list | `ram-map-gameshark.md` | shared, append-only | Raw code-derived addresses | reference |
| Missions: weapon detail +0x04 | `parts-tables.md` | data chat | Narrow check only: does the missions engine (FDAT 207) read weapon detail +0x04? Not the rest of the missions lane | done 2026-11-17: yes, and so do VS, arena and replay (the earlier "no reader" was wrong); see `parts-tables.md` |
| Menus: scenario hub | `menus-hub.md` | hub chat | The hub ring (FDAT 206 from `0x800552C0`): its 8 entries, tables, input, models and labels, and how to add / remove / replace entries. Not the screens behind the entries (garage, shop, …) | static read done 2026-11-17, icon and caption pictures found |
| Missions | `missions.md` | missions chat | Scenario missions apart from the shared fight code: picking and launching a mission (mission screen table `0x8019F7A8`, list builder `0x800942BC`, which routine sets the mission id), the missions engine FDAT 207, **per-mission code and maps = FDAT 0–99** (5 entries per mission, not MIS.T), MIS.T (mission screen data, briefings, mail), objectives, win/lose checks, timer, results and rewards, non-AC enemies and the enemy list, scripted sequences, mail triggers. The AC object, damage and weapons in flight stay with the battle chat | claimed 2026-11-17 |

Not claimed yet (claim before starting): the save → RAM unpack (the copy table of the 'N' inline unpack in LOAD,
see "Hub chat → launch chat"), menus and UI flow (other than mode launch and the hub ring), sound/music, stage data
(FDAT 100–199 look like per-stage code + maps for another engine: code based near `0x80099xxx`).

Already covered in code, not a mapping lane: **save / memory card file format.** The 8,192-byte scenario save block
(checksum = sum of inverted bytes over +432 for 3,240 bytes, stored at +384, checked in LOAD by `0x80070F1C`), its garage,
credits, names, owned parts, shop stock, arena flags and ranks, and the .mcd card image are read and written by
`games/master-of-arena/moa_card.c` (cards ↔ saves, story saves, the launcher's MEMORY CARD manager) and `moa_arena_run.c`
(`moa_run_make_save`). The field list is in the project doc `arena-run-design.md` ("Scenario save block"). Read those
before mapping anything save-related. Not covered: how LOAD copies a save into RAM (the open item above).

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

- Missile **launcher table `0x800912B0`** (VS, 8 bytes per model ID = slot +1): flight kind, ignition delay,
  missiles per volley from each side, spread. Model IDs → part names would make it readable
  (details in `battle-system.md`, "Missiles").

- Player block **+0x298 = boost button mask** (energy `0x80082D90` and air handler `0x8007FBAC` test it).
- Measured (VS, default AC): selected slot +7 / +8 = **0 / 0 for the rifle** (so +0x210 / +0x212 = 1 / 1),
  17 / 59 for the missile (3 / 28). `stat-block.md` says hand weapons get 64; worth a re-check.

VS AI chat → battle chat (2026-11-17, VS engine, plain VS on the default stage, harness-measured):
- **Stage area boxes** (the "stage flag" in Match start and end): stage code `0x80098214` makes one
  object per box with handler **`0x80098124`**. The box is centre (record +0 / +2 / +4) plus signed
  bytes × 256 (min +0x22 / +0x23 / +0x24, max +0x25 / +0x26 / +0x27), bit number +0x28. The test
  `0x80097FE0(player block, box, bit)` ORs `1 << bit` into player block +0x08 when the AC is inside.
  Test stage: box 0 = the whole level (bit 0), boxes 1 and 2 overlap to make the arena floor
  (x −9,856…9,344 / z −9,440…9,504, and x −13,184…12,928 / z −13,024…13,088).
  Measured: 7 inside, **5 = outside box 1 only (warning)**, **1 = outside both inner boxes → that side
  loses** (a balanced AI vs AI round ended this way at x −13,201). The bits are cleared each step
  before the stage sets them, so reads from inside the AC update see 0.
- **Inputs** (checked with pad presses): ✕ while standing = jump (state 0x40 → 0x4B airborne);
  a direction + ✕ = ground boost (state 0x30 forward, 0x33 strafing left, class 0xB0), but only once
  the AC is already moving: ✕ on the first step still jumps. L1 = strafe left = facing + 1,024.

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
- **The game's LOAD = `0x80073424(port, file, flag)`** (corrected 2026-11-17). It picks a path by the
  save's type letter in the card list (list + 0x890): **'N' = Master of Arena's own save**:
  checksum `0x80070F1C(buf + 432)` (sum of inverted bytes, 3,240 bytes) == word buf + 384, then an
  **inline** unpack inside LOAD (`0x800735E0` … `0x80073B78`), LoadImage `0x8002C75C(0x800CC070,
  buf + 1152)` / `(0x800CC078, buf + 3200)` (emblem, palette), DrawSync, then `0x800B6A80`,
  `0x80099098`, `0x800941F4`, `0x80093BC4`, `0x800A2318`, `0x800A2020`. The other paths (from
  `0x80073E28`) are the **imports**: Project Phantasma (type 'S', checksum at 512 over 3,288 from
  560) and original Armored Core (checksum `0x80070F70`, 3,044 bytes from 560, unpack
  `0x80072E54`). My first note called the AC1 import path "the game's LOAD"; that was wrong.
  Effect: the Rogue Arena direct boot (branch `rogue-direct`) used the import checksum/unpack, so
  on a real run card it always falls back to the old menu presses (safe, no speed-up). Fix =
  replay the 'N' inline copies in C (needs that unpack's copy table; save-format lane).

Boot chat → launch chat (details in `boot-loads.md`):
- **0x80044121 = `0x80058B04()`**, run at every menu start: which disc is in the drive (reads
  `\GG\COM\CD.DAT`, magic 0xBA; 0 = disc 1, 1 = disc 2, 250+ = drive errors). Disc 1 →
  NEW/LOAD menu. We only use disc 1.
- `mode-launch.md` §4: LOAD's callback `0x800AC93C` with result byte +0xA0 = 78 does not "go back";
  it shows two boxes and continues to the NEW GAME setup `0x800ACA0C`. Otherwise `0x800AC9E8`
  opens the hub and deletes the card-screen task.
- LOAD only unpacks. The card screen's state `0x8005CE68` does the rest after a good LOAD: owned
  parts → 0x8003EB20, `0x80090FD8`, `0x80083130`, `0x80067244(1)`, **`0x800822B0` (queue the AC's
  part textures)**, wait for the disc, back-weapon VRAM moves. Any direct Load Save needs it.
- Fight engines never read `*_T` texture archives: the AC textures must already be in VRAM
  from the menu (`boot-loads.md` rule 2).

Boot chat → Rogue Arena work:
- **Why the direct boot shows no AC (and missing graphics) until the second arena fight:** the
  direct boot replays LOAD but skips the card screen's finish (above), so `0x800822B0` never
  queues the AC's textures. The arena's return path (`0x800A70C0`) queues them after fight 1.
  A fix that replays the finish is on branch **`rogue-direct-hub-gfx`** (`moa_arena_run_boot.c`,
  one commit, not built or merged). Still to check: whether the hub background has the same cause.

Boot chat → Rogue Arena work (2026-11-17, after reading `rogue-arena-49` up to 476a989):
- `0x800544A0(n, 1)` is the title's **intro movie** routine (queues MENU_TIM 2, plays movie n
  with `0x80054B54`, then `0x800B6A80`). It is only reached on the logos/intro road, so dropping
  it from the direct boot was right.
- Boot trace labels (`moa_boot_trace.c`): `0x80057ED4` is not the model loader (it runs once
  at every menu start with (0, 0)); the models load through **`0x80057F74(slot, MENU_TMD entry)`**.
  `0x80056DBC` is the hub ring task (sound bank MENU_VAB 2/3, MENU_TIM 1 and 4 queued, model slots
  15/16 = MENU_TMD 1/2, then the AC part models into slots 3–14). Worth adding to the trace:
  `0x80057F74`, `0x800822B0` (AC textures), `0x8005CE68` (card screen's finish), `0x80058700`
  (sound bank). See `boot-loads.md`.
- 0x8003EA7A: in the code only the link-return check (`0x800A6BB0`) sets it to 1, and the menu
  entry clears it right after its title call (`0x800A6EA4`), even when a filter takes that call.
  If it was really still 1 after a fight, something else wrote it; the trace's RAM snapshots would
  show when. Clearing it in the direct boot is harmless either way.
- The launcher's new START GAME > LOAD GAME (branch `launcher-start-menu`,
  `moa_quick_start.c`) runs the same direct load as the Rogue Arena boot, card screen's finish
  included; once both work, one shared routine could serve both.

Boot chat → Rogue Arena work (2026-11-18):
- The arena screen loads the **opponent's textures** in `0x800C0D14(records, n)` from its
  64-byte arena record, between two `0x800C0E7C` swaps; 12 of the 158 opponents use a
  pre-painted texture set (record byte 14). Any direct arena launch or custom opponent must
  replay it (`boot-loads.md` §7).

Launch chat → data chat:
- Stage (compiled +0x05 = player block +0x255) at VS START = byte `0x800D5800[map − 1]`,
  map = 0x8003EA81 (1–10), written by `0x800B348C`. VS START compiles P1 then P2 in
  `0x800B2F9C` after loading each player's parts / extras / options into the garage.
