# OP-WLINK — custom option part (weapon link)

Status: **spec agreed 2026-11-17, not built yet.** First custom option part for Master of Arena.

## What it does

With OP-WLINK fitted, weapons of the same family fire together from one press of fire.
The goal is to open up builds and make weapon arms more viable, while leaving room to
experiment.

## Decisions (user, 2026-11-17)

| Topic | Decision |
|---|---|
| Name | `OP-WLINK` |
| Option ID | **13**: the first free bit in the fitted mask `0x8003EA70` (bits 0–12 are used, 13–15 are free; see `parts-tables.md`, "Room for more optional parts") |
| Getting it | Owned from the start in every save, for testing. Costs **1 option slot** |
| Look (placeholder) | Copy an existing option's record and display, with a different or wrong texture so it stands out. Replace it with real art later |
| Families that link | **Missiles** (any missile type with any other), **rockets**, and **back cannons** (back L + back R only, because no arm or hand weapon is a cannon) |
| Never links | **Hand weapons** never link. **Solid guns** don't link either (too strong). Energy weapons are not linked for now |
| Weapon arms | **Can** link with back weapons of the same family (e.g. missile arms + back missiles) |
| Shared (averaged) | **Lock** settings (lock range, lock box, lock value) and **fire interval** ("RELOAD TIME" in the garage), averaged across the group. The group locks as one |
| Kept per weapon | Damage, impact, ammo, energy per shot, projectile. Ammo is **not** synced: one weapon can run dry first |
| △ cycling | A linked group is **one entry**: △ skips the linked partners. The selected weapon is still one of the group |
| HUD | Show **each linked part's name and ammo** stacked in the **game's own HUD font** |
| Running dry | An empty weapon **drops out** of the group. Selection moves to a member that still has ammo (e.g. arms empty → back missiles selectable), and the rest keep firing together |
| Back cannons | Keep their own rules: two-legged ACs still crouch to fire them |
| First build | **Split-screen VS only** (VS engine, FDAT 209). That also covers netplay and AI Battle. Arena, missions and replays come later |

## How families are told apart

From the weapon detail record (`parts-tables.md`) as copied into the compiled weapon slots
(`stat-block.md`, slot +0 kind, slot +42 HUD type name from detail +7):

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

## Build plan

1. **Get the overlay code.** FDAT 206 (menu) and 209 (VS) from the user's disc 1
   (`tools/disc_entry.py "Disc 1.bin" GG/COM/FDAT.T 206 menu206.bin`, same for 209). They're
   needed to read the fire dispatch, the refire cooldown and the HUD weapon text routine.
2. **Menu (FDAT 206).** The table at `0x800D1AA0` has only 13 records, and ID 13 would read the
   weapon-arms table. Put a 14-record copy in plugin memory and point the option-list reads
   (`0x80091A5C`, around `0x80091DF0`, `0x8009241C`) at it. Set inventory option byte 13 to
   owned. Add the description text.
3. **Match start** (after `0x8008342C`, once per AC). If mask bit 13 is set, find the groups
   and write the averaged values.
4. **Fire.** After the selected weapon's fire routine (gun `0x80080DF4`, missile
   `0x80081888`, cannon `0x80081B18`), run each partner's routine on the same lock records,
   holding the refire cooldown so the first shot doesn't block the others. Skip members with
   no ammo.
5. **△ cycle** (`0x80082D90`, mask +0x296 → player block +0x11). Skip partners. When the
   selected member is empty, move to one with ammo.
6. **HUD.** Draw an extra name + ammo line per linked member in the game's font.
7. Test build on a branch, which the user reviews before it's merged.

## Open points

- Where the refire cooldown lives for guns and missiles (cannons use AC +0x48).
- Which firing animation plays when arms and back fire together (each fire routine writes
  AC +0x84; the last one wins).
- Whether the option list shows a picture for options, and so what the placeholder texture
  replaces.
