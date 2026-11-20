# Sound and music — sound chat

Lane scope: every sound the game makes, where it sits on the disc, how it is loaded and what
plays it. Static read of the USA disc 1 (SLUS-01030) on 2026-11-19, from the `ma-work` disc pack
and this repo at `7884f4c` (0.8.4.4.2). Nothing here is tested in game yet; `?` marks guesses.
A listening pack (every sample and voice clip as MP3, with CSV indexes) was made alongside this
doc: see §11. Checked against the weapon map (`weapon-map.md`) and the disc on 2026-11-20:
§3's shot-table list, ids 6, 7 and 53, and the play-once bits are corrected or confirmed.

## 1. What is on the disc

| Source | What | Format | Loaded / played by |
|---|---|---|---|
| `GG/MS/MENU_VAB.T` 0 + 1 | **menu sound effects** (1 program, 13 tones, 12 samples) | VAB header + body | every menu start, `0x8005803C` (sync reads of entries 0 and 1) |
| `GG/MS/MENU_VAB.T` 2 + 3 | instruments of the menu song (4 programs, 13 tones) | VAB | song task `0x80064F24(2)` → `0x80058700(2)` |
| `GG/MS/MENU_VAB.T` 4 | **the menu song** (one SEQ, 480 ticks per beat, 126 BPM, 4/4) | SEQ (`pQES`) | same song task: SsSeqOpen / SsSeqPlay endless |
| `GG/COM/FDAT.T` 205, blocks 0 + 1 | **shared fight bank** (4 programs, 51 tones, 38 samples) = bank slot 0 | VAB | every fight engine at load (VS `0x800559C4`) |
| `FDAT.T` 205, block 2 | **sound-id table** (256 ids × 10 bytes, after 448 other bytes) | data | copied to `0x80044598` at load; ids at `0x80044758` |
| `FDAT.T` 205, blocks 3 / 4 / 5 | the 250 messages, one block per COM voice type (`missions.md`) | text | carry the voice and music codes (§7, §8) |
| `FDAT.T` 5s + 4, block 5 | **the map's banks**: slot 1 (stage sounds) + enemy banks (slots 3–60) | bank list | every engine at map load (VS `0x80055B64` → `0x800528DC`) |
| `FDAT.T` 5s + 4, blocks 13–18 (missions, AC TEST) | three VABs, one per COM voice type → slot 2 | VAB × 3 | missions engine `0x80055A08`; **nothing plays slot 2** (§4) |
| `GG/BGM/BGM00–03.XA` | **music**: 8 stereo songs per file, 32 in all | XA (CD audio) | fight engines and missions (§7) |
| `GG/BGM/TALK00–06.XA` | **mission radio voices** (Lana Nielsen and others): 16 clips per file, 112 | XA, mono 37.8 kHz | message code `X` (§8) |
| `GG/BGM/CPU00–03, 10–13, 20–23.XA` | **AC computer (COM) voice**, three voice types × 64 clips | XA, mono 37.8 kHz | message code `V` (§8) |
| `GG/STR/*.STR` | movie sound (intro ACOPA, ending ACEND, attract DEMOPLAY, credits STAFF/STAFF2) | STR with XA audio | not read here (movie packs not used) |

The exe and the engines (FDAT 206–211) hold no sound data of their own.

## 2. How the fight engines play a sound

Every engine (missions 207, link 208, VS 209, arena 210, replay 211) has its own copy of the
same sound code, using the same RAM:

