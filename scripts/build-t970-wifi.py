#!/usr/bin/env python3
"""Build the full Wi-Fi stack of gts7xlwifi (QCA6390 hw2.0 on SM8250 PCIe0) into build/t970-wifi/libT970Wifi.a.

The driver is Linux v7.0.14 drivers/net/wireless/ath/ath11k (PCI, MHI, QMI) on the UniXNU Ath12kWifi module's shared ABI
(include/nw_abi.h) and Linux runtime shim, with the module's mainline cfg80211/mac80211, MHI host, QMI, QRTR and crypto objects
exactly as UniXNU-sheng links them (scripts/build-ath12k-wifi.py, tools/sources.txt), but with the ath12k transport replaced by
ath11k and nw_wifi.c/nw_debug.c by package/T970Ath11k.c. The board half (package/T970WifiStack.cpp: lx_platform/nw_board callbacks,
DMA arena in the stage-5 private SMMU context, DesignWare iMSI, control endpoint, worker) and the network half
(package/T970WifiNet.cpp, from UniXNU-sheng WifiNet.cpp, 21H461) are compiled with the kernel's own C++ recipe.

The archive is force-loaded by the kernel link (configs/gts7xlwifi_defconfig CONFIG_XNU_EXTRA_LD_ARCHIVES); nothing runs unless
t970-wifi=6 starts stage 6 in package/T970WifiProbe.cpp. No Linux source is copied into the repository: everything Linux is
generated under build/t970-wifi from the module's verified deps/ checkout.

The module directory comes from T970_ATH12K_WIFI_DIR, else drivers/Ath12kWifi in the tree, else
<device repo>/../sources/periph/Ath12kWifi (this host's layout).
"""
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE, check_object  # noqa: E402

OUT = ROOT / 'build/t970-wifi'
RAW = OUT / 'raw'


