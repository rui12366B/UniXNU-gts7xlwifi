#!/usr/bin/env python3
"""Swap the Turnip library inside an existing GPU addon's plugin bundle and trust its new CDHashes, keeping everything else.

  t970-turnip-variant.py --addon IN --addon-out OUT --trust IN --trust-out OUT [--turnip DYLIB] [--report FILE]

The board's addon carries more than the Metal plugin (audio driver, ADSP/BT/Wi-Fi firmware, the Wi-Fi supplicant service), so it is not
rebuilt with make-gpu-addon.py. Instead the plugin bundle (addons/NativeAdrenoMetal.bundle/*) is read out of IN, its
libvulkan_freedreno.dylib replaced by DYLIB (default: UniXNU-next build/mesa-tu-ios, scripts/build-turnip-darwin.py --target ios),
stripped and ad-hoc signed with 4 KiB pages exactly as UniXNU scripts/build-adreno-metal-plugin.py does (identifier
org.blu-sharky.turnip), the bundle re-sealed (identifier org.blu-sharky.NativeAdrenoMetal; the plugin binary itself is unchanged, but its
CDHash changes with the seal), and the files written back with UniXNU scripts/addon-add-dir.py. The trust bundle gets one new module
with the two new CDHashes (UniXNU scripts/trust-append-module.py); the old ones stay and do no harm. Print the two output SHA-256s:
the trust's goes into board.toml trust_cache_sha256."""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]
UNIXNU = DEVICE.parent / 'sources/UniXNU-next'
PREFIX = 'addons/NativeAdrenoMetal.bundle'


def cdhash(path):
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(path)], capture_output=True, text=True, check=True).stderr
    assert re.search(r'^Page size=4096$', detail, re.M), f'{path}: the signature must use 4 KiB pages'
    return re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--addon', type=Path, required=True)
    parser.add_argument('--addon-out', type=Path, required=True)
    parser.add_argument('--trust', type=Path, required=True)
    parser.add_argument('--trust-out', type=Path, required=True)
    parser.add_argument('--turnip', type=Path, default=UNIXNU / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.addon.resolve() == args.addon_out.resolve() or args.trust.resolve() == args.trust_out.resolve():
        sys.exit('write the variant to new files; the inputs stay as they are')
    # addon-add-dir.py's reader, without running its main().
    namespace = {}
    source = (UNIXNU / 'scripts/addon-add-dir.py').read_text().replace('\nmain()\n', '\n')
    exec(compile(source, 'addon-add-dir.py', 'exec'), namespace)
    files = namespace['read'](str(args.addon))
    bundle_files = [(name, mode, body) for name, mode, body in files if name.startswith(PREFIX + '/')]
    if not any(name == PREFIX + '/libvulkan_freedreno.dylib' for name, _, _ in bundle_files):
        sys.exit(f'{args.addon}: no {PREFIX}/libvulkan_freedreno.dylib')
    with tempfile.TemporaryDirectory(prefix='t970-turnip-') as scratch:
        bundle = Path(scratch) / 'NativeAdrenoMetal.bundle'
        for name, mode, body in bundle_files:
            path = bundle / name[len(PREFIX) + 1:]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(body)
            os.chmod(path, mode)
        turnip = bundle / 'libvulkan_freedreno.dylib'
        shutil.copyfile(args.turnip, turnip)
        os.chmod(turnip, 0o755)
        subprocess.run(['strip', '-x', str(turnip)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.turnip', str(turnip)],
                       check=True)
        turnip_cdhash = cdhash(turnip)
        subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.NativeAdrenoMetal',
                        str(bundle)], check=True)
        subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
        bundle_cdhash = cdhash(bundle)
        subprocess.run([sys.executable, str(UNIXNU / 'scripts/addon-add-dir.py'), str(args.addon), str(args.addon_out), PREFIX,
                        str(bundle)], check=True)
    subprocess.run([sys.executable, str(UNIXNU / 'scripts/trust-append-module.py'), str(args.trust), str(args.trust_out),
                    bundle_cdhash, turnip_cdhash], check=True)
    report = dict(addon_in=str(args.addon), addon_out=str(args.addon_out),
                  addon_sha256=hashlib.sha256(args.addon_out.read_bytes()).hexdigest(),
                  trust_in=str(args.trust), trust_out=str(args.trust_out),
                  trust_sha256=hashlib.sha256(args.trust_out.read_bytes()).hexdigest(),
                  turnip=str(args.turnip), turnip_sha256=hashlib.sha256(args.turnip.read_bytes()).hexdigest(),
                  turnip_cdhash=turnip_cdhash, bundle_cdhash=bundle_cdhash)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
