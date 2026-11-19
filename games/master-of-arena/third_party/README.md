# Third-party code (Rogue Arena custom music decoders)

| File | Source | Version | Licence |
|---|---|---|---|
| `stb_vorbis.c` | https://github.com/nothings/stb (commit 9600522) | v1.24 | public domain (or MIT) |
| `dr_mp3.h` | https://github.com/mackron/dr_libs (commit 0901cdc) | v0.7.5 (dev) | public domain (Unlicense) or MIT No Attribution |

Both are compiled once in `moa_music_decoders.c` and used by `moa_music.cpp` to read the
player's own music files (`.ogg`, `.mp3`). Unmodified copies.
