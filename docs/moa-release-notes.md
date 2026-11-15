An unofficial build of the Armored Core: Master of Arena recompilation (based on
[NeonLightning/AC1Recomp](https://github.com/NeonLightning/AC1Recomp)), with a
controller-first launcher and new modes. **No game data is included: you need
your own copy of Armored Core: Master of Arena (USA).**

This is a test release. Please report problems in this repository's Issues
(what you did, what you saw, Windows or Linux/Steam Deck).

## What's new in 0.8.3

- **Updater:** mods that ship with a new release (in Mods/bundled) are now
  installed when you update from the launcher. Your own mods are still left
  alone.
- **Demo movie first:** the title screen now plays the demo movie before the
  opening movie (the loop still alternates). Toggle "Demo movie first" on the
  launcher's MODS page; off restores the original order.
- **AI Battle time limit:** the countdown now starts at the time you pick
  (2:00 showed about 2:45 before; the arena engine counts 22 steps a second,
  not 30).
- **Mission picker:** MISSIONS now opens a short list first: Armored Core
  (not installed yet), Project Phantasma and Master of Arena. Pick one to see
  its missions; back out to return to the picker. The "PP:" and "M:" name
  prefixes are gone. While Phantasma missions are being tested, its list shows
  all of them except the ones that still hang (1, 7, 9, 11 and 14).
- **Phantasma missions no longer advance Master of Arena's story:** finishing
  one used to move MoA's chapter forward, unlocking MoA missions and mail
  early. Now only Phantasma's own progress changes.
