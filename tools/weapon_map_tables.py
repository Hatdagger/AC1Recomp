#!/usr/bin/env python3
"""Print the per-weapon tables of docs/mapping/weapon-map.md (section 6) from weapons.csv.

Usage: python3 -I tools/weapon_map_tables.py docs/mapping/weapon-map/weapons.csv
(weapons.csv comes from tools/weapon_map.py.)
"""
import csv
import sys


def short_flash(f):
    """'fireball (sprites 8-19), size 280, ..., sound 6 (...) vol 90' -> 'fireball 280, light orange, id 6 / 90'."""
    if not f or f == 'none':
        return f or ''
    if f.startswith(('spark burst', 'burst:', 'explosion:')):
        kind = f.split(':')[0]
        sid = f.split('sound ')[1].split(' ')[0] if 'sound ' in f else '?'
        vol = f.split(' vol ')[1].split(',')[0] if ' vol ' in f else '?'
        return f'{kind}: id {sid} / {vol} (flash not read)' if kind != 'spark burst' else f
    parts = f.split(', ')
    kind = parts[0].split(' (')[0]
    size = next((p.replace('size ', '') for p in parts if p.startswith('size')), '')
    light = next((p.replace('light ', '').split(' ')[0] for p in parts if p.startswith('light')), '')
    if 'record 7' in f:
        snd = 'rec 7 (id 0 loud, once?)'
    else:
        snd = next((p.split(' (')[0].replace('sound ', 'id ') for p in parts if p.startswith('sound')), '')
        vol = f.split(' vol ')[1] if ' vol ' in f else ''
        snd = f'{snd} / {vol}' if vol else snd
    return f'{kind} {size}, light {light}, {snd}'


def main():
    rows = list(csv.DictReader(open(sys.argv[1])))
    out = []
    for title, cat in [('Hand weapons (right arm)', 'hand (R arm)'), ('Weapon arms', 'weapon arms'), ('Back weapons', 'back')]:
        out += ['', f'### {title}', '',
                '| Weapon | Type | Per trigger | Atk | Dmg | Impact | KB lvl (38 / 128) | Push | Reload | Speed | Shot look | Colour | Fire snd | Hit effect |',
                '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
        for r in rows:
            if r['slot'] != cat:
                continue
            spr = r.get('sprite', '') or r.get('draw', '')
            if r['kind'] == 'missile':
                spr = 'missile model'
            if spr.startswith('raw'):
                spr = 'dropped bomb'
            tint = r.get('tint') or ''
            if tint.startswith('('):
                tint = ''
            elif tint:
                tint = '`' + tint.replace(' (sprite colour)', '') + '`' + ('\\*' if 'sprite colour' in tint else '')
            if r.get('glow_sprite'):
                tint += f"; glow {r['glow_sprite'].split(' ', 1)[1]} `{r['glow_tint']}`"
            spd = r.get('speed', '')
            if r['kind'] == 'missile':
                spd = spd.replace('start 100, thrust ', 'thr ').replace(', top ', ' / top ')
            snd = '2 + 12' if r['kind'] == 'missile' else (r.get('fire_sound') or '').split(' ')[0]
            rel = f"{r['reload_ticks']} ({r['reload_s']} s)" + (' +hold' if r.get('post_shot_hold') != '' else '')
            sp = r.get('special', '')
            # Shots per trigger (detail +2 bits 0-1, guns) times bullets per shot (pellets);
            # missiles: per volley from each side.
            pel = r.get('pellets', '')
            if r['kind'] == 'missile':
                per = pel.replace(' per volley', '').replace('+0', '')
            else:
                spt = r.get('shots_per_trigger') or '1'
                if spt.startswith('1 (alt'):
                    per = '1, alternating sides'
                else:
                    per = spt
                    if pel not in ('', '1'):
                        per = f'{spt} × {pel}' if spt != '1' else pel
            out.append(f"| {r['name']} | {r['hud_type']} | {per} | {r['attack']} | {r['damage_type']}{' — ' + sp if sp else ''} | "
                       f"{r['impact']} | {r['knockback_level_default_ac']} / {r['knockback_level_heavy_stab128']} | "
                       f"{r['push_per_step']} | {rel} | {spd} | {spr} | {tint} | {snd} | {short_flash(r.get('impact_flash', ''))} |")
    print('\n'.join(out))


if __name__ == '__main__':
    main()
