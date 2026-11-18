import sys, glob, os
from PIL import Image
src, outdir = sys.argv[1], sys.argv[2]
files = sorted(f for f in glob.glob(os.path.join(src, '[01]*.png')))
assert len(files) == 11, files
belts = []; blobs = []
for f in files:
    im = Image.open(f).convert('RGB'); assert im.size == (64, 64)
    cols = sorted({c for c in im.get_flattened_data()}, key=lambda c: (sum(c), c))
    assert len(cols) <= 16
    cols += [cols[-1]] * (16 - len(cols))
    idx = {c: i for i, c in reversed(list(enumerate(cols)))}
    clut = []
    for r, g, b in cols:
        v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
        clut.append(v | 0x8000 if v else 0)   # as the stock hub tile: black 0x0000 (see-through), others with the top bit
    px = list(im.get_flattened_data()); data = bytearray()
    for y in range(64):
        for x in range(0, 64, 2):
            data.append(idx[px[y*64+x]] | (idx[px[y*64+x+1]] << 4))   # left pixel = low nibble
    name = os.path.basename(f)[:-4]
    belts.append(name); blobs.append((clut, bytes(data)))
    # round-trip check
    back = Image.new('RGB', (64, 64)); out = []
    for y in range(64):
        for x in range(64):
            b_ = data[y*32 + x//2]; i = (b_ >> 4) if x & 1 else (b_ & 15)
            v = clut[i] & 0x7FFF
            out.append(((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3))
    back.putdata(out); assert list(back.get_flattened_data()) == px, name
L = []
L.append('/* Generated from hub-rank-tiles.zip (belt/) by tiles2c.py. Do not edit by hand.')
L.append(' * 11 hub background tiles, karate belt colours by arena rank.')
L.append(' * Each tile: 64x64, PS1 4-bit (2048 bytes, 32 bytes per row, left pixel in the low')
L.append(' * nibble) + 16-colour 15-bit palette, written like the stock hub tile (MENU_TIM 0,')
L.append(' * TIM at 0x9260): black = 0x0000 (see-through), every other colour has bit 15 set. */')
L.append('#pragma once\n#include <stdint.h>\n')
L.append('#define MOA_HUB_RANK_TILE_COUNT 11\n')
L.append('static const uint16_t moa_hub_rank_clut[MOA_HUB_RANK_TILE_COUNT][16] = {')
for n, (clut, _) in zip(belts, blobs):
    L.append('    /* %s */\n    {%s},' % (n, ', '.join('0x%04X' % v for v in clut)))
L.append('};\n')
L.append('static const uint8_t moa_hub_rank_pix[MOA_HUB_RANK_TILE_COUNT][2048] = {')
for n, (_, d) in zip(belts, blobs):
    L.append('    /* %s */\n    {' % n)
    for i in range(0, 2048, 16):
        L.append('        ' + ', '.join('0x%02X' % b for b in d[i:i+16]) + ',')
    L.append('    },')
L.append('};\n')
L.append('static const char *const moa_hub_rank_belt_name[MOA_HUB_RANK_TILE_COUNT] = {')
L.append('    ' + ', '.join('"%s"' % n.split('_')[-2] for n in belts) + ',\n};')
open(os.path.join(outdir, 'moa_hub_rank_tiles_data.h'), 'w').write('\n'.join(L) + '\n')
print('\n'.join(belts))
