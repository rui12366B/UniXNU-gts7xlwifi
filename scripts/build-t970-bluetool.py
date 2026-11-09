#!/usr/bin/env python3
"""Stage the Bluetooth bootstrap service (BlueTool) for the iPadOS 17 (21H461) payload addon.

  build-t970-bluetool.py --cache-in LAUNCHD_PLIST [--out DIR] [--firmware DIR]
                         [--addon IN --addon-out OUT] [--trust IN --trust-out OUT]

bluetoothd powers the controller through the Mach service com.apple.BlueTool: XPC "RunBuiltinScript" with kMsgArgs.script
"boot"/"init", reply keys result, firmware and manufacturer (read from 21H461's bluetoothd). The stock BlueTool runs Broadcom
bring-up on a /dev/*.bluetooth tty this board does not have; its failure is what makes bluetoothd exit 1 at start-up (combo13c:
"Unknown platform/chipset/manufacturer combination" / "timed out booting the chip" / "Init failed", 0x100438790 -> _exit(1)).
UniXNU/QcaBluetooth's userland/BlueTool.c answers the same messages through the kernel (kern.qbt_power_on, kern.qbt_boot,
kern.qbt_firmware from package/T970Bluetooth.cpp serve mode). This script
  1. builds it for arm64 (not arm64e) iOS 17.0, signed ad hoc as org.unixnu.bluetooth.BlueTool (no-container entitlement, 4 KiB
     pages) -> bin/BlueTool, into the addon under bt-bin/ (/private/var/ngpu/bt-bin/BlueTool, mode 0755, no mount),
  2. takes a launchd cache (--cache-in: the OS's /System/Library/xpc/launchd.plist, or the one build-t970-wifi-service.py wrote, so
     both services share the one cache launchd reads) and changes only com.apple.BlueTool's ProgramArguments[0] to --exec-path.
     Every other entry stays byte-for-byte equal. The executable is NOT put next to the cache: an executable inside a union-mounted
     system directory hung PID 1 (combo11 /usr/libexec, combo14 /System/Library/xpc); --exec-path follows the location the Wi-Fi
     service work finds safe (xpcproxy execs only system paths, combo12). Non-executable files next to the input cache are carried
     over. The result is signed detached -> xpc/launchd.plist + .sig,
  3. with --firmware DIR: adds htbtfw20.tlv / htnv20.bin from DIR (the gitignored .cache/firmware/bt) under firmware/qca6390/,
  4. with --addon/--trust: appends the cache under wifi-xpc/ (replacing the Wi-Fi script's launchd.plist(.sig) there), bt-bin/ and
     firmware to a GPU addon
     (UniXNU scripts/addon-add-dir.py) and the BlueTool and cache CDHashes to a trust bundle as one new module
     (UniXNU scripts/trust-append-module.py). The Wi-Fi module's earlier cache CDHash stays trusted but unused (harmless).
Order with Wi-Fi: build-t970-wifi-service.py --addon A --addon-out B ..., then this with --cache-in <wifi out>/xpc/launchd.plist
--addon B --addon-out C. Nothing Apple's or Qualcomm's is committed; the inputs are the extracted OS and the local cache."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
JOB = '/System/Library/LaunchDaemons/com.apple.BlueTool.plist'
OWNER_LABEL = 'org.unixnu.bluetooth.bluetool'
DAEMON = '/System/Library/LaunchDaemons/com.apple.bluetoothd.plist'
# 21H461 /usr/sbin/bluetoothd (arm64, ad hoc, UUID 0F836381-D07B-36E4-9DBB-8A44782B9C14) and the patches the board needs so far, as
# (file offset, original 4-byte words, patched words).
BLUETOOTHD_SHA256 = '3f1e4493ea6a76976625f7af58a8f330c0c90d82d9020b11110b4f3f5b758df4'
BLUETOOTHD_PATCHES = {
    # combo23 (keychain working, stock daemon): HCI init reads Supported Commands, LE features and data length, then
    # 0x1001840a0 submits the optional Write Default Erroneous Data Reporting (0x0c5b, descriptor 0x7c). The QCA6390 does not set its
    # support bit (byte 18 bit 3; the mask byte is 0x83), so the submit fails locally with status 628, and the step hands 628 to the
    # chain advance 0x10023e254 -> "HCI initialization failed" -> "Bluetooth error - restarting { reason=108 }" every run. Pass 0
    # instead (mov x1, x19 -> mov x1, xzr): the step still logs the rejection and the native chain continues. Same rule as
    # UniXNU/QcaBluetooth's iPadOS 27 adapter ("Optional command 0x0c5b ... unsupported controllers advance through the native
    # initialization chain").
    'optional-0c5b': ((0x18410c, (0xaa1303e1,), (0xaa1f03e1,)),),
    # combo24b: with 0x0c5b skipped each session runs ~0.1 s longer (more HCI) and still restarts, with no unified log on disk to name
    # the step. Several init steps already treat status 628 (0x274: the command is not in the controller's supported-commands bitmap,
    # so the stack never sends it) as "skip" and advance with 0x10023e1f0; the rest hand the 628 to the chain advance 0x10023e254,
    # which aborts initialization. Map 628 to 0 at its entry so every optional command the QCA6390 lacks (54 of the stack's
    # descriptors, e.g. LE Read Buffer Size v2 0x2060) is skipped the same way. Other statuses (controller errors, 0x278 timeouts)
    # still fail. mov x19, x1 ; mov x20, x0 ; adrp ; nop  ->  cmp w1, #0x274 ; csel x19, xzr, x1, eq ; adrp ; mov x20, x0.
    'unsupported-628': ((0x23e260, (0xaa0103f3, 0xaa0003f4, 0xf0003828, 0xd503201f), (0x7109d03f, 0x9a8103f3, 0xf0003828, 0xaa0003f4)),),
    # combo25: init completes; the first LE scan then fails: LE Set Scan Parameters (0x200b) -> 0x0c Command Disallowed ->
    # "Could not start LE scan ... STATUS 712" -> "Bluetooth error - restarting { reason=712 }" (Read LE Advertising TX Power 0x2007
    # also got 0x0c). Init step 0x100183ae8 sends LE Read Periodic Advertiser List Size (0x204a) when the controller reports the
    # periodic-advertising feature. That is one of the spec's extended advertising commands, and after it the controller rejects every
    # legacy advertising/scan command (Core spec Vol 4 Part E 3.1.1). Periodic advertising is disabled in this stack anyway. Take the
    # step's own skip exit (as when the feature is absent): tbz w8, #5, 0x100183b30 -> b 0x100183b30.
    'legacy-le-only': ((0x183b0c, (0x36280128,), (0x14000009,)),),
    # combo26: with 0x204a gone the first scan gets further, then "Reset scan in LE_Scan: STATUS 718" -> reason=718. 718 = 700 + 0x12
    # Invalid HCI Command Parameters: the scan parameters name a random own address, and the controller's random address was never
    # set (0x2005), which the spec answers with 0x12 on scan enable (UniXNU/QcaBluetooth's iPadOS 27 adapter hit the same). Use the
    # public address (own_address_type 0) in both LE Set Scan Parameters builders (0x100068004: ldurb w9,[x29,#-0x28];
    # 0x100078ad8: ldurb w8,[x29,#-0x60], also the extended variant's first parameter) -> mov w9/w8, #0. A passive scan sends no
    # packets, so this only affects active scans' SCAN_REQ address.
    'public-scan-address': ((0x68004, (0x385d83a9,), (0x52800009,)), (0x78ad8, (0x385a03a8,), (0x52800008,))),
    # combo22 (no keychain): readRootKeys -> SecItemAdd -25291 -> "Failed to set new identity root!" -> reason 130 -> _exit(1).
    # 0x1002d1eb4 stores the global root keys; returning success keeps them in memory (mov w0, #0 ; ret). Not needed since the
    # keychain AKS fix (combo23: no -25291); kept behind --volatile-root-keys.
    'volatile-root-keys': ((0x2d1eb4, (0xa9bc5ff8, 0xa90157f6), (0x52800000, 0xd65f03c0)),),
}
OWNER_JOB = f'/System/Library/LaunchDaemons/{OWNER_LABEL}.plist'


def module_dir(name, env, candidates):
    for candidate in (os.environ.get(env), *candidates):
        if candidate and Path(candidate).is_dir():
            return Path(candidate).resolve()
    raise SystemExit(f'build-t970-bluetool: {name} not found; set {env}')


def run(*command, **kwargs):
    subprocess.run([str(c) for c in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cache-in', type=Path, required=True, help='launchd.plist to extend (OS or Wi-Fi service output)')
    parser.add_argument('--out', type=Path, default=ROOT / '.cache/bluetool')
    parser.add_argument('--firmware', type=Path, help='directory with htbtfw20.tlv and htnv20.bin (gitignored cache)')
    parser.add_argument('--exec-path', default='/private/var/ngpu/bt-bin/BlueTool',
                        help='com.apple.BlueTool ProgramArguments[0]. On Data, not in a union-mounted system directory (that hung PID 1:'
                             ' combo11 /usr/libexec, combo14 /System/Library/xpc); xpcproxy may exec it through the exact-path Sandbox '
                             'exemption of patches/xnu/t970-wifi-service-mount.patch (the Wi-Fi session adds this path)')
    parser.add_argument('--bin-prefix', default='bt-bin', help='addon directory of the executable (bt-bin -> /private/var/ngpu/bt-bin/)')
    parser.add_argument('--user', default='root',
                        help='the job\'s UserName. The stock job runs as mobile; upstream runs its BlueTool as root, and the one job '
                             'proven to start from /private/var/ngpu (the Wi-Fi supplicant) is root too (combo19: as mobile, BlueTool '
                             'never ran and bluetoothd got no XPC reply -> 0xd5 -> exit 1)')
    parser.add_argument('--job-style', choices=('unixnu', 'apple'), default='unixnu',
                        help='unixnu: replace com.apple.BlueTool by an org.unixnu RunAtLoad job vending the same Mach service; '
                             'apple: only repoint the stock on-demand job (did not spawn in combo19/20)')
    parser.add_argument('--log', default='/private/var/log/unixnu-bluetool.log', help='StandardOut/ErrorPath of the job ("" for none)')
    parser.add_argument('--bluetoothd-log', default='/private/var/log/bluetoothd.log',
                        help='also give com.apple.bluetoothd OS_ACTIVITY_DT_MODE=YES (its os_log mirrored to stderr) and this '
                             'StandardOut/ErrorPath, exported by package/BootDiagExport.c ("" leaves bluetoothd as it is)')
    parser.add_argument('--bluetoothd', type=Path,
                        help='the OS\'s /usr/sbin/bluetoothd (21H461, sha256-checked): patched (optional HCI command 0x0c5b), re-signed ad hoc as '
                             'com.apple.bluetoothd with its own entitlements, shipped as <bin-prefix>/bluetoothd, and the job pointed at '
                             'it (--bluetoothd-exec). Its exact path needs the Wi-Fi patch\'s exec exemption too.')
    parser.add_argument('--volatile-root-keys', action='store_true',
                        help='also keep root keys in memory when the keychain cannot store them (combo22; not needed with the AKS fix)')
    parser.add_argument('--bluetoothd-exec', default='/private/var/ngpu/bt-bin/bluetoothd')
    parser.add_argument('--cache-prefix', default='wifi-xpc',
                        help='addon directory of the cache (wifi-xpc -> /private/var/ngpu/wifi-xpc, mounted over /System/Library/xpc '
                             'with t970-wifi-svc bit 0)')
    parser.add_argument('--addon', type=Path)
    parser.add_argument('--addon-out', type=Path)
    parser.add_argument('--trust', type=Path)
    parser.add_argument('--trust-out', type=Path)
    args = parser.parse_args()
    if bool(args.addon) != bool(args.addon_out) or bool(args.trust) != bool(args.trust_out):
        raise SystemExit('--addon needs --addon-out and --trust needs --trust-out')
    qbt = module_dir('QcaBluetooth', 'T970_QCA_BLUETOOTH_DIR', (ROOT / 'drivers/QcaBluetooth', ROOT.parent / 'sources/periph/QcaBluetooth'))
    unixnu = module_dir('UniXNU checkout', 'UNIXNU_DIR', (ROOT.parent / 'sources/UniXNU-next',))
    spec = importlib.util.spec_from_file_location('boot_runtime', unixnu / 'scripts/build-boot-runtime.py')
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)

    original = plistlib.loads(args.cache_in.read_bytes())
    if original.get('VersionNumber') != 7:
        raise SystemExit('unexpected launchd cache version: ' + str(original.get('VersionNumber')))
    stock = original['LaunchDaemons'].get(JOB)
    if not stock or stock.get('MachServices', {}).get('com.apple.BlueTool') is not True:
        raise SystemExit('the cache has no com.apple.BlueTool Mach service job')

    out = args.out.resolve()
    shutil.rmtree(out, ignore_errors=True)
    (out / 'xpc').mkdir(parents=True)
    (out / 'bin').mkdir()
    for extra in sorted(args.cache_in.parent.iterdir()):
        # the launchd cache only: executables next to it are what hung PID 1 in combo14, so nothing else is carried
        if extra.is_file() and extra.name not in ('launchd.plist', 'launchd.plist.sig') and not os.access(extra, os.X_OK):
            shutil.copy2(extra, out / 'xpc' / extra.name)

    # 1. BlueTool for arm64 iOS 17 (the board's userland is arm64, not arm64e)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    binary = out / 'bin' / Path(args.exec_path).name
    # A copy with one start-up line: reading kern.qbt_boot once counts in qbt_transport_activity[1], so the kernel's
    # "[t970 bt sk] act a/b/c/d" row shows b > 0 as soon as launchd has started BlueTool, even before any XPC message arrives.
    source = (qbt / 'userland/BlueTool.c').read_text()
    anchor = '    logger=os_log_create("org.unixnu.bluetooth","BlueTool");\n'
    if source.count(anchor) != 1:
        raise SystemExit('userland/BlueTool.c changed: review the start-up ping')
    source = source.replace(anchor, anchor + '    os_log(logger,"start, controller state %d",state());   /* t970: start-up ping */\n'
                            '    fprintf(stderr,"t970 BlueTool: start\\n");fflush(stderr);\n')
    # every incoming message and every script result also to stderr (the job's StandardErrorPath, exported by BootDiagExport.c)
    for old, new in (('#include <stdint.h>\n', '#include <stdint.h>\n#include <stdio.h>\n'),
                     ('    const char *id=xpc_dictionary_get_string(message,"kMsgId");\n',
                      '    const char *id=xpc_dictionary_get_string(message,"kMsgId");\n'
                      '    fprintf(stderr,"t970 BlueTool: message %s\\n",id?id:"(no kMsgId)");fflush(stderr);\n'),
                     ('        os_log(logger,"script %{public}s result %d",script?script:"missing",error);\n',
                      '        os_log(logger,"script %{public}s result %d",script?script:"missing",error);\n'
                      '        fprintf(stderr,"t970 BlueTool: script %s result %d\\n",script?script:"missing",error);fflush(stderr);\n')):
        if source.count(old) != 1:
            raise SystemExit('userland/BlueTool.c changed: review the stderr lines')
        source = source.replace(old, new)
    generated = out / 'BlueTool-t970.c'
    generated.write_text('/* Generated by build-t970-bluetool.py from QcaBluetooth userland/BlueTool.c (start-up ping added). */\n' + source)
    run('xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-Werror',
        '-fblocks', '-fstack-protector-strong', generated, '-o', binary)
    binary.chmod(0o755)
    entitlements = out / 'entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
    run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.unixnu.bluetooth.BlueTool',
        '--entitlements', entitlements, binary)
    binary_cdhash = runtime.verified_cdhash(binary)

    # 1b. the patched bluetoothd
    daemon_cdhash = None
    if args.bluetoothd:
        import hashlib, struct
        data = bytearray(args.bluetoothd.read_bytes())
        if hashlib.sha256(data).hexdigest() != BLUETOOTHD_SHA256:
            raise SystemExit('bluetoothd is not the 21H461 binary the patch table was made for')
        chosen = ['optional-0c5b', 'unsupported-628', 'legacy-le-only', 'public-scan-address'] + (['volatile-root-keys'] if args.volatile_root_keys else [])
        for offset, old, new in (site for name in chosen for site in BLUETOOTHD_PATCHES[name]):
            if struct.unpack_from(f'<{len(old)}I', data, offset) != old:
                raise SystemExit(f'bluetoothd patch site {offset:#x} does not hold the expected instructions')
            struct.pack_into(f'<{len(new)}I', data, offset, *new)
        patched = out / 'bin/bluetoothd'
        patched.write_bytes(bytes(data))
        patched.chmod(0o755)
        ents = subprocess.run(['codesign', '-d', '--entitlements', '-', '--xml', str(args.bluetoothd)], capture_output=True, check=True).stdout
        if not ents.lstrip().startswith(b'<?xml'):
            raise SystemExit('could not read bluetoothd entitlements')
        (out / 'bluetoothd.entitlements.plist').write_bytes(ents)
        run('codesign', '--remove-signature', patched)
        run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'com.apple.bluetoothd',
            '--entitlements', out / 'bluetoothd.entitlements.plist', patched)
        daemon_cdhash = runtime.verified_cdhash(patched)

    # 2. the cache. --job-style unixnu (default since combo20): the stock com.apple.BlueTool entry is replaced by an owner job,
    # org.unixnu.bluetooth.bluetool, that vends the same Mach service com.apple.BlueTool and starts at boot (RunAtLoad + KeepAlive),
    # the shape of the one owner job proven to run from /private/var/ngpu (the Wi-Fi supplicant). combo19/20: with the stock
    # on-demand Apple entry pointed at /private/var/ngpu, launchd never even tried to spawn it (no exec, no AMFI line) and
    # bluetoothd's RunBuiltinScript got an XPC error (0x1002efd24 -> 0xd5 -> exit 1). --job-style apple keeps the old edit.
    merged = plistlib.loads(args.cache_in.read_bytes())
    jobs = merged['LaunchDaemons']
    if args.job_style == 'apple':
        job = jobs[JOB]
        job['ProgramArguments'] = [args.exec_path] + list(stock.get('ProgramArguments', ['/usr/sbin/BlueTool', '-R'])[1:])
        job.pop('Program', None)
        changed = {JOB}
    else:
        del jobs[JOB]
        job = {'Label': OWNER_LABEL, 'ProgramArguments': [args.exec_path, '-R'], 'MachServices': {'com.apple.BlueTool': True},
               'RunAtLoad': True, 'KeepAlive': True, 'ThrottleInterval': 3, 'ProcessType': 'Interactive'}
        jobs[OWNER_JOB] = job
        changed = {JOB, OWNER_JOB}
    job['UserName'] = args.user
    if args.log:
        job['StandardOutPath'] = job['StandardErrorPath'] = args.log
    if args.bluetoothd:
        jobs[DAEMON]['ProgramArguments'][0] = args.bluetoothd_exec
        jobs[DAEMON].pop('Program', None)
        changed.add(DAEMON)
    if args.bluetoothd_log:
        daemon = jobs[DAEMON]
        daemon.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
        daemon['StandardOutPath'] = daemon['StandardErrorPath'] = args.bluetoothd_log
        changed.add(DAEMON)
    check, base = plistlib.loads(plistlib.dumps(merged)), plistlib.loads(plistlib.dumps(original))
    for key in changed:
        check['LaunchDaemons'].pop(key, None)
        base['LaunchDaemons'].pop(key, None)
    if check != base:
        raise SystemExit('cache edit changed more than the BlueTool and bluetoothd jobs')
    if sum(1 for j in jobs.values() if 'com.apple.BlueTool' in j.get('MachServices', {})) != 1:
        raise SystemExit('com.apple.BlueTool must be vended by exactly one job')
    cache = out / 'xpc/launchd.plist'
    cache.write_bytes(plistlib.dumps(merged, fmt=plistlib.FMT_XML, sort_keys=False))
    cache_report = runtime.sign_launchd_cache(cache)

    # 3. firmware
    if args.firmware:
        fw = out / 'firmware/qca6390'
        fw.mkdir(parents=True)
        for name in ('htbtfw20.tlv', 'htnv20.bin'):
            source = args.firmware / name
            if not source.is_file():
                raise SystemExit('missing firmware ' + str(source))
            shutil.copy2(source, fw / name)
            (fw / name).chmod(0o644)

    hashes = sorted({binary_cdhash, cache_report['cdhash']} | ({daemon_cdhash} if daemon_cdhash else set()))
    report = {'bluetool_cdhash': binary_cdhash, 'bluetoothd_cdhash': daemon_cdhash,
              'bluetoothd_job': jobs[DAEMON].get('ProgramArguments'), 'launchd_cache': cache_report, 'trust_cdhashes': hashes,
              'job': {k: v for k, v in job.items() if k in ('Label', 'ProgramArguments', 'UserName', 'MachServices', 'StandardErrorPath',
                                                    'RunAtLoad', 'KeepAlive')},
              'cache_in': str(args.cache_in), 'carried': sorted(p.name for p in (out / 'xpc').iterdir())}
    if args.addon:
        pairs = [args.cache_prefix, out / 'xpc', args.bin_prefix, out / 'bin'] + (['firmware/qca6390', out / 'firmware/qca6390'] if args.firmware else [])
        run(sys.executable, unixnu / 'scripts/addon-add-dir.py', args.addon, args.addon_out, *pairs)
        report['addon'] = str(args.addon_out)
    if args.trust:
        run(sys.executable, unixnu / 'scripts/trust-append-module.py', args.trust, args.trust_out, *hashes)
        report['trust'] = str(args.trust_out)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
