#!/usr/bin/env python3
# Extracts files from a Fallout 4 general archive (BA2, GNRL) whose names match a pattern, to read the game's own menu
# movies (docs/UI.md). Reads only; what it writes stays out of the repo (the game's files are Bethesda's).
#
#   python3 -I tools/flash/ba2extract.py "<game>/Data/Fallout4 - Interface.ba2" <out dir> 'mainmenu\.swf$'
import os
import re
import struct
import sys
import zlib

path, out, pattern = sys.argv[1], sys.argv[2], re.compile(sys.argv[3], re.I)
with open(path, 'rb') as f:
    magic, version, kind, count, names_at = struct.unpack('<4sI4sIQ', f.read(24))
    if magic != b'BTDX' or kind != b'GNRL':
        sys.exit(f'not a general BA2: {magic} {kind}')
    if version in (2, 3):  # Starfield's headers are longer; Fallout 4's (1, 7, 8) are not
        f.read(8 if version == 2 else 12)
    records = [struct.unpack('<I4sIIQIII', f.read(36)) for _ in range(count)]
    f.seek(names_at)
    names = []
    for _ in range(count):
        (length,) = struct.unpack('<H', f.read(2))
        names.append(f.read(length).decode('latin1'))
    for name, record in zip(names, records):
        if not pattern.search(name):
            continue
        offset, packed, size = record[4], record[5], record[6]
        f.seek(offset)
        data = f.read(packed or size)
        if packed:
            data = zlib.decompress(data)
        target = os.path.join(out, name.replace('\\', '/'))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'wb') as w:
            w.write(data)
        print(name, len(data))
