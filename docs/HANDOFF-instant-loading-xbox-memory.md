# Hand-off: instant loading and the Xbox-class memory budget

Two pieces of work on this fork, built and pushed but **not yet tested by the
user on real hardware**. Branches (each rebased onto `moa-launcher` 0.8.1):

| Branch | Contains |
| --- | --- |
| `moa-instant-loading` | instant loading + the function-replacement runtime fix |
| `moa-xbox-memory` | everything above + the memory budget |

Rules for this fork: never push to NeonLightning upstream; the user reviews
changes before a build goes out; nothing derived from the discs goes in the
repo; the runtime forbids printf/log debugging (use the debug server and
counters); commits as Hatdagger <surreal64@hatdag.com>.

## 1. Instant loading (`moa-instant-loading`, commit "Master of Arena: instant loading")

**What:** game data loads straight from the disc image instead of through the
emulated CD drive, so loading lasts only as long as the game's unpacking.

**How:** every archive read in Master of Arena goes through one boot-EXE
routine, `0x800167A0` read_sectors(CdlLOC*, dest, count): Setloc + CdRead +
CdReadSync with 3 retries, returns 1. A function-filter plugin completes that
call itself:

- reads the sectors from the mounted, effective disc image (installed mods
  such as the Phantasma content apply) with the new runtime API
  `psx_mod_read_disc_sectors(lba, count, buf)` (`mod_runtime.cpp`);
- delivers them to RAM through the CD-DMA path, `psx_mod_dma_write_ram`
  (overlay capture and code-page invalidation included);
- sets $v0 = 1 and returns, without advancing guest time.

It steps aside (runs the original body) whenever the drive is in use: the
game's async CD queue at `0x80043C70+0x378/0x37C` is not empty, XA or FMV is
streaming, or a data read is in flight. The plan's hooks are parked during
netplay, so online matches always use the original reads.

**Files:**
- `psxrecomp/runtime/src/mod_builtin_moa_loading.c`: the plugin (guards on
  the routine's instruction words; counters `moa.instant-loading.*`).
- `mods/preloaded/packages/acrecomp.moa.instant-loading/1.0.0/manifest.toml`:
  feature "Instant loading", on by default, toggle on the launcher MODS page.
- `games/master-of-arena/game.toml`: `0x800167A0` added to
  `mod_function_entry_funcs`; `generated/SLUS_010.30_full_01.c` regenerated
  with `psxrecomp-game` (only the two hook lines changed; built from
  `scripts/build_recompiler_tools.sh`, which lays tools-compat over the tree).

**Runtime fix that came with it** (commit "Runtime: implement native function
replacements (overlay ABI v32)"): the code generator emits
`psx_mod_try_function_replacement()` at every hooked entry, but the runtime
never defined it. The boot EXE failed to link, and compiled overlay shards
holding a hooked entry (menu, link-versus engine, arena engine) could not link
on Windows or load on Linux (dlopen RTLD_NOW), leaving that code interpreted.
Now implemented as `docs/MOD_FUNCTION_REPLACEMENTS.md` describes, forwarded to
overlay shards (`OverlayCallbacks`, ABI 30 -> 32, stale shards recompile).
This may also improve Steam Deck performance; worth measuring.

**To test:** build from the branch (GitHub Actions "Master of Arena builds",
Run workflow on the branch). The first launch recompiles the code cache.
Compare load times with the toggle on and off (boot to garage, arena match,
missions, Phantasma content); check music and voice after loads, saving, and a
LAN match. If menu music plays during loads, the plugin steps aside and the
gain is small; then consider letting it run alongside XA.

## 2. Xbox-class memory budget (`moa-xbox-memory`, commit "Xbox-class memory budget")

**What:** a 64 MiB target matching the original Xbox: 32 MiB guest memory,
24 MiB retained textures, 8 MiB audio. Full description:
`psxrecomp/docs/MEMORY_BUDGET.md`.

- Mod data memory moved from Expansion 1 (1 MiB) to `0x01000000-0x01FFFFFF`
  (16 MiB, KSEG0 `0x81000000`); with 8 MB main RAM and the 8 MiB GPU DMA
  aperture that is 32 MiB. GPU-reachable pools stay below the 24-bit tag limit.
- Texture banks capped at 24 MiB; SPU sample banks at 8 MiB total.
- Mod-memory savestate layout bumped to version 2.
- Tests updated (`test_mod_memory_snapshot.c`, `test_pgxp.cpp`).

Nothing in the game uses the new memory yet; it stays open bus until a mod
allocates. Suggested first use: move the Ex Arena opponent list into mod data
memory to lift the arena table size limit.

## Related

The user's new project, an original engine built from what was learned here:
https://github.com/Hatdagger/Armored-CoreR (its `docs/research/` holds the
Master of Arena findings: timing, movement, compile formulas, fitting rules).
