# OP-WLINK — custom option part (weapon link)

Status: **first test build written 2026-11-17 (branch `op-wlink`), not yet tested in game.**
First custom option part for Master of Arena. Code: `psxrecomp/runtime/src/mod_builtin_moa_wlink.c`,
turned on by the preloaded mod `acrecomp.moa.op-wlink` (launcher Mods, "OP-WLINK option part").

## What it does

With OP-WLINK fitted, weapons of the same family fire together from one press of fire.
The goal is to open up builds and make weapon arms more viable, while leaving room to
experiment.

## Decisions (user, 2026-11-17)

| Topic | Decision |
|---|---|
| Name | `OP-WLINK` |
| Option ID | **13**: the first free bit in the fitted mask `0x8003EA70` (bits 0–12 are used, 13–15 are free; see `../mapping/parts-tables.md`, "Room for more optional parts") |
| Getting it | Owned from the start in every save, for testing. Costs **1 option slot** |
| Look (placeholder) | Copy an existing option's record and display, with a different or wrong texture so it stands out. Replace it with real art later |
| Families that link | **Missiles** (any missile type with any other), **rockets**, and **back cannons** (back L + back R only, because no arm or hand weapon is a cannon) |
| Never links | **Hand weapons** never link. **Solid guns** don't link either (too strong). Energy weapons are not linked for now |
| Weapon arms | **Can** link with back weapons of the same family (e.g. missile arms + back missiles) |
| Shared (averaged) | **Lock** settings (lock range, lock box, lock value) and **fire interval** ("RELOAD TIME" in the garage), averaged across the group. The group locks as one |
| Kept per weapon | Damage, impact, ammo, energy per shot, projectile. Ammo is **not** synced: one weapon can run dry first |
| △ cycling | A linked group is **one entry**: △ skips the linked partners. The selected weapon is still one of the group |
| HUD | One line per linked weapon, stacked in the **game's own HUD font**: the **weapon type** the game already shows (DUAL MISSILE, MULTI MISSILE…, compiled slot +42), **not** the part name, plus its ammo. When more than one weapon is linked, each line also shows its **slot**: `ARM`, `BACK L` or `BACK R` |
| Running dry | An empty weapon **drops out** of the group. Selection moves to a member that still has ammo (e.g. arms empty → back missiles selectable), and the rest keep firing together |
| Back cannons | Keep their own rules: two-legged ACs still crouch to fire them |
| First build | **Split-screen VS only** (VS engine, FDAT 209). Correction: netplay runs the link versus engine (FDAT 208) and AI Battle the arena engine (FDAT 210), so they are **not** covered yet; the fire code there is the same at other addresses, so they're next. Missions and replays later |

## How families are told apart

From the weapon detail record (`../mapping/parts-tables.md`) as copied into the compiled weapon slots
(`../mapping/stat-block.md`, slot +0 kind, slot +42 HUD type name from detail +7):

| Family | Rule |
|---|---|
| Missiles | Kind 1. Type names 3 DUAL MISSILE, 13 SMALL, 14 MISSILE, 15 LARGE, 16 MULTI, 17 TRIPLE, 28 SPECIAL MISSILE |
| Rockets | Type names 19 SMALL ROCKET, 20 ROCKET, 21 LARGE ROCKET, 30 SPECIAL ROCKET |
| Back cannons | Kind 2, back L and back R slots only |
| Excluded | Everything else, including dispensers (18, 29, 31) and every hand weapon (slot 0 when it holds a hand weapon) |

Slot 0 counts only when it holds **weapon arms** (arms ID 12+).

## Averaging (per linked group)

Written into each member's compiled slot at match start (the VS engine restores the player
blocks when it exits, so nothing leaks back into the save):

- slot +20 lock range, +24…+30 lock box, +32 lock value, +34 locks at once: mean of the group,
  rounded down, kept within the game's limits (lock value 1–255, locks at once ≥ 1)
- slot +35 fire interval: mean of the group, rounded down

## How the test build does it

**Menu (FDAT 206)**

- Option ID 13 is one of six `DUMMY` records already in the table (`0x800D1CDC`); the menus
  already loop over IDs 0–18. The plugin keeps its own copy of the 19-record table with ID 13
  as OP-WLINK (name `OP-WLINK`, type `WEAPON LINKER`, 24,000 credits, 1 slot), and the ten
  places that load the table address (`lui 0x800D` / `addiu 0x1AA0`) are given the copy.
  Bit 13 of the owned-options word `0x8004B558` is set just before the option list reads it
  (`0x80091A1C`).
- **Never write into the loaded menu module.** The first test build wrote the record straight
  into it from a VBlank callback, and the game hung on its first loading screen (fixed by
  turning the mod off, 2026-11-17). The runtime watches overlay memory for code changes.
- Description: the 13-entry pointer table `0x800CDE8C` is read at `0x80092004`; for ID 13 the
  read is pointed at "Fires same-type weapons together."
- After `compile(player)` (`0x800A7BB4`) the plugin marks each weapon slot in its spare bytes:
  +59 family (0 = not linked), +60 `0xA0` + slot (0 ARM, 1 HAND, 2 BACK L, 3 BACK R) when
  OP-WLINK is fitted, +61 group size. +58 stays 0 because it ends the HUD name. Slot order
  follows compile: weapon arms or hand weapon, back L, back R (radars, M118-TD and P77-ST
  skipped). Linked slots get the averages of slot +20, +24…+30, +32, +34, +35, and +9 for
  missiles. These marks travel inside the compiled block, so other engines can use them later.

**VS engine (FDAT 209)**

- Fire: the game calls the selected slot's fire routine (a0 = AC, a1 = slot). The plugin runs it,
  then each partner's routine with that partner's own burst (AC +0x47) and cooldown (AC +0x48)
  swapped in and the post-shot hold cleared. Afterwards the whole AC object and the lock state
  (player block +0x1A…+0x2F, +0x78…+0x1FB) are put back, so partner shots don't move the AC,
  play animations or speed up the lock. A partner missile salvo keeps its own copy of the locks
  it started with until it has fired. Partner state is kept in plugin guest memory (works with
  save states and rollback).
- △: at `0x80083030` the pick skips the selected weapon's partners (and empty weapons, as the
  game already does). If the selected weapon is empty, the pick moves to a partner with ammo.
- HUD: the game already lists every usable weapon with its own ammo (picked weapon on top, the
  rest below). For linked weapons the name line becomes "<type> ARM / BACK L / BACK R", moved
  left so it ends where the game's name ended.

## Things to check in the first test

- OP-WLINK shows in OPTIONAL PARTS, with its description, and fits for 1 slot.
- Missile arms + back missile(s), rockets, and two back cannons fire together; a hand weapon
  and solid guns don't link.
- △ skips partners; an empty weapon drops out and the selection moves on.
- HUD names and slot tags fit and line up in split screen.
- Linked missiles lock together and fire at the same targets.

## Still open

- Placeholder picture: the option list draws text only (name from the record), so for now
  OP-WLINK looks like any other option apart from its name. If a picture shows elsewhere
  (shop or AC SETUP), it will show whatever ID 13's DUMMY slot points to.
- Which VS AC SETUP list uses the owned word `0x8004B558` (the scenario garage does).
- Cannons with a post-shot hold (WC-GN230, WC-IR24): the partner ignores the hold, so it may
  fire slightly out of step.
- Netplay (FDAT 208) and AI Battle (FDAT 210) engines.