| RAM | What |
|---|---|
| `0x800441D8` / `0x800441EC` | listener (the camera eyes, written by `0x8005301C` in VS) |
| `0x80044218` + 8 × slot | **bank slots**: s16 SPU VAB id (−1 = none, 254 = skip) + header pointer |
| `0x80044428` | 22 voice records × 16 bytes (+0 handle, bit 15 = keyed; +2 SPU voice; +3 louder side; +4 / +5 note and fine; +6 sound id; +8 weight × volume) |
| `0x80044598` | FDAT 205 block 2 (3,008 bytes) |
| `0x80044758` + 10 × id | **sound-id record**: s16 bank slot, s16 program, s16 tone, s16 note (60), u16 weight |
| `0x80045178` | one bit per id: already played. **Play once per frame**, confirmed: each frame the VS engine clears 8 words = ids 0–255 (`0x8005C0D4` → exe fill `0x8001589C`). The play routine sets bit `id & 31` of word `id >> 5`, so an id ≥ 256 sets a bit that is never cleared (hit-sound record 7, `weapon-map.md` §3) |

**Play routine** (id, handle, volume L, volume R, pitch offset on the stack): VS `0x80053284`,
arena `0x8005376C`, missions `0x80053320`, link `0x80053498`, replay `0x800531EC`. id 255 = none.
It looks up the id record and its bank slot, picks a voice (`0x80053154` in VS), and keys it on
through the exe's `SsUtKeyOnV`-style call `0x80022C74` (or changes the pitch of a running handle,
`0x80022344`). Note = record +6 + pitch offset ÷ 16. Wrappers (VS): `0x80052B18` and
`0x80052D9C` (3D, from a position `?`), `0x80052E70`, `0x800530AC` (id, volume), `0x800530D8`,
`0x80053130`, `0x80053DD0` (plays a list). **3D sounds** (confirmed, VS): `0x80052D9C(id, 0,
position, volume, cut-off, fade, …)` measures the distance d from the listener (GTE,
`0x80052C88`): silent when d ≥ cut-off, else volume × (fade − d) ÷ fade, 0 below 30, at most
127. `0x80052E70` uses 8,000 / 10,000 (shot hits); explosions (ids 6, 7 below) use
12,000 / 14,000, so they carry 50% further. The play routine
looks the record up by **id & 0xFF**. Arena wrappers: `0x80052F2C`, `0x80053190`,
`0x80053380`, `0x8005357C`, `0x800535A8`, `0x80053618`, `0x800542B8`.

**Bank open** `0x80052834(slot, header, body)` (VS; arena `0x80052D50`, missions `0x80052948`):
SsVabOpenHead (`0x80027EF4`) then the body to the SPU in 2,048-byte pieces. **Bank list**
`0x800528DC` (arena `0x80052DF8`, missions `0x800529F0`): u32 count, then *count* size-prefixed
headers, then *count* bodies, each body starting with its **slot number** (u32).

## 3. Sound ids

The full list (256 rows: slot, program, tone, note, weight and the sample file for slot 0) is
`sound-ids.csv` in the listening pack. Summary:

| Ids | Bank | Notes |
|---|---|---|
| 0–63 | slot 0, shared fight bank | 24–29 and 60–63 repeat tone 0; 19, 33, 34, 36, 37, 49 point at empty tones (silent) |
| 64–127 | slot 1, the map's own bank | programs 0–2; a map's bank only has the tones it needs, the rest are silent |
| 128–193 | slots 3–60, enemy banks | one slot per enemy kind (table below) |
| 194–255 | slot 0, program 0, tones 0–5 repeated | filler `?` |

Ids played with a constant (same in VS, arena and missions unless noted; routine = VS address):

