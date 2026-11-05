#!/usr/bin/env python3
"""Add files to a GPU addon made by UniXNU's scripts/make-gpu-addon.py, in place.

  t970-addon-add.py ADDON PATH=CONTENT [PATH=CONTENT ...]

PATH is relative to /private/var/ngpu (the kernel writes each addon file there before its addon mount, sheng-gpu-addon=1), CONTENT
the file's text (an empty value makes an empty file), or @FILE for a local file's bytes (firmware). gts7xlwifi uses it for the AdrenoMetal diagnostic switches under debug/
(NativeBridgeMetalFrontend.mm reads /private/var/ngpu/debug/<name> when a process starts), which make-gpu-addon.py has no option for.
Layout as make-gpu-addon.py writes it: "NGPUADD2", u32 count, u32 0, u64 total bytes; per file u32 path bytes (with its NUL), u32 mode,
u64 size, u64 compressed size, the path padded to 8 bytes, the zlib stream padded to 8 bytes."""
import struct
import sys
import zlib
from pathlib import Path


def pad(data):
    return data + bytes(-len(data) % 8)


def main():
    if len(sys.argv) < 3 or any('=' not in a for a in sys.argv[2:]):
        sys.exit(__doc__)
    addon = Path(sys.argv[1])
    blob = addon.read_bytes()
    if blob[:8] != b'NGPUADD2':
        sys.exit('Not a GPU addon: ' + str(addon))
    count, _, total = struct.unpack('<IIQ', blob[8:24])
    if total != len(blob):
        sys.exit('Addon size does not match its header')
    names, at = set(), 24
    for _ in range(count):
        length, _, _, packed = struct.unpack('<IIQQ', blob[at:at + 24])
        names.add(blob[at + 24:at + 24 + length - 1].decode())
        at += 24 + (length + 7) // 8 * 8 + (packed + 7) // 8 * 8
    body = blob[24:]
    for argument in sys.argv[2:]:
        name, content = argument.split('=', 1)
        encoded = name.encode() + b'\0'
        if name in names or len(encoded) > 160 or '..' in name.split('/') or name.startswith('/'):
            sys.exit('Bad or duplicate addon path ' + name)
        data = Path(content[1:]).read_bytes() if content.startswith('@') else content.encode()
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), 0o644, len(data), len(packed)) + pad(encoded) + pad(packed)
        names.add(name)
        count += 1
        print(f'644 {len(data):>10} {name}')
    addon.write_bytes(b'NGPUADD2' + struct.pack('<IIQ', count, 0, 24 + len(body)) + body)
    print(f'{addon}: {count} files, {24 + len(body)} bytes')


if __name__ == '__main__':
    main()
