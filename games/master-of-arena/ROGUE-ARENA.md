# Rogue Arena: list fix, 49 + 1 plan, direct boot status (2026-11-17)

Repo: github.com/Hatdagger/AC1Recomp. Mapping notes live on master in docs/mapping/
(read README.md first; this chat owns the "Menus: scenario hub" lane, menus-hub.md).
Tools: tools/mips_dis.py (disassembler), tools/disc_entry.py (dump any archive entry,
e.g. FDAT 206 = menu module at 0x80050DD8). The user attaches their MoA disc 1 zip.

## Done
- Hub ring fully mapped (menus-hub.md): 8 entries, open table 0x80050ED8, checks 0x80050EB8,
  icon models MENU_TMD 0 objects 1-8, icon art = MENU_TIM 1 TIMs, captions = MENU_TIM 0 TIM #1.
- The game's LOAD: 0x80073424, three paths by type letter (README, corrected). MoA's own save ('N'):
  checksum 0x80070F1C(buf+432) == word buf+384, INLINE unpack 0x800735E0-0x80073B78, emblem
  LoadImage from buf+1152 / palette buf+3200, then the six setup calls. 0x80072E54 / 0x80070F70
  are the original-AC1 import path only.
- Branch `rogue-direct` (1 commit, patch included here, NOT built/tested): Rogue Arena boots
  straight to the Arena list — title-open filter loads the session card's save with the
  game's own unpack, opens the hub on entry 5 and presses X once. Falls back to the old
  menu presses on any failed check; PSX_MOA_RUN_DIRECT=0 forces the old route.
  Pushing a branch does not start a build (only master / moa-launcher / tags / manual).
  BUG (found after the push): it uses the AC1 import checksum + unpack, so on a real run card
  the checksum never matches and it always falls back to the old menu presses (safe, no gain).
  FIX: keep everything else, but check with 0x80070F1C(buf+432) vs buf+384 and replace the
  0x80072E54 call with the 'N' path's copies done in C (transcribe 0x800735E0-0x80073B78 into an
  offset -> RAM table; another chat is mapping the save format, check README first), and use
  buf+1152 / buf+3200 for the emblem upload.

## Rogue Arena list problems: diagnosis
Arena screen state (base = state + 0x80): +0x5C page (0 ranked, 1 sub arena), +0x5D arena,
+0x5F count, +0x60 rows, +0x68 cursor, +0x69 scroll, +0x40 row buffer (12 B/row),
+0x44 first row (= +0x40, or +180 on the sub page; set at 0x800BBC6C from +0x5C).
- Open (0x800BBA38): arena 0 only — rank < 16 → page 0, 15 rows; else page 1, 15 or 34 rows
  (0x8003EAC9). Other arenas: rows = count, one page.
- Page flip L1/R1 at 0x800BD308: arena 0 only (`lbu v0,93(s1)` / `bne v0,zero,0x800BD474`
  at 0x800BD310 skips it for every other arena).
- Rogue Arena uses arena 0 with MOA_RUN_LADDER = 15 (ranked page only). R1 still flips to the
  sub page (entries 15-49), which the run never fills, while its hooks force ranked rules
  there → likely the hang.
- Names/builds/AI are patched after the disc read (run_queue_fix → run_apply_fixes at the
  CD-busy poll 0x800589DC). Queue holds 4; a fix keeps the buffer address it saw; when full,
  new reads are refused → stock names ("list doesn't populate"); a stale fix can write into
  reused memory → possible hang.

## Plan agreed so far (awaiting go-ahead)
1. One 50-row list in arena 0, Phantasma style (49 opponents + player at the bottom, all
   visible, climb one at a time): open hook rows = 50 / page 0; hook 0x800BD310 so arena 0
   takes the "no flip" branch during a run; the existing 50-rank hooks (subfight, ranked,
   cursor, clear) already cover the rules.
2. Race-free data: at the 4 read sites (0x800BBB3C names, 0x800BBCFC rewards, 0x800C0250
   builds, 0x800C04FC AI; jal 0x800160C4, no callback) build the run's entry in C from the
   disc (psx_mod_read_disc_sectors) and write it straight into the game's buffer, and point
   a3 at a scratch buffer so the real read lands there. No queue, no stale pointers.
3. Run model: MOA_RUN_LADDER 15 → 50 (19 uses), `beaten` 32 → 64 bits, run-file version bump,
   launcher lineup picks 49.
Open question for the user: a run of 49 fights is long — keep 49, or 49 rows with fewer fights?
