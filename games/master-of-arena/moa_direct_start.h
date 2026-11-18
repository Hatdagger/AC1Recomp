/* Master of Arena: the shared direct start (no title, no NEW/LOAD menu, no
 * card screens), used by the launcher's START GAME > LOAD GAME and by Rogue
 * Arena's FAST BOOT. Implemented in moa_quick_start.c. Mapped in
 * docs/mapping/boot-loads.md (§3 SCENARIO, §4a LOAD + the card screen's
 * finish) and menus-hub.md (the hub's input state).
 *
 * Call both from the title-open filter (0x800AAE94), in this order:
 *
 *   1. moa_direct_start_load(cpu, save)
 *      Checks the menu code in memory and the save (checksum, version), then
 *      does what SCENARIO + LOAD + the card screen's finish do: game state
 *      from the save, emblem to VRAM, the AC's part textures (waiting for the
 *      disc queue as the game does), the two hub colour strips the title
 *      would have kept, the back-weapon texture block. The title flags are
 *      cleared. 1 = done; 0 = nothing was changed (let the title open).
 *      Anything a mode adds to RAM (prices, run state ...) goes between 1
 *      and 2.
 *
 *   2. moa_direct_start_hub(cpu, entry, open)
 *      Opens the hub with the ring on `entry` (0 Garage ... 5 Arena, 6 Ex
 *      Arena, 7 Arena Maker). open = 1 also opens that entry on the hub's
 *      first input frame, through the hub's own ✕ path (0x80055848, an entry
 *      hook listed in game.toml). Returns the hub task (0 if not opened).
 */
#ifndef MOA_DIRECT_START_H
#define MOA_DIRECT_START_H

#include <stdint.h>

struct CPUState;

#define MOA_DIRECT_SAVE_BYTES 3712u   /* what LOAD reads (0xE80) */

int moa_direct_start_load(struct CPUState *cpu, const uint8_t save[MOA_DIRECT_SAVE_BYTES]);
uint32_t moa_direct_start_hub(struct CPUState *cpu, int entry, int open);

#endif
