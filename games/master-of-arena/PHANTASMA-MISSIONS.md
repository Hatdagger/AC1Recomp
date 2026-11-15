# Project Phantasma missions in Master of Arena (notes, 2026-11-13)

Built from the player's discs by `moa_phantasma.c` (EXTRAS -> "Install data from Project
Phantasma", package `community.moa.phantasma-arena` 2.0.0, features `arena` + `missions`);
runtime plugin `moa.phantasma-missions` in `psxrecomp/runtime/src/mod_builtin_moa_phantasma.c`.

## Pack (disc LBA 286200, 10455 sectors, 4 blocks per mission)
- textures = Phantasma RTIM.T entry m (verbatim)
- common = MoA FDAT 205 bytes [0, 237388) + Phantasma FDAT 200 message chunks (from 241152) with
  control codes `X<n>` and `C0` removed, `\x9a`/`\x9b` -> `\xf0\x81`/`\xf0\x82`, small rewraps
- code = Phantasma FDAT 5m relocated: Phantasma module base 0x80097A38 -> MoA 0x800A0DD0
  (delta 0x9398), engine calls/data mapped to MoA (fn map ~180, data map ~70), object struct
  field offsets remapped (context dependent: the old converter typed registers)
- map = 84-byte MoA header (80, 19, 4 x19) + Phantasma FDAT 5m+4 without its checksum
- every block ends with sum(words)+0x12345678
- `moa_phantasma_missions.inc` = zlib recipe (copy ranges + XOR corrections) reproducing the
  user's MoA-PhantasmaMissionTest 3.2.0 pack byte for byte (sha256 e43ccd36...), then
  `k_ppm_fixes` (14 words the old port left unrelocated, missions 3,5,6,8,13,15,16).

## MIS.T copy (LBA 296656): stock MoA MIS.T (409 entries, 460 sectors) + Phantasma MIS.T whole
(header at sector 460). Plugin index: entry 410+k = Phantasma entry k.

## Mission screen (menu module = FDAT 206, base 0x80050DD8)
- table T = 0x8019F7A8: +2 chapter, +4 mode (1 = story), +20 flags[24] (0x02 done), +56 list[24],
  +80 = copy of MIS.T entry 0 (468 words) -> +80 windows[24] (lo,hi,0,ff), +176 names[24] x32,
  +1040 rewards u32[24], +1136 engine mission no.[24], +1160 ?[24],
  +1184 page records 32 bytes/slot (8 image entries-1, 8 text entries-1).
  Phantasma entry 0 has the same tables for 64 slots: names 256, rewards 2560, pages 2944.
- 0x80094150 copies entry 0 to T+80 (hook after: 0x8009417C); list builder 0x800942bc
  (returns at 0x800943AC); rows filled by ~0x80095100 from T+176 (drawn once per build).
- MoA uses slots 0-19 (19 = Final Mission); 20-23 are "Dummy". Slot 20 = rolling PP mission.
- Font has no `{4` (Phantasma narrow space). Row fits ~23 chars before the reward.
- Engine (FDAT 207): loads 800556b8 textures (file 1, a1 = slot), 800556d4 common (205),
  80055874 code (slot*5, dest 0x800A0DD0), 80055894 map (slot*5+4). File table 0x8004402C.

## Headless smoke test (35 s each, after fixes)
in mission: 2,3,4,5,6,8,10,12,17; intro scene: 13,15,16; wrong view: 11;
black/hang: 1 (spins at 0x80098DA0 = engine data, i.e. a bad indirect jump), 7, 9, 14.

## TODO
- missions 1, 7, 9, 14 (hang), 11 (view); replay list after all 17 (needs a separate page:
  the list's 24 slots are fixed); progress is per memory card (all saves share it).
