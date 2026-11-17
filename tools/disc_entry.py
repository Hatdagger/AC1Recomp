#!/usr/bin/env python3
"""Dump one entry of a Master of Arena archive (.T file) from the disc image.

usage: python3 tools/disc_entry.py "Disc 1.bin" GG/MS/MENU_TMD.T 0 out.bin
       python3 tools/disc_entry.py "Disc 1.bin" GG/COM/FDAT.T 206 menu206.bin

Entry numbering is the game's own (0x800161D0 in the exe): entry N runs from
halfword N to halfword N+1 of the table that starts 2 bytes into the archive's
first sector. FDAT 206 is the menu module (loads at 0x80050DD8); MENU_TMD 0
holds the hub icons (see docs/mapping/menus-hub.md).
"""
import struct
import sys


def main():
    disc, path, n, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    f = open(disc, 'rb')
    sec = lambda k: (f.seek(k * 2352 + 24), f.read(2048))[1]
    root = sec(16)[156:190]
    cur = (struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0])
    for part in path.split('/'):
        data = b''.join(sec(cur[0] + i) for i in range((cur[1] + 2047) // 2048))
        i, found = 0, None
        while i < len(data):
            ln = data[i]
            if ln == 0:
                i = (i // 2048 + 1) * 2048
                continue
            name = data[i + 33:i + 33 + data[i + 32]].decode('latin1').split(';')[0]
            if name == part:
                found = (struct.unpack('<I', data[i + 2:i + 6])[0], struct.unpack('<I', data[i + 10:i + 14])[0])
            i += ln
        if not found:
            sys.exit(f'{part} not found')
        cur = found
    hdr = sec(cur[0])
    a, b = struct.unpack('<HH', hdr[2 + 2 * n:6 + 2 * n])
    blob = b''.join(sec(cur[0] + a + i) for i in range(b - a))
    open(out, 'wb').write(blob)
    print(f'{path} entry {n}: sectors {a}-{b}, {len(blob)} bytes -> {out}')


if __name__ == '__main__':
    main()