| Id | Sample | Played by (VS) |
|---|---|---|
| 0 | v12 | cannon fire `0x80081B18` (WC-GN230, at volume 120) |
| 1 | v9 | `0x8007E698` (movement, next to the turn handlers) `?` |
| 2, 12 | v18, v19 | missile update `0x8008907C` (ignition / flight `?`) |
| 4 | v21 (**rifle shot**, also used by `moa_vs_sound`) | projectile kinds (below) |
| 6 | v5 | `0x8008D0A4`, the **mine** update (model IDs 90+, WRR-10): the explosion, volume 127; also the hit table (most shots) |
| 7 | v34 | **big explosion**, volume 127: `0x80089B64` = the timed-shell update (WC-GN230 and WC-IR24 bursts; set by `0x8008B1C0`); `0x8008E8FC` = the update of the objects `0x8008EBAC` makes (below) |
| 8, 9, 40, 41, 59 | v27, v28, v30, v29, v11 | `0x8005675C` (engine level: warnings `?`; arena `0x80056F9C`) |
| 14 | v13 | animation-finished handler `0x8008389C`, `0x80085D40` |
| 15 | v23 | blade `0x8007EB48`, `0x8007F264` |
| 30, 31, 32 | v8, v16, v17 | missions only, `0x80095078` |
| 35 | v26 | missions only, `0x80091064` |
| 38, 39 | v24, v25 | missions only, `0x8009115C` (next to the start-music call) |
| 43, 44 | v32, v33 | missile lock: new lock / last free lock (`0x80080638`; `battle-system.md` says 0x2B / 0x2C) |
| 45 | v11 | energy code `0x80082D90` area (`0x80083030`) and AC update `0x800828BC` (alarm `?`) |
| 46 | v11 | gun / cannon lock `0x800800E4` |
| 47 | v11 | missile lock tick every 4 steps (`0x80080638`) |
| 52 | v6 | `0x8008F034` |
| 53 | v22 | gun fire `0x80080DF4`: **mines only** (`0x80081250`; every other gun's fire sound comes from its shot record); cannon fire `0x80081B18` (WC-IR24, volume 120) |
| 54, 55 | v16, v30 | VS main loop `0x80051F4C` (match start / end `?`); missions: message routine `0x80058B58` (open / close `?`) |
| 57 | v36 | message readers (arena `0x8005A2C4`, missions `0x800592BC`) |

Several ids share a sample at a different pitch (45 / 46 / 47 / 59 are all v11, at centre notes
84 / 72 / 100 / 72 and note 60 or 50). The pack renders each sample once, at its first tone's
pitch.

**Ids taken from data** (VS; corrected 2026-11-20 against `weapon-map.md` §2): the shot
spawners read a sound id from the record picked by the weapon's model ID:

| Model IDs | Spawner | Table (record) | Sound at |
|---|---|---|---|
| 0–19 | `0x8008AC30` bullet streak | `0x80091198` (28 bytes) | +21 |
| 20–39 | `0x8008DEC0` billboard | `0x80092980` (40) | +10 |
| 40–59 | `0x8008E664` model shot | `0x80092C20` (36) | +8 |
| 60–69 | `0x8008D8CC` energy streak | `0x80092824` (24) | +10 |
| 70–89 | `0x8008CBCC` dispenser | `0x80092910` (28) | +6 |
| 90+ | `0x8008D38C` mine | — | 53, from the gun fire routine |

The ids used are 4, 48, 50, 51, 53 and 0 (48 = machine gun 2, only in the first table).
Every weapon's fire and hit sound is in `weapon-map.md` §6 / §7. `0x8008EBAC` (table
`0x80092E60`, 36 bytes, sound at +8, played at volume 100) is **not** reached from the gun,
missile or cannon fire routines; its one caller is `0x8007C378` (not identified). Hits:
`0x8008B60C` reads 8-byte records at `0x800930A0` (ids 6 and 22 at volumes 90 / 50; only
records 0–3 are sounds, see `weapon-map.md` §3). Footsteps: the three turn /
walk handlers `0x8007E1DC`, `0x8007E2DC`, `0x8007E3E0` play the id in **byte +17 of the AC's leg
record** (AC data +68). Wreck: `0x80079828` plays ids from its record (+16, +26). Which part
uses which kind still needs the weapon tables (data chat).

**Enemy banks (ids 128–193).** One bank slot per enemy kind, filled only by the missions that
have that enemy; ids whose slot is never filled in MoA are silent (the table is shared with
older games `?`).

