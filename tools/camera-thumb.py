#!/usr/bin/env python3
"""Rebuild the camera thumbnail from a boot log (package/T970Camera.cpp stage C4, t970-camera bit 64).

BootLog prints the 64x48 thumbnail (8-bit MSBs of one Bayer site, so one colour channel) as 48 rows
"[t970 camera thumb NN] <128 hex digits>". This reads the newest complete set from a slots.txt or
latest.txt that tools/read-boot-log.py wrote and saves it as a binary PGM, optionally scaled up.

  camera-thumb.py logs/<tag>-<boot id>/latest.txt --out front.pgm --scale 8
"""
import argparse
import re
import sys

ROW = re.compile(r'^\[t970 camera thumb (\d\d)\] ([0-9a-f]{128})\s*$')
W, H = 64, 48


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', help='latest.txt or slots.txt from read-boot-log.py')
    ap.add_argument('--out', default='camera-thumb.pgm')
    ap.add_argument('--scale', type=int, default=8, help='integer upscale (nearest neighbour); 1 = the raw 64x48')
    args = ap.parse_args()
    rows, best = {}, None
    for line in open(args.log, encoding='latin1'):
        if line.startswith('===== slot') or line.startswith('#slot'):
            rows = {}
            continue
        m = ROW.match(line.rstrip('\n'))
        if m:
            rows[int(m.group(1))] = bytes.fromhex(m.group(2))
            if len(rows) == H:
                best = dict(rows)   # the last complete set wins
    if best is None:
        print('camera-thumb: no complete thumbnail (48 rows) in %s' % args.log, file=sys.stderr)
        return 1
    s = max(1, args.scale)
    with open(args.out, 'wb') as f:
        f.write(b'P5\n%d %d\n255\n' % (W * s, H * s))
        for y in range(H):
            line = bytes(v for v in best[y] for _ in range(s))
            for _ in range(s):
                f.write(line)
    flat = b''.join(best[y] for y in range(H))
    print('camera-thumb: %s %dx%d, mean %.1f min %d max %d' % (args.out, W * s, H * s, sum(flat) / len(flat), min(flat), max(flat)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
