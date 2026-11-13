An unofficial build of the Armored Core: Master of Arena recompilation (based on
[NeonLightning/AC1Recomp](https://github.com/NeonLightning/AC1Recomp)), with a
controller-first launcher and new modes. **No game data is included: you need
your own copy of Armored Core: Master of Arena (USA).**

This is a test release. Please report problems in this repository's Issues
(what you did, what you saw, Windows or Linux/Steam Deck).

## What's new in 0.6.0

- **AC builder in the launcher** (AI Battle → Card Workshop: START for a new
  card, or a card's menu → EDIT BUILD). It works like the Raven Card Builder,
  from the controller:
  - names, all eleven part slots (choose with left/right or open the full
    list, grouped like the web builder), optional parts against the core's
    slots, paint (all sections or one by one), emblem, Human PLUS and the AI;
  - part stats, totals and the game's own checks (legs load, arm load,
    energy, option slots). A build the game would refuse can't be saved;
  - the part list is read from your own Master of Arena disc, so it always
    matches your game;
  - emblems: put pictures in the `Emblems` folder next to your cards (or
    browse for one). The centre square becomes the game's 64×64, 16-colour
    emblem, and the paint can follow it.
- Names keep the web builder's limit of 7 characters. On a keyboard you
  can type them.

## What's new in 0.5.0

- **AI Battle → Card Workshop:** your AI cards in one list. Edit a card's AI,
  duplicate it or delete it (with a confirmation), all from the controller.
- **Cards carry their own AI now.** Pick a preset (Balanced, Aggressor,
  Sniper, Duelist, Evasive) or go CUSTOM with the game's own Ranker AI sliders:
  REBEL POINT, SHOOT, BLADE, LOCK, AVOID, FLY and RANGE TYPE. The sliders set
  the AI exactly the way the game's Ranker MK AI editor does. An ADVANCED page
  lets you set each of the 36 AI bytes directly.
- AI no longer comes from arena opponents with the same legs. Cards without AI
  settings (older cards) and random arena opponents fly the Balanced preset.
- The STANDARD / AGGRESSOR style switch is gone; use the card's AI instead.
- Saving a card redraws its image (card format 4). The web card builder does
  not read format 4 yet.
- REBEL POINT is the Human PLUS level. At 0, the load and energy limits apply,
  and a build over them won't start the match.

## What's new in 0.4.2

- **Fix: Project Phantasma missions now show up** in the mission list (and
  Master of Arena's missions get their "M:" prefix). On PCs that compile the
  game's code for speed, the missions' hooks were skipped, so nothing changed
  in the list even with the missions switched on. The first start after
  updating may take a little longer while the game's code is compiled again.
- The Raven Card Builder link now opens the web page.

## What's new in 0.4.1

- **Extras → Reinstall additional content:** removes the installed Project
  Phantasma content and builds it again from your discs.
- **Fix:** installing the Phantasma content over an older arena-only install
  now replaces it, so the missions actually switch on. (In 0.4.0 the old
  version stayed selected and the missions never appeared.) If you're on
  0.4.0 with the arena already installed, select *Reinstall additional
  content* once.

## What's new in 0.4.0

- **Project Phantasma missions:** 12 of the 17 missions, one at a time, at the
  top of the mission list as "PP: …" (details below).

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
  [Raven Card Builder](https://hatdagger.github.io/ACMOA-AI-CARDBUILDER/)
  (or exported from your own memory card save in the launcher), and go in the
  `Memory Card` folder. Each card carries its own AI (preset or the game's
  Ranker AI sliders), set in AI Battle → Card Workshop.
- **Extras → Install data from Project Phantasma:** if you also own Project
  Phantasma (USA), its 49 opponents are added as a tenth Ex Arena with their
  own ranking, and **(new in 0.4.0) its missions**: the next Phantasma mission
  sits at the top of the mission list as "PP: …" (Master of Arena's own show
  "M: …"), with Phantasma's briefings, unlocking one at a time. 12 of the 17
  missions are in for now (1, 7, 9, 11 and 14 are left out until they are
  fixed). Progress is kept in `Phantasma Missions.txt` in the Memory Card
  folder (one progress for the whole card). If you installed the arena with
  an older version, select *Reinstall additional content* to add the
  missions. The launcher looks for the Phantasma disc image (`.cue` +
  `.bin`) in the same folder as your Master of Arena disc; if it is not there
  you pick the file yourself. Everything is built on your computer from your
  own discs; nothing from them is included in or sent by this download.
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