| Slot | Ids | Bank in |
|---|---|---|
| 3 | 134–138 | missions 0, 9 |
| 4 | 128 | missions 6, 12, 13, 16 |
| 5 | 129 | missions 3, 7, 13 |
| 8 | 146 | missions 2, 5 |
| 9 | 131 | mission 1 |
| 11 | 153–158 | missions 8, 16 |
| 13 | 160–161 | mission 6 |
| 19 | 167–171 | mission 7 |
| 23 | 188 | mission 4 |
| 24 | 189 | missions 8, 16 |
| 25 | 190 | missions 9, 10 |
| 26, 27 | 192, 191 | mission 7 |
| 59 | 181–183 | mission 3 |
| 60 | 184–187 | AC TEST (stage 40) |
| 6, 7, 10, 14, 15, 17, 18, 20, 21, 22, 53, 54, 56, 57, 58 | 130, 132–133, 139–145, 147–152, 159, 162–166, 172–180, 193 | **no bank on the disc** (silent) |

## 4. The map banks

FDAT 5s + 4 for s = 0–19 are missions 0–19, s = 20–39 the VS / arena stages, s = 40 the AC
TEST stage. Block 5 is the bank list (slot 1 first). Samples per slot-1 bank:

| Missions | 0: 3, 1: 9, 2: 5, 3: 2, 4: 7, 5: 4, 6: 10, 7: 9, 8: 4, 9: 5, 10: 1, 11: 1, 12: 5, 13: 6, 14: 1, 15: 4, 16: 7, 17: 3, 18: 4, 19: 4 |
|---|---|
| **Stages** | 20: 1, 21: 8, 22: none, 23: none, 24: 4, 25: 1, 26: 6, 27: 3, 28: 4, 29: 1, 30: 1, 31: 5, 32: 4, 33: 1, 34: 6, 35: 3, 36: 1, 37: 4, 38: 1, 39: 5, 40: none |

Every stage sample is also in some mission's bank (byte-identical), so the stages reuse mission
sounds. VS maps 1–10 = stages 20, 21, 22, 23, 30, 33, 34, 35, 36, 37 (`menus-screens.md`); the
arena's stage list is at `0x800D59F0` (stages 20, 22, 24, 25, 27, 28, 29, 31, 33, 36, 37, 39).
Stages 22 and 23 (VS maps 3 and 4) have no stage sounds at all.

**Slot 2 (missions 0, 2, 3, 6–10, 12, 16, 18, 19; the others have empty VABs there; only five
different sets on the disc).** The missions engine picks one
of the three VABs in blocks 13/14, 15/16, 17/18 by the **COM voice type** (player block +0x27A,
§8) and opens it as slot 2 (`0x80055AC4`). No sound id uses slot 2 and no code reads its bank
id, so these are most likely left over from an older game where the COM voice was SPU samples
`?`. They are in the pack as `mNN-headvoiceK` for listening.

## 5. Menu sounds

Every menu sound is `SsUtKeyOn` (exe `0x800227D4`) on the menu bank (vab = s16 at `0x801B1A80`),
program 0, note 60 (`menus-screens.md` §1d). Counting the ~350 calls in FDAT 206:

| Tone | Sample | Calls | Plays when |
|---|---|---|---|
| 0 | v10 | ~60 | back / cancel |
| 1 | v1 | ~110 | cursor |
| 2 | v2 | ~50 | OK |
| 3, 4, 5 | v3, v4, v5 | none found as constants | unused `?` |
| 6 | v11 | ~100 | window opens |
| 7 | v6 | ~20 | question / message box; refused action |
| 8 | v8 | 1 (`0x80055548`, hub ring) | hub `?` |
| 9 | v7 | 2 (`0x80095B50`, `0x800A3BE0`) | `?` |
| 10 | v9 | 2 | part fitted |
| 11 | v12 | — (register) | part removed |
| 12 | v4 | 4 | ASSEMBLY row cursor |

**Menu song**: `0x80064F24(2)` (task) loads MENU_VAB 2/3, waits, then SsSeqOpen, SsSeqSetVol
(BGM option × 2) and SsSeqPlay endless; 0x801D0AFC = 1 while it plays (`rogue-arena-music.md`).
Started by the hub open (`0x80056DBC`), the VS menu's first state (`0x800B0FE4`) and the link menu
(`0x800ACF10`). It is the only SEQ on the disc.

