#!/usr/bin/env python3
"""Master of Arena weapon map: every weapon's numbers, shot, look, sound and hit effect.

Reads the menu module (FDAT 206) for the weapon detail tables and the VS engine (FDAT 209)
for the shot tables. Usage: python3 -I weapon_map.py <FDAT dir> [out.csv]
(FDAT dir = the disc pack's unpacked/GG/COM/FDAT, files 206.bin and 209.bin.)
Every address is the VS engine's (FDAT 209); the other fight engines carry the same tables.
"""
import csv, struct, sys

B = 0x80050DD8
TICK = 22.0  # game steps per second


class Mod:
    def __init__(self, path):
        self.d = open(path, 'rb').read()

    def raw(self, a, n): return self.d[a - B:a - B + n]
    def u8(self, a): return self.d[a - B]
    def s16(self, a): return struct.unpack_from('<h', self.d, a - B)[0]
    def u16(self, a): return struct.unpack_from('<H', self.d, a - B)[0]
    def u32(self, a): return struct.unpack_from('<I', self.d, a - B)[0]
    def name(self, a, n=18): return self.raw(a, n).split(b'\0')[0].decode('latin1').strip()


# ---- what the sound ids are (gen1-sound.md: ear + code) ----
SOUND = {0: 'cannon shot', 4: 'rifle shot', 5: 'cannon shot', 6: 'small explosion / hit',
         7: 'big explosion', 10: 'machine gun', 20: 'rifle shot (quiet)', 21: 'machine gun (quiet)',
         22: 'small explosion / hit (quiet)', 23: 'big explosion (quiet)', 48: 'machine gun 2',
         50: 'pulse / plasma shot', 51: 'laser / energy shot', 52: 'unnamed (sample 06)',
         53: 'pulse / plasma shot 2', 2: 'missile launch', 12: 'missile flight (loop)', 255: 'none'}

# ---- the effect sprite atlas, 0x80091A28, 12 bytes: clut, tpage, u, v, w-1, h-1, r, g, b ----
SPRITE_NAME = {0: 'glowing capsule', 1: 'small blobs', 2: 'banded gradient', 3: 'lightning',
               4: 'round head with tail', 5: 'wide capsule', 6: 'two thin lines', 7: 'diamond with streaks'}
for i in range(8, 20): SPRITE_NAME[i] = f'fireball frame {i - 8}'
for i in range(20, 32): SPRITE_NAME[i] = f'hot glow frame {i - 20}'
SPRITE_NAME.update({32: 'spark spray 0', 33: 'spark spray 1', 34: 'smoke cloud', 35: 'lens flare (white)',
                    36: 'lens flare (orange)', 37: 'lens flare (blue)', 38: 'lens flare (magenta)', 39: 'tall dark (add)',
                    40: 'tall dark', 41: 'shadow (subtract)', 282: 'round glow (white)', 283: 'round glow (cyan)',
                    284: 'purple orbs', 289: 'noise', 290: 'eight sparkles', 291: 'ring', 292: 'sparkle dot',
                    293: 'smoke puff (yellow)', 294: 'smoke puff (grey)', 295: 'smoke puff (blue-grey)',
                    296: 'smoke puff (orange)'})
for i in range(42, 50): SPRITE_NAME[i] = f'flame frame {i - 42}'

# impact flash kinds, 0x800930DC: first sprite, frames, size curve pointer
FLASH = {0: 'fireball (sprites 8-19)', 1: 'hot glow (20-31)', 2: 'spark spray (32-33)',
         3: 'flames (42-49)', 4: 'blue lens flare (37)', 5: 'magenta lens flare (38)', 6: 'orange lens flare (36)', 7: 'none'}
LIGHT = {0: 'orange ff8250', 1: 'yellow-green e6ff64', 2: 'blue 4080f0', 3: 'green 40f0a0', 7: 'none'}
# hit sound records 0x800930A0: id, volume
HIT_SOUND = {0: (6, 90), 1: (6, 50), 2: (22, 90), 3: (22, 50), 31: (255, 0)}


