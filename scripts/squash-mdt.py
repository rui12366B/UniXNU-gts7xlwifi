#!/usr/bin/env python3
"""Join a split Qualcomm firmware image (NAME.mdt, NAME.b00, NAME.b01, ...) into one NAME.mbn.

  squash-mdt.py DIR/NAME.mdt OUT.mbn

NAME.mdt holds the ELF header and program headers; segment i of the image is NAME.bNN (NN = i, two digits) and goes at that
program header's p_offset, as the kernel's PIL loader and pil-squasher place them. gts7xlwifi uses it for the audio DSP image
(/vendor/firmware_mnt/image/adsp.*, see proprietary-files.txt), which package/T970Adsp.cpp reads as one file."""
import struct
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    mdt = Path(sys.argv[1])
    head = mdt.read_bytes()
    if head[:4] != b'\x7fELF':
        sys.exit(f'{mdt}: not an ELF file')
    is64 = head[4] == 2
    if is64:
        phoff, = struct.unpack_from('<Q', head, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', head, 0x36)
    else:
        phoff, = struct.unpack_from('<I', head, 0x1c)
        phentsize, phnum = struct.unpack_from('<HH', head, 0x2a)
    segments = []
    for i in range(phnum):
        at = phoff + i * phentsize
        if is64:
            _, _, offset, _, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', head, at)
        else:
            _, offset, _, _, filesz, _, _, _ = struct.unpack_from('<8I', head, at)
        segments.append((offset, filesz))
    size = max([len(head)] + [o + f for o, f in segments])
    out = bytearray(size)
    out[:len(head)] = head
    for i, (offset, filesz) in enumerate(segments):
        if not filesz:
            continue
        piece = mdt.with_suffix('.b%02d' % i)
        if not piece.is_file():
            sys.exit(f'{piece}: missing (segment {i}, {filesz} bytes)')
        data = piece.read_bytes()
        if len(data) != filesz:
            sys.exit(f'{piece}: {len(data)} bytes, program header says {filesz}')
        out[offset:offset + filesz] = data
    Path(sys.argv[2]).write_bytes(out)
    print(f'{sys.argv[2]}: {len(out)} bytes, {sum(1 for _, f in segments if f)} segments')


if __name__ == '__main__':
    main()
