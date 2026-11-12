# Master of Arena: versus over LAN or online

Two players, one on each PC, each with their own full-screen view. The match
is the game's link-cable versus mode with the cable replaced, so nothing is
emulated over the network except the two controllers.

## Playing

Both players need the same build and the same disc.

1. Open the NETPLAY page in the launcher. One player hosts a room (LAN or
   online), the other joins, and the host presses Play.
2. The host is player 1. The guest is player 2, and the guest's own memory
   card (their slot 1) is the match's slot 2.
3. From the title screen choose VS Mode. Load an AC for each player (SLOT 1 is
   the host's saves, SLOT 2 the guest's), pick the stage and time limit, and
   press VS START.
4. Each player sees and hears their own side. Start pauses for both.

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