## 6. XA streams

The exe registers 23 XA files at `0x80036C74` (36-byte records: 16 u16 values, then the CdlLOC).
**XA id = file × 16 + channel**:

| File | Name | Ids |
|---|---|---|
| 0–3 | BGM00–03 | 0–63 (8 channels used per file) |
| 4–10 | TALK00–06 | 64–175 |
| 11–14 | CPU00–03 | 176–239 (COM voice type 0) |
| 15–18 | CPU10–13 | 240–303 (type 1) |
| 19–22 | CPU20–23 | 304–367 (type 2) |

**XA play** (id; 0xFFFF = stop): VS `0x80053C04`, arena `0x800540EC`, missions `0x80053CA0`, link
`0x80053E18`, replay `0x80053B6C`. It sets the CD filter (file 1, channel id & 15, CdlSetfilter)
and seeks to the file's CdlLOC. Every engine stops XA on exit (`0x800526EC` in VS).

## 7. Music

Fight music is the byte **`0x8003EA29`** (an XA id; 255 = none), played at the start of the fight (VS
`0x8008FA80` and arena `0x800969B8` at step 44; missions `0x800911B8`). Who sets it:

| Mode | Value | Where |
|---|---|---|
| VS | random pick of 10: **16, 17, 19, 20, 23, 32, 33, 38, 50, 54** | VS START `0x800B348C`, table `0x800538A8` (every 4th byte) |
| Arena | random pick of the same 10 | arena launch `0x800C0878` (jump table `0x80054048`) |
| Missions | **MIS.T entry 0, T+1160 [slot]**: mission 2 → 38, mission 5 → 35, mission 8 → 52, others 255 | mission launch `0x800976CC` |
| AC TEST, RANKER TEST | 255 (no music) | `0x8007DB44`, `0x8007DE4C` |
| `0x800AFEDC` (link-cable launch `?`) | 50 | |

**Music changes inside missions** come from message code `B`*n* (§8): when such a message plays,
the reader calls the XA play with *n* (missions `0x80059628`, arena `0x8005A630`):

| Song | Messages |
|---|---|
| 20 | 29 (cannon charging, mission 3) |
| 21 | 101 |
| 32 | 11, 119 |
| 33 | 36 (Fritz: "Who is THAT?!", mission 6) |
| 34 | 45 (mission 9) |
| 36 | 61 (mission 14 timer) |
| 37 | 49 (mission 10) |
| 38 | 28, 120, 121 |
| 39 | 25 (mission 1) |
| 49 | 6 (mission 4, units approaching) |
| 50 | 8, 21, 32, 40, 43, 47, 51, 58 (an enemy Raven appears: boss music) |
| 52 | 44 (mission 8, "Evacuate immediately!") |
| 54 | 118 |

**Never referenced:** 0–7 (all of BGM00), 18, 22, 48, 51, 53, 55. The music packs weren't used,
so what these are is open (title / ending / Phantasma leftovers `?`).

## 8. Voices

Message strings (FDAT 205 blocks 3–5) start with codes before the text. Readers: missions
`0x800592BC`, arena `0x8005A2C4` (jump table on the letter − 'B'):

| Code | Effect |
|---|---|
| `M`*n* | lines in the box `?` |
| `X`*n* | **radio voice**: XA 64 + *n* (TALK file *n* ÷ 16, channel *n* mod 16) |
| `V`*n* | **COM voice**: XA 176 + 64 × type + *n* |
| `B`*n* | **music**: XA *n* (§7) |
| `N`*n* | sets +174 = 56 (n = 0) or 58 (n = 1) `?` (portrait / box style) |
| `H`*n*, `T`*n* | no sound (`H` COM lines and `T` radio lines are text only) |

