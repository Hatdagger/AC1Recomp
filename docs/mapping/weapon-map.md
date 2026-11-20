# Weapon map: shots, looks, sounds, impact and stun

Lane: every weapon in Master of Arena, from its part record to what it fires, how the shot looks
and sounds, what a hit does and how impact / stun works. Static read of the USA disc 1
(SLUS-01030) from the `ma-work` pack on 2026-11-20: menu module FDAT 206 (part tables) and the
**VS engine FDAT 209** (everything else; the other fight engines carry the same tables). Repo at
`7884f4c` (0.8.4.4.2). Run-tested values are marked ✔ (from `battle-system.md` / Armored-CoreR);
everything else is read from code, `?` = a guess.

Tools: `tools/moa_weapon_map.py` (reads FDAT 206 + 209, writes a CSV with one row per weapon
and every field below) and `tools/moa_fx_atlas.py` (renders the effect sprites from RTIM 61).
The pictures made from them (`weapon_shots.png`, `colour_variants.png`, `atlas.png`) are disc art
and stay out of the repo (run the tools on your own disc).

Units: a **step** = one game tick, 22 a second. Speeds in world units a step. Angles 4096 = 360°.

## 1. The path of a shot

1. **Weapon detail record** (24 bytes, menu module; `parts-tables.md`): kind, **model ID**, attack,
   impact, flags, ammo, fire interval… The compile routine copies it into a weapon slot
   (player block +0x2D8 + 62 × slot; `stat-block.md`).
2. **Fire routine by kind**: gun `0x80080DF4`, missile `0x80081888`, cannon `0x80081B18`.
3. **Guns: the model ID picks a family** (spawner + table). Cannons: the model ID (0–7) picks a
   case in a jump table at `0x800511CC`. Missiles: the launcher table `0x800912B0`.
4. The spawner fills a **pool record** (256 × 228 bytes at `0x801D6F6C`): speed, life, the
   6-byte **hit record** (flags, attack, impact), the **draw kind** (+0x10), the look fields and two
   packed effect words (§3), and plays the **fire sound**.
5. The **update routine** moves it each step and runs the hit test. On a hit: the debris word →
   chips off the target (`0x8008C164`), the hit dispatcher `0x8008821C` → damage
   (`0x800795DC`) → the target's on-hit callback (`0x80082430`: knockback, stun, rumble); and
   the flash word → the impact flash, its light and the hit sound (`0x8008B60C`).

## 2. Shot families (guns, by model ID)

From the gun fire routine (`0x80080E38` picks the speed table, `0x8008122C…0x80081484` the spawner):

| Model ID | Family | Spawner / update | Table (record size) | Drawn as | Used by |
|---|---|---|---|---|---|
| 0–19 | **A bullet streak** | `0x8008AC30` / `0x80088674` | `0x80091198` (28), index = ID; only 0–9 exist | streak (pool kind 1) | rifles, machine guns, WA-Finger |
| 20–39 | **B billboard shot** | `0x8008DEC0` / `0x8008DB18` | `0x80092980` (40), ID − 20 | billboard sprite (kind 2) | handguns, pulse, AR1000, GT2000, flamethrower, WR-RS7 |
| 40–59 | **C model shot** | `0x8008E664` / `0x8008E1B0` | `0x80092C20` (36), ID − 40 | 3D model (kind 0, or kind 4 rigid for #6, 7, 11, 12) | sniper slugs, bazookas, rockets, grenades, weapon-arm cannons, WX-ED2, WX-C/4 |
| 60–69 | **D energy streak** | `0x8008D8CC` / `0x8008D584` | `0x80092824` (24), ID − 60 | streak (kind 1) | laser / plasma rifles, AW-XC65 |
| 70–89 | **E dispenser** | `0x8008CBCC` / `0x8008C948` | `0x80092910` (28), ID − 70 | billboard | WM-X5-AA, WM-X10, WM-X15-EX |
| 90+ | **F mine** | `0x8008D38C` / `0x8008D0A4` | (not read) | rigid model | WRR-10 (fire sound 53 played by the fire routine) |

Per-ID oddities in the fire routine: **ID 8** (WA-Finger) fires 5 bullets per trigger and takes 4
extra ammo; **ID 30** (WG-HG770) flies ¼ of its range, then re-spawns as B #11 with 3× range;
**ID 52** (WX-ED2) sets a flag (+0x4E = 5 `?`); **ID 66** (WG-XW11) fires a second shot from the
other side. Shots per trigger come from detail +2 bits 0–1 (`parts-tables.md`).

**Family records** (offsets in the record; "→ pool +x" = where the spawner puts it):

| Field | A (28 B) | B (40 B) | C (36 B) | D (24 B) | E (28 B) |
|---|---|---|---|---|---|
| Speed | +8 | +0 | +0 | +0 | +0 |
| Random spread (mask) | +10 | +2 | — | +2 | +10 / +12 |
| Width / size | +12 → +0xCA (streak half-width) | +4 → +0xBE (sprite size) | +2 scale (rigid only) | +4 → +0xCA | — |
| Trail | +14 → +0x22 (steps the tail lags) | — | — | — | +4 → +0x22 |
| Picture | +16 → +0xC8 **sprite** (atlas) | +6 → +0xBC **sprite** | +6 **model number** | +6 → +0xC8 sprite | +14 (4 bytes) → +0xBC |
| Colour | +24 → +0xC4 **RGB tint** | shot: the atlas entry's colour; **trailing glow: +28 RGB** | model texture | +12 → +0xC4 RGB tint | — |
| Fire sound id | +21 (u8) | +10 (u8) | +8 (u8) | +10 (u8) | +6 (u8) |
| Pellets | — | +14 (u8) | — | — | +8 (count) |
| Flash word | +0 → +0x34 | +32 → +0x38 | +28 → +0x38 | +16 → +0x34 | +20 → +0x34 |
| Debris word | +4 → +0x38 | +36 → +0x3C | +32 → +0x3C | +20 → +0x38 | +24 → +0x38 |
| Other | +18, +19, +20, +22, +23 not read here | +8 life factor; +12 byte = 1 → normal flash, else a spark burst (`0x8008B7B8`, +12 / +13); u32 +12 bits 24–25 = **trailing glow** each step (`0x8008A700`, pool kind 6): sprite + size from table `0x80092C00` (4 bytes) by bits 26–31, colour = +28; +16 / +18 = shot sprite animation (frames from +6) | +2 for others `?` | +8 life factor | — |

Life: the spawners work it out from the weapon's range and the speed (`0x8008A898`).

**Streak draw** (`0x800645D8`): textured quads from the shot's head (+0x00) to its tail (+0xBC,
the position *trail* steps ago), sprite from the atlas, half-width = +0xCA × 40 ÷ depth,
**colour = pool +0xC4** (the per-weapon tint, modulating the grey sprite), semi-transparent
additive. **Billboard draw** (`0x80064C68`): sprite = atlas[+0xBC], size +0xBE, colour = that
atlas entry's own RGB.

