# Master of Arena: versus over LAN or online

Two players, one on each PC, each with their own full-screen view. The match
is the game's link-cable versus mode with the cable replaced, so nothing is
emulated over the network except the two controllers.

## Playing

Both players need the same build and the same disc.

1. Open MULTIPLAYER in the launcher. One player hosts a room, the other
   joins. The host is player 1, the guest player 2.
2. In the VERSUS ROOM each pilot sets an AC in GARAGE: loaded from a Master
   of Arena save on one of their memory cards, or from an AI card, then
   changed with ASSEMBLY, OPTIONAL PARTS, PAINT and EMBLEM. With the host's
   FULL GARAGE off (the default) only the pilot's own parts fit: those the
   loaded save owns, and those the AC came with; HUMAN PLUS stays as the save
   has it. FULL GARAGE on unlocks every part and the EXTRA GARAGE AC.
   Neither pilot sees the other's AC before the match.
3. The host sets MATCH SETTINGS: stage (Green Silo by default, or RANDOM),
   time limit (the game's 2:00 / 5:00 / none), rounds (single, best of 3,
   best of 5), FULL GARAGE, and loading (FAST: both PCs read the disc at the
   fast speed until the first fight is running, then at the drive's real
   speed). The guest sees them as they change.
4. Both pilots press READY. After a three-second count the match starts by
   itself: no logos, title or menus; a black screen covers the start until
   the game's own loading picture. Between rounds the game's CONTINUE? answers itself and the next
   round loads. When a pilot has won the set, both go back to the room,
   which shows the result; READY again for another set.
5. Each player sees and hears their own side. There is no pause: holding
   START for a second opens a small menu over the running fight (RESUME,
   RESTART ROUND, CHANGE STAGE, RETURN TO LOBBY; `moa_netplay_menu.c`).
   Restart and stage changes are asked of the other pilot, who accepts or
   declines from their own menu within 10 seconds; the score stays.
   RETURN TO LOBBY (press twice) or Esc takes both PCs back to the room.

Memory cards are match-only copies on both sides: nothing done in a netplay
session is written back to a player's own save file.

Offline, VS Mode is the stock split-screen versus on controllers 1 and 2.

## What is different from the original

- Sound is mono during a match. The left channel carries what player 1 hears
  and the right what player 2 hears; each PC plays its own channel on both
  speakers. Music is therefore mono too.
- The guest's view needs the OpenGL renderer. With another renderer the guest
  sees the host's view.
- A few cues that the original plays only for the console's own player are
  still tied to player 1 and are heard by both players.

## How it works

`moa_netplay_room.c` brings the room's choices into the match: the time
limit before VS START, the stage when the link engine starts (both are guest
memory, written by hooks the game's own code runs, so rollback replays them),
the set's rounds (the versus win table in the exe's data) and the return to
the room. The room's options travel in
PSX_NET_GAME_OPTS, which the host settles into the START message
(`room=1;stage;time;rounds;garage;fastdisc`); `fastdisc` sets both drives'
speed when the session starts (main.cpp), and `moa_np_room_disc_frame` puts
them back to real speed from a guest hook once the first fight runs
(`psx_game_set_disc_speed_in_hook`, the same instant on both PCs). Each pilot's AC is the only save on a
match-only card (PSX_MEMCARD1_NETPLAY); READY is that card's share bit.

`moa_vs_direct.c` (netplay section) starts the first match without menus:
the boot's title is replaced by the versus menu's setup behind a black
cover, both cards' first saves are written into the versus staging area, and
when the menu reaches its input state that state is switched straight to VS
START (nothing presses its buttons); the units' graphics load while VS START
is held back, then the game's own VS START launches the link engine. A match boots in lockstep and cannot be fast forwarded, so this is what
saves the waiting. Its progress lives in 8 bytes of guest memory (the link
struct's send packet, idle until the link engine starts) so rollback can
rewind it; the cards are the synced match cards, identical on both PCs.

`moa_link_netplay.c` (game-owned netplay hooks, active only in a netplay
session):

- The versus menu launches the link engine (FDAT entry 208) instead of the
  split-screen one. Both player blocks are already filled by the menu.
- The link engine's serial calls are rejected at once, its role negotiation
  is answered ("master"), the AC data exchange is skipped, and every
  per-frame packet is supplied: matching check bytes and controller port 2
  as the other console's pad, one frame late as on the cable.
- The guest redraws each frame from player 2's side inside the framework's
  local-view sandbox (camera routine and scene render run again with the
  local and remote players swapped). The camera's smoothing state and the
  HUD state the render keeps in the player block are carried between
  redraws on the guest's PC only.
- Positional sounds are evaluated once per player and started with
  left = loudness for player 1, right = loudness for player 2.

Build settings (top-level `CMakeLists.txt`): netplay on, two players, the
guest's card as slot 2 by default, match-only cards for the host, and a fixed
"idle" status for the unemulated serial port (`PSX_SIO1_IDLE_STAT`), which is
also what makes the title menu offer VS Mode instead of VS Mode (Link).

The framework's rule that drops a peer's own view when it falls behind is
switched off for this title (`PSX_NET_LOCAL_VIEW_SHED=0` unless the player
sets it), because a healthy match sat at its threshold and never got the view
back.
