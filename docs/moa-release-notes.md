An unofficial build of the Armored Core: Master of Arena recompilation (based on
[NeonLightning/AC1Recomp](https://github.com/NeonLightning/AC1Recomp)), with a
controller-first launcher and new modes. **No game data is included: you need
your own copy of Armored Core: Master of Arena (USA).**

This is a test release. Please report problems in this repository's Issues
(what you did, what you saw, Windows or Linux/Steam Deck).

## Downloads

- **Windows:** `Armored-Core-MoA-…-windows-x64.zip`. Unzip anywhere and run
  `Armored Core Moa.exe`.
- **Linux / Steam Deck:** `Armored-Core-MoA-…-linux-x64.AppImage`. Mark it
  executable and run it.

Put your disc image (`.cue` + `.bin`) in the same folder as the program (or the
AppImage); it is found automatically. Only disc 1 is needed: everything from
disc 2 plays from the single disc.

## What's in it

- **Single disc:** the whole game from disc 1, no disc swaps.
- **Network play:** two-player versus over LAN or online, each player on their
  own screen and memory card (link-cable versus over the network).
- **Controller-first launcher** in the game's own style: start, network, AI
  battle, settings, mods and extras, all from a gamepad.
- **AI Battle:** pick two AC cards and watch the computer fly both. Cards are
  images made with the
  [Raven Card Builder](https://github.com/Hatdagger/ACMOA-AI-CARDBUILDER)
  (or exported from your own memory card save in the launcher), and go in the
  `Memory Card` folder. Each card borrows the AI of an arena opponent with the
  same legs.
- **Extras → Install data from Project Phantasma:** if you also own Project
  Phantasma (USA), its 49 opponents are added as a tenth Ex Arena with their
  own ranking. The launcher looks for the Phantasma disc image (`.cue` + `.bin`)
  in the same folder as your Master of Arena disc; if it is not there you pick
  the file yourself. The arena is built on your computer from your own discs;
  nothing from them is included in or sent by this download.
- **Updates:** a new entry below Mods. *Check for update* asks GitHub for the
  newest release; *Download and apply update* installs it, only when you press
  it. Nothing is checked or downloaded by itself. When a newer version is found
  the main menu's status panel shows "Update available". The download is
  verified against the release's checksum. Only the game's own files are
  replaced: memory cards, mods, disc images and settings are not touched. On
  Linux the AppImage is replaced in place and the old one is kept beside it as
  `<name>.previous`. It uses the system's `curl` (SteamOS and Windows 10 and
  later include it). Versions before 0.3.1 have no Updates entry, so update
  to 0.3.1 by hand once.

## Folders

- `Memory Card/` - memory cards, and AI Battle cards (`.png`)
- `Mods/` - mods; new mods can be installed from the launcher
- `RecompData/` - everything else
