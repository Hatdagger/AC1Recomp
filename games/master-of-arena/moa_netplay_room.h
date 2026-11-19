/* Master of Arena: the launcher's versus room, in the game (netplay only).
 * See moa_netplay_room.c. */
#ifndef MOA_NETPLAY_ROOM_H
#define MOA_NETPLAY_ROOM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* This match came from the launcher's versus room (its settled options are
 * in PSX_NET_GAME_OPTS). Everything below does nothing otherwise. */
int moa_np_room_active(void);

/* The session's first title (moa_vs_direct.c), before anything else of the
 * match: what an earlier match in this process left here is forgotten. */
void moa_np_room_session_start(void);

/* The boot's title became the versus menu (moa_vs_direct.c): the room's time
 * limit goes into guest memory before VS START compiles the players, and the
 * VS screen goes up over the rest of the start. */
void moa_np_room_title(void);

/* The link engine starts a match (its role answer / AC data exchange): the
 * room's stage into both player blocks. Guest memory, the same instant on
 * both PCs. */
void moa_np_room_link_start(void);

/* Every frame of a fight (the link engine's packet). */
void moa_np_room_fight_frame(void);

/* The exe's loading screen before an engine: between rounds of a set. */
void moa_np_room_engine_load(void);

/* The versus menu opens again after a match. */
void moa_np_room_menu_back(void);

/* Any other moment the start is busy (menus, disc reads): the VS screen's
 * own redraw (a few times a second). Presentation only. */
void moa_np_room_tick(void);

#ifdef __cplusplus
}
#endif

#endif
