#!/usr/bin/env python3
"""Cross-compile the ported translation units with the tree's own flags.

Reads the real compile records that the existing xnu-10063 build already
produced, strips only the source/output/dependency operands, and adds

    -DXNU_QCOM_USB_DEBUG=1 -I<module include dir>

then compiles the *patched* sources from this port's fixture.  Nothing in the
XNU tree is read-write; outputs go to the directory given after the records.

  compile_units.py <port-relative fixture root> <module include dir> <out dir>

Run it on orb (the compile records name /home/xinyang paths).
"""
import json
import pathlib
import subprocess
import sys

TREE = pathlib.Path('/home/xinyang/Lab/Bridge/WorkSpace/alioth/NEW/UniXNU/out/alioth/'
                    'ipados-17.7.11-21H461/alioth/tree')
RECORDS = TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
UNITS = {
    'pexpert/DEVELOPMENT/pe_kprintf.o.json': 'pexpert/arm/pe_kprintf.c',
    'osfmk/DEVELOPMENT/debug.o.json': 'osfmk/kern/debug.c',
    'osfmk/DEVELOPMENT/printf.o.json': 'osfmk/kern/printf.c',
    'osfmk/DEVELOPMENT/serial_console.o.json': 'osfmk/console/serial_console.c',
    'osfmk/DEVELOPMENT/serial_general.o.json': 'osfmk/console/serial_general.c',
    'bsd/DEVELOPMENT/bsd_init.o.json': 'bsd/kern/bsd_init.c',
}
CONSUMES = {'-o', '-MF', '-MT', '-MQ', '-MJ', '-dependency-file',
            '--serialize-diagnostics', '-serialize-diagnostics'}
DISCARD = {'-c', '-fsyntax-only', '-M', '-MM', '-MD', '-MMD', '-MP', '-MG',
           '-save-temps', '-save-temps=obj'}


def flags(record, source):
    args = record['arguments']
    cwd = pathlib.Path(record['directory'])
    out, i = [args[0]], 1
    while i < len(args):
        a = args[i]
        if a in CONSUMES:
            i += 2
            continue
        if a in DISCARD:
            i += 1
            continue
        if not a.startswith('-') and (cwd / a).name == pathlib.Path(source).name:
            i += 1
            continue
        if any(a.startswith(p) and len(a) > len(p) for p in ('-o', '-MF', '-MT', '-MQ', '-MJ')):
            i += 1
            continue
        out.append(a)
        i += 1
    return out, cwd


def main():
    fixture = pathlib.Path(sys.argv[1]).resolve()
    include = pathlib.Path(sys.argv[2]).resolve()
    outdir = pathlib.Path(sys.argv[3]).resolve()
    outdir.mkdir(parents=True, exist_ok=False)
    results = {}
    for record, src in UNITS.items():
        rp = RECORDS / record
        if not rp.is_file():
            results[src] = {'ok': False, 'why': 'no compile record ' + str(rp)}
            continue
        d = json.loads(rp.read_text().rstrip().rstrip(','))
        argv, cwd = flags(d, src)
        obj = outdir / (pathlib.Path(src).stem + '.o')
        for mode in ('off', 'on'):
            extra = ['-I' + str(include), '-DXNU_QCOM_USB_DEBUG=' + ('1' if mode == 'on' else '0')]
            cmd = argv + extra + ['-c', str(fixture / src), '-o', str(obj if mode == 'on' else
                                                                    outdir / (pathlib.Path(src).stem + '-off.o'))]
            r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
            results.setdefault(src, {})[mode] = {
                'ok': r.returncode == 0,
                'errors': (r.stdout + r.stderr).strip().splitlines()[-12:] if r.returncode else [],
            }
        print(('PASS  ' if results[src].get('on', {}).get('ok') else 'FAIL  ') + src)
        for line in results[src].get('on', {}).get('errors', []):
            print('        ' + line)
    (outdir / 'compile-report.json').write_text(json.dumps(results, indent=2) + '\n')
    bad = [k for k, v in results.items() if not v.get('on', {}).get('ok') or not v.get('off', {}).get('ok')]
    print(('\nAll units compile with the hook on and off' if not bad
           else '\nFailed: ' + ', '.join(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
