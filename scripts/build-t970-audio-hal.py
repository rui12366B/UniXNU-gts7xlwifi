#!/usr/bin/env python3
"""Build the gts7xlwifi audio server plug-in (hal/T970AudioPlugIn.c) as an iOS driver bundle, signed ad hoc.

  build-t970-audio-hal.py [OUTDIR]      (default .cache/audio-hal)

Writes OUTDIR/stage/T970Audio.driver (the directory the payload addon carries under audio/, which the kernel mounts over
/System/Library/Audio/Plug-Ins/HAL: patches/xnu/t970-audio-addon-mount.patch) and OUTDIR/trust.json (the executable's CDHash, for
the owner trust cache). As UniXNU-sheng's scripts/build-sheng-audio-hal.py: the iOS SDK has no audio server plug-in headers; the macOS
SDK's CoreAudio headers declare the same interface and are used for the build only. Target arm64-apple-ios17.0 (iPadOS 17.7.11's
audio server on this board is arm64)."""
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) > 2:
    sys.exit(__doc__)
out = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else ROOT / '.cache/audio-hal'
ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
mac = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
bundle = out / 'stage/T970Audio.driver'
shutil.rmtree(out / 'stage', ignore_errors=True)
bundle.mkdir(parents=True)
with tempfile.TemporaryDirectory() as tmp:
    headers = Path(tmp) / 'CoreAudio'
    headers.mkdir()
    for name in ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h'):
        (headers / name).symlink_to(Path(mac) / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-I', tmp, '-I', str(ROOT / 'package'), '-O2',
                    '-g0', '-Wall', '-Wextra', '-Werror', '-bundle', '-fvisibility=hidden', '-framework', 'CoreFoundation',
                    '-framework', 'IOKit', str(ROOT / 'hal/T970AudioPlugIn.c'), '-o', str(bundle / 'T970Audio')], check=True)
shutil.copy(ROOT / 'hal/Info.plist', bundle / 'Info.plist')
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.unixnu.t970.audio', str(bundle)], check=True)
subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', str(bundle)], capture_output=True, text=True, check=True).stderr
cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1)
(out / 'trust.json').write_text(json.dumps({'cdhash': cdhash}, indent=2) + '\n')
print(f'{bundle} CDHash {cdhash}')
