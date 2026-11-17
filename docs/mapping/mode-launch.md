# Master of Arena — How the game starts Scenario and VS

Goal: know exactly how the game goes from boot to the scenario hub and from the VS menu
into a fight, so the launcher can jump straight there instead of tapping through menus.

Disc: USA disc 1 (SLUS-01030, v1.0). Read statically from the boot exe and FDAT entries
206–211 (2026-11-17). Addresses below are measured from the code unless marked `?`.

## 1. The big picture

The boot exe owns one loop. The **menu module (FDAT 206)** is everything you see outside a
fight: logos, title, Scenario/VS choice, the scenario hub (garage, arena lists, missions),
and the VS menu. When the player starts a fight, the menu sets the **mode byte** and returns.
The exe then loads the matching **engine** into the same memory (from 0x80050DD8), runs it,
and loads the menu again when the fight is over.

**Scenario mode is not an engine.** It is the hub inside the menu module. Only the fights
it launches (arena, missions) are separate engines.

| Mode byte `0x8003EA2A` | Engine (FDAT entry) | Entry point | Memory cleared before load |
|---|---|---|---|
| 0 | 207 missions | 0x800524B8 | up to 0x801FE000 |
| 1 | 208 link-cable versus | 0x80051FDC | up to 0x801FE000 (replay: 0x801F8000) |
| 2 | 209 split-screen VS | 0x80051F4C | up to 0x801FE000 |
| 2 + replay flag | **211 VS replay** (new) | 0x80051FA4 | up to 0x801F8000 (keeps the replay buffer) |
| 3 | 210 arena (also AI Battle) | 0x80052378 | up to 0x801FC898 |

FDAT numbering: entry N is sectors `tab[N-1] … tab[N]` of the u16 table after the 4-byte
header (same as `moa_parts.c`). Entry 211 holds the string "REPLAY".

## 2. The exe's main loop (`0x800124B0`)

1. One-time setup, registers the disc archives (`0x80016AE8(n, name)`): 1 = `COM\RTIM.T`
   (loading-screen pictures), 2 = `COM\FDAT.T` (code modules), 4… = the `MS\*.T` part files.
2. **`0x80012640`** — clear the module area, load FDAT 206 (`0x80016A58(2, 206, 0x80050DD8)`),
   call the menu, **`0x800A69C8`**. It returns when a fight should start.
3. Loading screen: `0x80012348(picture, 0)` shows RTIM picture 62, or for missions
   (mode 0) picture `stage + 41` (player block +0x255; stage 19 and 40 use 62).
4. Clears 0x8003EA94, 0x8003EA91, 0x8003EA96, the replay flag 0x8003EA92, player block
   +0x394 for both players.
5. **`0x800126D8`** — dispatch on the mode byte (table above), load the engine, call the
   entry word stored at 0x80050DDC (module header +4).
6. Engine return value:
   - **1** → loading screen, run the same engine again (no reload).
   - **2** → loading screen, back to step 5 *without* clearing the replay flag. The VS engine
     does this after setting 0x8003EA92 = 1 when the player picks replay
     (`0x80052290` in 209), so the exe loads 211, the VS replay engine.
   - anything else → back to step 2 (menu). Loading picture 62 is skipped only in one
     case (0x800A0DB0 == 19, 0x8003EC50 == 0, 0x8003EAC9 == 0) `?` meaning.

## 3. The menu's entry (`0x800A69C8`) — where it resumes

Common setup first, then it decides where to open from a set of **resume flags** in the
exe's data (they survive engine loads):

| Address | Start value | Meaning |
|---|---|---|
| 0x8003EA7A | **1** | "Go to the title." Set in the exe's data, so first boot shows the title. Cleared once the title opens |
| 0x8003EA7F | 0 | Title opens **without logos/intro** when set (`0x800AAE94(a0 = 1)`). Cleared after use |
| 0x8003EA7E | 0 | **Return to the VS menu.** Set to 1 by the VS menu's setup (`0x800B1D9C`), cleared when VS is left (`0x800B5BA4`) |
| 0x8003EA80 | 0 | Return to the link-cable versus menu (set `0x800AD218`) |
| 0x8003EA7C | 0 | An arena fight just finished (set at arena launch `0x800C08E8`) |
| 0x8003EA7D | — | Arena sub-mode (5 set by `0x8007DE4C`) `?` |

