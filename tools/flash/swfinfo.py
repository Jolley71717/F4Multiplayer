#!/usr/bin/env python3
# What a movie (SWF) holds, without a decompiler: its version and stage size, its exported classes, and the string
# pool of its ActionScript 3 (class, member, event and instance names, text keys). Enough to find how a game menu is
# built and what to call (docs/UI.md).
#
#   python3 -I tools/flash/swfinfo.py MainMenu.swf | grep -i list
import struct
import sys
import zlib

data = open(sys.argv[1], 'rb').read()
body = zlib.decompress(data[8:]) if data[:3] == b'CWS' else data[8:]
if data[:3] not in (b'CWS', b'FWS'):
    sys.exit('not an uncompressed or zlib movie')
bits = ''.join(f'{b:08b}' for b in body[:20])
n = int(bits[:5], 2)
xmin, xmax, ymin, ymax = (int(bits[5 + i * n:5 + (i + 1) * n], 2) for i in range(4))
print(f'# SWF {data[3]}, stage {(xmax - xmin) / 20:g} x {(ymax - ymin) / 20:g}')


def u30(b, i):
    result = shift = 0
    while True:
        byte = b[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, i


p = (5 + n * 4 + 7) // 8 + 4
while p < len(body):
    (header,) = struct.unpack_from('<H', body, p)
    p += 2
    code, length = header >> 6, header & 0x3F
    if length == 0x3F:
        (length,) = struct.unpack_from('<I', body, p)
        p += 4
    tag = body[p:p + length]
    p += length
    if code == 0:
        break
    if code == 76:  # SymbolClass: the classes the movie exports, with their symbols
        (count,) = struct.unpack_from('<H', tag, 0)
        i = 2
        for _ in range(count):
            i += 2
            end = tag.index(b'\0', i)
            print('# class', tag[i:end].decode('utf8', 'replace'))
            i = end + 1
    if code in (72, 82):  # DoABC: the string pool comes after the int, uint and double pools
        abc = tag[tag.index(b'\0', 4) + 1:] if code == 82 else tag
        i = 4
        for width in (None, None, 8):
            count, i = u30(abc, i)
            for _ in range(max(count - 1, 0)):
                if width:
                    i += width
                else:
                    _, i = u30(abc, i)
        count, i = u30(abc, i)
        for _ in range(max(count - 1, 0)):
            size, i = u30(abc, i)
            print(abc[i:i + size].decode('utf8', 'replace'))
            i += size