### Cannons (kind 2, case by model ID, `0x80081F70`)

Before the shot every cannon kicks the firing AC back: push = player block +0x214 (from the
slot's recoil, `stat-block.md`) × cannon table +2 >> 8, taken off the AC's velocity. Cannon table
`0x80091310`, 6 bytes: +0 speed, +2 recoil scale, +4 `?`.

| Model | Weapons | Shot |
|---|---|---|
| 0 | WC-CN35 chain gun | A-style streak with constants in code: speed 500, sprite 7 (diamond), tint `fa3200`, width 20, trail 3, flash / debris words at `0x800918E4` / `0x800918E8`, sound 4 |
| 1 | WC-LN350 linear gun | B #6 |
| 2 | WC-ST120, WC-SPGUN slug guns | B #7 |
| 3 | WC-GN230 grenade launcher | **timed shell** `0x8008B1C0`, model 49, sound 0: its life is the flight time to the lead point on the lock target (`0x800155DC`), so it bursts there; unlocked = range ÷ speed |
| 4 | WC-XP4000 pulse cannon | B #5 |
| 5 | WC-XC8000 plasma cannon | D #1 |
| 6 | WC-01QL laser cannon | C #3 (rigid model 56) |
| 7 | WC-IR24 laser cannon | timed shell like model 3, model 56, sound 53 |

### Missiles

Unchanged from `battle-system.md` ("Missiles: lock slots, volleys and steering"): launcher table
`0x800912B0` → flight kind → flight table `0x80093114`. New here: flight record **+0x20 = flash
word**, **+0x24 = debris word**, +0x16 (byte) = blast count on stage objects (`0x8008BB78`).
Missiles draw as a posed model (pool kind 0). Sounds (missile update `0x8008907C`): **id 2**
launch, **id 12** flight loop.

## 3. The packed effect words

**Flash word** (read by `0x8008B60C` when a shot ends: wall, floor or target):

| Bits | Meaning |
|---|---|
| 0–2 | **Flash kind** (7 = none), table `0x800930DC` (8 bytes: first sprite, frames, size-curve pointer): 0 fireball (sprites 8–19, 12 frames), 1 hot glow (20–31, 12), 2 spark spray (32–33, 2), 3 flames (42–49, 8), 4 blue lens flare (37), 5 magenta flare (38), 6 orange flare (36) |
| 3–4 | Frames advanced per step |
| 5–9 | Length (steps), with bits 13–16 |
| 10–12 | **Light colour** (7 = none): the flash lights up the area through the per-area light list (`0x8005B96C`), strength = size curve × size. Table `0x80092814`: 0 orange `ff8250`, 1 yellow-green `e6ff64`, 2 blue `4080f0`, 3 green `40f0a0` |
| 13–16 | Delay (steps) |
| 17–26 | **Size** |
| 27–31 | **Hit sound** record (31 = none), table `0x800930A0` (8 bytes: id, —, volume): 0 = id 6 at 90, 1 = id 6 at 50, 2 = id 22 at 90, 3 = id 22 at 50. Three weapons use record 7, past the end of the table (reads id 2048): likely silent `?` (AW-DC/2, WX-C/4, AW-R/4) |

The flash's own update is `0x8008A000` (draw kind 2 billboard).

**Debris word** (`0x8008C164`, on hitting a target): bits 18–22 = **chip count** (≤ 31), bits 0–9
spread, bits 10–17 life. The chips are triangles cut from the target's own model (debris spawner
`0x8008BDF8`, pool kind 3), so they take the target's colours.

