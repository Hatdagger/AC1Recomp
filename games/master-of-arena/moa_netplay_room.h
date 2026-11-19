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

/* For the hold-START menu (moa_netplay_menu.c). The room's stages: count,
 * the id (20..39) and name of entry i. The stage the set is on now (player
 * 2's block +0x255, guest memory). Writing one into both blocks (guest
 * memory: only from a hook the guest's code runs). Whether the set is
 * decided (the win table against the rounds; the same on both PCs). A
 * pilot's name for this PC's screen (slot 0 host, 1 guest). Leave the match
 * for the room now (both PCs go back). */
int moa_np_room_stage_count(void);
int moa_np_room_stage_at(int i, const char **name);
int moa_np_room_stage_now(void);
void moa_np_room_set_stage(int id);
int moa_np_room_set_over(void);
const char *moa_np_room_pilot(int slot);
void moa_np_room_leave(void);

/* Any other moment the start is busy (menus, disc reads): the VS screen's
 * own redraw (a few times a second). Presentation only. */
void moa_np_room_tick(void);

#ifdef __cplusplus
}
#endif

#endif
