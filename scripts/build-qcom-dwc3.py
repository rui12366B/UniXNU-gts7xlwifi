#!/usr/bin/env python3
"""Build the QcomDwc3 SM8250 port (drivers/QcomDwc3/QcomDwc3Sm8250.cpp + package/T970UsbProbe.cpp) for iPadOS 17.7.11 (21H461).

The counterpart of UniXNU-sheng's USB block in build-gop-legacy-display-27.py, for this board's arm64 (no pointer authentication)
split prelinked kernelcache, the same way build-sheng-hid-input.py binds NativeHidInput to the prelinked IOHIDDevice:

  1. OriginalUsbXdciABI.h: the original AppleUSBXDCI as opaque storage (0x2440 bytes) and its 267 virtual slots, from
     scripts/usb-abi-21H461.json (scripts/gen-usb17-abi.py). The hook slots and field offsets below were read from Apple's own
     AppleT8011USBXDCI and AppleUSBXDCI in the same kernelcache (docs/usb-plan.md section 4).
  2. Both sources compiled with the kernel's C++ recipe (IOPlatformExpert.cpo.json), as LegacyDisplayProbe.cpp is.
  3. The compiled QcomDwc3Xdci vtable checked slot by slot: exactly 267 entries, owned entries only at the hooks and the
     per-class ones, every inherited AppleUSBXDCI entry bound to that slot's original target, IOService entries only where the
     original has the kernel's own.
  4. usb-bindings.S: absolute symbols for every original AppleUSBXDCI symbol the object uses (vtable entries, the qualified start
     and controllerReset calls, constructor, destructor, metaclass, vtable), at the original address plus the profile's prelink
     slide (-0x1000000: original kexts sit 16 MiB lower in the emitted kernel; checked against the previous kernel when it holds
     AppleUSBXDCI).
  5. QcomDwc3Sm8250.late.o without __mod_init_func (T970UsbProbe.cpp runs the initializer between preModLoad and postModLoad),
     archived with the probe and the bindings into build/qcom-dwc3/libQcomDwc3.a, which configs/gts7xlwifi_defconfig force-loads.

usage: build-qcom-dwc3.py [--out DIR]   (default: <tree>/build/qcom-dwc3)
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_prelink import macho, backed_offset  # noqa: E402

SLIDE = -0x1000000
# 21H461 AppleUSBXDCI hooks (slot: macro, return, evidence in AppleT8011USBXDCI / AppleUSBXDCI):
HOOKS = {
    179: ('XDCI_KEEP_CONFIGURATIONS', 'bool', 'pure; T8011: IOPort transport type check (upstream 24A446 slot 179)'),
    255: ('XDCI_TRANSPORT_ACTIVE', 'bool', 'pure; T8011: IOPort type check; caller returns this+0x23d8 when true'),
    256: ('XDCI_POWER_ON', 'IOReturn', 'pure; T8011 log string "powerOn"; caller fails on nonzero'),
    257: ('XDCI_POWER_OFF', 'IOReturn', 'pure; T8011 log string "powerOff"; result ignored'),
    258: ('XDCI_PARAMETERS', 'void', 'pure; T8011 stores {4, 0x2ee, 16, 3} at this+0x2410'),
    263: ('XDCI_CONTROLLER_RESET', 'IOReturn', 'concrete; log "controllerReset ... resetting core"; caller cbz w0'),
    264: ('XDCI_PERIODIC_ON', 'void', 'pure; T8011 ret; called when the periodic masks (+0x36c/+0x370) leave 0'),
    265: ('XDCI_PERIODIC_OFF', 'void', 'pure; T8011 ret; called when both masks return to 0'),
}
# AppleUSBXDCI's own overrides of IOService slots (targets in __PLK_TEXT_EXEC), named from the native IOService vtable.
IOSERVICE_OVERRIDES = {
    14: 'void free() override;',
    21: 'bool init(OSDictionary *properties=nullptr) override;',
    44: 'IOReturn setProperties(OSObject *) override;',
    76: 'bool willTerminate(IOService *,IOOptionBits) override;',
    86: 'bool start(IOService *) override;',
    87: 'void stop(IOService *) override;',
    111: 'IOWorkLoop *getWorkLoop() const override;',
    117: 'IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) override;',
}
FIELDS = {'kXdciRegisterMap': 0x230, 'kXdciRegisters': 0x238, 'kXdciParameters': 0x2410}
# AppleUSBXDCI C2 (calls IOUSBDeviceController C2, stores the vptr) and D2 (AppleT8011USBXDCI's D1/D0 reach it through a stub).
C2, D2 = 0xfffffff0065420b4, 0xfffffff0065420d4
OWNED = ('__ZN12QcomDwc3Xdci', '__ZNK12QcomDwc3Xdci')


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=180, **kw)


def require(value, why):
    if not value:
        raise SystemExit('build-qcom-dwc3: ' + why)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def empty_archive(out, why):
    # Until QcomDwc3 has passed a device test it must never fail the board build: an empty archive keeps the defconfig's
    # force-load linking, and the weak t970_usb_start / t970_dwc3_stats definitions stand in.
    archive = out / 'libQcomDwc3.a'
    archive.unlink(missing_ok=True)
    run([shutil.which('llvm-ar') or 'llvm-ar', 'rcs', archive])
    print(f'build-qcom-dwc3: WARNING: {why}; wrote an empty {archive} (t970-usb does nothing in this kernel)')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', type=Path, default=ROOT / 'build/qcom-dwc3')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    try:
        build(out)
    except BaseException as error:   # require() raises SystemExit; a compiler failure CalledProcessError
        if isinstance(error, KeyboardInterrupt):
            raise
        empty_archive(out, str(error) or type(error).__name__)


def build(out):
    abi = json.loads((ROOT / 'scripts/usb-abi-21H461.json').read_text())
    x = abi['classes']['AppleUSBXDCI']
    slots = [int(v, 16) for v in x['slots']]
    size = int(x['instance_size'], 16)
    pure = set(x['pure_slots'])
    require(len(slots) == 267 and size == 0x2440, 'unexpected AppleUSBXDCI layout in usb-abi-21H461.json')
    require(pure == {179, 255, 256, 257, 258, 264, 265}, f'unexpected pure slots {sorted(pure)}')
    require(pure <= set(HOOKS), 'every pure slot needs a hook')
    vtable, metaclass = int(x['vtable'], 16), int(x['gMetaClass'], 16)

    # Optional cross-check: the previous kernel, if it already prelinks AppleUSBXDCI, holds the same 267 targets 16 MiB lower.
    previous = ROOT.parent / 'kernel' / 'kernel.macho'
    checked = False
    if previous.is_file():
        raw = previous.read_bytes()
        image = macho(raw)
        try:
            at = backed_offset(image['segments'], vtable + 16 + SLIDE, 8 * 267, len(raw))
            got = struct.unpack_from('<267Q', raw, at)
            if got[7] == slots[7] + SLIDE:
                # Only kext-code targets move by the slide; inherited kernel methods are rebound to the native kernel.
                plk = (0xfffffff005920000, 0xfffffff005920000 + 0x1380000)
                moved = [i for i in range(267) if plk[0] <= slots[i] < plk[1]]
                require(all(got[i] == slots[i] + SLIDE for i in moved), 'previous kernel: AppleUSBXDCI vtable differs from the fixture')
                checked = True
        except ValueError:
            pass

    header = '''// Generated by scripts/build-qcom-dwc3.py from scripts/usb-abi-21H461.json (iPadOS 17.7.11, 21H461): opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
'''
    for slot, (macro, _, why) in sorted(HOOKS.items()):
        header += f'#define {macro} opaque{slot}   // {why}\n'
    for name, value in FIELDS.items():
        header += f'constexpr unsigned {name} = {value:#x};\n'
    header += f'''class AppleUSBXDCI : public IOService {{
 OSDeclareAbstractStructors(AppleUSBXDCI)
 unsigned char originalPrivate[{size:#x}-sizeof(IOService)];
public:
'''
    for slot in sorted(IOSERVICE_OVERRIDES):
        header += ' ' + IOSERVICE_OVERRIDES[slot] + '\n'
    for i in range(168, 267):
        ret = HOOKS[i][1] if i in HOOKS else 'void'
        header += f' virtual {ret} opaque{i}(){"=0" if i in pure else ""};\n'
    header += f'}};\nstatic_assert(sizeof(AppleUSBXDCI)=={size:#x},"Original controller instance size");\n'
    (out / 'OriginalUsbXdciABI.h').write_text(header)

    # The kernel's C++ recipe, replayed as build-gop-legacy-display.py replays it for LegacyDisplayProbe.cpp.
    record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                        .read_text().strip().rstrip(','))
    probes = sorted(ROOT.glob('kits/*/T970UsbProbe.cpp')) or [ROOT / 'package/T970UsbProbe.cpp']
    # The build tree links only selected drivers: the device repository is where its package (kits/<board>) points.
    roots = [ROOT] + ([probes[0].resolve().parent.parent] if probes[0].is_file() else [])
    driver = next((r / 'drivers/QcomDwc3/QcomDwc3Sm8250.cpp' for r in roots if (r / 'drivers/QcomDwc3/QcomDwc3Sm8250.cpp').is_file()), None)
    qmp = next((r / 'drivers/QcomXnuUsbDebug/soc' for r in roots if (r / 'drivers/QcomXnuUsbDebug/soc/Sm8250QmpUsb3.h').is_file()), None)
    if not (probes[0].is_file() and driver and qmp):
        empty_archive(out, 'QcomDwc3 sources not found')
        return
    stage = Path(tempfile.mkdtemp(prefix='unixnu-qcom-dwc3-'))

    def compile_cpp(source, obj):
        staged = stage / source.name
        shutil.copyfile(source, staged)
        require(sha(staged) == sha(source), f'{source} changed while it was copied; rerun')
        command, args, i, suppressions = [], record['arguments'], 0, []
        while i < len(args):
            v = args[i]
            if v == record['file']:
                command.append(str(staged))
            elif v == '-o':
                command += ['-o', str(obj)]
                i += 1
            elif v in ('-MF', '-MT', '-MJ'):
                i += 1
            elif v == '-Xclang' and args[i + 1].startswith('-W'):
                i += 1
            elif v.startswith('-Wno-'):
                suppressions.append(v)
            elif not v.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
                command.append(v)
            i += 1
        command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-deprecated-enum-enum-conversion',
                    '-I', str(record['directory']), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                    '-I', str(out), '-I', str(driver.parent), '-I', str(qmp)] + suppressions
        run(command, cwd=record['directory'])
        return command

    obj, probe_obj = out / 'QcomDwc3Sm8250.o', out / 'T970UsbProbe.o'
    command = compile_cpp(driver, obj)
    compile_cpp(probes[0], probe_obj)

    # The compiled vtable, through its relocations (build-sheng-hid-input.py's reader).
    raw = obj.read_bytes()
    om = macho(raw)
    st = next(p for c, p, _ in om['commands'] if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    symbols = []
    for i in range(count):
        ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
    vt = next(va for name, va, _ in symbols if name == '__ZTV12QcomDwc3Xdci')
    relocations = {}
    for segment in om['segments']:
        for sec in segment['sections']:
            if sec['va'] <= vt < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                        relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]
    compiled = [relocations.get(vt + 16 + 8 * i) for i in range(267)]
    require(all(compiled) and vt + 16 + 8 * 267 not in relocations, 'compiled vtable is not exactly 267 entries')
    plk = (0xfffffff005920000, 0xfffffff005920000 + 0x1380000)   # __PLK_TEXT_EXEC of this kernelcache
    bindings = {'__ZN12AppleUSBXDCIC2EPK11OSMetaClass': C2 + SLIDE, '__ZN12AppleUSBXDCID2Ev': D2 + SLIDE,
                '__ZN12AppleUSBXDCI10gMetaClassE': metaclass + SLIDE, '__ZTV12AppleUSBXDCI': vtable + SLIDE}
    owned = []
    for i, name in enumerate(compiled):
        if name.startswith(OWNED):
            owned.append(i)
        elif '12AppleUSBXDCI' in name:
            require(i not in pure, f'slot {i} ({name}) inherits a pure original slot')
            require(bindings.setdefault(name, slots[i] + SLIDE) == slots[i] + SLIDE, f'{name}: two original addresses')
        else:
            require(i < 168 and not plk[0] <= slots[i] < plk[1], f'slot {i} ({name}) skips an AppleUSBXDCI override')
    want = sorted({0, 1, 7, 86} | set(HOOKS))
    require(owned == want, f'owned slots {owned}, expected {want}')
    undefined = {n for n, _, k in symbols if k == 1 and '12AppleUSBXDCI' in n}
    by_name = {n: i for i, n in enumerate(compiled)}
    for name in sorted(undefined - set(bindings)):
        # A qualified call (AppleUSBXDCI::start, AppleUSBXDCI::opaque263) names a slot the vtable inherits or overrides.
        slot = 86 if name == '__ZN12AppleUSBXDCI5startEP9IOService' else (
            int(name.split('opaque')[1].split('E')[0]) if 'opaque' in name else by_name.get(name))
        require(slot is not None and slot not in pure, f'{name}: not an original concrete AppleUSBXDCI slot')
        bindings[name] = slots[slot] + SLIDE
    require(undefined <= set(bindings), 'unbound original imports: ' + str(sorted(undefined - set(bindings))))
    probe_raw = probe_obj.read_bytes()
    require(b'12AppleUSBXDCI' not in probe_raw, 'the probe must not reach the original class directly')

    from kernel_target import TRIPLE
    asm = out / 'usb-bindings.S'
    asm.write_text(''.join(f'.globl {n}\n.set {n}, {v:#x}\n' for n, v in sorted(bindings.items())))
    clang = shutil.which('clang') or 'clang'
    run([clang, '-target', TRIPLE, '-c', asm, '-o', out / 'usb-bindings.o'])
    llvm = Path(shutil.which('llvm-objcopy')).parent
    late = out / 'QcomDwc3Sm8250.late.o'
    run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_QcomDwc3Sm8250.cpp',
         obj, late])
    archive = out / 'libQcomDwc3.a'
    archive.unlink(missing_ok=True)
    run([llvm / 'llvm-ar', 'rcs', archive, late, probe_obj, out / 'usb-bindings.o'])
    (out / 'build-report.json').write_text(json.dumps({
        'os': abi['os'], 'kernelcache_sha256': abi['kernelcache_sha256'], 'slide': hex(SLIDE),
        'previous_kernel_vtable_checked': checked, 'owned_slots': owned, 'hooks': {str(k): v[0] for k, v in HOOKS.items()},
        'fields': {k: hex(v) for k, v in FIELDS.items()}, 'bindings': {n: hex(v) for n, v in sorted(bindings.items())},
        'compile_command': command, 'archive_sha256': sha(archive),
        'source_sha256': {str(s): sha(s) for s in (driver, probes[0], qmp / 'Sm8250QmpUsb3.h',
                                                   ROOT / 'scripts/usb-abi-21H461.json')}}, indent=2) + '\n')
    print(f'QcomDwc3: {len(bindings)} original bindings, owned slots {owned}, previous-kernel check {"done" if checked else "skipped"}')
    print(archive)


if __name__ == '__main__':
    main()
