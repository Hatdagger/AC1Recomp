# Credits and licenses

This build stands on a lot of other people's work. This page lists who made
what, and the license each part comes under. Every release also carries these
notices in its `licenses` folder (Windows: `RecompData/licenses`).

## The game

**Armored Core: Master of Arena** and **Armored Core: Project Phantasma** are
© FromSoftware, published in North America by ASCII Entertainment / Agetec.
PlayStation is a trademark of Sony Interactive Entertainment.

This is an unofficial fan project, not affiliated with or endorsed by
FromSoftware, Agetec or Sony. **No game data is included** in this repository
or in any release: the game reads your own disc.

## The projects this build is made from

| Who | What | License |
|---|---|---|
| [NeonLightning](https://github.com/NeonLightning) | [AC1Recomp](https://github.com/NeonLightning/AC1Recomp), the Armored Core recompilation this fork started from | none stated |
| mstan (Matthew Stanley) and [RetroPortingToolkit](https://retroportingtoolkit.com/) | [PSXRecomp](https://github.com/mstan/psxrecomp), the PS1 recompilation framework (`psxrecomp/`) | [PolyForm Noncommercial 1.0.0](psxrecomp/LICENSE) |
| mstan (Matthew Stanley) and RetroPortingToolkit | [recomp-ui](https://github.com/mstan/recomp-ui), the launcher and settings UI (`recomp-ui/`) | [MIT](recomp-ui/LICENSE) |
| recomp-net contributors (RetroPortingToolkit) | [recomp-net](https://github.com/RetroPortingToolKit/recomp-net), the netplay layer | [MIT](psxrecomp/lib/recomp-net/LICENSE) |
| retcomm-rbengine contributors (RetroPortingToolkit) | retcomm-rbengine, rollback engine | [MIT](psxrecomp/lib/retcomm-rbengine/LICENSE) |
| Alex Vanderveen | [retcomm-studio](https://github.com/RetroPortingToolKit/Retro-Studio) disc-set tools | MIT |
| PCSX-Redux authors | [OpenBIOS](https://github.com/grumpycoders/pcsx-redux), the free PS1 BIOS that ships with the game, so you don't need a Sony BIOS | [MIT](psxrecomp/bios/OpenBIOS.LICENSE) |
| JRickey | Pieces of [gba-recomp](https://github.com/JRickey/gba-recomp) (audio check, screen colour science), used with permission | MIT or Apache-2.0 |
| Hyllian | The xBR texture filter algorithm | MIT |

**This fork's own work** (the Master of Arena additions by Hatdagger) is under
the same [PolyForm Noncommercial 1.0.0](LICENSE) license as PSXRecomp.

**What PolyForm Noncommercial means for you:** you may use, share and change
this build for any **noncommercial** purpose. Don't sell it or use it to make
money. Because every release is built on PSXRecomp, this applies to the
releases as a whole.

The framework's own full attribution list is in
[psxrecomp/THIRD_PARTY_ATTRIBUTION.md](psxrecomp/THIRD_PARTY_ATTRIBUTION.md).

## Libraries in the game

| Library | By | License |
|---|---|---|
| [SDL 3](https://libsdl.org/) | Sam Lantinga and SDL contributors | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut | [MIT](recomp-ui/src/third_party/imgui/LICENSE.txt) |
| [libchdr](https://github.com/rtissera/libchdr) (with Zstandard, LZMA SDK and miniz) | Romain Tisserand and contributors | BSD-3-Clause (Zstandard BSD-3-Clause / GPL-2.0, LZMA SDK public domain, miniz MIT) |
| [libjuice](https://github.com/paullouisageneau/libjuice) | Paul-Louis Ageneau and contributors | MPL-2.0 |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | Google Inc. and WebM contributors | BSD-3-Clause |
| [toml11](https://github.com/ToruNiina/toml11) | Toru Niina | MIT |
| [stb_image](https://github.com/nothings/stb) | Sean Barrett | MIT or public domain |
| [stb_vorbis](https://github.com/nothings/stb) (Rogue Arena custom music) | Sean Barrett | MIT or public domain |
| [dr_mp3](https://github.com/mackron/dr_libs) (Rogue Arena custom music) | David Reid | public domain (Unlicense) or MIT No Attribution |
| [xxHash](https://github.com/Cyan4973/xxHash) | Yann Collet | BSD-2-Clause |

### Shipped with the compiler bundle

The release includes a small compiler bundle that turns the code the game
loads from the disc into native code on your PC.

| Tool | By | License |
|---|---|---|
| [TinyCC](https://bellard.org/tcc/) | Fabrice Bellard and contributors | LGPL-2.1 (notice ships as `tcc/COPYING`) |
| [Python](https://www.python.org/) | Python Software Foundation | PSF License (its own license text ships with it) |

### Build tools only (not in a release)

[rabbitizer](https://github.com/Decompollaborate/rabbitizer) (Decompollaborate),
[ELFIO](https://github.com/serge1/ELFIO) (Serge Lamikhov-Center) and
[{fmt}](https://github.com/fmtlib/fmt) (Victor Zverovich), all MIT.

## Fonts and pictures

| What | By | License |
|---|---|---|
| Lato (launcher font) | Łukasz Dziedzic | SIL Open Font License 1.1 |
| Noto Sans Symbols 2 (launcher symbols) | The Noto Project Authors | SIL Open Font License 1.1 |
| OpenMoji (launcher emoji) | OpenMoji Project | CC BY-SA 4.0 |
| Country flags (drawn from Noto Color Emoji) | The Noto Project Authors | SIL Open Font License 1.1 |
| AI card text (glyphs drawn from DejaVu Sans Condensed Bold) | DejaVu fonts team, Bitstream | Bitstream Vera / DejaVu fonts license |

Details: [recomp-ui/assets/common/fonts/NOTICE.md](recomp-ui/assets/common/fonts/NOTICE.md)
and [recomp-ui/assets/common/img/NOTICE.md](recomp-ui/assets/common/img/NOTICE.md).

## Community knowledge

- Public NTSC-U **GameShark / CodeBreaker** code lists (almarsguides.com,
  cheatcc.com) were the starting point for mapping the game's memory
  ([docs/mapping/ram-map-gameshark.md](docs/mapping/ram-map-gameshark.md)).

## Related

- [Raven Card Builder](https://hatdagger.github.io/ACMOA-AI-CARDBUILDER/)
  ([source](https://github.com/Hatdagger/ACMOA-AI-CARDBUILDER)): make AI
  cards in a web browser.

If your work is used here and isn't credited properly, please open an
[issue](https://github.com/Hatdagger/AC1Recomp/issues) and it will be fixed.
