An unofficial build of the Armored Core: Master of Arena recompilation (based on
[NeonLightning/AC1Recomp](https://github.com/NeonLightning/AC1Recomp)), with a
controller-first launcher and new modes. **No game data is included: you need
your own copy of Armored Core: Master of Arena (USA).**

This is a test release. Please report problems in this repository's Issues
(what you did, what you saw, Windows or Linux/Steam Deck).

## What's new in 0.8.3.1

- **Faster saving:** saving to the memory card no longer rewrites the whole
  card file for every small piece of the save; the card file is now written
  once, about half a second after the game finishes saving. The file is
  written to a temporary copy first and then swapped in, so a crash mid-save
  can't damage your card.
- **Updater:** now understands four-part version numbers like 0.8.3.1, so
  point releases show up as updates.
