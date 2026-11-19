import re, sys
from PIL import Image
src = open(sys.argv[1]).read()
def arr(name):
    m = re.search(r'%s\[[^\]]*\]\[\d+\]\s*=\s*\{(.*?)\n\};' % name, src, re.S)
    body = m.group(1)
    rows = re.findall(r'\{([^{}]*)\}', body)
    return [[int(x, 0) for x in r.replace('\n', ' ').split(',') if x.strip()] for r in rows]
clut = arr('moa_hub_rank_clut'); pix = arr('moa_hub_rank_pix')
names = re.findall(r'/\* (\d\d_[^ ]+) \*/', src)[:11]
print(len(clut), len(pix), names)
for t in range(11):
    im = Image.new('RGB', (64, 64))
    pal = []
    for c in clut[t]:
        r = (c & 31) << 3; g = ((c >> 5) & 31) << 3; b = ((c >> 10) & 31) << 3
        pal.append((r, g, b))
    mx = max(max(p) for p in pal) or 1
    k = 230 / mx   # brighten the dimmed tile so the pattern reads in a README
    for y in range(64):
        for x in range(64):
            byte = pix[t][y * 32 + x // 2]
            i = (byte >> 4) if x & 1 else (byte & 15)
            p = pal[i]
            im.putpixel((x, y), tuple(min(255, int(v * k)) for v in p))
    im.save('tiles/tile%02d.png' % t)
