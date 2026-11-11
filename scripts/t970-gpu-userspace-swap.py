#!/usr/bin/env python3
"""Replace the GPU userspace inside an existing GPU addon with a fresh plugin build, keeping every other file (Wi-Fi, Bluetooth, audio,
firmware), and trust the new CDHashes.

  t970-gpu-userspace-swap.py --plugin DIR --addon IN --addon-out OUT --trust IN --trust-out OUT [--report FILE]

DIR is UniXNU's build/adreno-metal-plugin (scripts/build-adreno-metal-plugin.py --target ios): NativeAdrenoMetal.bundle/, adrenoprobed,
the daemon plists and trust.json. Out of IN go every addons/NativeAdrenoMetal.bundle/* file (a file the new bundle no longer has must not
linger in the addon), bin/adrenoprobed and daemons/org.blu-sharky.adrenoprobed.plist; in come the new ones, as make-gpu-addon.py lays them
out. The build's stand-in jobs (com.apple.bluetoothd, com.apple.iomfb_fdr_loader) are added only where IN has no such job.
addons/NativeAdrenoMetal.publish and .primelist stay as IN has them. The trust bundle gets one module with trust.json's CDHashes (plugin,
probe daemon, Turnip; UniXNU scripts/trust-append-module.py). Prints both output SHA-256s; the trust's goes into board.toml."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]
UNIXNU = DEVICE.parent / 'sources/UniXNU-main'
BUNDLE_PREFIX = 'addons/NativeAdrenoMetal.bundle/'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--plugin', type=Path, default=UNIXNU / 'build/adreno-metal-plugin')
    parser.add_argument('--addon', type=Path, required=True)
    parser.add_argument('--addon-out', type=Path, required=True)
    parser.add_argument('--trust', type=Path, required=True)
    parser.add_argument('--trust-out', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.addon.resolve() == args.addon_out.resolve() or args.trust.resolve() == args.trust_out.resolve():
        sys.exit('write to new files; the inputs stay as they are')
    namespace = {}
    source = (UNIXNU / 'scripts/addon-add-dir.py').read_text().replace('\nmain()\n', '\n')
    exec(compile(source, 'addon-add-dir.py', 'exec'), namespace)
    read, write = namespace['read'], namespace['write']
    plugin = args.plugin
    trust = json.loads((plugin / 'trust.json').read_text())
    bundle = plugin / 'NativeAdrenoMetal.bundle'
    files = read(str(args.addon))
    present = {name for name, _, _ in files}
    # The build's job plists: its own probe daemon's replaces IN's; the stand-in jobs it also writes (com.apple.bluetoothd and
    # com.apple.iomfb_fdr_loader run adrenoprobed in their place) only go in where IN has none, so a real job another session layered
    # in (the Bluetooth daemon) is kept. -entitlements.plist is a build input, not a job.
    plists = [p for p in sorted(plugin.glob('*.plist')) if not p.name.endswith('-entitlements.plist') and
              (p.name == 'org.blu-sharky.adrenoprobed.plist' or 'daemons/' + p.name not in present)]
    new = []
    for path in sorted(p for p in bundle.rglob('*') if p.is_file()):
        new.append((BUNDLE_PREFIX + path.relative_to(bundle).as_posix(), 0o755 if os.access(path, os.X_OK) else 0o644, path.read_bytes()))
    new.append(('bin/adrenoprobed', 0o755, (plugin / 'adrenoprobed').read_bytes()))
    for plist in plists:
        new.append(('daemons/' + plist.name, 0o644, plist.read_bytes()))
    replaced = {name for name, _, _ in new}
    dropped = [name for name, _, _ in files if name.startswith(BUNDLE_PREFIX) and name not in replaced]
    kept = [f for f in files if not f[0].startswith(BUNDLE_PREFIX) and f[0] not in replaced]
    write(str(args.addon_out), kept + new)
    assert [f[0] for f in read(str(args.addon_out))] == [f[0] for f in kept + new]
    hashes = sorted({trust['cdhash'], trust['daemon_cdhash'], trust['turnip_cdhash']})
    subprocess.run([sys.executable, str(UNIXNU / 'scripts/trust-append-module.py'), str(args.trust), str(args.trust_out), *hashes],
                   check=True)
    report = dict(addon_in=str(args.addon), addon_out=str(args.addon_out),
                  addon_sha256=hashlib.sha256(args.addon_out.read_bytes()).hexdigest(), files=len(kept) + len(new),
                  replaced=sorted(replaced), dropped=dropped, trust_in=str(args.trust), trust_out=str(args.trust_out),
                  trust_sha256=hashlib.sha256(args.trust_out.read_bytes()).hexdigest(), cdhashes=trust)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'replaced'}, indent=2))


if __name__ == '__main__':
    main()