def require(value, message):
    if not value:
        raise SystemExit('build-t970-wifi: ' + message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


packages = sorted(ROOT.glob('kits/*/T970WifiProbe.cpp'))
require(len(packages) == 1, 'expected one device package with T970WifiProbe.cpp under kits/')
PACKAGE = packages[0].parent.resolve()


def module_dir():
    for candidate in (os.environ.get('T970_ATH12K_WIFI_DIR'), ROOT / 'drivers/Ath12kWifi',
                      PACKAGE.parent.parent / 'sources/periph/Ath12kWifi'):
        if candidate and (Path(candidate) / 'src/nw_mainline.c').is_file():
            return Path(candidate).resolve()
    raise SystemExit('build-t970-wifi: the Ath12kWifi module with the mainline runtime (src/nw_mainline.c) was not found; '
                     'set T970_ATH12K_WIFI_DIR')


MODULE = module_dir()
LINUX = MODULE / 'deps/linux'
ATH11K = LINUX / 'drivers/net/wireless/ath/ath11k'
require((ATH11K / 'pci.c').is_file(), f'{ATH11K}/pci.c missing: the module checkout has no ath11k (tools/fetch-sources.sh)')
OUT.mkdir(parents=True, exist_ok=True)
RAW.mkdir(exist_ok=True)
(OUT / 'build-report.json').unlink(missing_ok=True)
(OUT / 'compile-errors.txt').unlink(missing_ok=True)

# The module's own generated trees (pinned headers, mac80211/cfg80211/crypto copies, MHI and QMI with their OS patches).
run(['bash', MODULE / 'tools/fetch-sources.sh'], stdout=subprocess.DEVNULL)
run(['bash', MODULE / 'tools/prepare.sh'], stdout=subprocess.DEVNULL)


# ---- the ath11k tree: links to the v7.0.14 sources, generated copies where this port changes one ----------------------------
GEN = OUT / 'ath11k-src'
if GEN.exists():
    shutil.rmtree(GEN)
GEN.mkdir()
for f in sorted(ATH11K.iterdir()):
    if f.suffix in ('.c', '.h') and f.name != 'trace.h':
        (GEN / f.name).symlink_to(f)
(OUT / 'spectral_common.h').unlink(missing_ok=True)
(OUT / 'spectral_common.h').symlink_to(ATH11K.parent / 'spectral_common.h')   # spectral.h: "../spectral_common.h"
traces = sorted({t for f in ATH11K.glob('*.[ch]') for t in re.findall(r'\btrace_ath11k_[a-z0-9_]+', f.read_text())})
(GEN / 'trace.h').write_text('#ifndef T970_ATH11K_TRACE_H\n#define T970_ATH11K_TRACE_H\n' +
                             ''.join(f'#define {t}(...) ' + ('0' if t.endswith('_enabled') else '((void)0)') + '\n' for t in traces) +
                             '#endif\n')
patches = []


def patch(name, edits):
    """Replace GEN/name (a link) by a copy with each (old, new, count) edit applied exactly count times."""
    target = GEN / name
    text = (ATH11K / name).read_text()
    for old, new, count in edits:
        require(text.count(old) == count, f'{name}: v7.0.14 text differs at {old[:60]!r} ({text.count(old)} != {count})')
        text = text.replace(old, new)
    target.unlink()
    target.write_text(text)
    patches.append({'file': name, 'upstream_sha256': sha(ATH11K / name), 'output_sha256': sha(target), 'edits': len(edits)})


# The shim's vsnprintf has no %pV, so ath11k_info/err/warn/dbg printed the va_format's address ("0x...V", combo1b): the
# message is formatted here first. Debug lines also print when nw_wifi_t970_dbg (T970Ath11k.c, boot-arg t970-wifi-dbg) has the mask.
T970_FMT = '\t{ va_list t970c; char t970b[224]; va_copy(t970c, args); vsnprintf(t970b, sizeof t970b, fmt, t970c); va_end(t970c);\n\t  %s }\n'
patch('debug.c', [
    ('\tdev_info(ab->dev, "%pV", &vaf);\n', T970_FMT % 'dev_info(ab->dev, "%s", t970b);', 1),
    ('\tdev_err(ab->dev, "%pV", &vaf);\n', T970_FMT % 'dev_err(ab->dev, "%s", t970b);', 1),
    ('\tdev_warn_ratelimited(ab->dev, "%pV", &vaf);\n', T970_FMT % 'dev_warn(ab->dev, "%s", t970b);', 1),
    ('\tif (ath11k_debug_mask & mask)\n\t\tdev_printk(KERN_DEBUG, ab->dev, "%s %pV", ath11k_dbg_str(mask), &vaf);\n',
     '\t{ extern volatile unsigned int nw_wifi_t970_dbg;\n\t  if ((ath11k_debug_mask | nw_wifi_t970_dbg) & mask)\n'
     + T970_FMT % 'dev_printk(KERN_DEBUG, ab->dev, "%s %s", ath11k_dbg_str(mask), t970b);' + '\t}\n', 1),
])
# A kernel-wide jiffies macro cannot also name this statistics member (as UniXNU-pong build-pong-ath11k.py).
patch('core.h', [('unsigned long jiffies;', 'unsigned long bp_jiffies;', 1)])
# The board files of this tablet are Qualcomm's CNSS2 names (bdwlan.elf, bdwlan.eNN), not Linux's board-2.bin container: the default
# board file request becomes the file the firmware's QMI board ID selects (UniXNU-pong's rule; cnss2 cnss_get_bdf_file_name).
patch('core.c', [('\tbd->fw = ath11k_core_firmware_request(ab, name);', r"""	char t970_bdf[48];
	if (!strcmp(name, ATH11K_DEFAULT_BOARD_FILE)) {
		u32 id = ab->qmi.target.board_id;
		if (id == 0xff)
			snprintf(t970_bdf, sizeof(t970_bdf), "bdwlan.elf");
		else if (id < 0xff)
			snprintf(t970_bdf, sizeof(t970_bdf), "bdwlan.e%02x", id);
		else
			snprintf(t970_bdf, sizeof(t970_bdf), "bdwlan%02x.e%02x", (id >> 8) & 0xff, id & 0xff);
		name = t970_bdf;
		ath11k_info(ab, "t970: board file %s for board id 0x%x\n", name, id);
	}
	bd->fw = ath11k_core_firmware_request(ab, name);""", 1)])
# Statistics member rename, its one use.
patch('dp_rx.c', [('bp_stats->jiffies', 'bp_stats->bp_jiffies', 1)])
# t970-wifi-quirks bit 16: no RDDM. mhi_prepare_for_power_up then neither allocates the dump table nor rings the BHIe RX vector
# doorbell (BAR 0x284..0x294) before the boot loader is offered; int10/int11's write trace shows stage 6 making those writes
# while the passing stage 5 makes none.
patch('mhi.c', [('\tmhi_ctrl->rddm_size = RDDM_DUMP_SIZE;\n',
                 '\t{ extern volatile int nw_wifi_t970_quirks; mhi_ctrl->rddm_size = (nw_wifi_t970_quirks & 16) ? 0 : RDDM_DUMP_SIZE; }\n', 1)])
# The PCI driver's module entry points become callable.
patch('pci.c', [('\tclear_bit(ATH11K_FLAG_DEVICE_INIT_DONE, &ab->dev_flags);\n\tath11k_pci_sw_reset(ab_pci->ab, true);\n',
                 # t970-wifi-quirks bit 0 (nw_wifi_t970_quirks, package/T970Ath11k.c): no sw_reset before MHI power-up
                 '\tclear_bit(ATH11K_FLAG_DEVICE_INIT_DONE, &ab->dev_flags);\n'
                 '\t{ extern volatile int nw_wifi_t970_quirks; if (!(nw_wifi_t970_quirks & 1)) ath11k_pci_sw_reset(ab_pci->ab, true); }\n', 1),
                ('\tval &= ~QFPROM_PWR_CTRL_VDD4BLOW_MASK;\n',
                 # cnss2 set_wlaon_pwr_ctrl (and stage 5, periph23) clears SHUTDOWN_EN (bit 0) as well as VDD4BLOW (bit 2)
                 '\tval &= ~(QFPROM_PWR_CTRL_VDD4BLOW_MASK | 0x1);\n', 1),
                ('module_exit(ath11k_pci_exit);\n', 'module_exit(ath11k_pci_exit);\n\n'
                 'int t970_ath11k_pci_init(void);\nvoid t970_ath11k_pci_exit(void);\n'
                 'int t970_ath11k_pci_init(void) { return ath11k_pci_init(); }\n'
                 'void t970_ath11k_pci_exit(void) { ath11k_pci_exit(); }\n', 1)])
# A radio whose firmware reports no valid address (or Qualcomm's random 00:03:7f default) takes the board's (nw_board_mac in
# package/T970Ath11k.c: wlan_mac.bin, else an address derived from /chosen/unique-chip-id), as the module does for ath12k.
patch('mac.c', [('#include "mac.h"\n', '#include "mac.h"\nvoid nw_board_mac(u8 *a);\n', 1),
                ('\t\t\tar->mac_addr[4] += i;\n\t\t}\n',
                 '\t\t\tar->mac_addr[4] += i;\n\t\t}\n\t\tif (!is_valid_ether_addr(ar->mac_addr) ||\n'
                 '\t\t    (!ar->mac_addr[0] && ar->mac_addr[1] == 0x03 && ar->mac_addr[2] == 0x7f)) {\n'
                 '\t\t\tnw_board_mac(ar->mac_addr);\n\t\t\tar->mac_addr[4] += i;\n\t\t}\n', 1)])

# MHI's boot.c (the module's generated copy, with its pre-load hook) once more: the BHI boot loader buffer is announced to the
# board (nw_wifi_bhi_next, package/T970Ath11k.c) right before it is allocated, so the arena can put it at IOVA 0xa0000000 as stage 5
# did. ath11k's RDDM table (nine 512 KiB segments, allocated earlier by mhi_prepare_for_power_up) otherwise takes that spot (int6).
MHI_BOOT = OUT / 'mhi-src/boot.c'
MHI_BOOT.parent.mkdir(exist_ok=True)
boot = (MODULE / 'out/mhi-src/boot.c').read_text()
mark = '\tfw_load_type = mhi_fw_load_type_get(mhi_cntrl);\n'
require(boot.count(mark) == 1, 'MHI boot.c: the load-type mark differs')
boot = boot.replace(mark, '\t{ extern volatile int nw_wifi_bhi_next; nw_wifi_bhi_next = 1; }\n' + mark)
# t970-wifi-quirks bit 1: no pre-load hook (the module's 200 ms settle and MHI context programming before BHI, a WCN7850 finding)
hook = '\t{ extern void nw_mhi_pre_load(struct mhi_controller *); nw_mhi_pre_load(mhi_cntrl); }\n'
require(boot.count(hook) == 1, 'MHI boot.c: the pre-load hook differs')
boot = boot.replace(hook, '\t{ extern void nw_mhi_pre_load(struct mhi_controller *); extern volatile int nw_wifi_t970_quirks;\n'
                          '\t  if (!(nw_wifi_t970_quirks & 2)) nw_mhi_pre_load(mhi_cntrl); }\n')
MHI_BOOT.write_text(boot)

# MHI's main.c: combo4 (int20) stopped the BDF download after 4 of 10 chunks on a transfer event with code 0x11 (BAD_TRE),
# printed only as "Unknown event 0x11". Say which channel, the event's words, the ring's read/write offsets and the TRE the event
# names (read back by the CPU), so the next run shows whether the TRE the device rejected is the one the host wrote.
MHI_MAIN = OUT / 'mhi-src/main.c'
main_c = (MODULE / 'out/mhi-src/main.c').read_text()
mark = '\tdefault:\n\t\tdev_err(dev, "Unknown event 0x%x\\n", ev_code);\n\t\tbreak;\n'
require(main_c.count(mark) == 1, 'MHI main.c: the unknown-event mark differs')
main_c = main_c.replace(mark, '''\tdefault:
\t{
\t\tdma_addr_t t970p = MHI_TRE_GET_EV_PTR(event);
\t\tstruct mhi_ring_element *t970t = is_valid_ring_ptr(tre_ring, t970p) ? mhi_to_virtual(tre_ring, t970p) : NULL;
\t\tdev_err(dev, "Unknown event 0x%x ch %u ev %llx %08x %08x ring %llx+%x rp %x wp %x\\n", ev_code, mhi_chan->chan,
\t\t\t(unsigned long long)le64_to_cpu(event->ptr), le32_to_cpu(event->dword[0]), le32_to_cpu(event->dword[1]),
\t\t\t(unsigned long long)tre_ring->iommu_base, (unsigned)tre_ring->len,
\t\t\t(unsigned)((u8 *)tre_ring->rp - (u8 *)tre_ring->base), (unsigned)((u8 *)tre_ring->wp - (u8 *)tre_ring->base));
\t\tif (t970t)
\t\t\tdev_err(dev, "t970: TRE %llx %08x %08x\\n", (unsigned long long)le64_to_cpu(t970t->ptr),
\t\t\t\tle32_to_cpu(t970t->dword[0]), le32_to_cpu(t970t->dword[1]));
\t\t{\t/* int24: the channel context as the device left it (combo7: rp read 0x100000001) and the TREs around the rejected one */
\t\t\tconst __le32 *w = (const __le32 *)&mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];
\t\t\tdev_err(dev, "t970: ctxt %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\\n", le32_to_cpu(w[0]), le32_to_cpu(w[1]),
\t\t\t\tle32_to_cpu(w[2]), le32_to_cpu(w[3]), le32_to_cpu(w[4]), le32_to_cpu(w[5]), le32_to_cpu(w[6]), le32_to_cpu(w[7]),
\t\t\t\tle32_to_cpu(w[8]), le32_to_cpu(w[9]), le32_to_cpu(w[10]));
\t\t\tif (t970t) {
\t\t\t\tsize_t n = tre_ring->len / sizeof(*t970t), i = (size_t)(t970t - (struct mhi_ring_element *)tre_ring->base);
\t\t\t\tfor (size_t k = 0; k < 5; k++) {
\t\t\t\t\tconst struct mhi_ring_element *e = (const struct mhi_ring_element *)tre_ring->base + (i + n - 3 + k) % n;
\t\t\t\t\tdev_err(dev, "t970: TRE[%zu] %llx %08x %08x\\n", (i + n - 3 + k) % n, (unsigned long long)le64_to_cpu(e->ptr),
\t\t\t\t\t\tle32_to_cpu(e->dword[0]), le32_to_cpu(e->dword[1]));
\t\t\t\t}
\t\t\t}
\t\t}
\t\tbreak;
\t}
''')
MHI_MAIN.write_text(main_c)

# The module's QRTR name service (src/nw_qrtr.c) sends a broadcast HELLO when the endpoint comes up and, having sent one, never
# answers the remote's HELLO. Linux's qrtr_ns answers every remote HELLO, and the firmware announces its servers (WLFW 0x45) to a
# node only once that node's HELLO reaches its name service. combo2b/combo5 (and int19's combo2): our HELLO went out 0.5 ms
# after the channels started, the firmware sent one packet (its HELLO) and nothing more, so QMI never began. combo4 got HELLO and
# NEW_SERVER together and worked. The firmware's first HELLO is now answered once, whatever we sent before, and each control
# packet from the remote is logged.
NW_QRTR = OUT / 'nw-src/nw_qrtr.c'
NW_QRTR.parent.mkdir(exist_ok=True)
qrtr_c = (MODULE / 'src/nw_qrtr.c').read_text()
for old, new in [
        ('static int g_hello_sent;\n', 'static int g_hello_sent;\nstatic int g_hello_answered;   /* T970: remote HELLOs answered */\n'),
        ('    if (src_node != LOCAL_NODE && !g_hello_sent) { g_hello_sent = 1; send_remote(',
         '    if (src_node != LOCAL_NODE && (!g_hello_sent || !g_hello_answered)) { g_hello_sent = 1; g_hello_answered = 1;\n'
         '      lx_log(6, "qrtr: HELLO from node %u answered", src_node); send_remote('),
        ('  g_xmit = 0; g_hello_sent = 0;\n', '  g_xmit = 0; g_hello_sent = 0; g_hello_answered = 0;\n'),
        ('  g_xmit = xmit; g_ctx = ctx; g_hello_sent = xmit != 0;\n',
         '  g_xmit = xmit; g_ctx = ctx; g_hello_sent = xmit != 0; g_hello_answered = 0;\n'),
        ('  if (len < sizeof *p) return;\n  switch (le32_to_cpu(p->cmd)) {\n',
         '  if (len < sizeof *p) return;\n'
         '  if (src_node != LOCAL_NODE) lx_log(6, "qrtr: ctrl %u from %u:%u svc %x", le32_to_cpu(p->cmd), src_node, src_port,\n'
         '                                     le32_to_cpu(p->server.service));\n'
         '  switch (le32_to_cpu(p->cmd)) {\n')]:
    require(qrtr_c.count(old) == 1, f'nw_qrtr.c: text differs at {old[:50]!r}')
    qrtr_c = qrtr_c.replace(old, new)
NW_QRTR.write_text(qrtr_c)

# ---- compile ------------------------------------------------------------------------------------------------------------------
clang = subprocess.run(['xcrun', '-f', 'clang'], check=True, capture_output=True, text=True).stdout.strip()
resource = subprocess.run([clang, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip() + '/include'
COMPAT = PACKAGE / 'ath11k-compat'
# The module's kernel flags (UniXNU-sheng build-ath12k-wifi.py) plus the ath11k configuration: PCI only, no debugfs/tracing/thermal/
# spectral/testmode/coredump (their objects are not built and their headers compile them out).
flags = ['-target', TRIPLE, '-mkernel', '-O2', '-g0', '-ffreestanding', '-fno-builtin', '-fno-common', '-fno-strict-aliasing', '-fwrapv',
         '-fno-stack-protector', '-Wno-incompatible-pointer-types', '-Wno-error', '-fno-delete-null-pointer-checks', '-mgeneral-regs-only',
         '-momit-leaf-frame-pointer', '-std=gnu11', '-w',
         '-DCONFIG_CFG80211=1', '-DCONFIG_MAC80211=1', '-DCONFIG_MAC80211_STA_HASH_MAX_SIZE=0', '-DCONFIG_MAC80211_RC_DEFAULT=""',
         '-DCONFIG_ATH11K_PCI=1',
         '-nostdinc', '-isystem', str(MODULE / 'shim/libc'), '-isystem', resource, '-include', 'lx_pci.h',
         '-I', str(COMPAT), '-I', str(MODULE / 'shim/include'), '-I', str(MODULE / 'include'), '-I', str(MODULE / 'out/pinned'),
         '-I', str(MODULE / 'src'), '-I', str(MODULE / 'out')]
ath11k_tree = ['-DCONFIG_ATH11K_DEBUG=1', '-include', 'T970Ath11kCompat.h', '-I', str(GEN), '-I', str(ATH11K.parent)]
trees = {'mhi': ['-I', str(MODULE / 'out/mhi-src')], 'qmi': ['-I', str(MODULE / 'out/os-src')],
         'mac80211': ['-I', str(MODULE / 'out/mac80211')], 'cfg80211': ['-I', str(MODULE / 'out/cfg80211')]}
trees['generated'] = trees['cfg80211']
trees['crypto'] = ['-I', str(MODULE / 'shim/crypto'), '-I', str(MODULE / 'out/crypto')]
# Module sources replaced here: the ath12k transport and its start/debug glue (nw_wifi.c, nw_debug.c -> package/T970Ath11k.c).
REPLACED = {'src/nw_wifi.c', 'src/nw_debug.c'}
jobs = []
for line in (MODULE / 'tools/sources.txt').read_text().splitlines():
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    tree, _, name = line.partition(':') if ':' in line else ('', '', line)
    if tree == 'ath12k' or line in REPLACED:
        continue
    if tree == 'mhi':
        src = {'boot.c': MHI_BOOT, 'main.c': MHI_MAIN}.get(name, MODULE / 'out/mhi-src' / name)
    elif tree == 'qmi':
        src = MODULE / 'out/os-src' / name
    elif tree in ('mac80211', 'cfg80211', 'crypto'):
        src = MODULE / 'out' / tree / name
    elif tree == 'generated':
        src = MODULE / 'out' / name
    else:
        src = NW_QRTR if name == 'src/nw_qrtr.c' else MODULE / name
    obj = OUT / ((tree + '_' if tree else '') + src.stem + '.o')
    extra = trees.get(tree, [])
    # src/ files that see driver headers saw ath12k's in the module build; here they see ath11k's (only nw_mhi/nw_pci/nw_qrtr
    # look at them, through the MHI controller and lx_pci.h).
    if name.startswith('src/') and not extra:
        extra = ath11k_tree
    if name.startswith('src/nw_cfg80211_'):
        extra = trees['cfg80211']
    if name == 'src/nw_fils.c':
        extra = trees['mac80211']
    if name == 'src/nw_crypto_mac80211.c':
        extra = trees['crypto'] + trees['mac80211']
    if tree == 'crypto' and name == 'aes-siv.c':
        extra = [*extra, '-Daes_siv_encrypt=nw_aes_siv_encrypt', '-Daes_siv_decrypt=nw_aes_siv_decrypt']
    jobs.append((src, obj, extra))
ATH11K_OBJECTS = 'core hal hal_tx hal_rx wmi mac reg htc qmi dp dp_tx dp_rx debug ce peer dbring hw pcic fw p2p mhi pci'.split()
jobs += [(GEN / (n + '.c'), OUT / ('ath11k_' + n + '.o'), ath11k_tree) for n in ATH11K_OBJECTS]
jobs.append((PACKAGE / 'T970Ath11k.c', OUT / 'T970Ath11k.o', ath11k_tree))
names = [o.name for _, o, _ in jobs]
require(len(names) == len(set(names)), 'two sources map to one object name')

only = os.environ.get('T970_WIFI_ONLY')   # development: compile the matching sources only and stop


def build(job):
    src, obj, extra = job
    raw_obj = RAW / obj.name
    deps = raw_obj.with_suffix('.d')
    identity = raw_obj.with_suffix('.inputs.json')
    command = [clang, '-c', *extra, *flags, '-MD', '-MF', str(deps), str(src), '-o', str(raw_obj)]
    if raw_obj.exists() and identity.exists():
        record = json.loads(identity.read_text())
        if record.get('command') == command and record.get('object_sha256') == sha(raw_obj) and all(
                Path(p).is_file() and sha(p) == d for p, d in record.get('dependencies', {}).items()):
            return None
    r = subprocess.run(command, capture_output=True, text=True)
    if r.returncode:
        return f'{src}\n{r.stderr}'
    headers = shlex.split(deps.read_text().replace('\\\n', '').partition(':')[2])
    identity.write_text(json.dumps({'command': command, 'object_sha256': sha(raw_obj),
                                    'dependencies': {str(Path(p).absolute()): sha(p) for p in headers}}, sort_keys=True) + '\n')
    return None


selected = [j for j in jobs if not only or re.search(only, str(j[0]))]
with ThreadPoolExecutor(os.cpu_count() or 8) as pool:
    failures = [f for f in pool.map(build, selected) if f]
if failures:
    (OUT / 'compile-errors.txt').write_text('\n\n'.join(failures))
    for f in failures[:6]:
        sys.stderr.write(f[:6000] + '\n')
    raise SystemExit(f'build-t970-wifi: {len(failures)} of {len(selected)} sources failed (build/t970-wifi/compile-errors.txt)')
if only:
    raise SystemExit(0)

# ---- namespace the module: its Linux names (sock_release, kernel_sendmsg, mutex_lock ...) are common in a kernel ------------------
# Every defined global becomes _t970w<name> except the board boundary: nw_wifi_* (nw_abi.h, nw_control_endpoint.h).
llvm = Path(subprocess.run(['brew', '--prefix', 'llvm'], check=True, capture_output=True, text=True).stdout.strip()) / 'bin'
nm, objcopy = llvm / 'llvm-nm', llvm / 'llvm-objcopy'
module_objects = [o for _, o, _ in jobs]
defined, undefined = set(), set()
for o in module_objects:
    out = subprocess.run([nm, '-g', str(RAW / o.name)], check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] != 'U':
            defined.add(parts[2])
        elif len(parts) == 2 and parts[0] == 'U':
            undefined.add(parts[1])
kept = sorted(n for n in defined if n.startswith('_nw_wifi_'))
mapping = OUT / 'rename.txt'
mapping.write_text(''.join(f'{n} _t970w{n}\n' for n in sorted(defined) if n not in kept))
for o in module_objects:
    shutil.copyfile(RAW / o.name, o)
    run([objcopy, f'--redefine-syms={mapping}', o])
    check_object(o.read_bytes(), o.name)
imports = sorted(undefined - defined)
# The XNU functions the module may import: libc names the shim maps to the kernel's own. Anything else is a missing implementation.
ALLOWED = {'_bzero', '_memcmp', '_memcpy', '_memmove', '_memset', '_snprintf', '_sscanf', '_strchr', '_strcmp', '_strlen', '_strncmp',
           '_strncpy', '_strtoul', '_vsnprintf'}
require(not set(imports) - ALLOWED, 'module imports not provided by XNU: ' + ' '.join(sorted(set(imports) - ALLOWED)))

# ---- the board half, with the kernel's own C++ recipe (as scripts/build-gop-legacy-display.py compiles the board services) -------
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
arguments = record['arguments']


def board_object(source, obj):
    staged = OUT / 'staged' / source.name   # a private copy: the source is edited from other sessions while this runs
    staged.parent.mkdir(exist_ok=True)
    shutil.copyfile(source, staged)
    command, suppressions, i = [], [], 0
    while i < len(arguments):
        value = arguments[i]
        if value == record['file']:
            command.append(str(staged))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value == '-Xclang' and arguments[i + 1].startswith('-W'):
            i += 1
        elif value.startswith('-Wno-'):
            suppressions.append(value)
        elif not value.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
            command.append(value)
        i += 1
    command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-missing-designated-field-initializers',
                '-I', str(record['directory']), '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(MODULE / 'include'), *suppressions]
    r = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[:12000])
        raise SystemExit(f'build-t970-wifi: {source.name} failed to compile')
    return {'source': str(source), 'source_sha256': sha(staged), 'command': command}


board = []
board.append(board_object(PACKAGE / 'T970WifiStack.cpp', OUT / 'T970WifiStack.o'))
board.append(board_object(PACKAGE / 'T970WifiNet.cpp', OUT / 'T970WifiNet.o'))
# T970WifiNet.cpp's IOService subclasses register when stage 6 runs its initialiser between preModLoad and postModLoad (as
# UniXNU-sheng stages WifiNet.late.o), not from the kernel's __mod_init_func at boot: the default boot never sees them.
run([objcopy, '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_T970WifiNet.cpp',
     OUT / 'T970WifiNet.o', OUT / 'T970WifiNet.late.o'])
board_objects = [OUT / 'T970WifiStack.o', OUT / 'T970WifiNet.late.o']
for o in board_objects:
    check_object(o.read_bytes(), o.name)
for o in [*module_objects, *board_objects]:   # the default boot must not run any of it: no static initialiser anywhere
    sections = subprocess.run([llvm / 'llvm-objdump', '--section-headers', str(o)], check=True, capture_output=True, text=True).stdout
    require('__mod_init_func' not in sections, f'{o.name} would run a static initialiser at boot')

archive = OUT / 'libT970Wifi.a'
archive.unlink(missing_ok=True)
run([llvm / 'llvm-ar', 'rcs', archive, *module_objects, *board_objects])
(OUT / 'build-report.json').write_text(json.dumps({
    'driver': 'Linux v7.0.14 ath11k (PCI/MHI/QMI), QCA6390 hw2.0', 'runtime': 'Ath12kWifi mainline (cfg80211/mac80211)',
    'module': str(MODULE), 'module_revision': subprocess.run(['git', '-C', str(MODULE), 'rev-parse', 'HEAD'], capture_output=True,
                                                             text=True).stdout.strip(),
    'source_identity': json.loads((MODULE / 'out/source-identity.json').read_text()),
    'ath11k_patches': patches, 'module_imports': imports, 'kept_symbols': kept, 'renamed_symbols': len(defined) - len(kept),
    'objects': [{'path': str(o), 'source': str(s), 'source_sha256': sha(s), 'object_sha256': sha(o)} for s, o, _ in jobs],
    'board': board, 'archive': str(archive), 'archive_sha256': sha(archive),
    'hardware_verified': False, 'runs_with': 't970-wifi=6',
}, indent=2) + '\n')
print(f'build-t970-wifi: {archive} ({len(module_objects)} module objects, {len(board_objects)} board objects, '
      f'{len(kept)} nw_wifi_* exports, imports {" ".join(imports)})')
