#!/usr/bin/env python3
"""Write scripts/usb-abi-21H461.json: the class layouts QcomDwc3 derives from on iPadOS 17.7.11 (21H461).

The counterpart of UniXNU-sheng's gen-usb27-abi.py / gen-usb27-host-abi.py (iPadOS 27, 24A446, arm64e fileset). This board's
kernelcache is the iPad Pro 12.9" (2nd gen) one, kernelcache.release.ipad7: an arm64 (no pointer authentication) split prelinked
cache, so vtable entries are plain pointers and there is no discriminator to match. For each class the script finds
  - the OSMetaClass constructor call in the kext's initialiser (x0 gMetaClass, x1 name, x2 super's gMetaClass via the GOT,
    w3 instance size),
  - getMetaClass() (adrp x0 gMetaClass; add; ret) and the vtable that holds it (slot 7), counted to its terminating zero,
  - the pure virtual slots (___cxa_pure_virtual, the most common entry of the abstract class), i.e. the platform hooks a SoC
    subclass must supply, and which slots Apple's own T8011 subclasses override.

usage: gen-usb17-abi.py KERNELCACHE_DECOMPRESSED [OUTPUT]
  KERNELCACHE_DECOMPRESSED: ../inputs/donor/ipsw/21H461/kernelcache.decompressed (the board's [ipsw] member, decompressed)
Requires UniXNU's kits/XnuPrelink (kc_image.py) on the path (UNIXNU env or ../sources/UniXNU-next).
"""
import hashlib
import json
import os
import struct
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
UNIXNU = Path(os.environ.get('UNIXNU', HERE.parents[1] / 'sources/UniXNU-next'))
sys.path.insert(0, str(UNIXNU / 'kits/XnuPrelink'))
from kc_image import Image, signed  # noqa: E402

CLASSES = ['IOUSBDeviceController', 'AppleUSBXDCI', 'AppleT8011USBXDCI',
           'AppleUSBHostController', 'AppleUSBXHCI', 'AppleNubT8011USBXHCI']
PAIRS = [('AppleUSBXDCI', 'IOUSBDeviceController'), ('AppleT8011USBXDCI', 'AppleUSBXDCI'),
         ('AppleUSBXHCI', 'AppleUSBHostController'), ('AppleNubT8011USBXHCI', 'AppleUSBXHCI')]