## 4. The effect sprite atlas (`0x80091A28`)

297 entries of 12 bytes: clut, tpage, u, v, width − 1, height − 1, R, G, B (128 = neutral). The
pictures are RTIM entry 61 (`gen1-effects.md`), drawn semi-transparent additive. `tools/moa_fx_atlas.py` renders
them; the ones weapons use:

| Index | Picture | Used for |
|---|---|---|
| 0–7 | shot shapes, 32 × 64: 0 glowing capsule, 1 small blobs, 2 banded gradient, 3 lightning, **4 round head with tail**, **5 wide capsule**, 6 two thin lines, **7 diamond with streaks** | streak shots (tinted per weapon) |
| 8–19 | fireball, growing then breaking up | flash kind 0; flamethrower shot (8) |
| 20–31 | hot glow, then embers | flash kind 1 |
| 32–33 | spark spray | flash kind 2 |
| 34 | smoke cloud | AW-XC65 beam (tinted orange) |
| 35–38 | lens flares: white, orange, blue, magenta | flash kinds 4–6; WG-HG770 shot (36) |
| 42–49 | flames | flash kind 3 |
| 50–278 | HUD pieces (tpage 896, 256) | — |
| 282 / 283 | round glow, white / cyan | |
| 284–287 | purple orbs | WR-RS7 shot; the lock-jam sparks (§5) |
| 289–292 | noise, eight sparkles, **ring**, **sparkle dot** | pulse shots (291), handgun / AR / gatling / slug / linear shots (292) |
| 293–296 | smoke puffs: yellow, grey, blue-grey, orange | |

## 5. Impact, knockback and stun

### Damage (unchanged, `battle-system.md` ✔)
`raw = attack × (shell or energy factor) >> 6`, damage = raw × 2 ÷ 3. No distance fall-off.

### What a hit does to the target (on-hit callback `0x80082430`, after the AP write)

1. **Rumble**: the target's pad vibrates, strength = damage × 128 ÷ 1000 + 128, 5 steps
   (`0x800174E4`; skipped in replays).
2. **Damage indicator**: player block +0x24E = 40. AP at 0 → wreck (`0x80079774`).
3. **Impact 0 = nothing else.** Every energy weapon in the game has impact 0, so **energy weapons
   never knock back or stun**; the light machine guns (MGA1, MG500, AR1000, …) don't either.
4. **Knockback level** = impact × 256 ÷ (AC +0x16C), 0–5 (`0x80079AA4`). AC +0x16C = stability
   stat × 25 ÷ 4, and the stability stat (32–255) = legs stability × carried weight ÷ about
   4,070,000 (+800 legs stability with SP-ABS, +1,600 with SP-ABS_Re; `acr_gen1_compile.c`).
   So **level ≈ impact × 41 ÷ stability stat**: the default AC (38) gets ≈ impact × 1.08, a
   stat of 128 ≈ impact × 0.32, 255 ≈ impact × 0.16. Level 0 → no knockback at all.
5. With level ≥ 1:
   - The booster rise flags (AC +0x150, +0x164) are cleared `?` (boost interrupted).
   - **Push**: along the line from the hit point to the target (horizontal) plus the shot's
     direction ÷ 16; speed = impact × 3,276 × 8 >> 12, scaled by the unit vector ÷ 8192 (so
     **about impact × 3.2 units a step**), capped per axis at the AC's width (100), less 2.
     Rifle: 7 a step = the measured −448 ✔. Big hits (impact ≥ 31) all hit the cap.
   - **Ground**: state **0x80**, a **directional hit animation** by the hit's side (37 / 38 / 39
     / 40: front, back, the two sides, from ((angle − 512) & 4095) >> 10), the AC is flagged busy
     (AC +0x36 bit 0x10: no blade swing), vertical push dropped, and the **stagger handler
     `0x8007FECC`** takes over: no movement input until the animation ends.
     **AC +0xDA = the level**, counted down 1 a step; at 0 the busy flag clears. The rest of the
     animation still plays out before control returns (its length is in the motion data, not
     read `?`).
   - **Airborne** (AC +0x46 = 0xC0, or a strong upward push): state **0x81**, air hit
     animations 41–44, full push including half the vertical, handler `0x8007BC1C` with the
     air movement routine.
   - **In the cannon stance** (state 34): pushed, stays crouched (`0x8007B454`).
6. The cannon kick (§2) is the firing AC's own recoil, separate from impact.

So: **impact decides whether and how hard you're pushed; the level (impact vs. your stability)
decides how long you're flagged busy.** The real lockout is the hit animation; how long each of
37–44 runs, and whether the level changes it, needs a live check.