**COM voice type** = player block **+0x27A** (compiled +0x2A = head byte +0x32). It picks the CPU
file set (CPU0x / 1x / 2x), the message block (3 / 4 / 5: "Combat system engaged." / "Main
system. Engaging combat mode." / "SYSTEM ENGAGED.") and the slot-2 VAB. So it's the head's voice,
not a HUD style as `stat-block.md` guesses.

- **Radio**: X0–X97 are all used (one clip per line, messages 3–123 and 147–148; speaker and
  text in `voices.csv`). X98–X111 have no message.
- **COM**: only V0 engaged, V2 battle over / target achieved, V3 out of area, V16 destroyed, V37
  test mode on, V38 test mode off, V39 test mode, damaged, V43 mission failed (messages 151–158).
  The other 56 clips of each type have no reader.

## 9. Sounds added by the recomp (not on the disc)

| Module | What |
|---|---|
| `moa_ui_sound.c` | overlay clicks from the menu bank: tones 0 back, 1 cursor, 2 OK, 10 equip, on a private SPU voice (slot 19, banks 23–26) |
| `moa_vs_sound.c` | VS screen: shared-bank **sample** 21 (rifle, = id 4) for the count, sample 1 (= id 3) for the shutters, with `moa_sfx.c`'s room finish (slots 20–21, banks 27–29) |
| `moa_chime.c` | victory sting and type tick, made at run time (no disc data; slots 22–23, banks 30–31) |
| `moa_music.cpp` + `psxrecomp/runtime/src/mod_host_music.c` | menu-song control for Rogue Arena and custom music files on the runtime's host channel (`rogue-arena-music.md`) |

`moa_vs_sound.h` calls the rifle "sound 21": that's sample (VAG) 21. As a sound id it is 4 (or 20,
quieter).

## 10. Open items

- Listen through the pack and name the samples (ids 0–63, stage and enemy banks).
- ~~Projectile kind → weapon part~~: done, `weapon-map.md` §6 / §7.
- What `0x8008EBAC` / `0x8007C378` are (makes objects that explode with id 7).
- What `0x8005675C` plays ids 8, 9, 40, 41, 59 for (warnings?), and ids 54 / 55 in the VS loop.
- Slot 2 VABs and the unused COM / radio clips: confirm nothing plays them (runtime trace).
- BGM00 and the other unreferenced songs (needs the music packs); movie audio.
- The 448 bytes before the id table in FDAT 205 block 2 (`0x80044598`).
- The u16 values in the XA file records (`0x80036C74`): song lengths? Units unknown.

## 11. Listening pack

`moa-sound-pack.zip` (made by the sound chat, not in the repo): `samples/` (147 unique samples,
MP3; renders at the first tone's pitch, 44.1 kHz at the centre note), `voices/` (304 XA clips),
`samples.csv` (every bank → sample file), `sound-ids.csv`, `voices.csv` (clip → message text).

## Handed over

To the missions chat:
- MIS.T entry 0 **T+1160 is the mission's start music** (XA id, copied to `0x8003EA29`), not an
  enemy AC.
- Message codes: `X` radio voice, `V` COM voice, `B` music change, `H` / `T` text only (§8).
- Blocks 13–18 of a mission map = three VABs (one per COM voice type) into slot 2; no reader.

To the data chat:
- Compiled +0x2A (head byte +0x32, player block +0x27A) is the **COM voice type**: it picks the
  XA voice set, the message block and the slot-2 VAB (arena `0x8005A438` is the voice call, not
  a HUD item).
- Leg record byte +17 = footstep sound id (VS `0x8007E1DC` / `0x8007E2DC` / `0x8007E3E0`).

To the battle chat:
- `0x8008B7B8` (listed as "a sound" after a hit) spawns sparks; the hit sound is from the table
  at `0x800930A0` via `0x8008B60C`.

To the weapon map (2026-11-20, folded into `weapon-map.md`): ids 6 / 7 from the mine and
timed-shell updates; the play-once bits.