def main():
    path = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / 'usb-abi-21H461.json'
    raw = path.read_bytes()
    img = Image(raw)
    seg = {s['name']: s for s in img.segments}

    def u64(va):
        return struct.unpack('<Q', img.read(va, 8))[0]

    def words(s):
        data = raw[s['fileoff']:s['fileoff'] + s['filesize']]
        return struct.unpack_from('<%dI' % (len(data) // 4), data)

    # Kext headers in __PRELINK_TEXT, their C strings, and every metaclass constructor call.
    pt = seg['__PRELINK_TEXT']
    meta = {}
    for off in range(pt['fileoff'], pt['fileoff'] + pt['filesize'], 4):
        if raw[off:off + 4] != b'\xcf\xfa\xed\xfe':
            continue
        try:
            k = Image(raw, off)
        except ValueError:
            continue
        strs = {}
        for sec in k.sections:
            if sec['name'] == '__cstring':
                try:
                    data, pos = img.read(sec['va'], sec['size']), 0
                except ValueError:
                    continue
                for part in data.split(b'\0'):
                    strs[sec['va'] + pos] = part.decode('latin1')
                    pos += len(part) + 1
        for s in k.segments:
            if 'EXEC' not in s['name'] or not s['size']:
                continue
            code = img.read(s['va'], s['size'])
            w = struct.unpack_from('<%dI' % (len(code) // 4), code)
            regs, pg = {}, {}
            for i, x in enumerate(w):
                pc = s['va'] + 4 * i
                if x & 0x9f000000 == 0x90000000:
                    pg[x & 31] = (pc & ~4095) + (signed(((x >> 5) & 0x7ffff) << 2 | ((x >> 29) & 3), 21) << 12)
                elif x & 0xff800000 == 0x91000000 and ((x >> 5) & 31) in pg:
                    regs[x & 31] = pg[(x >> 5) & 31] + ((x >> 10) & 0xfff)
                elif x & 0xffc00000 == 0xf9400000 and ((x >> 5) & 31) in pg:
                    try:
                        regs[x & 31] = u64(pg[(x >> 5) & 31] + ((x >> 10) & 0xfff) * 8)
                    except ValueError:
                        pass
                elif x & 0x7f800000 == 0x52800000:
                    regs[('w', x & 31)] = ((x >> 5) & 0xffff) << (16 * ((x >> 21) & 3))
                elif x & 0xfc000000 == 0x94000000:
                    name = strs.get(regs.get(1)) if isinstance(regs.get(1), int) else None
                    if name in CLASSES and ('w', 3) in regs and isinstance(regs.get(0), int):
                        meta[name] = {'kext_header': hex(pt['va'] + off - pt['fileoff']), 'gMetaClass': regs[0],
                                      'super_gMetaClass': regs.get(2), 'instance_size': regs[('w', 3)]}
                    regs, pg = {}, {}
                elif x == 0xd65f03c0 or x & 0xfc000000 == 0x14000000:
                    regs, pg = {}, {}
    missing = [c for c in CLASSES if c not in meta]
    if missing:
        raise SystemExit(f'metaclass not found: {missing}')

    # getMetaClass() for each class, then the vtable holding it at slot 7.
    by_mc = {m['gMetaClass']: c for c, m in meta.items()}
    getmc = {}
    for name in ('__PLK_TEXT_EXEC', '__TEXT_EXEC'):
        s = seg[name]
        w = words(s)
        for i in range(len(w) - 2):
            a, b, c = w[i], w[i + 1], w[i + 2]
            if a & 0x9f00001f == 0x90000000 and b & 0xffc003ff == 0x91000000 and c == 0xd65f03c0:
                pc = s['va'] + 4 * i
                t = (pc & ~4095) + (signed(((a >> 5) & 0x7ffff) << 2 | ((a >> 29) & 3), 21) << 12) + ((b >> 10) & 0xfff)
                if t in by_mc:
                    getmc[pc] = by_mc[t]
    vt = {}
    for name in ('__PLK_DATA_CONST', '__DATA_CONST'):
        s = seg[name]
        data = raw[s['fileoff']:s['fileoff'] + s['filesize']]
        q = struct.unpack_from('<%dQ' % (len(data) // 8), data)
        for i, v in enumerate(q):
            if v in getmc and i >= 9 and q[i - 8] == 0 and q[i - 9] == 0:
                end = i
                while end < len(q) and q[end]:
                    end += 1
                vt[getmc[v]] = (s['va'] + 8 * (i - 9), list(q[i - 7:end]))
    missing = [c for c in CLASSES if c not in vt]
    if missing:
        raise SystemExit(f'vtable not found: {missing}')

    pure = Counter(vt['AppleUSBXDCI'][1]).most_common(1)[0][0]
    classes = {}
    for c in CLASSES:
        m, (vva, slots) = meta[c], vt[c]
        sup = by_mc.get(m['super_gMetaClass'])
        classes[c] = {'kext_header': m['kext_header'], 'gMetaClass': hex(m['gMetaClass']),
                      'super': sup or (hex(m['super_gMetaClass']) if m['super_gMetaClass'] else None),
                      'instance_size': hex(m['instance_size']), 'vtable': hex(vva), 'slot_count': len(slots),
                      'pure_slots': [i for i, v in enumerate(slots) if v == pure],
                      'slots': [hex(v) for v in slots]}
    for sub, base in PAIRS:
        a, b = vt[base][1], vt[sub][1]
        classes[sub]['overrides_of_' + base] = [i for i in range(len(a)) if a[i] != b[i]]
        classes[sub]['new_slots'] = list(range(len(a), len(b)))
    abi = {'os': 'iPadOS 17.7.11 (21H461)', 'kernelcache_sha256': hashlib.sha256(raw).hexdigest(), 'arch': 'arm64 (no PAC)',
           'cxa_pure_virtual': hex(pure), 'classes': classes}
    out.write_text(json.dumps(abi, indent=1) + '\n')
    for c in CLASSES:
        x = classes[c]
        print(f"{c:24} size {x['instance_size']:>7} slots {x['slot_count']} super {x['super']} pure {x['pure_slots']}")
    for sub, base in PAIRS:
        print(f"{sub} overrides of {base}: {classes[sub]['overrides_of_' + base]} new {classes[sub]['new_slots']}")
    print('wrote', out)


if __name__ == '__main__':
    main()
