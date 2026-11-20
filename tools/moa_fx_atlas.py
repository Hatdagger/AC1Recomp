#!/usr/bin/env python3
"""Render Master of Arena's fight effect sprites (atlas 0x80091A28 in the VS engine) as one PNG.

usage: python3 -I tools/moa_fx_atlas.py RTIM_061.bin FDAT_209.bin out.png [first-last ...]
RTIM entry 61 holds the effect pictures (33 TIMs, placed in VRAM by their own coordinates);
FDAT 209 (loads at 0x80050DD8) holds the 297 atlas entries (12 bytes: clut, tpage, u, v,
w - 1, h - 1, R, G, B; 128 = neutral). Default ranges: 0-49 and 279-296 (50-278 are HUD).
Needs Pillow. See docs/mapping/weapon-map.md section 4.
"""
import struct, sys
from PIL import Image, ImageDraw

B = 0x80050DD8


def load_vram(path):
    vr = [[0] * 1024 for _ in range(512)]
    d = open(path, 'rb').read()
    o = 0
    while o + 8 <= len(d) and struct.unpack_from('<I', d, o)[0] == 0x10:
        fl = struct.unpack_from('<I', d, o + 4)[0]
        o += 8
        for _ in range(2 if fl & 8 else 1):
            ln, x, y, w, h = struct.unpack_from('<IHHHH', d, o)
            for r in range(h):
                for c in range(w):
                    vr[y + r][x + c] = struct.unpack_from('<H', d, o + 12 + 2 * (r * w + c))[0]
            o += ln
    return vr


def c15(v):
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31)


def sprite(vr, eng, i, tint=None):
    r = eng[0x80091A28 - B + 12 * i:0x80091A28 - B + 12 * i + 12]
    clut, tp = struct.unpack('<HH', r[:4])
    u, v, w, h = r[4], r[5], r[6] + 1, r[7] + 1
    rgb = tint or tuple(r[8:11])
    tx, ty = (tp & 15) * 64, ((tp >> 4) & 1) * 256
    bpp = [4, 8, 16, 0][(tp >> 7) & 3]
    cx, cy = (clut & 63) * 16, clut >> 6
    im = Image.new('RGB', (w, h))
    px = im.load()
    for yy in range(h):
        for xx in range(w):
            uu, vv = (u + xx) & 255, (v + yy) & 255
            if bpp == 4:
                idx = (vr[ty + vv][tx + uu // 4] >> ((uu & 3) * 4)) & 15
            else:
                idx = (vr[ty + vv][tx + uu // 2] >> ((uu & 1) * 8)) & 255
            col = vr[cy][cx + idx]
            if col:
                cr = c15(col)
                px[xx, yy] = tuple(min(255, cr[k] * rgb[k] // 128) for k in range(3))
    return im


def main():
    vr = load_vram(sys.argv[1])
    eng = open(sys.argv[2], 'rb').read()
    ranges = sys.argv[4:] or ['0-49', '279-296']
    ids = []
    for rg in ranges:
        a, b = (int(x) for x in rg.split('-'))
        ids += range(a, b + 1)
    cell = 72
    sheet = Image.new('RGB', (cell * 10, (len(ids) + 9) // 10 * (cell + 12)), (20, 20, 24))
    dr = ImageDraw.Draw(sheet)
    for k, i in enumerate(ids):
        im = sprite(vr, eng, i)
        im.thumbnail((64, 64))
        x, y = (k % 10) * cell, (k // 10) * (cell + 12)
        sheet.paste(im, (x + 4, y + 4))
        dr.text((x + 4, y + cell - 2), str(i), fill=(200, 200, 200))
    sheet.save(sys.argv[3])


if __name__ == '__main__':
    main()