### Special flags (detail +0x12)

**0x40 — lock jammer (WR-RS7 "special rocket" only).** On a hit, AC +0x3A = the hit's attack
(200), **halved if the target's head has a NOISE CANCELER** (player block +0x284 bit 0 clear;
`0x800796A4`). While it counts down (1 a step, `0x8007CF28`):
- every step **all missile locks are wiped** (`0x80080084` from the AC update `0x800830CC`), and
- the **gun lock is skipped** (`0x8008012C` in the gun lock `0x800800E4`);
- purple electric orbs (atlas 284–287) crackle around the AC.

So a hit jams the target's FCS for **200 steps ≈ 9.1 s** (≈ 4.5 s with a Noise Canceler). It
also does its normal damage (attack 200, solid) and impact 59 (max knockback on most ACs). This
is the only fight effect of the NOISE CANCELER found so far; `parts-tables.md` says it has none.

**0x80 — burn (WG-FGI-00 "flame shell" only).** On a hit, AC +0x3C = 100 (steps) and AC +0x3E =
damage × 4 >> 4 (= damage ÷ 4, in AP quarters). Every step while +0x3C runs (`0x8007D038`): AP −=
+0x3E (no defence), flames and chips spawn on the AC, and the on-hit callback runs with impact 0
(rumble, indicator, no knockback). **Total burn = 100 × damage ÷ 16 = 6.25 × the hit's damage**,
over 4.5 s. A new hit restarts it. Default AC: 56 on the hit + about 350 burning.

## 6. Every weapon

