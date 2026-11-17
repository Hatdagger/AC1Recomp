# Master of Arena — The scenario hub (the icon ring)

Lane: **hub chat** (see `README.md`). Disc: USA disc 1 (SLUS-01030). Read statically from
FDAT 206 (menu module, loads at `0x80050DD8`), `MS\MENU_TMD.T` and the boot exe, 2026-11-17.
Addresses are measured from the code unless marked `?`. File offset in 206 = address − `0x80050DD8`.
`tools/disc_entry.py` pulls any archive entry off the disc (FDAT 206, MENU_TMD 0, …).

## 1. What the hub is

The hub is a **ring of 8 icons** that turns 45° per press. Every way into Scenario ends at
`0x800552C0` (see `mode-launch.md`), which creates the hub task. The ring always has exactly
8 entries; nothing is locked or hidden at this level (a screen may still refuse you once it opens,
e.g. "Arena license suspended.").

| # | Entry | Opens (table `0x80050ED8`) | Confirmed by |
|---|---|---|---|
| 0 | Garage | `0x8007C6B0(page)` — page 0, or **6 = AC TEST** when back from a test sortie (`0x8003EA7B`) | strings (ASSEMBLY … RANKER TEST), arena-run test 3 log |
| 1 | Shop | `0x800990AC()`, then sets `0x800CE644 = 1` | strings (Buy/Sell Part?) , test log |
| 2 | Missions | `0x800943B4()` | strings (REQUEST, REWARD), test log |
| 3 | System | `0x80059554(hub, 0)` | strings (DATA SAVE, MISSION/ARENA REPORT, EXIT GAME?), test log |
| 4 | Mail | `0x800A273C()` | strings (SUBJECT, READ), test log |
| 5 | Arena | `0x800BA64C(0)` → arena 0, lock byte `0x8004B686` | `moa_arena_run_boot.c` |
| 6 | Ex Arena | `0x800BA64C(1)` → list from `0x8003EAD4` (≥ 6 = Ex arenas) | code, test log |
| 7 | Arena Maker | `0x800C5580()` | strings (ARENA NAME, EDIT, "Changing AC rankings…"), test log |

The order of the ring = the entry number; the ring wraps 7 ↔ 0.

## 2. The hub task (`0x800552C0`)

Created with `0x800575B8(0)`; callbacks: +0x74 init (`0x800552C0`), **+0x78 per-frame state** (swapped
as the hub moves on), +0x7C `0x80057C24`. Fields used:

| Offset | Meaning |
|---|---|
| +0x80 | ring backdrop object (`0x80056564(…)`) |
| **+0x84 … +0xA0** | the **8 icon holders**, one word each (entry 0 … 7). Full: +0xA4 is the next field |
| +0xA4 | the open entry's title banner task (`0x80057068`) |
| +0xA8 | a sprite from `0x8006527C(0, 0x800CB720, 134, 172, 2)`, shown only for some entries (6–7, or 2/4/5 when `0x80044121` is set) `?` meaning |
| +0xAC | a task whose byte +1 is set when an entry opens `?` (writer not found yet) |
| +0xB0 / +0xB4 | two sprites at x 210 / 95, y 132 (`0x800647B8`, records `0x800CE66C` / `0x800CE660`), hidden while turning — the side arrows `?` |
| **+0xBE** | **selected entry** (signed byte 0–7) |
| +0xBF | turn direction (+32 / −32) |
| +0xC0 | turn frame (0–15) |

**Where the cursor starts** (init, `0x8005531C`): if an arena fight just finished (`0x8003EA7C`) →
5 or 6 (`0x8003EAD4 != 0` → 6); else if `0x8003EA7D == 5` (back from a ranker test) → 7; else 0.

## 3. The states, in order

| State (+0x78) | What it does |
|---|---|
| `0x800554F4` | wait for the backdrop |
| `0x80055548` | **build the ring**: 8 × `0x80056744(2, n, selected)` for n = 1…8 → +0x84…+0xA0; arrows |
| `0x800556A0` | wait for the icons; caption `0x80056FAC(selected)`; set up the backdrop |
| `0x80055798` | first visit with unread mail: "You have new mail." (once, `0x8003EA82`) |
| **`0x80055848`** | **input**. Pad words at `0x801B1CB8` (+0x18 / +0x1C). ✕ (`0x4000`) = open. Also opened *without* a press when coming back from AC TEST (`0x8003EA7B`), an arena fight (`0x8003EA7C`) or a ranker test (`0x8003EA7D == 5`), so you land back inside that screen. Pad `0x80` → selected + 1, `0x20` → selected − 1 (`?` which is left on screen), with wrap at 8 / −1 |
| `0x80055EEC` | **turn the ring**: 16 frames, each icon's angle (+0x46) ± step table `0x800CB72C` (16 halfwords, total **512 = 45°**); the selected icon slides forward (+0x44 += 55) |
| `0x8005623C` … `0x800563B4` | entry opened: title banner, caption slides to the corner, wait for the disc queue (`0x800589DC`) |
| **`0x80056414`** | **open the entry**: jump table `0x80050ED8` by +0xBE (bounds check `sltiu 8` at `0x80056438`) |
| `0x80056528` | **back to the hub**: called when a screen closes; finds the hub task, puts it back to `0x80055548` |

**Checks before an entry opens** (still in `0x80055848`, table `0x80050EB8`, bound at `0x800559CC`):
entry 4 (Mail) with no mail at all (32 flag bytes at `0x801A4F98`, bit 0) and no new mail →
"You have no mail." and the ring stays. Entry 0 (Garage) copies 256 bytes `0x80036FEC → 0x8003EB20`,
calls `0x80090FD8`, and refuses with " You don't have a CORE part." when the core slot is empty.
Entry 1 (Shop) runs `0x8006354C` then `0x80099048`. Entries 2–7 go straight on (`0x80055B78`).

