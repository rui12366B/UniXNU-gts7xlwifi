#!/usr/bin/env python3
"""Stage the camera extension daemon (tools/t970camerad/t970camerad.m, camera stage C6) for the iPadOS 17 (21H461) payload addon.

  build-t970camerad.py --cache-in LAUNCHD_PLIST [--out DIR] [--addon IN --addon-out OUT] [--trust IN --trust-out OUT]

iPadOS 17's mediaserverd discovers CoreMediaIO camera extensions (CMCapture BWFigCaptureDeviceVendor _updateCMIOExtensionDevices,
CMIOExtensionFigCaptureStream) and hands them to AVFoundation as external cameras; it publishes the launchd event stream
com.apple.cmio.registerassistantservice.system-extensions.matching (21H461 com.apple.mediaserverd.plist PublishesEvents). A provider
is a launchd job subscribed to that stream with CMIOExtensionBundleIdentifier, vending a Mach service of the same name that its
embedded Info.plist names under CMIOExtension/CMIOExtensionMachServiceName: the shape of Apple's own macOS camera daemons
(appleh13camerad, cameracaptured, UVCAssistant). This script
  1. builds t970camerad for arm64 iOS 17.0 against the macOS SDK's CoreMediaIO headers (the iOS SDK ships only the .tbd; the
     classes and constants are exported by 21H461's CoreMediaIO), once per identity with that identity's Info.plist in
     __TEXT,__info_plist, signed ad hoc with no-container and IOKit user-client entitlements -> bin/<name>,
  2. adds one owner job per identity to the launchd cache from --cache-in (the BlueTool script's output, so all services share the
     one cache launchd reads): RunAtLoad + KeepAlive (the shape proven from /private/var/ngpu), the LaunchEvents subscription and
     the Mach service; every other entry stays equal. Signed detached -> xpc/launchd.plist + .sig,
  3. with --addon/--trust: appends the cache under wifi-xpc/ (replacing the previous launchd.plist(.sig) there) and bin/ under cam-bin/
     to a GPU addon (UniXNU scripts/addon-add-dir.py), and the executables' and cache's CDHashes to a trust bundle as one module.
Two identities by default, so one boot shows which one mediaserverd accepts: org.unixnu.t970camerad (status word d0) and
com.apple.cmio.uvcassistantextension (d1; the name of Apple's UVC extension, absent from the j120 image, in case mediaserverd only
takes known providers). Their executables need patches/xnu/t970-wifi-service-mount.patch's exec exemption (listed there) and
t970-wifi-svc bit 0; the kernel service needs t970-camera bit 512. Order: Wi-Fi service, BlueTool, then this with --cache-in
<bluetool out>/xpc/launchd.plist. Nothing Apple's is committed."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STREAM = 'com.apple.cmio.registerassistantservice.system-extensions.matching'
# (identity, executable, status slot, device name)
IDENTITIES = {
    'org.unixnu.t970camerad': ('t970camerad', 0, 'T970 Front Camera'),
    'com.apple.cmio.uvcassistantextension': ('t970camerad-uvc', 1, 'T970 Front Camera (UVC id)'),
}


def module_dir(name, env, candidates):
    for candidate in (os.environ.get(env), *candidates):
        if candidate and Path(candidate).is_dir():
            return Path(candidate).resolve()
    raise SystemExit(f'build-t970camerad: {name} not found; set {env}')


def run(*command, **kwargs):
    subprocess.run([str(c) for c in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cache-in', type=Path, required=True, help='launchd.plist to extend (the BlueTool script output)')
    parser.add_argument('--out', type=Path, default=ROOT / '.cache/t970camerad')
    parser.add_argument('--identity', action='append', choices=sorted(IDENTITIES),
                        help='extension identities to install (default: both)')
    parser.add_argument('--bayer', default='grbg', choices=('rggb', 'grbg', 'gbrg', 'bggr'))
    parser.add_argument('--bin-prefix', default='cam-bin', help='addon directory of the executables (-> /private/var/ngpu/cam-bin/)')
    parser.add_argument('--cache-prefix', default='wifi-xpc', help='addon directory of the cache (mounted over /System/Library/xpc)')
    parser.add_argument('--log-dir', default='/private/var/log', help='StandardErrorPath directory ("" for none)')
    parser.add_argument('--probe', type=int, default=25,
                        help='seconds after start for the AVFoundation probe in the first identity (C6b: list cameras, capture 6 s from '
                             'our device, BMP to /private/var/log/t970camerad-avf.bmp); 0 = off')
    parser.add_argument('--addon', type=Path)
    parser.add_argument('--addon-out', type=Path)
    parser.add_argument('--trust', type=Path)
    parser.add_argument('--trust-out', type=Path)
    args = parser.parse_args()
    if bool(args.addon) != bool(args.addon_out) or bool(args.trust) != bool(args.trust_out):
        raise SystemExit('--addon needs --addon-out and --trust needs --trust-out')
    identities = args.identity or list(IDENTITIES)
    unixnu = module_dir('UniXNU checkout', 'UNIXNU_DIR', (ROOT.parent / 'sources/UniXNU-main', ROOT.parent / 'sources/UniXNU-next'))
    spec = importlib.util.spec_from_file_location('boot_runtime', unixnu / 'scripts/build-boot-runtime.py')
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)

    original = plistlib.loads(args.cache_in.read_bytes())
    if original.get('VersionNumber') != 7:
        raise SystemExit('unexpected launchd cache version: ' + str(original.get('VersionNumber')))
    jobs = original['LaunchDaemons']
    for identity in identities:
        for path, job in jobs.items():
            if job.get('Label') == identity or identity in job.get('MachServices', {}):
                raise SystemExit(f'{identity} is already used by {path}')

    out = args.out.resolve()
    shutil.rmtree(out, ignore_errors=True)
    (out / 'xpc').mkdir(parents=True)
    (out / 'bin').mkdir()
    for extra in sorted(args.cache_in.parent.iterdir()):
        if extra.is_file() and extra.name not in ('launchd.plist', 'launchd.plist.sig') and not os.access(extra, os.X_OK):
            shutil.copy2(extra, out / 'xpc' / extra.name)

    # 1. the daemon, one signed executable per identity
    ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    mac = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    entitlements = out / 'entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({
        'com.apple.private.security.no-container': True,
        'com.apple.security.iokit-user-client-class': ['T970CameraUserClient', 'IOSurfaceRootUserClient'],
        'com.apple.private.cmio.extension.configuration': True,
        'com.apple.private.tcc.manager.check-by-audit-token': ['kTCCServiceCamera'],
        # C6b probe: an AVCaptureSession from a daemon (mediaserverd checks these for non-app clients; names from 21H461 CMCapture)
        'com.apple.private.tcc.allow': ['kTCCServiceCamera'],
        'com.apple.private.avfoundation.capture.allow': True,
        'com.apple.private.avfoundation.capture.nonstandard-client.allow': True,
        'com.apple.private.avfoundation.capture.nonstandard-client.allowed-media-types': ['vide'],
        'com.apple.private.avfoundation.background-camera-access': True,
    }))
    binaries = {}
    with tempfile.TemporaryDirectory() as tmp:
        headers = Path(tmp) / 'CoreMediaIO'
        headers.mkdir()
        for header in (Path(mac) / 'System/Library/Frameworks/CoreMediaIO.framework/Headers').glob('*.h'):
            (headers / header.name).symlink_to(header)
        for identity in identities:
            name, slot, device = IDENTITIES[identity]
            info = Path(tmp) / f'{name}.Info.plist'
            info.write_bytes(plistlib.dumps({
                'CFBundleIdentifier': identity, 'CFBundleExecutable': name, 'CFBundleName': device,
                'CFBundleInfoDictionaryVersion': '6.0', 'CFBundlePackageType': 'SYSX', 'CFBundleShortVersionString': '1.0',
                'CFBundleVersion': '1', 'CFBundleSupportedPlatforms': ['iPhoneOS'],
                'CMIOExtension': {'CMIOExtensionMachServiceName': identity},
                # C6c: AVFoundation's per-bundle switch for external cameras (AVCaptureBundleUseExternalDeviceTypeKey) and TCC's
                # usage string, in case either decides what the in-daemon probe may see
                'NSCameraUseExternalDeviceType': True,
                'NSCameraUsageDescription': 'Camera bring-up probe (lists cameras and captures a test frame).',
            }))
            binary = out / 'bin' / name
            run('xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-I', tmp, '-I', ROOT / 'package',
                '-fobjc-arc', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-fstack-protector-strong',
                '-framework', 'Foundation', '-framework', 'CoreMedia', '-framework', 'CoreVideo', '-framework', 'IOKit',
                '-framework', 'CoreMediaIO', '-framework', 'AVFoundation', '-Wl,-sectcreate,__TEXT,__info_plist,' + str(info),
                ROOT / 'tools/t970camerad/t970camerad.m', '-o', binary)
            binary.chmod(0o755)
            run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', identity, '--entitlements', entitlements, binary)
            binaries[identity] = (binary, runtime.verified_cdhash(binary))

    # 2. the cache
    merged = plistlib.loads(args.cache_in.read_bytes())
    added = {}
    for identity in identities:
        name, slot, device = IDENTITIES[identity]
        exec_path = f'/private/var/ngpu/{args.bin_prefix}/{name}'
        arguments = [exec_path, '-s', str(slot), '-n', device, '-b', args.bayer] + (['-p', str(args.probe)] if args.probe and slot == 0 else [])
        job = {'Label': identity, 'ProgramArguments': arguments,
               'MachServices': {identity: True}, 'LaunchEvents': {STREAM: {identity: {'CMIOExtensionBundleIdentifier': identity}}},
               'RunAtLoad': True, 'KeepAlive': True, 'ThrottleInterval': 5, 'ProcessType': 'Interactive', 'UserName': 'root'}
        if args.log_dir:
            job['StandardOutPath'] = job['StandardErrorPath'] = f'{args.log_dir}/{name}.log'
        key = f'/System/Library/LaunchDaemons/{identity}.plist'
        merged['LaunchDaemons'][key] = job
        added[key] = job
    check, base = plistlib.loads(plistlib.dumps(merged)), plistlib.loads(plistlib.dumps(original))
    for key in added:
        check['LaunchDaemons'].pop(key)
    if check != base:
        raise SystemExit('cache edit changed more than the camera jobs')
    cache = out / 'xpc/launchd.plist'
    cache.write_bytes(plistlib.dumps(merged, fmt=plistlib.FMT_XML, sort_keys=False))
    cache_report = runtime.sign_launchd_cache(cache)

    hashes = sorted({h for _, h in binaries.values()} | {cache_report['cdhash']})
    report = {'executables': {k: {'path': str(b), 'cdhash': h} for k, (b, h) in binaries.items()}, 'jobs': added,
              'launchd_cache': cache_report, 'trust_cdhashes': hashes, 'cache_in': str(args.cache_in)}
    if args.addon:
        run(sys.executable, unixnu / 'scripts/addon-add-dir.py', args.addon, args.addon_out, args.cache_prefix, out / 'xpc',
            args.bin_prefix, out / 'bin')
        report['addon'] = str(args.addon_out)
    if args.trust:
        run(sys.executable, unixnu / 'scripts/trust-append-module.py', args.trust, args.trust_out, *hashes)
        report['trust'] = str(args.trust_out)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