Generated from the disc by `tools/moa_weapon_map.py` (full columns in its CSV). **Reload** = fire
interval in steps (seconds), "+hold" = also a post-shot hold (`parts-tables.md`). **KB lvl** at a
stability stat of 38 (default AC) and 128. **Push** in units a step. **Tint** = the shot's RGB
(\* = the sprite's own colour). Sound ids as in `sound-map.md` (§7 below). Hit effect = flash
kind, size, light colour, hit sound id. 

### Hand weapons (right arm)

| Weapon | Type | Atk | Dmg | Impact | KB lvl (38 / 128) | Push | Reload | Speed | Shot look | Colour | Fire snd | Hit effect |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| WG-RF35 | RIFLE | 218 | solid | 3 | 3 / 0 | 7 | 5 (0.23 s) +hold | 700 | 4 round head with tail | `fa64fa` | 4 | hot glow 160, light orange, id 6 |
| WG-MGA1 | MACHINE GUN | 85 | solid | 0 | 0 / 0 | 0 | 1 (0.05 s) | 700 | 4 round head with tail | `fa64fa` | 48 | spark spray 160, light orange, id 6 |
| WG-MG500 | MACHINE GUN | 135 | solid | 0 | 0 / 0 | 0 | 2 (0.09 s) | 700 | 4 round head with tail | `fa9600` | 48 | spark spray 260, light orange, id 6 |
| WG-AR1000 | MACHINE GUN | 105 | solid | 0 | 0 / 0 | 0 | 1 (0.05 s) | 800 | 292 sparkle dot | `808080`\*; glow hot glow frame 0 `787878` | 4 | hot glow 160, light orange, id 6 |
| WG-HG235 | HAND GUN ×3 | 226 | solid | 3 | 3 / 0 | 7 | 5 (0.23 s) | 720 | 292 sparkle dot | `808080`\*; glow hot glow frame 1 `787878` | 4 | spark spray 200, light none, id 6 |
| WG-RF/5 | SNIPER RIFLE | 795 | solid | 7 | 5 / 2 | 20 | 10 (0.45 s) | 780 | model 48 |  | 4 | hot glow 320, light orange, id 6 |
| WG-RF/P | SNIPER RIFLE | 918 | solid | 9 | 5 / 2 | 26 | 12 (0.55 s) | 840 | model 48 |  | 4 | hot glow 288, light green, id 6 |
| WG-HG512 | HAND GUN | 437 | solid | 4 | 4 / 1 | 10 | 8 (0.36 s) | 800 | 292 sparkle dot | `808080`\*; glow hot glow frame 1 `3c3cf0` | 4 | fireball 200, light orange, id 6 |
| WG-FG99 | FLAMETHROWER | 512 | solid | 0 | 0 / 0 | 0 | 1 (0.05 s) | 88 | 8 fireball frame 0 | `808080`\* | 53 | fireball 240, light orange, id 6 |
| WG-B2120 | BAZOOKA | 1730 | solid | 15 | 5 / 4 | 45 | 16 (0.73 s) | 512 | model 54 |  | 53 | fireball 280, light orange, id 6 |
| WG-B2180 | BAZOOKA | 2330 | solid | 28 | 5 / 5 | 87 | 22 (1.0 s) | 576 | model 54 |  | 53 | fireball 512, light orange, id 6 |
| WG-XP1000 | PULSE RIFLE | 302 | energy | 0 | 0 / 0 | 0 | 3 (0.14 s) | 800 | 291 ring | `808080`\*; glow ring `202080` | 50 | hot glow 160, light blue, id 6 |
| WG-XP2000 | PULSE RIFLE | 435 | energy | 0 | 0 / 0 | 0 | 6 (0.27 s) | 840 | 291 ring | `808080`\*; glow ring `401860` | 50 | hot glow 160, light blue, id 6 |
| WG-XC4 | PLASMA RIFLE | 820 | energy | 0 | 0 / 0 | 0 | 10 (0.45 s) | 900 | 5 wide capsule | `60a0f0` | 51 | hot glow 140, light blue, id 6 |
| WG-1-KARASAWA | LASER RIFLE | 1550 | energy | 0 | 0 / 0 | 0 | 8 (0.36 s) | 900 | 5 wide capsule | `60f0a0` | 51 | hot glow 256, light green, id 6 |
| WG-RFM118 | RIFLE | 340 | solid | 0 | 0 / 0 | 0 | 5 (0.23 s) | 700 | 4 round head with tail | `fa64fa` | 4 | hot glow 160, light orange, id 6 |
| WG-XFwPPk | LASER RIFLE | 1120 | energy | 0 | 0 / 0 | 0 | 10 (0.45 s) | 900 | 5 wide capsule | `60f0a0` | 51 | hot glow 256, light green, id 6 |
| WG-HG1 | HAND GUN ×3 | 280 | solid | 10 | 5 / 3 | 29 | 8 (0.36 s) | 720 | 292 sparkle dot | `808080`\*; glow hot glow frame 1 `787878` | 4 | spark spray 200, light none, id 6 |
| WG-PB26 | GRENADE LAUNCHER | 2200 | solid | 40 | 5 / 5 | 98 | 10 (0.45 s) +hold | 512 | model 54 |  | 53 | fireball 280, light orange, id 6 |
| WA-Finger | MACHINE GUN ×5 | 155 | solid | 0 | 0 / 0 | 0 | 2 (0.09 s) | 1000 | 4 round head with tail | `fa64fa` | 4 | hot glow 160, light orange, id 6 |
| WG-HG770 | HAND GUN | 540 | solid | 1 | 1 / 0 | 1 | 7 (0.32 s) | 720 | 36 lens flare (orange) | `b48c50`\* | 4 | spark spray 200, light none, id 6 |
| WG-XW11 | LASER RIFLE ×2 | 549 | energy | 0 | 0 / 0 | 0 | 6 (0.27 s) | 900 | 5 wide capsule | `808020` | 51 | hot glow 140, light yellow-green, id 6 |
| WG-FGI-00 | FLAME SHELL | 200 | solid — burn (0x80) | 0 | 0 / 0 | 0 | 15 (0.68 s) | 448 | model 53 |  | 53 | fireball 240, light orange, id 6 |
| WG-RF/E | SNIPER RIFLE | 3800 | energy | 0 | 0 / 0 | 0 | 48 (2.18 s) | 2700 | 5 wide capsule | `32fa32` | 51 | hot glow 140, light yellow-green, id 6 |
| WG-MG500/E | MACHINE GUN | 185 | energy | 0 | 0 / 0 | 0 | 2 (0.09 s) | 900 | 4 round head with tail | `7878fa` | 48 | spark spray 260, light orange, id 6 |

### Weapon arms

| Weapon | Type | Atk | Dmg | Impact | KB lvl (38 / 128) | Push | Reload | Speed | Shot look | Colour | Fire snd | Hit effect |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| AW-MG25/2 | MACHINE GUN | 158 | solid | 0 | 0 / 0 | 0 | 2 (0.09 s) | 700 | 4 round head with tail | `fa9600` | 48 | spark spray 260, light orange, id 6 |
| AW-GT2000 | GATLING GUN | 305 | solid | 0 | 0 / 0 | 0 | 2 (0.09 s) | 800 | 292 sparkle dot | `808080`\*; glow hot glow frame 0 `787878` | 4 | hot glow 160, light orange, id 6 |
| AW-RF105 | CANNON | 1530 | solid | 17 | 5 / 5 | 52 | 15 (0.68 s) | 448 | model 53 |  | 53 | fireball 240, light orange, id 6 |
| AW-30/3 | DUAL MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 10 (0.45 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| AW-RF120 | CANNON | 2120 | solid | 22 | 5 / 5 | 68 | 18 (0.82 s) | 448 | model 53 |  | 53 | fireball 240, light orange, id 6 |
| AW-S60/2 | DUAL MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 10 (0.45 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| AW-XC5500 | PLASMA CANNON | 1241 | energy | 0 | 0 / 0 | 0 | 7 (0.32 s) | 544 | model 56 |  | 53 | fireball 340, light orange, id 6 |
| AW-XC65 | LASER CANNON | 2322 | energy | 0 | 0 / 0 | 0 | 10 (0.45 s) | 900 | 34 smoke cloud | `ff5a1e` | 51 | hot glow 256, light yellow-green, id 6 |
| AW-DC/2 | GRENADE LAUNCHER | 3822 | solid | 20 | 5 / 5 | 61 | 60 (2.73 s) | 600 | model 49 |  | 0 | fireball 280, light orange, rec 7 (silent?) |
| AW-R/4 | CANNON | 1024 | solid | 5 | 5 / 1 | 13 | 15 (0.68 s) | 576 | model 54 |  | 53 | fireball 180, light orange, rec 7 (silent?) |

### Back weapons

| Weapon | Type | Atk | Dmg | Impact | KB lvl (38 / 128) | Push | Reload | Speed | Shot look | Colour | Fire snd | Hit effect |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| WM-S40/1 | SMALL MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 10 (0.45 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WM-S40/2 | SMALL MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 10 (0.45 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WM-S60/4 | SMALL MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 10 (0.45 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WM-S60/6 | SMALL MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 10 (0.45 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WM-MVG404 | MISSILE | 1560 | solid | 23 | 5 / 5 | 71 | 10 (0.45 s) | thr 70 / top 320 | missile model |  | 2 + 12 | fireball 600, light orange, id 6 |
| WM-MVG802 | MISSILE | 1560 | solid | 23 | 5 / 5 | 71 | 10 (0.45 s) | thr 70 / top 320 | missile model |  | 2 + 12 | fireball 600, light orange, id 6 |
| WM-L201 | LARGE MISSILE | 4300 | solid | 64 | 5 / 5 | 98 | 10 (0.45 s) | thr 60 / top 300 | missile model |  | 2 + 12 | fireball 900, light orange, id 6 |
| WM-X201 | MULTI MISSILE | 980 | solid | 14 | 5 / 4 | 42 | 15 (0.68 s) | thr 60 / top 300 | missile model |  | 2 + 12 | fireball 900, light orange, id 6 |
| WM-X5-AA | BOMB DISPENCER ×8 | 675 | solid | 10 | 5 / 3 | 29 | 50 (2.27 s) | 70 | dropped bomb |  | 4 | fireball 512, light orange, id 6 |
| WM-X10 | BOMB DISPENCER ×16 | 675 | solid | 10 | 5 / 3 | 29 | 50 (2.27 s) | 70 | dropped bomb |  | 4 | fireball 512, light orange, id 6 |
| WM-P4001 | DUAL MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 10 (0.45 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WM-PS-2 | TRIPLE MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 10 (0.45 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WM-AT | LARGE MISSILE | 9830 | solid | 64 | 5 / 5 | 98 | 35 (1.59 s) | thr 30 / top 100 | missile model |  | 2 + 12 | fireball 900, light orange, id 6 |
| WM-TO100 | SMALL MISSILE | 230 | solid | 5 | 5 / 1 | 13 | 5 (0.23 s) | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WM-SMSS24 | MISSILE | 1630 | solid | 30 | 5 / 5 | 93 | 10 (0.45 s) | thr 60 / top 300 | missile model |  | 2 + 12 | fireball 900, light orange, id 6 |
| WM-MVG812 | SMALL MISSILE | 830 | solid | 12 | 5 / 3 | 36 | 30 (1.36 s) | thr 30 / top 100 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WM-X15-EX | PLASMA DISPENSER ×4 | 1560 | energy | 10 | 5 / 3 | 29 | 15 (0.68 s) | 120 | dropped bomb |  | 4 | hot glow 256, light orange, id 6 |
| WR-S50 | SMALL ROCKET | 1310 | solid | 19 | 5 / 5 | 58 | 8 (0.36 s) | 448 | model 53 |  | 53 | fireball 240, light orange, id 6 |
| WR-S100 | SMALL ROCKET | 1310 | solid | 19 | 5 / 5 | 58 | 12 (0.55 s) | 448 | model 53 |  | 53 | fireball 240, light orange, id 6 |
| WR-M50 | ROCKET | 2240 | solid | 33 | 5 / 5 | 98 | 12 (0.55 s) | 512 | model 54 |  | 53 | fireball 280, light orange, id 6 |
| WR-M70 | ROCKET | 2240 | solid | 33 | 5 / 5 | 98 | 16 (0.73 s) | 512 | model 54 |  | 53 | fireball 280, light orange, id 6 |
| WR-L24 | LARGE ROCKET | 3980 | solid | 59 | 5 / 5 | 98 | 16 (0.73 s) | 576 | model 54 |  | 53 | fireball 512, light orange, id 6 |
| WR-RS7 | SPECIAL ROCKET | 200 | solid — lock jammer (0x40) | 59 | 5 / 5 | 98 | 40 (1.82 s) | 512 | 284 purple orbs | `4040a0`\*; glow round glow (cyan) `4040a0` | 53 | spark spray 150, light none, id 6 |
| WRR-10 | MINE DISPENSER | 2240 | solid | 59 | 5 / 5 | 98 | 8 (0.36 s) |  | rigid model |  | 53 |  |
| WC-CN35 | CHAIN GUN | 338 | solid | 0 | 0 / 0 | 0 | 2 (0.09 s) | 500 | 7 diamond with streaks | `fa3200` | 4 | hot glow 160, light orange, id 6 |
| WC-ST120 | SLUG GUN ×7 | 183 | solid | 1 | 1 / 0 | 1 | 22 (1.0 s) | 700 | 292 sparkle dot | `808080`\*; glow lens flare (magenta) `181818` | 4 | hot glow 240, light orange, id 6 |
| WC-LN350 | LINEAR GUN | 690 | solid | 3 | 3 / 0 | 7 | 6 (0.27 s) | 720 | 292 sparkle dot | `808080`\*; glow lens flare (magenta) `181818` | 4 | hot glow 240, light orange, id 6 |
| WC-GN230 | GRENADE LAUNCHER | 3520 | solid | 52 | 5 / 5 | 98 | 32 (1.45 s) +hold | 600 | model 49 |  | 0 |  |
| WC-XP4000 | PULSE CANNON | 770 | energy | 0 | 0 / 0 | 0 | 5 (0.23 s) | 840 | 291 ring | `808080`\*; glow ring `401860` | 50 | hot glow 160, light blue, id 6 |
| WC-XC8000 | PLASMA CANNON | 2065 | energy | 0 | 0 / 0 | 0 | 10 (0.45 s) | 900 | 5 wide capsule | `60f0a0` | 51 | hot glow 256, light green, id 6 |
| WC-01QL | LASER CANNON | 1531 | energy | 0 | 0 / 0 | 0 | 7 (0.32 s) | 544 | model 56 |  | 53 | fireball 340, light orange, id 6 |
| WC-SPGUN | SLUG GUN ×7 | 208 | energy | 2 | 2 / 0 | 4 | 8 (0.36 s) | 700 | 292 sparkle dot | `808080`\*; glow lens flare (magenta) `181818` | 4 | hot glow 240, light orange, id 6 |
| WC-IR24 | LASER CANNON | 3025 | energy | 48 | 5 / 5 | 98 | 40 (1.82 s) +hold | 800 | model 56 |  | 53 |  |
| WX-S800/2 | DUAL MISSILE | 1120 | solid | 16 | 5 / 5 | 49 | 12 (0.55 s) +hold | thr 80 / top 350 | missile model |  | 2 + 12 | fireball 400, light orange, id 6 |
| WX-S800-GF | DUAL MISSILE | 1120 | solid | 16 | 5 / 5 | 49 | 10 (0.45 s) +hold | thr 70 / top 320 | missile model |  | 2 + 12 | fireball 600, light orange, id 6 |
| XCS-9900 | MULTI MISSILE | 980 | solid | 14 | 5 / 4 | 42 | 15 (0.68 s) +hold | thr 60 / top 300 | missile model |  | 2 + 12 | fireball 900, light orange, id 6 |
| WX-ED2 | PLASMA CANNON | 572 | energy | 0 | 0 / 0 | 0 | 6 (0.27 s) +hold | 384 | model 60 |  | 0 | hot glow 320, light orange, id 6 |
| WX-C/4 | LASER CANNON | 1259 | energy | 0 | 0 / 0 | 0 | 32 (1.45 s) +hold | 600 | model 65 |  | 0 | fireball 280, light orange, rec 7 (silent?) |
### Same picture, different colour

One row per picture; every weapon that draws it and the colour it gets. Streak shots multiply the
grey picture by the weapon's own tint; billboard shots take the atlas entry's colour; trailing
glows take the B record's +28 colour.

| Picture | Colour: weapons (layer) |
|---|---|
| sparkle dot | `808080`: WG-AR1000, WG-HG235, WG-HG512, WG-HG1, WC-ST120, WC-LN350, WC-SPGUN, AW-GT2000 (shot) |
| round head with tail | `7878fa`: WG-MG500/E (shot)<br>`fa64fa`: WG-RF35, WG-MGA1, WG-RFM118, WA-Finger (shot)<br>`fa9600`: WG-MG500, AW-MG25/2 (shot) |
| ring | `202080`: WG-XP1000 (trailing glow)<br>`401860`: WG-XP2000, WC-XP4000 (trailing glow)<br>`808080`: WG-XP1000, WG-XP2000, WC-XP4000 (shot) |
| wide capsule | `32fa32`: WG-RF/E (shot)<br>`60a0f0`: WG-XC4 (shot)<br>`60f0a0`: WG-1-KARASAWA, WG-XFwPPk, WC-XC8000 (shot)<br>`808020`: WG-XW11 (shot) |
| lens flare (35-38) | `181818`: WC-ST120, WC-LN350, WC-SPGUN (trailing glow)<br>`b48c50`: WG-HG770 (shot) |
| hot glow frame 1 | `3c3cf0`: WG-HG512 (trailing glow)<br>`787878`: WG-HG235, WG-HG1 (trailing glow) |
| hot glow frame 0 | `787878`: WG-AR1000, AW-GT2000 (trailing glow) |
| fireball frame 0 | `808080`: WG-FG99 (shot) |
| purple orbs | `4040a0`: WR-RS7 (shot) |
| round glow (cyan) | `4040a0`: WR-RS7 (trailing glow) |
| diamond with streaks | `fa3200`: WC-CN35 (shot) |
| smoke cloud | `ff5a1e`: AW-XC65 (shot) |

**Model shots** (colour is in the model's texture; same number = same look):

| Model | Weapons |
|---|---|
| 48 | WG-RF/5, WG-RF/P |
| 49 | WC-GN230, AW-DC/2 |
| 53 | WG-FGI-00, WR-S50, WR-S100, AW-RF105, AW-RF120 |
| 54 | WG-B2120, WG-B2180, WG-PB26, WR-M50, WR-M70, WR-L24, AW-R/4 |
| 56 | WC-01QL, WC-IR24, AW-XC5500 |
| 60 | WX-ED2 |
| 65 | WX-C/4 |

**Recoloured atlas entries** (one picture, several colours or palettes): 35–38 lens flare white `787878` / orange `b48c50` / blue `5050dc` / magenta `dc50dc`; 282 / 283 round glow white `808080` / cyan `20a0a0`; 293–296 smoke puff in four palettes. The impact flashes use the atlas colour, so their only per-weapon colour is the **light** they cast (flash word bits 10–12).

## 7. Sounds per weapon (summary)

Fire sound ids from the shot tables (names by ear, `gen1-sound.md` / `sound-map.md`):

| Id | Sample | Weapons |
|---|---|---|
| 4 | rifle shot (v21) | rifles, handguns, AR1000, GT2000, WA-Finger, sniper rifles, slug / linear guns, chain gun, dispensers |
| 48 | machine gun 2 (v15) | MGA1, MG500, MG500/E, AW-MG25/2 |
| 50 | pulse / plasma (v20) | XP1000, XP2000, XP4000 |
| 51 | laser / energy (v7) | XC4, KARASAWA, XFwPPk, XW11, RF/E, XC8000, AW-XC65 |
| 53 | pulse / plasma 2 (v22) | bazookas, rockets, grenade launcher PB26, flamethrower, flame shell, WR-RS7, mines, WC-01QL, WC-IR24, AW cannons and plasma cannon |
| 0 | cannon shot (v12) | WC-GN230, WX-ED2, WX-C/4, AW-DC/2 |
| 2 / 12 | missile launch / flight loop | every missile |

The gun fire routine itself plays a sound only for mines (id 53, `0x80081250`); `sound-map.md`'s "53 with the shot" is that call. Hit sounds come from the
flash word: **id 6** (small explosion) at volume 90 or 50 for almost everything; none for
three weapons (record 7). Big explosion (id 7) is the AC wreck, not a weapon hit. So `sound-map.md`'s
open item "projectile kind → weapon" is settled by this table.

## 8. Handed over

To the data chat (`parts-tables.md`, `stat-block.md`):
- **NOISE CANCELER does something in fights**: it halves the WR-RS7 lock jam (stat +0x34 bit 0,
  `0x800796A4`). The "no effect in fights" line needs changing.
- Detail +0x12 bit 0x40 = **lock jammer**, 0x80 = **burn** (both now read, §5).
- Stability stat ≈ legs stability × carried weight ÷ 4.07 M (from `acr_gen1_compile.c`).

To the battle chat (`battle-system.md`):
- AC +0x3A is the **lock-jam timer**, not a stagger; +0x3C / +0x3E are the **burn** timer and
  amount; AC +0xDA = knockback level countdown (busy flag); states 0x80 / 0x81 with hit
  animations 37–40 (ground, by side) / 41–44 (air). Push ≈ impact × 3.2 a step, capped at 98.
- On-hit rumble strength = damage × 128 ÷ 1000 + 128.
- Cannons kick the shooter back by player block +0x214 × cannon table +2 >> 8.

To the renderer chat (`renderer.md`):
- The atlas `0x80091A28` has **297** entries (0–49 effects, 50–278 HUD, 279–296 more effects).
  Kind 1 colour = pool +0xC4; kind 2 colour = the atlas entry; kind 6 colour = pool +0xBC
  (trailing glows). Flashes add a light to the per-area light list (`0x8005B96C`).

To the sound chat (`sound-map.md`): §7 above; hit sound table `0x800930A0` is 4 records
(id, —, volume), the flash word's bits 27–31 pick one.

To Armored-CoreR (`gen1-effects.md`, `gen1-weapons.md`): every weapon's shot picture, tint,
glow and hit flash are in the CSV from `tools/moa_weapon_map.py` (the play-test's "rifles fire a purple-ish blob" is the
round-head sprite tinted `fa64fa`); knockback push and the jam / burn rules in §5.

## 9. Open items

- **Live checks**: the stun length (state 0x80 duration) for each knockback level and hit side;
  the burn total on the default AC (expected 56 + ~350); the jam time (200 steps) with and
  without a Noise Canceler; the push distance after a big hit.
- Timed shells (`0x8008B1C0`: WC-GN230, WC-IR24): draw kind and hit effect not read.
- Dispensers (E) and mines (F): picture fields, the mine trigger (`0x8008D0A4`).
- B records +16 / +18 (shot animation) and A records +18, +19, +20, +22, +23.
- Flash record 7 (three weapons): confirm silent in game.
- The arena / missions engines: same tables expected at other addresses (pattern match only).
