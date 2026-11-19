/* Master of Arena: the hold-START menu of a network fight (versus rooms
 * only). See moa_netplay_menu.c. */
#ifndef MOA_NETPLAY_MENU_H
#define MOA_NETPLAY_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The link engine's per-frame packet wait (moa_link_netplay.c), with both
 * players' pads of this frame (game layout, 1 = pressed; port 1 = host,
 * port 2 = guest). Reads the menu's codes, carries out an accepted restart
 * or stage change, and leaves the pads the fight should see (codes and START
 * taken out). Guest memory changes only here, the same frame on both PCs. */
void moa_np_menu_pads(uint16_t *port1, uint16_t *port2);

/* The same wait, after the remote pad is in place: at CONTINUE? the answer
 * both players give (between rounds X, the next round; once the set is
 * decided nothing, the room takes both PCs back). */
void moa_np_menu_continue(void);

#ifdef __cplusplus
}
#endif

#endif
