/*
 * Armored Core: Master of Arena - attract movie order.
 *
 * The title screen's attract loop (menu module) plays its two movies in
 * turn from a counter that starts at 0 on boot:
 *
 *   800AAFC8  lbu  a0,0x5698(s0)   ; counter: 0 = opening (ACOPA.STR),
 *                                  ;          1 = demo play (DEMOPLAY.STR)
 *   800AAFCC  sw   ra,0x18(sp)
 *   800AAFD0  jal  800544A0        ; play movie a0
 *
 * The opening plays first after every boot. With this feature the demo
 * movie plays first and the opening second; the loop still alternates.
 * Only the choice of movie changes: the counter in RAM is untouched.
 * The hook is on disc-loaded code and guarded by exact instruction words.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#define MOA_INTRO_HOOK_PC     0x800AAFCCu
#define MOA_INTRO_HOOK_INSN   0xAFBF0018u /* sw ra,0x18(sp) */
#define MOA_INTRO_LOAD_PC     0x800AAFC8u
#define MOA_INTRO_LOAD_INSN   0x92045698u /* lbu a0,0x5698(s0) */
#define MOA_INTRO_CALL_PC     0x800AAFD0u
#define MOA_INTRO_CALL_INSN   0x0C015128u /* jal 0x800544A0 */
#define MOA_REG_A0 4

static void moa_intro_swap(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (psx_mod_read_word(MOA_INTRO_LOAD_PC) != MOA_INTRO_LOAD_INSN ||
        psx_mod_read_word(MOA_INTRO_CALL_PC) != MOA_INTRO_CALL_INSN) {
        psx_mod_counter_add("moa.intro-order.guard_rejected", 1);
        return;
    }
    if (cpu->gpr[MOA_REG_A0] > 1u) return;
    cpu->gpr[MOA_REG_A0] ^= 1u;
    psx_mod_counter_add("moa.intro-order.swapped", 1);
}

PSX_MOD_CONSTRUCTOR(psx_register_moa_intro_order_plugin) {
    (void)psx_mod_register_instruction_plugin(
        "moa.intro-order", MOA_INTRO_HOOK_PC, MOA_INTRO_HOOK_INSN, moa_intro_swap);
}