## 4. What each icon is made of

| Piece | Where | Per entry |
|---|---|---|
| **3D icon** | `MS\MENU_TMD.T` **entry 0** (12 KB TMD, 9 objects), loaded into model slot 0 by the menu's setup (`0x80057ED4(0,0)` at `0x800A6B24`). Object 0 = flat quad (floor), **objects 1–8 = the 8 icons** (30 vertices each) | object = entry + 1 |
| Icon task | `0x80056BC8`: draw kind 0 (+0x0A), **model object +0x0C**, model slot +0x0E = 0 (drawn by `0x800A7698` → `0x8005F878`) | |
| Ring position | `0x80056744`: switch table `0x80050EF8` (15 cases, offset −7…+7 from the selected entry); each case hard-codes angle 512 × k and x / z | |
| **Title banner** (shown when opened) | 12-byte sprite records at **`0x800CB660`** (u, v, tpage, w 96, h 48, clut) | record = entry |
| **Caption** (under the ring) | 12-byte sprite records at **`0x800CB6C0`** (w 128, h 24) | record = entry |
| Pictures in VRAM | `MS\MENU_TIM.T` entries 1 and 4, loaded by `0x80056DBC` (the backdrop); `MENU_TMD` entry 1 → slot 15 (backdrop model) | |

### Where the pictures live (found 2026-11-17)

**Icon art = title-banner art.** Each icon model is a set of textured quads on texture page
`0x00A5` (VRAM x 320, y 0, 8-bit) with palette `0x7800` (VRAM 0, 480), and its UVs fall inside the
same 96 × 48 picture its title banner shows. All eight are 96 × 48, 8-bit TIMs in
**`MS\MENU_TIM.T` entry 1** (16 TIMs; the six icons + two more, plus other menu art):

| Entry | Banner record → VRAM | TIM in MENU_TIM 1 (0-based, file offset) |
|---|---|---|
| 0 Garage | (320, 48) | #0, 0x00000 |
| 1 Shop | (320, 144) | #5, 0x064A0 |
| 2 Missions | (320, 96) | #2, 0x02840 |
| 3 System | (368, 48) | #4, 0x05080 |
| 4 Mail | (320, 0) | #1, 0x01420 |
| 5 Arena | (320, 192) | #3, 0x03C60 |
| 6 Ex Arena | (368, 0) | #13, 0x1FDBC |
| 7 Arena Maker | (384, 192), page `0x00A6` | #14, 0x2121C |

**Captions** (the 128 × 24 name under the ring): one 4-bit sheet, page `0x0027` (VRAM 448, 0),
palette `0x3817` (VRAM 368, 224) = **`MS\MENU_TIM.T` entry 0, TIM #1** (256 × 216 px, file offset
0x2620). Records at `0x800CB6C0` give each caption's u, v: 0 (0, 144), 1 (0, 168), 2 (0, 120),
3 (128, 168), 4 (128, 120), 5 (128, 144), 6 (128, 192), 7 (0, 192).

All three files (MENU_TIM 0, 1 and 4) are put into VRAM by the title's SCENARIO row
(`0x80057CB0(4, 0 / 1 / 4)`, `0x800AB5F4`) and again by the hub backdrop (`0x80056DBC`, 1 and 4).
So a new icon look = replace a 96 × 48 TIM in entry 1 (same size, same palette, or give it its
own palette row); a new name = redraw a 128 × 24 box on the caption sheet. Both can be done by
writing VRAM after those loads instead of editing the disc file.

## 5. Adding, removing or replacing entries

Everything that knows "8": the 8 builder calls (`0x80055548`), the 8 holder words (+0x84…+0xA0,
task full after that), wraps (`0x80055D00` `8`, `0x80055D68` `7`), loops (`slti 8` at `0x80055C38`,
`0x80055DF8`, `0x80056008`, `0x800562A8`), both jump tables (`0x80050EB8`, `0x80050ED8`, 8 words),
the position switch (`0x80050EF8`, 512 per step), the turn table (sum 512), the 8 banner and 8
caption records (packed back to back: no room to grow in place), and 8 icon objects in MENU_TMD 0.

**Replace an entry — easy.** Hook `0x80056414` (the pick; `moa_arena_run_boot.c` already filters it
for arena runs, so share that hook) and run your own screen for entry N. Your screen must call
`0x80056528` when it closes. For entries 0 / 1 also skip the checks in `0x80050EB8` (point the word
at `0x80055B78`). New caption / banner: point the 12-byte records at a new picture in spare VRAM.
The 3D icon can stay as is or be swapped in MENU_TMD 0 later.

**Remove an entry — easy if "greyed out", hard if "gone".** Blocking it is one hook (what arena
runs already do: the pick falls through to the default, the ring stays). Taking it off the ring means a
7-icon ring: 4096 / 7 isn't a whole number, so the position switch, the turn table and every "8"
above need new values — doable, but a full rework for one icon less.

**Add entries — two ways.**
1. **Folder entry (recommended).** Keep 8 icons; one icon opens a small pick list in the game's own
   window (like the mission picker), and each line opens a screen. Example: **Arena** asks Arena /
   Ex Arena / Phantasma / …, which frees icon 6 for something new. Only the replace-hook above is
   needed, plus the existing picker code.
2. **Bigger ring.** Rewrite the ring in C (build, input, turn: `0x80055548`, `0x80055848`,
   `0x80055EEC`) with N icons, holders kept in mod memory, new step size 4096 / N, new
   banner / caption records, and extra icon models. Real work, and every icon needs art.
