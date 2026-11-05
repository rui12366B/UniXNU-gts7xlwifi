#!/usr/bin/env python3
"""QcomXnuUsbDebug (drivers/QcomXnuUsbDebug, FFT-infra ab6e363) for gts7xlwifi: .cache/qcom-usb-debug/libQcomXnuUsbDebug.a, the
archive build-xnu-4k-baseline.sh force-loads under CONFIG_XNU_QCOM_USB_DEBUG. The same invocation as FFT-infra/UniXNU's catalog entry,
with the module's SM8250 profile: the T970's bootloader leaves the DWC3 powered, clocked and in DEVICE mode (v50 log: usb30_prim_gdsc
on, master clock on, GCTL PRTCAPDIR=device), which is the state the module takes over."""
import os
import subprocess
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
# The module is not in UniXNU's component catalog, so the tree has no drivers/QcomXnuUsbDebug of its own: link the device
# repository's copy (the tree links its package as kits/gts7xlwifi), which build-xnu-4k-baseline.sh's include path also names.
DEVICE = Path(os.path.realpath(ROOT / 'kits/gts7xlwifi')).parent
module = ROOT / 'drivers/QcomXnuUsbDebug'
if not module.exists():
    module.symlink_to(DEVICE / 'drivers/QcomXnuUsbDebug')
subprocess.run([sys.executable, str(module / 'scripts/build.py'),
                '--compile-record', str(ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json'),
                '--soc', 'soc/Sm8250.h', '--output', str(ROOT / '.cache/qcom-usb-debug'), '--replace'], check=True, cwd=module)