def flash_word(w):
    """Impact word read by 0x8008B60C (shot ends on a wall, the floor or a target)."""
    if w & 7 == 7:
        return 'none'
    snd = (w >> 27) & 31
    sid, vol = HIT_SOUND.get(snd, (None, None))
    s = f"{FLASH[w & 7]}, size {(w >> 17) & 0x3FF}, step {(w >> 3) & 3}, light {LIGHT.get((w >> 10) & 7, (w >> 10) & 7)}"
    if sid is not None:
        s += f", sound {sid} ({SOUND.get(sid, '?')}) vol {vol}"
    else:
        # Only records 0-3 are sound records; 4-7 are the flash size curves (0x800930C0 / D8).
        # Record 7 reads id 2048 at volume 3080: the play routine 0x80053284 uses id & 0xFF = 0
        # (cannon shot) at full volume, but its play-once bit for id 2048 lies past the 32 bytes
        # cleared each frame (0x8005C0D4), so later hits are probably blocked (live check).
        s += f", sound record {snd} (past the 4 sound records: plays id 0 loud once, then likely blocked ?)"
    return s


def debris_word(w):
    """Debris word read by 0x8008C164: chips knocked off the target's model."""
    n = (w >> 18) & 31
    return 'none' if n == 0 else f'{n} chips, spread {w & 0x3FF}, life {(w >> 10) & 0xFF}'