Order of checks:

1. If 0x8003EA7E → mode = 2, **`0x800B0CB4`** (VS menu). Done.
2. Else if 0x8003EA80 → mode = 1, `0x800ACE14` (link versus menu).
3. Else if 0x8003EA7A == 0 and mode 0 → back from a mission: mission results
   (`0x80093CD8`, `0x800A2190`, `0x80093C20`, `0x800A20D8`), credits clamped to
   ±99,999,999, final-mission ending check (`0x800ABE38`), then **`0x800552C0`** (hub).
4. Else if 0x8003EA7A == 0 and mode 3 → back from the arena: results, `0x800C0E7C`,
   `0x800822B0`, wait for the disc, then **`0x800552C0`** (hub).
5. Otherwise → title, `0x800AAE94(a0 = 0x8003EA7F ? 1 : 0)`, clear 0x8003EA7A.

Then the menu runs frames until **`0x801B1CC8`** (u16) becomes non-zero, and returns.
That flag is the "start the engine now" signal; every fight launch ends by setting it.

## 4. Title → Scenario (`0x800AB500`, mode row confirmed)

The highlighted row's widget (task +0x80 → +0x14 → +0x50) is a Y position: **176 =
SCENARIO MODE, 192 = VS MODE** (as the repo's autopilots already use).

SCENARIO (176):
- mode byte = 0, 0x8003EA94 = 255, `0x800631FC`, `0x800BA854`, `0x800C566C`,
  `0x800B69F8`, `0x800B6A80`; clears 0x8003EAC8…0x8003EACF (Human PLUS, Ex Arena flags,
  overweight flag — the save fills them back in).
- If 0x80044121 == 0 → next state **`0x800AC3AC`** (NEW GAME / LOAD GAME menu).
  Otherwise straight into the card load (`0x80059554(task, 0x800AC93C)`) `?` what 0x80044121 is
  (set at menu entry from `0x80058B04`).

NEW GAME / LOAD GAME (frame `0x800AC4B4`, cursor = state +0xA2):
- **LOAD GAME `0x800AC8E8`** → memory card load screen `0x80059554` with callback
  `0x800AC93C`. If the load ends with result byte +0xA0 = 78, it goes back; otherwise it opens
  **the hub, `0x800552C0`**.
- **NEW GAME `0x800AC820`** → … → `0x800ACA0C`: credits = 100,000 (0x8003EA74),
  `0x80068A58`, `0x80064918`, `0x80068D7C`, then `0x8008E8D8(task, 0x800ACA88, 80)` (the name
  entry `?`), then `0x800ACB10` → **hub `0x800552C0`** + `0x800941F4` (new-game message `?`).

So every road into Scenario ends at **`0x800552C0` = open the hub** (creates the hub task,
first state `0x800554F4`). That is the routine to call once a save (or a new game) is in RAM.

## 5. Title → VS, and VS START → fight

VS (192) in `0x800AB500`:
- If the second controller is usable (0x801B1CD0 bit 9) or a pad bit 0x80 from `0x8002F7CC`
  is set → `0x80074DC0`, `0x800633A4`, clear 0x8003EAC8…CF, mode = **2**, **`0x800B0CB4`**.
- Otherwise → mode = **1**, `0x800ACE14` (link-cable versus menu).

**VS menu setup `0x800B0CB4` → first state `0x800B0FE4`:**
- Map choice **0x8003EA81** (1…10). First visit: 1. Map option 0x8004B59B: 1 = random
  each time, 2 = next map each time.
- **First visit only (0x8003EA7E == 0)**: both players get the current garage — the 12 part
  ids at 0x80036FB0 go to **0x80045940 (P1) and 0x8004594C (P2)**, owned parts
  0x80036FEC → 0x8004A7E0 (+256 for P2), names from 0x800371EC → 0x8004AB18 / 0x8004AB3C,
  0x8004A7D0 (per-player extras) and 0x8003EAC0 (option bits) zeroed, paint pattern / variant
  copied to 0x8003EAD2+p / 0x8003EAB8+p. Then 0x8003EA7E = 1.

**VS staging area** (what AC SETUP / DATA LOAD edit and VS START reads):

| Address | Per player | Meaning |
|---|---|---|
| 0x80045940 | +12 | 12 part ids (same order as the garage 0x80036FB0) |
| 0x8004A7D0 | +8 | Copied to 0x8003EAC8 at launch (Human PLUS level, the flags after it) |
| 0x8003EAC0 | +2 (u16) | Option parts, copied to 0x8003EA70 at launch |
| 0x8003EAD2 / 0x8003EAB8 | +1 | Paint pattern / variant |
| 0x8003EAD8 / 0x8003EAFC | 36 bytes | Buffer passed to the build check `?` (name) |
| 0x8004AB18 / 0x8004AB3C | 36 bytes | Names copied from the garage on first visit |
| 0x8004B730 | +48 | Paint (colour bytes), per the repo's `MOA_VS_PAINT` |
| 0x8004A7E0 | +256 | Owned-part counts |

**VS START** (`0x800B2D7C`, then states):
1. `0x800B2D7C` / `0x800B2E90`: for P1 then P2, load that player's parts into the garage
   slot and run the **build check `0x8007E164`** (weight/energy legality). A failed check
   shows a message box and stops.
2. **`0x800B2F9C`**: for each player: parts → 0x80036FB0, extras → 0x8003EAC8, options →
   0x8003EA70, then **`0x800A7BB4(player)`** — compile the stat block (data chat's lane,
   `stat-block.md`) — and `0x800C0E7C` (AC graphics swap) when needed. Then restore P1's
   paint into the garage (0x8003EA72/73) and fade out.
3. **`0x800B348C`** (after the fade): stage for both players (player block +0x255) =
   byte at **0x800D57FF + map** (0x800D5800[map − 1]); fight music 0x8003EA29 picked from the
   table at 0x800538A8 (every 4th byte) with the random byte 0x8003EABC; 0x8003EA94 = 0;
   0x801B1CD8 = 0; **0x801B1CC8 = 1** → the menu returns, the exe loads FDAT 209.

After the fight, 0x8003EA7E is still 1, so the menu re-opens straight on the VS menu.

## 6. Other launches found on the way (not followed yet)

| Routine | What it sets | Probably |
|---|---|---|
| `0x800C0878` | 0x8003EA7C = 1, opponent 0x8003EAD5, arena 0x8003EAD4, stage from 0x800D59F0[0x8003EA78 × 2], music, exit | Arena fight launch (mode 3 already set by `0x800BB93C`) |
| `0x8007DE4C` | mode 3, stage 29, 0x8003EA7D = 5, exit | Another arena-engine fight (test fight?) |
| `0x800671A4` | exit only | A launch from a task state `?` (missions) |
| `0x800BB660`, `0x800BDAA8`, `0x800C8E2C` | mode 0 | Mission list / mission launch path |

## 7. What this means for the launcher

**VS — skip the title (easy, uses the game's own code):** at the title-open hook
(`0x800AAE94`, already a hook point) do what `0x800AB500` does for row 192 instead of
opening the title: `0x80074DC0`, `0x800633A4`, clear 0x8003EAC8…CF, mode = 2, call
`0x800B0CB4`. Lands directly on the VS menu with no logos, no title and no START taps.

**VS — straight into the fight (no menu at all):** every piece is now known; it only needs
putting together and testing. In order:
1. Write the staging area: part ids 0x80045940 (+12), paint 0x8004B730 (48 bytes per player),
   options 0x8003EAC0 (+2), extras 0x8004A7D0 (+8), pattern/variant 0x8003EAD2 / 0x8003EAB8.
2. Load each player's AC graphics: **already done in the repo** by AI Battle's
   `moa_load_unit` / `moa_commit_unit` (`moa_ai_battle.c`): point the working file table
   0x801FBB00 (sizes 0x801D0B28) at the player's table 0x8004AB60 + 60 × player (sizes
   0x8004A9E0 + 30 × player), slots 3–14, put the parts and paint in the garage, call the
   garage rebuild **`0x80081E20`**, wait for the disc (`0x800589DC`), copy the table back.
   `0x800C0E7C` trades the two players' texture sets in video memory.
3. Run `0x800A7BB4(0)` and `(1)` (compile the stat blocks).
4. Stage → player block +0x255 for both (compiled +0x05; the engine copies it to
   0x80097EA0 at load), music 0x8003EA29, 0x8003EA94 = 0, mode = 2, 0x8003EA7E = 1.
5. Return from the menu (0x801B1CC8 = 1 inside the menu loop, or skip the menu call).

**Where direct VS goes (decided 2026-11-17):** into the netplay and AI Battle code, as one
shared routine, not as a separate feature. All three versus paths already pass through the
same VS menu and only differ in the mode byte the menu leaves behind:

| User | Mode byte at the end | Engine | Today |
|---|---|---|---|
| Plain VS | 2 | 209 split screen | menu by hand |
| Netplay (`moa_link_netplay.c`) | 1 (set by its hook on `0x800B0CB4`) | 208 link | players use the VS menu |
| AI Battle (`moa_ai_battle.c`) | 3 | 210 arena | scripted menu presses in fast forward, hidden screen |

The shared routine = steps 1–5 above, with the mode byte as a parameter. For netplay both
peers must run it with the same inputs (host's and guest's builds and the stage agreed in
the lobby) so the simulation stays identical. For AI Battle it replaces the scripted
DATA LOAD / SELECT MAP presses (the build comes from the card instead of a session memory
card). After the fight 0x8003EA7E = 1 brings the player back to the VS menu, where the
existing "return to launcher" logic already takes over.

**Implementation (branch `vs-direct`, 2026-11-17, not yet built or tested):**
`games/master-of-arena/moa_vs_direct.c` holds steps 1–3 (boot to the versus menu, units with
names and emblem into the staging area, graphics at VS START). AI Battle uses it by default
(`PSX_MOA_VS_DIRECT=0` brings back the old scripted DATA LOAD route for comparison).
Netplay (branch `netplay-direct`, 2026-11-17, not yet built or tested): the same module's
netplay hooks boot both peers to the versus menu with each match card's first save
(slot 1 = P1, slot 2 = P2), press VS START by script and load the graphics; progress kept
in guest memory (link struct send packet) for rollback. See NETPLAY.md.

**Scenario — Load Save / New Game without the title:** set mode = 0 and the Scenario
values from `0x800AB500`, then run the LOAD GAME state (`0x800AC8E8`) — card screens still
show — or, for a true skip, put the save into RAM ourselves and call `0x800552C0` (open the
hub). The repo already has most of the save ↔ RAM map: `moa_keep[]` in `moa_ai_battle.c`
(save +456 → 0x8003EA94, +496 → 0x8004B508, +640 → 0x80036FEC owned parts, +896 →
0x800370EC, +3232 → 0x8004AC50, +3276 → 0x8004B568 settings, +3332 → 0x8004B5A0 Ex Arena,
+3572 → 0x8004B688; checksum at +384 over +432 … +3671, routine `0x80070F1C`), and the
Arena Run code writes whole cards (`moa_arena_run_save.inc`). Still to map: where the
AC, names, paint and emblem land in RAM, and the full DATA LOAD copy routine (`0x8007361C`
reads the settings).

**Faster boot for everything:** setting 0x8003EA7F = 1 before the menu first runs opens the
title without logos or intro — the game's own switch, no movie hook needed.

## 8. Open questions

- 0x80044121: what decides "NEW/LOAD menu" vs "card load at once" after SCENARIO.
- `0x8002F7CC` bit 0x80 on the VS row (a held button forcing split-screen?).
- Direct VS launch (§7) is assembled from known parts but untested.
- Is the VS replay (FDAT 211) the same as the arena engine's built-in replay, or its own code?
- Mission launch: which routine sets the mission id and returns.
