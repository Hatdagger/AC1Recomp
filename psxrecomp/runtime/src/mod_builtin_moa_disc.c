/*
 * Armored Core: Master of Arena - single-disc play.
 *
 * Master of Arena ships on two discs (SLUS-01030 / SLUS-01081) whose data is
 * identical except for the one-byte disc id in \GG\COM\CD.DAT. The game keeps
 * the id of the inserted disc in a global byte (0x80044121). A small state
 * function in the disc-loaded code module compares that byte with the disc the
 * current task wants and shows the "insert DISC n" prompt on a mismatch:
 *
 *   800c4d30  lbu  v1,0x4121(v0)        ; v1 = inserted disc id
 *   800c4d4c  lbu  v0,8(s0)             ; v0 = wanted disc id
 *   800c4d50  sb   v1,9(s0)             ; task.found = v1       <- hooked
 *   800c4d58  bne  v1,v0,mismatch
 *
 * This plugin runs just before the `sb` and makes the game see the wanted disc
 * as inserted: it updates the global disc byte and $v1. No game or disc byte is
 * modified, so integrity checks on the loaded module keep passing. The hook
 * works for interpreted (disc-loaded) code, and is guarded by exact
 * instruction words at the hooked site and around it, so it cannot fire on
 * other code that happens to share the address.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#define MOA_HOOK_PC        0x800C4D50u
#define MOA_HOOK_INSN      0xA2030009u /* sb v1,9(s0)  */
#define MOA_GUARD_PC_A     0x800C4D2Cu
#define MOA_GUARD_INSN_A   0x3C028004u /* lui v0,0x8004 */
#define MOA_GUARD_PC_B     0x800C4D30u
#define MOA_GUARD_INSN_B   0x90434121u /* lbu v1,0x4121(v0) */
#define MOA_GUARD_PC_C     0x800C4D4Cu
#define MOA_GUARD_INSN_C   0x92020008u /* lbu v0,8(s0) */
#define MOA_DISC_ID_ADDR   0x80044121u
#define MOA_REG_V1         3
#define MOA_REG_S0         16

static void moa_adopt_wanted_disc(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (psx_mod_read_word(MOA_GUARD_PC_A) != MOA_GUARD_INSN_A ||
        psx_mod_read_word(MOA_GUARD_PC_B) != MOA_GUARD_INSN_B ||
        psx_mod_read_word(MOA_GUARD_PC_C) != MOA_GUARD_INSN_C) {
        psx_mod_counter_add("moa.single-disc.guard_rejected", 1);
        return;
    }
    const uint32_t task = cpu->gpr[MOA_REG_S0];
    const uint8_t wanted = psx_mod_read_byte(task + 8u);
    if (wanted > 1u) {
        psx_mod_counter_add("moa.single-disc.bad_wanted", 1);
        return;
    }
    const uint8_t inserted = psx_mod_read_byte(MOA_DISC_ID_ADDR);
    if (inserted == wanted) return; /* right disc: stock behavior */
    psx_mod_write_byte(MOA_DISC_ID_ADDR, wanted);
    cpu->gpr[MOA_REG_V1] = wanted;
    psx_mod_counter_add("moa.single-disc.adopted", 1);
}

static void moa_single_disc_activate(void) {
    /* The instruction table is flattened from the registered hooks when the
     * plan activates this plugin id; nothing else to do here. */
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_single_disc_plugin) {
    (void)psx_mod_register_activation_plugin(
        "moa.single-disc", moa_single_disc_activate);
    (void)psx_mod_register_instruction_plugin(
        "moa.single-disc", MOA_HOOK_PC, MOA_HOOK_INSN, moa_adopt_wanted_disc);
}
