#!/usr/bin/env python3
"""Host checks for the SM8250 / xnu-10063.141.1 port of QcomXnuUsbDebug.

Read-only with respect to every repository and to the live XNU build tree.
Everything this script writes goes under its own --output directory.

  A  patch round trip    git apply --check/apply/reverse on a fixture copy
  B  patch structure     each hunk lands in the expected function/region
  C  profile            Sm8250.h satisfies QcomUsbSoc.h's static assertions
  D  console ownership  the tee keeps every console byte on USB exactly once,
                        including while zzz-boot-progress.patch is active
  E  early safety       the console hook is safe before the scheduler exists

Usage:
  check_port.py --module <QcomXnuUsbDebug clone> --output <new dir>
                [--include <module include dir>] [--target-clang <clang>]
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
PORT = HERE.parent
FIXTURE = PORT / 'fixture' / 'stock'
PATCHES = sorted((PORT / 'patches' / 'darwin23').glob('*.patch'))
FILES = ['pexpert/arm/pe_kprintf.c', 'osfmk/kern/debug.c', 'osfmk/kern/printf.c',
         'osfmk/console/serial_console.c', 'osfmk/console/serial_general.c',
         'bsd/kern/bsd_init.c']


def run(argv, **kw):
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, **kw)


def must(argv, **kw):
    r = run(argv, **kw)
    if r.returncode:
        raise RuntimeError(' '.join(map(str, argv)) + '\n' + r.stdout + r.stderr)
    return r.stdout


class Checks:
    def __init__(self):
        self.results = []

    def add(self, name, ok, detail=''):
        self.results.append({'check': name, 'pass': bool(ok), 'detail': detail})
        print(('PASS  ' if ok else 'FAIL  ') + name + ('  ' + detail if detail else ''))
        return ok


# ---------------------------------------------------------------- A: patches
def check_roundtrip(tmp, checks, git_available):
    tree = tmp / 'roundtrip'
    shutil.copytree(FIXTURE, tree)
    if not git_available:
        checks.add('A1 patch round trip', False, 'git not available')
        return
    must(['git', 'init', '-q'], cwd=tree)
    must(['git', 'add', '-A'], cwd=tree)
    must(['git', '-c', 'user.name=check', '-c', 'user.email=check@local',
          'commit', '-q', '-m', 'fixture'], cwd=tree)
    names = [p.name for p in PATCHES]
    checks.add('A1 patch series numbering',
               all(n.startswith(f'{i:04d}-') for i, n in enumerate(names, 1)), ' '.join(names))
    r = run(['git', 'apply', '--check', *PATCHES], cwd=tree)
    checks.add('A2 git apply --check', r.returncode == 0, r.stderr.strip()[:400])
    r = run(['git', 'apply', *PATCHES], cwd=tree)
    if not checks.add('A3 git apply', r.returncode == 0, r.stderr.strip()[:400]):
        return
    mismatch = []
    for f in FILES:
        if (tree / f).read_bytes() != (PORT / 'fixture' / 'ported' / f).read_bytes():
            mismatch.append(f)
    checks.add('A4 applied tree equals ported fixture', not mismatch, ','.join(mismatch))
    r = run(['git', 'apply', '--reverse', *reversed(PATCHES)], cwd=tree)
    checks.add('A5 git apply --reverse', r.returncode == 0, r.stderr.strip()[:400])
    restored = all((tree / f).read_bytes() == (FIXTURE / f).read_bytes() for f in FILES)
    checks.add('A6 reverse restores stock bytes', restored)
    r1 = run(['git', 'apply', '--check', *PATCHES], cwd=tree)
    r2 = run(['git', 'apply', '--check', *PATCHES], cwd=tree)
    checks.add('A7 apply is idempotent-safe', r1.returncode == 0 and r2.returncode == 0)


# ------------------------------------------------------- B: patch structure
def stock_functions(text):
    return set(re.findall(r'^(\w+)\s*\(', text, re.M))


def check_structure(checks):
    """Every hunk must name, as its trailing context, a function that still
    exists in the xnu-10063 stock file it edits, or the file scope marker.
    That is the mechanical statement of "the tee point still exists"."""
    text = ''.join(p.read_text() for p in PATCHES)
    current = None
    bad, named = [], {}
    for line in text.splitlines():
        if line.startswith('--- a/'):
            current = line[6:]
        elif line.startswith('@@'):
            m = re.search(r'@@ -\d+(?:,\d+)? \+\d+(?:,\d+)? @@ ?(.*)', line)
            ctx = (m.group(1) if m else '').strip()
            if current is None:
                bad.append('hunk before file header')
                continue
            named.setdefault(current, []).append(ctx)
    for f, contexts in named.items():
        stock = (FIXTURE / f).read_text()
        funcs = stock_functions(stock)
        for ctx in contexts:
            if not ctx:
                bad.append(f'{f}: hunk with no context')
            elif ctx == '<file scope>':
                continue
            elif ctx not in funcs:
                bad.append(f'{f}: {ctx!r} is not a function in the stock file')
    checks.add('B1 every hunk names a function that exists in xnu-10063',
               not bad, '; '.join(bad[:4]))
    # Anchors that must survive: the exact tee points this port relies on.
    anchors = {
        'pexpert/arm/pe_kprintf.c': ['PE_init_kprintf', 'kprintf', '_kprintf',
                                     'PE_init_kprintf_config'],
        'osfmk/kern/debug.c': ['panic_trap_to_debugger', 'panic_spin_forever'],
        'osfmk/kern/printf.c': ['consdebug_putc', 'consdebug_putc_unbuffered'],
        'osfmk/console/serial_console.c': ['_cnputs'],
        'osfmk/console/serial_general.c': ['console_printbuf_putc'],
        'bsd/kern/bsd_init.c': ['bsdinit_task'],
    }
    missing = [f'{f}:{a}' for f, a in anchors.items()
               for a in a if a not in named.get(f, [])]
    checks.add('B2 all declared tee anchors are present', not missing, ','.join(missing))
    # PE-side tee must resolve all three real PE_kputc owners.
    pk = (PORT / 'fixture' / 'ported' / 'pexpert/arm/pe_kprintf.c').read_text()
    checks.add('B3 PE tee gives USB the byte for every non-console owner',
               'PE_kputc != console_write_unbuffered' in pk and
               pk.count('PE_kputc(c);') == 1)
    sc = (PORT / 'fixture' / 'ported' / 'osfmk/console/serial_console.c').read_text()
    body = sc[sc.index('_cnputs(char *c'):]
    body = body[:body.index('\n}\n')]
    checks.add('B4 tee precedes the disableConsoleOutput return',
               0 <= body.find('qcom_usb_debug_write') < body.find('disableConsoleOutput'))
    cnputc = sc[sc.index('_cnputc(char c'):]
    cnputc = cnputc[:cnputc.index('\n}\n')]
    checks.add('B5 tee lives in _cnputs; the boot-progress redirect is untouched',
               'qcom_usb_debug_write' not in cnputc and
               'boot_progress_putc(c); return;' in cnputc and
               sc.count('boot_progress_putc(c); return;') == 1 and
               pk.count('PE_kputc = boot_progress_putc; return;') == 1)


# ------------------------------------------------------------ C: SoC profile
def check_profile(tmp, checks, module, target_clang):
    soc_include = PORT / 'soc'
    probe = tmp / 'profile.c'
    probe.write_text('#include <QcomUsbSoc.h>\n'
                     'int soc_probe(void) { return QUD_DWC3_BYTES; }\n')
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror',
             '-I' + str(module / 'include'), '-I' + str(soc_include),
             '-DQUD_SOC_HEADER="Sm8250.h"', '-fsyntax-only']
    cc = target_clang or shutil.which('clang')
    if not cc:
        checks.add('C1 profile compiles under the module assertions', False, 'no clang')
        return
    r = run([cc, *flags, probe])
    if not checks.add('C1 profile compiles under the module assertions',
                      r.returncode == 0, (r.stdout + r.stderr).strip()[:600]):
        return
    # Offsets must name SM8250 registers, not Kalama ones.
    h = (soc_include / 'Sm8250.h').read_text()
    checks.add('C2 five CBCRs are the SM8250 prim branches',
               re.search(r'QUD_GCC_CBCR_OFFSETS \{\s*0xf010u,\s*0xf018u,\s*0xf01cu,\s*0xf054u,\s*0xf058u\}', h) is not None)
    checks.add('C3 stream ID is SM8250 SID 0x0', 'QUD_STREAM_ID 0x0u' in h)
    checks.add('C4 QSCRATCH offset carries wrapper arithmetic',
               'QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0xf8810u' in h and '0x0a6f8800' in h)
    checks.add('C5 no SM8550 Kalama clock offset survived',
               '0x39018' not in h and '0x39088' not in h and '0x3908c' not in h)


# ------------------------------------------------------- D: console ownership
def check_console_model(tmp, checks):
    """Compile the real patched _cnputs/_cnputc/Pe_kputc logic against a
    stand-in console and count how many times each byte reaches USB."""
    src = (PORT / 'fixture' / 'ported' / 'osfmk/console/serial_console.c').read_text()
    driver = tmp / 'console_model.c'
    preamble = r'''
/* Stand-ins for exactly the XNU services the copied functions call.  Anything
 * not listed here is absent on purpose: if the ported code grows a new XNU
 * dependency, this model stops compiling and the test fails. */
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#define XNU_QCOM_USB_DEBUG 1
#define CPU_CONS_BUF_SIZE 256

typedef void (*PE_kputc_t)(char);
static PE_kputc_t PE_kputc;

static bool disableConsoleOutput;
static bool disable_serial_output;
static int  console_bytes, video_bytes, bp_bytes, serial_bytes;
static unsigned kernel_debugger_entry_count;

static void qcom_usb_debug_putc(char c);
size_t qcom_usb_debug_write(const void *b, size_t n);
static void boot_progress_putc(char c);
bool boot_progress_active(void);
void console_write_unbuffered(char c);
void console_write_char(char c);
static bool console_is_serial(void);

struct cons_op { void (*putc)(char, bool); };
static struct cons_op cons_ops[2];
static uint32_t cons_ops_index;
static const uint32_t nconsops __attribute__((unused)) = 2;

static uint32_t get_cons_ops_index(void) { return cons_ops_index; }
static void debug_putc(char c) { (void)c; }
'''
    driver.write_text(preamble)
    sc = src
    for fn in ('_cnputc', '_cnputs'):
        m = re.search(r'^static inline void\n' + fn + r'\(.*?\n\}\n', sc, re.M | re.S)
        assert m, fn
        driver.write_text(driver.read_text() + m.group(0) + '\n')
    pr = (PORT / 'fixture' / 'ported' / 'osfmk/kern/printf.c').read_text()
    m = re.search(r'static inline bool\nqcom_usb_pe_kputc_reenters_console\(void\)\n\{.*?\n\}\n', pr, re.S)
    assert m, 'printf re-entry predicate moved; update the model'
    driver.write_text(driver.read_text() + m.group(0) + '\n')
    for fn in ('consdebug_putc', 'consdebug_putc_unbuffered'):
        m = re.search(r'^void\n' + fn + r'\(char c\)\n\{.*?\n\}\n', pr, re.M | re.S)
        assert m, fn
        driver.write_text(driver.read_text() + m.group(0) + '\n')
    pk = (PORT / 'fixture' / 'ported' / 'pexpert/arm/pe_kprintf.c').read_text()
    m = re.search(r'static bool\nqcom_usb_pe_kputc_owns_usb_byte\(void\)\n\{.*?\n\}\n', pk, re.S)
    assert m, 'PE ownership predicate moved; update the model'
    driver.write_text(driver.read_text() + m.group(0))
    driver.write_text(driver.read_text() + r'''
static void qcom_usb_tee(char c)
{
	/* body copied from the patch; see 0001-qcom-usb-debug-console.patch */
	if (PE_kputc && qcom_usb_pe_kputc_owns_usb_byte()) qcom_usb_debug_putc(c);
	if (PE_kputc) PE_kputc(c);
}

/* ---- implementations of the services above --------------------------- */
static void video_putc(char c, bool poll) { (void)c; (void)poll; video_bytes++; }
static void qcom_usb_debug_putc(char c) { (void)c; console_bytes++; }
size_t qcom_usb_debug_write(const void *b, size_t n)
{
	(void)b; console_bytes += (int)n; return n;
}
static void boot_progress_putc(char c) { (void)c; bp_bytes++; }
bool boot_progress_active(void) { return PE_kputc == boot_progress_putc; }
static bool console_is_serial(void) { return false; }
/* console_write_char and console_write_unbuffered feed _cnputs, which is
 * where this port tees. */
void console_write_char(char c) { _cnputs(&c, 1, true); }
void console_write_unbuffered(char c) { _cnputs(&c, 1, true); }
static void serial_putc_crlf(char c) { (void)c; serial_bytes++; }

static int fails;
static void expect(const char *name, int want_usb, int want_video)
{
	int bad = (console_bytes != want_usb) || (video_bytes != want_video);
	printf("%s %-52s usb=%d video=%d\n", bad ? "FAIL" : "PASS", name,
	       console_bytes, video_bytes);
	if (bad) fails++;
	console_bytes = video_bytes = bp_bytes = serial_bytes = 0;
	disableConsoleOutput = false;
}

int main(void)
{
	cons_ops[0].putc = video_putc;
	cons_ops[0].putc = video_putc;

	PE_kputc = console_write_unbuffered;
	qcom_usb_tee('x');
	expect("fallback console, console enabled: 1 byte", 1, 1);

	disableConsoleOutput = true;
	qcom_usb_tee('x');
	expect("fallback console, console muted: 1 byte", 1, 0);

	PE_kputc = boot_progress_putc;
	qcom_usb_tee('x');
	expect("boot-progress active: 1 byte, no duplicate", 1, 0);

	PE_kputc = serial_putc_crlf;
	qcom_usb_tee('x');
	expect("serial PE_kputc: 1 byte", 1, 0);

	PE_kputc = console_write_unbuffered;
	console_write_char('p');
	expect("printf path, console enabled", 1, 1);

	disableConsoleOutput = true;
	console_write_char('p');
	expect("printf path, console muted: 1 byte", 1, 0);

	disableConsoleOutput = false;
	consdebug_putc('k');
	expect("consdebug_putc, console enabled", 1, 1);

	consdebug_putc_unbuffered('k');
	expect("consdebug_putc_unbuffered", 1, 1);

	disableConsoleOutput = true;
	consdebug_putc_unbuffered('k');
	expect("consdebug_putc_unbuffered, console muted", 1, 0);

	PE_kputc = boot_progress_putc;
	disableConsoleOutput = false;
	consdebug_putc('b');
	expect("consdebug_putc with boot-progress", 1, 0);

	PE_kputc = console_write_unbuffered;
	{
		char buf[1024];
		memset(buf, 'a', sizeof(buf));
		_cnputs(buf, sizeof(buf), true);
	}
	expect("1024-byte console write", 1024, 1024);

	printf("%s console ownership model\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
''')
    cc = shutil.which('clang') or shutil.which('cc')
    exe = tmp / 'console_model'
    r = run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             driver, '-o', exe])
    if not checks.add('D1 console ownership model compiles', r.returncode == 0,
                      (r.stdout + r.stderr).strip()[:800]):
        return
    r = run([exe])
    print(r.stdout.rstrip())
    checks.add('D2 every console byte reaches USB exactly once',
               r.returncode == 0, r.stdout.strip().splitlines()[-1] if r.stdout else '')
    checks.add('D3 no console byte is lost while the console is muted',
               all('usb=1 video=0' in l for l in r.stdout.splitlines() if 'muted' in l))


# ----------------------------------------------------------- E: early safety
def check_early(tmp, checks, module):
    """The console hook runs before the scheduler and before CPU data exists.
    Prove the reached implementation never calls anything that needs them."""
    mod_src = module / 'kernel' / 'QcomXnuUsbDebug.c'
    if not mod_src.is_file():
        checks.add('E1 early hook is scheduler-free', False, 'module source missing')
        return
    text = mod_src.read_text()
    body = re.search(r'size_t qcom_usb_debug_write\(.*?\n\}\n', text, re.S)
    if not body:
        checks.add('E1 early hook is scheduler-free', False, 'write not found')
        return
    write_body = body.group(0)
    banned = ['cpu_number(', 'ml_io_map(', 'ml_static_vtop(', 'kmem_alloc', 'zalloc',
              'kalloc', 'kernel_thread_start', 'thread_block', 'assert_wait',
              'PE_parse_boot_argn', 'kprintf', 'printf(', 'panic(']
    early = write_body[:write_body.index('acquire(&flags)')] if 'acquire(&flags)' in write_body else write_body
    hits = [b for b in banned if b in early]
    checks.add('E1 write prefix is scheduler-free', not hits, ','.join(hits))
    # backlog_write must mask DAIF around the ring guard.
    bw = re.search(r'static size_t backlog_write\(.*?\n\}\n', text, re.S)
    ok = bool(bw) and 'msr daifset' in bw.group(0) and 'msr daif' in bw.group(0)
    checks.add('E2 early producer masks IRQ/FIQ around the ring', ok)
    # and must not take the scheduler lock the arm_init path cannot have.
    code = re.sub(r'/\*.*?\*/', '', bw.group(0) if bw else '', flags=re.S)
    checks.add('E3 early producer never spins',
               bool(bw) and 'while' not in code and 'delay' not in code)
    # confirm the call site the patch installs is after serial_init()
    pk = (PORT / 'fixture' / 'ported' / 'pexpert/arm/pe_kprintf.c').read_text()
    body = pk[pk.index('PE_init_kprintf(void)'):]
    body = body[:body.index('STARTUP(KPRINTF')]
    checks.add('E4 init runs inside PE_init_kprintf, before the boot-progress return',
               body.index('qcom_usb_debug_init()') < body.index('boot_progress_active()') and
               'serial_init()' in body and
               'PE_kputc = qcom_usb_debug_putc' in body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--module', type=pathlib.Path, required=True)
    ap.add_argument('--output', type=pathlib.Path, required=True)
    ap.add_argument('--target-clang', type=pathlib.Path)
    a = ap.parse_args()
    out = a.output.resolve()
    if out.exists():
        ap.error('output must be a new directory')
    out.mkdir(parents=True)
    module = a.module.resolve()
    checks = Checks()
    git = shutil.which('git') is not None
    check_roundtrip(out, checks, git)
    check_structure(checks)
    check_profile(out, checks, module, a.target_clang)
    check_console_model(out, checks)
    check_early(out, checks, module)
    ok = sum(1 for r in checks.results if r['pass'])
    total = len(checks.results)
    report = {
        'hardware_verified': False,
        'kernel_linked': False,
        'patched_tree_built': False,
        'checks': checks.results,
        'passed': ok,
        'total': total,
        'source_sha256': {str(p.relative_to(PORT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(PORT.rglob('*'))
                          if p.is_file() and p.suffix in ('.c', '.h', '.patch', '.py')},
    }
    (out / 'check-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'\nPassed {ok}/{total} checks; report {out / "check-report.json"}')
    return 0 if ok == total else 1


if __name__ == '__main__':
    sys.exit(main())
