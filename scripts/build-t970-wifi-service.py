#!/usr/bin/env python3
"""Stage the Wi-Fi supplicant service for the iPadOS 17 (21H461) payload addon, as UniXNU-sheng scripts/build-wifi-userspace.py.

  build-t970-wifi-service.py --os-root DIR [--out DIR] [--addon IN --addon-out OUT] [--trust IN --trust-out OUT]

The mainline Wi-Fi runtime hands scan and join requests to a userspace wpa_supplicant 2.12 with the native backend (Ath12kWifi
userspace/driver_sheng.c) over the kernel control socket org.unixnu.wifi.control (package/T970WifiStack.cpp). This script
  1. builds it for arm64 (not arm64e) iOS 17.0 (Ath12kWifi tools/build-supplicant.py, whose iOS target is 27.0; output out/wpa-ios17),
  2. signs it ad hoc as org.unixnu.wifi.supplicant (no-container entitlement, 4 KiB pages) -> wifi-bin/unixnu-wpa_supplicant,
  3. writes etc/wifi/wpa_supplicant.conf (networks come over the control channel),
  4. adds the launchd job org.unixnu.wifi.supplicant to the OS's /System/Library/xpc/launchd.plist (all original entries kept) and
     signs the result detached -> addon wifi-xpc/launchd.plist + .sig (UniXNU scripts/build-boot-runtime.py functions),
  5. with --addon/--trust: appends wifi-bin/, etc/wifi/ and xpc/ to a GPU addon (UniXNU scripts/addon-add-dir.py) and the two
     CDHashes (executable, launchd cache) to a trust bundle as one new module (UniXNU scripts/trust-append-module.py).
Addon paths land under /private/var/ngpu; patches/xnu/t970-wifi-service-mount.patch union-mounts xpc/ over /System/Library/xpc and
wifi-bin/ over /usr/libexec when the boot argument t970-wifi-svc=1 is given. launchd_cache_loader checks launchd.plist.sig and the
CDHash in the trust cache: without both, launchd loads no job at all, which is why the mounts need the boot argument.
--os-root is the extracted 21H461 OS (offline-userland-files/os); nothing Apple's is committed, only built into the output."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LABEL = 'org.unixnu.wifi.supplicant'


def module_dir(name, env, candidates):
    for candidate in (os.environ.get(env), *candidates):
        if candidate and Path(candidate).is_dir():
            return Path(candidate).resolve()
    raise SystemExit(f'build-t970-wifi-service: {name} not found; set {env}')


def run(*command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def build_supplicant(module):
    """Ath12kWifi tools/build-supplicant.py for plain arm64 iOS 17.0, in its own output directory.

    Not arm64e: combo10b's arm64e build (ptrauth ABI subtype USR00 from the iOS 27 SDK) never ran; xpcproxy exited 78 (EX_CONFIG)
    before the supplicant started. Every binary this board's addon already runs (adrenoprobed, T970Audio) is plain arm64."""
    path = module / 'tools/build-supplicant.py'
    source = path.read_text()
    for old in ('arm64e-apple-ios27.0', 'out/wpa-{args.target}'):
        if source.count(old) != 1:
            raise SystemExit(f'build-supplicant.py changed at {old!r}')
    source = source.replace('arm64e-apple-ios27.0', 'arm64-apple-ios17.0').replace('out/wpa-{args.target}', 'out/wpa-{args.target}17')
    shutil.rmtree(module / 'out/wpa-ios17', ignore_errors=True)   # make keeps objects across a target change
    script = module / 'out/build-supplicant-ios17.py'
    script.parent.mkdir(exist_ok=True)
    script.write_text(source)
    try:
        run(sys.executable, script, '--target', 'ios')
    finally:
        script.unlink(missing_ok=True)
    binary = module / 'out/wpa-ios17/bin/wpa_supplicant'
    report = json.loads((module / 'out/wpa-ios17/report.json').read_text())
    return binary, report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--os-root', type=Path, required=True, help='extracted 21H461 OS root (System/Library/xpc/launchd.plist)')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/t970-wifi-service')
    parser.add_argument('--addon', type=Path)
    parser.add_argument('--addon-out', type=Path)
    parser.add_argument('--trust', type=Path)
    parser.add_argument('--trust-out', type=Path)
    parser.add_argument('--exec-path', default='/private/var/ngpu/wifi-bin/unixnu-wpa_supplicant',
                        help='ProgramArguments[0]. The default is the addon wifi-bin/ path, which t970-wifi-service-mount.patch exempts '
                             'from the Sandbox exec check (combo12: xpcproxy refused there). Executables inside a union over a system '
                             'directory hung PID 1 (combo11 /usr/libexec, combo14 /System/Library/xpc), so no system path here.')
    parser.add_argument('--remove-prefix', action='append', default=[],
                        help='drop addon entries under this path (from an earlier wifi build, e.g. xpc/) before adding')
    parser.add_argument('--drop-cdhash', action='append', default=[],
                        help='remove trust modules made only of these CDHashes (an earlier wifi build) before appending')
    args = parser.parse_args()
    if bool(args.addon) != bool(args.addon_out) or bool(args.trust) != bool(args.trust_out):
        raise SystemExit('--addon needs --addon-out and --trust needs --trust-out')
    wifi = module_dir('Ath12kWifi', 'T970_ATH12K_WIFI_DIR', (ROOT / 'drivers/Ath12kWifi', ROOT.parent / 'sources/periph/Ath12kWifi'))
    unixnu = module_dir('UniXNU checkout', 'UNIXNU_DIR', (ROOT.parent / 'sources/UniXNU-next',))
    spec = importlib.util.spec_from_file_location('boot_runtime', unixnu / 'scripts/build-boot-runtime.py')
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)

    source_cache = args.os_root / 'System/Library/xpc/launchd.plist'
    original = plistlib.loads(source_cache.read_bytes())
    if original.get('VersionNumber') != 7:
        raise SystemExit('unexpected launchd cache version: ' + str(original.get('VersionNumber')))

    out = args.out.resolve()
    shutil.rmtree(out, ignore_errors=True)
    for name in ('wifi-bin', 'etc/wifi', 'xpc', 'daemons'):
        (out / name).mkdir(parents=True, exist_ok=True)

    built, supplicant_report = build_supplicant(wifi)
    binary = out / 'wifi-bin/unixnu-wpa_supplicant'
    shutil.copy2(built, binary)
    binary.chmod(0o755)
    entitlements = out / 'entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
    run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', LABEL, '--entitlements', entitlements, binary)
    binary_cdhash = runtime.verified_cdhash(binary)
    if args.exec_path.startswith('/System/Library/xpc/'):
        # the same signed bytes next to the cache, so the launchd-cache mount alone makes it visible at a system path
        inside = out / 'xpc' / Path(args.exec_path).name
        shutil.copy2(binary, inside)
        if runtime.verified_cdhash(inside) != binary_cdhash:
            raise SystemExit('copied executable changed its CDHash')

    conf = '/private/var/ngpu/etc/wifi/wpa_supplicant.conf'
    (out / 'etc/wifi/wpa_supplicant.conf').write_text('# Networks arrive from the native Wi-Fi control channel.\nap_scan=1\n')
    job = {'Label': LABEL, 'ProgramArguments': [args.exec_path, '-Dsheng', '-ien0', '-c' + conf, '-q'],
           'RunAtLoad': True, 'KeepAlive': True, 'ThrottleInterval': 3, 'UserName': 'root', 'Umask': 0o77,
           'ProcessType': 'Interactive', 'StandardOutPath': '/var/log/unixnu-wifi.log', 'StandardErrorPath': '/var/log/unixnu-wifi.log'}
    # daemons/ is the merge input only; it does not go into the addon (the cache is what launchd reads on iPadOS 17)
    (out / f'daemons/{LABEL}.plist').write_bytes(plistlib.dumps(job))
    cache = out / 'xpc/launchd.plist'
    cache_report = runtime.merge_launchd_cache(source_cache, out / 'daemons', cache)
    cache_report.update(runtime.sign_launchd_cache(cache))
    merged = plistlib.loads(cache.read_bytes())
    if merged['LaunchDaemons'].pop(f'/System/Library/LaunchDaemons/{LABEL}.plist') != job or merged != original:
        raise SystemExit('launchd cache merge changed an original entry')

    hashes = sorted({binary_cdhash, cache_report['cdhash']})
    files = [p for name in ('wifi-bin', 'etc/wifi', 'xpc') for p in sorted((out / name).rglob('*')) if p.is_file()]
    report = {'built': True, 'target': 'ios17 (21H461)', 'hardware_verified': False, 'label': LABEL,
              'executable_cdhash': binary_cdhash, 'launchd_cache': cache_report, 'trust_cdhashes': hashes,
              'addon_files': {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
              'supplicant': supplicant_report}

    if args.addon:
        source_addon = args.addon
        if args.remove_prefix:
            import struct
            import zlib
            data = args.addon.read_bytes()
            count, at, kept, removed = struct.unpack_from('<I', data, 8)[0], 24, [], []
            for _ in range(count):
                length, mode, size, packed = struct.unpack_from('<IIQQ', data, at)
                end = at + 24 + ((length + 7) & ~7) + ((packed + 7) & ~7)
                name = data[at + 24:at + 24 + length - 1].decode()
                (removed if any(name.startswith(prefix) for prefix in args.remove_prefix) else kept).append(data[at:end])
                at = end
            body = b''.join(kept)
            source_addon = args.addon_out.with_suffix('.stripped')
            source_addon.write_bytes(b'NGPUADD2' + struct.pack('<IIQ', len(kept), 0, 24 + len(body)) + body)
            report['addon_removed'] = len(removed)
        run(sys.executable, unixnu / 'scripts/addon-add-dir.py', source_addon, args.addon_out,
            'wifi-bin', out / 'wifi-bin', 'etc/wifi', out / 'etc/wifi', 'wifi-xpc', out / 'xpc')   # mounted from /private/var/ngpu/wifi-xpc
        if source_addon != args.addon:
            source_addon.unlink()
        report['addon'] = {'in': str(args.addon), 'out': str(args.addon_out),
                           'sha256': hashlib.sha256(args.addon_out.read_bytes()).hexdigest()}
    if args.trust:
        trust = args.trust
        if args.drop_cdhash:
            # the bundle layout of bundle-ipsw-trust.py: u32 count, u32 offsets, modules (u32 version, 16-byte id, u32 entries, entries)
            data = trust.read_bytes()
            count = int.from_bytes(data[:4], 'little')
            offsets = [int.from_bytes(data[4 + 4 * i:8 + 4 * i], 'little') for i in range(count)] + [len(data)]
            modules, dropped, drop = [], [], {h.lower() for h in args.drop_cdhash}
            for i in range(count):
                module = data[offsets[i]:offsets[i + 1]]
                version, entries = int.from_bytes(module[:4], 'little'), int.from_bytes(module[20:24], 'little')
                stride = {0: 20, 1: 22, 2: 24}[version]
                owned = {module[24 + j * stride:44 + j * stride].hex() for j in range(entries)}
                (dropped if owned and owned <= drop else modules).append(module)
            cursor, table = 4 + 4 * len(modules), []
            for module in modules:
                table.append(cursor.to_bytes(4, 'little'))
                cursor += len(module)
            trust = args.trust_out.with_suffix('.dropped')
            trust.write_bytes(len(modules).to_bytes(4, 'little') + b''.join(table) + b''.join(modules))
            report['trust_dropped_modules'] = len(dropped)
        run(sys.executable, unixnu / 'scripts/trust-append-module.py', trust, args.trust_out, *hashes)
        if trust != args.trust:
            trust.unlink()
        report['trust'] = {'in': str(args.trust), 'out': str(args.trust_out),
                           'sha256': hashlib.sha256(args.trust_out.read_bytes()).hexdigest()}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'staged {LABEL}: {out}; executable CDHash {binary_cdhash}, launchd cache CDHash {cache_report["cdhash"]}, '
          f'{cache_report["original_jobs"]} original jobs kept')


if __name__ == '__main__':
    main()