def kb_level(impact, ac16c=237):
    """0x80079AA4: impact x 256 / AC +0x16C, 0..5. AC +0x16C = stability stat x 25 / 4:
    237 for the default AC (stat 38), 800 for a stat of 128."""
    if impact == 0:
        return 0
    return max(0, min(5, impact * 256 // ac16c))


def push(impact):
    """Model of the ground push, fitted to one measurement: impact x 3.2 - 2, at most 98.
    0x80079ADC: push = (impact x 3276 x 8 >> 12) x unit vector >> 13 (about impact x 3.2), added to
    the AC's current velocity; the total is capped per axis at AC +0x28 (+0x2A / 2 vertically).
    The -2 is not in that routine (measured rifle, impact 3: -448 / 64 = 7 a step). Only applies
    when the knockback level is >= 1."""
    if not impact:
        return 0
    return max(0, min((impact * 3276 * 8 >> 12) // 2, 100) - 2)


def main():
    fdat = sys.argv[1].rstrip('/')
    menu, vs = Mod(fdat + '/206.bin'), Mod(fdat + '/209.bin')
    types = [menu.name(0x800D5488 + 16 * i, 16) for i in range(33)]

    def sprite(i):
        r = vs.raw(0x80091A28 + 12 * i, 12)
        return r[8:11].hex()

    def glow(a):
        w = vs.u32(a + 12)
        if not (w >> 24) & 3:
            return {}
        g = 0x80092C00 + 4 * (w >> 26)
        sp = vs.s16(g)
        return dict(glow_sprite=f'{sp} {SPRITE_NAME.get(sp, "?")}', glow_size=vs.s16(g + 2),
                    glow_tint=vs.raw(a + 28, 3).hex())

    def pic(i):
        r = vs.raw(0x80091A28 + 12 * i, 12)
        return f'tp{struct.unpack_from("<H", r, 2)[0]:x} uv{r[4]},{r[5]} {r[6] + 1}x{r[7] + 1}', f'clut{struct.unpack_from("<H", r, 0)[0]:x}'

    rows = []
    for cat, base, ta, ts, n, off in [('hand (R arm)', 0x800D1F64, 0x800CFD50, 44, 26, 0),
                                      ('back', 0x800D21D4, 0x800D01C8, 52, 60, 0),
                                      ('weapon arms', 0x800D1DE4, 0x800CF870, 52, 12, 12)]:
        for i in range(n):
            w = menu.raw(base + 24 * i, 24)
            nm = menu.name(ta + ts * (i + off))
            if nm.startswith('DUMMY') or not any(w) or (w[0x8] == 0 and w[0x9] == 0 and w[0x14] == 0 and w[0x15] == 0):
                continue
            kind, mid, pat = w[0], w[1], w[2]
            atk = struct.unpack_from('<H', w, 0x14)[0]
            imp = w[0x16]
            fl = w[0x12]
            row = dict(slot=cat, name=nm, hud_type=types[w[7]], kind=['gun', 'missile', 'cannon'][kind], model_id=mid,
                       attack=atk, damage_type='energy' if fl & 0x20 else 'solid',
                       special=('lock jammer (0x40)' if fl & 0x40 else '') + ('burn (0x80)' if fl & 0x80 else ''),
                       impact=imp, knockback_level_default_ac=kb_level(imp), knockback_level_heavy_stab128=kb_level(imp, 800), push_per_step=push(imp),
                       ammo=struct.unpack_from('<H', w, 8)[0], energy_per_shot=struct.unpack_from('<H', w, 10)[0],
                       range=struct.unpack_from('<H', w, 14)[0], locks=w[0x10],
                       reload_ticks=w[0x11], reload_s=round(w[0x11] / TICK, 2),
                       post_shot_hold='' if w[4] == 255 else w[4],
                       shots_per_trigger={0: 1, 1: 2, 2: 4, 3: '1 (alternating)'}[pat & 3] if kind == 0 else '')
            fam = shot = ''
            if kind == 0:
                if mid < 20:
                    a = 0x80091198 + 28 * mid
                    fam = f'A bullet streak (0x8008AC30, table 0x80091198 #{mid})'
                    snd = vs.u8(a + 21)
                    rgb = vs.raw(a + 24, 3).hex()
                    row.update(spawner=fam, speed=vs.s16(a + 8), spread=vs.s16(a + 10), draw='streak (pool kind 1)',
                               sprite=f'{vs.s16(a + 16)} {SPRITE_NAME.get(vs.s16(a + 16))}', tint=rgb,
                               width=vs.s16(a + 12), trail_steps=vs.s16(a + 14),
                               pellets=5 if mid == 8 else 1, fire_sound=f'{snd} {SOUND.get(snd, "?")}',
                               impact_flash=flash_word(vs.u32(a)), hit_debris=debris_word(vs.u32(a + 4)))
                elif mid < 40:
                    a = 0x80092980 + 40 * (mid - 20)
                    snd = vs.u8(a + 10)
                    sp = vs.s16(a + 6)
                    cnt = vs.u8(a + 14)
                    row.update(spawner=f'B billboard shot (0x8008DEC0, table 0x80092980 #{mid - 20})',
                               speed=vs.s16(a), spread=vs.s16(a + 2), draw='billboard (pool kind 2)',
                               sprite=f'{sp} {SPRITE_NAME.get(sp, "?")}', tint=sprite(sp) + ' (sprite colour)',
                               width=vs.s16(a + 4), trail_steps='', pellets=cnt,
                               fire_sound=f'{snd} {SOUND.get(snd, "?")}',
                               impact_flash=flash_word(vs.u32(a + 32)) if vs.u8(a + 12) == 1 else f'spark burst (0x8008B7B8: {vs.u8(a + 12)}, {vs.u8(a + 13)})',
                               hit_debris=debris_word(vs.u32(a + 36)),
                               note=('flies 1/4 range, then re-spawns as #11 with 3x range' if mid == 30 else ''),
                               **glow(a))
                elif mid < 60:
                    a = 0x80092C20 + 36 * (mid - 40)
                    snd = vs.u8(a + 8)
                    rigid = (mid - 40) in (6, 7, 11, 12)
                    row.update(spawner=f'C model shot (0x8008E664, table 0x80092C20 #{mid - 40})',
                               speed=vs.s16(a), spread='', draw='rigid model, scale %d (pool kind 4)' % vs.s16(a + 2) if rigid else 'posed model (pool kind 0)',
                               sprite=f'model {vs.s16(a + 6)}', tint='(model texture)', width='', trail_steps='',
                               pellets=1, fire_sound=f'{snd} {SOUND.get(snd, "?")}',
                               impact_flash=flash_word(vs.u32(a + 28)), hit_debris=debris_word(vs.u32(a + 32)),
                               note='second shot from the other side' if mid == 66 else '')
                elif mid < 70:
                    a = 0x80092824 + 24 * (mid - 60)
                    snd = vs.u8(a + 10)
                    sp = vs.s16(a + 6)
                    row.update(spawner=f'D energy streak (0x8008D8CC, table 0x80092824 #{mid - 60})',
                               speed=vs.s16(a), spread=vs.s16(a + 2), draw='streak (pool kind 1)',
                               sprite=f'{sp} {SPRITE_NAME.get(sp, "?")}', tint=vs.raw(a + 12, 3).hex(),
                               width=vs.s16(a + 4), trail_steps='', pellets=2 if mid == 66 else 1,
                               fire_sound=f'{snd} {SOUND.get(snd, "?")}',
                               impact_flash=flash_word(vs.u32(a + 16)), hit_debris=debris_word(vs.u32(a + 20)),
                               note='fires twice (both sides)' if mid == 66 else '')
                elif mid < 90:
                    a = 0x80092910 + 28 * (mid - 70)
                    snd = vs.u8(a + 6)
                    row.update(spawner=f'E dispenser (0x8008CBCC, table 0x80092910 #{mid - 70})',
                               speed=vs.s16(a), spread=f'{vs.s16(a + 10)}/{vs.s16(a + 12)}', draw='billboard',
                               sprite='raw ' + vs.raw(a + 14, 4).hex(' '), tint='', width='', trail_steps='',
                               pellets=vs.s16(a + 8), fire_sound=f'{snd} {SOUND.get(snd, "?")}',
                               impact_flash=flash_word(vs.u32(a + 20)), hit_debris=debris_word(vs.u32(a + 24)),
                               note='dropped behind/below, no aim')
                else:
                    row.update(spawner='F mine (0x8008D38C)', draw='rigid model', fire_sound='53 ' + SOUND[53],
                               impact_flash='explosion: sound 6 (small explosion / hit) vol 127, heard to 12000 (update 0x8008D0A4); flash not read',
                               note='lies on the ground until an AC comes near (update 0x8008D0A4, not traced)')
            elif kind == 2:
                ct = 0x80091310 + 6 * mid
                row.update(cannon_speed=vs.s16(ct), recoil_scale=vs.s16(ct + 2))
                if mid == 0:
                    row.update(spawner='cannon 0: A-style streak, constants in code (0x80081FA0)', speed=vs.s16(ct),
                               draw='streak (pool kind 1)', sprite='7 ' + SPRITE_NAME[7],
                               tint=vs.raw(0x800918E0, 3).hex(), width=20, trail_steps=3, pellets=1,
                               fire_sound='4 ' + SOUND[4], impact_flash=flash_word(vs.u32(0x800918E4)),
                               hit_debris=debris_word(vs.u32(0x800918E8)))
                else:
                    redirect = {1: ('B', 6), 2: ('B', 7), 4: ('B', 5), 5: ('D', 1), 6: ('C', 3)}
                    if mid in redirect:
                        f, k = redirect[mid]
                        row['spawner'] = f'cannon {mid} -> family {f} #{k}'
                        if f == 'B':
                            a = 0x80092980 + 40 * k
                            sp = vs.s16(a + 6); snd = vs.u8(a + 10)
                            row.update(speed=vs.s16(a), spread=vs.s16(a + 2), draw='billboard (pool kind 2)',
                                       sprite=f'{sp} {SPRITE_NAME.get(sp, "?")}', tint=sprite(sp) + ' (sprite colour)', width=vs.s16(a + 4),
                                       pellets=vs.u8(a + 14), fire_sound=f'{snd} {SOUND.get(snd, "?")}',
                                       impact_flash=flash_word(vs.u32(a + 32)) if vs.u8(a + 12) == 1 else f'spark burst ({vs.u8(a + 12)}, {vs.u8(a + 13)})',
                                       hit_debris=debris_word(vs.u32(a + 36)), **glow(a))
                        elif f == 'D':
                            a = 0x80092824 + 24 * k
                            sp = vs.s16(a + 6); snd = vs.u8(a + 10)
                            row.update(speed=vs.s16(a), draw='streak (pool kind 1)', sprite=f'{sp} {SPRITE_NAME.get(sp, "?")}',
                                       tint=vs.raw(a + 12, 3).hex(), width=vs.s16(a + 4), pellets=1,
                                       fire_sound=f'{snd} {SOUND.get(snd, "?")}',
                                       impact_flash=flash_word(vs.u32(a + 16)), hit_debris=debris_word(vs.u32(a + 20)))
                        else:
                            a = 0x80092C20 + 36 * k
                            snd = vs.u8(a + 8)
                            row.update(speed=vs.s16(a), draw='posed model (pool kind 0)', sprite=f'model {vs.s16(a + 6)}',
                                       pellets=1, fire_sound=f'{snd} {SOUND.get(snd, "?")}',
                                       impact_flash=flash_word(vs.u32(a + 28)), hit_debris=debris_word(vs.u32(a + 32)))
                    else:  # 3 grenade, 7 IR laser: timed shell via 0x8008B1C0
                        row.update(spawner=f'cannon {mid}: timed shell 0x8008B1C0 (fuse = flight time to the lock point)',
                                   speed=vs.s16(ct), draw='timed shell model', sprite=f'model {49 if mid == 3 else 56}', pellets=1,
                                   fire_sound='0 cannon shot' if mid == 3 else '53 ' + SOUND[53],
                                   impact_flash='burst: sound 7 (big explosion) vol 127, heard to 12000 (update 0x80089B64); flash not read',
                                   note='bursts at the predicted target point (lead from 0x800155DC); no lock = range / speed; '
                                        'fire sound played by the cannon routine at volume 120')
            else:  # missile
                lt = 0x800912B0 + 8 * mid
                fk = vs.u8(lt)
                ft = 0x80093114 + 40 * fk
                row.update(spawner=f'missile (0x8008B070); launcher #{mid}', draw='posed model (pool kind 0)',
                           speed=f'start 100, thrust {vs.s16(ft + 12)}, top {vs.s16(ft + 14)}',
                           missile_flight_kind=fk, ignition_delay=vs.u8(lt + 1),
                           pellets=f'{vs.u8(lt + 2)}+{vs.u8(lt + 3)} per volley',
                           missile_fuel_life=f'{vs.s16(ft)}/{vs.s16(ft + 2)} steps', missile_turn=vs.s16(ft + 16),
                           fire_sound='2 missile launch; 12 flight loop',
                           impact_flash=flash_word(vs.u32(ft + 32)), hit_debris=debris_word(vs.u32(ft + 36)),
                           note={3: 'splits into 4 after 6 steps', 5: 'never gives up', 4: 'turns during the delay',
                                 11: 'never gives up'}.get(fk, ''))
            spr = str(row.get('sprite', ''))
            if spr.split(' ')[0].isdigit():
                row['picture'], row['palette'] = pic(int(spr.split(' ')[0]))
            rows.append(row)

    keys = []
    for r in rows:
        for k in r:
            if k not in keys:
                keys.append(k)
    out = open(sys.argv[2], 'w', newline='') if len(sys.argv) > 2 else sys.stdout
    wr = csv.DictWriter(out, fieldnames=keys)
    wr.writeheader()
    for r in rows:
        wr.writerow(r)


if __name__ == '__main__':
    main()
