#!/usr/bin/env python3
"""Dump Master of Arena (USA disc 1) part tables, raw, for mapping work.

Usage: python3 tools/moa_parts_dump.py "path/to/Disc 1.bin" [head core legs ...]
       python3 tools/moa_parts_dump.py "path/to/Disc 1.bin" weapons
Reads GG/COM/FDAT.T entry 206 (the menu module) straight from the .bin.
See docs/mapping/parts-tables.md for what each column means.
"""
import struct, sys

B = 0x80050DD8
TABLES = [('head', 0x800CE690, 60, 0x800CEAC8), ('core', 0x800CEAC8, 56, 0x800CED30),
          ('legs', 0x800CED30, 72, 0x800CF870), ('arms', 0x800CF870, 52, 0x800CFD50),
          ('hand', 0x800CFD50, 44, 0x800D01C8), ('back', 0x800D01C8, 52, 0x800D0DF8),
          ('blade', 0x800D0DF8, 52, 0x800D0F30), ('boost', 0x800D0F30, 48, 0x800D11D0),
          ('fcs', 0x800D11D0, 96, 0x800D17D0), ('gen', 0x800D17D0, 48, 0x800D1AA0)]


def menu_module(bin_path):
    f = open(bin_path, 'rb')
    sec = lambda n: (f.seek(n * 2352 + 24), f.read(2048))[1]
    root = sec(16)[156:190]
    cur = (struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0])
    for part in ('GG', 'COM', 'FDAT.T'):
        data = b''.join(sec(cur[0] + i) for i in range((cur[1] + 2047) // 2048))
        i, found = 0, None
        while i < len(data):
            l = data[i]
            if l == 0:
                i = (i // 2048 + 1) * 2048
                continue
            name = data[i + 33:i + 33 + data[i + 32]].decode('latin1').split(';')[0]
            if name == part:
                found = (struct.unpack('<I', data[i + 2:i + 6])[0], struct.unpack('<I', data[i + 10:i + 14])[0])
            i += l
        cur = found
    hdr = sec(cur[0])
    a, b = struct.unpack('<HH', hdr[4 + 2 * 205:8 + 2 * 205])
    return b''.join(sec(cur[0] + a + i) for i in range(b - a))


def name_at(d, addr):
    return d[addr - B:addr - B + 18].split(b'\0')[0].decode('latin1')


def main():
    d = menu_module(sys.argv[1])
    want = sys.argv[2:]
    if want == ['weapons']:
        for lab, base, ta, ts, n, off in [('hand', 0x800D1F64, 0x800CFD50, 44, 26, 0),
                                          ('back', 0x800D21D4, 0x800D01C8, 52, 60, 0),
                                          ('weapon arms', 0x800D1DE4, 0x800CF870, 52, 12, 12)]:
            print('==', lab, '(24-byte detail records, bytes in hex)')
            for i in range(n):
                w = d[base - B + 24 * i:base - B + 24 * i + 24]
                nm = name_at(d, ta + ts * (i + off))
                if not nm.startswith('DUMMY') and any(w):
                    print(f'{i:2} {nm:16.16}', w.hex(' '))
        return
    for name, a, s, e in TABLES:
        if want and name not in want:
            continue
        print('==', name, '(16-bit words from byte 0x24)')
        for i in range((e - a) // s):
            r = d[a - B + i * s:a - B + (i + 1) * s]
            nm, ty = (r[o:o + 18].split(b'\0')[0].decode('latin1') for o in (0, 18))
            if not nm.startswith('DUMMY'):
                w = [r[o] | r[o + 1] << 8 for o in range(36, s - 1, 2)]
                print(f'{i:2} {nm:16.16} {ty:15.15}', ' '.join(f'{x:5}' for x in w))


if __name__ == '__main__':
    main()
