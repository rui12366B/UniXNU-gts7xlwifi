#!/usr/bin/env python3
"""Build the camera stage C5c client (tools/t970cam/t970cam.c) for iPadOS, signed ad hoc.

  build-t970cam.py [OUTDIR]      (default .cache/t970cam)

Writes OUTDIR/t970cam and OUTDIR/trust.json (the executable's CDHash, for the owner trust cache), like build-t970-audio-hal.py.
The kernel service it opens is published with t970-camera bit 512 (package/T970CameraService.cpp); its sandbox grant names the
process "t970cam". Getting the binary onto the device (payload addon or the data partition) is a separate staging step."""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) > 2:
    sys.exit(__doc__)
out = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else ROOT / '.cache/t970cam'
out.mkdir(parents=True, exist_ok=True)
ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
exe = out / 't970cam'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-I', str(ROOT / 'package'), '-O2', '-g0',
                '-Wall', '-Wextra', '-Werror', '-framework', 'IOKit', '-framework', 'CoreFoundation',
                str(ROOT / 'tools/t970cam/t970cam.c'), '-o', str(exe)], check=True)
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.unixnu.t970cam', str(exe)], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', str(exe)], capture_output=True, text=True, check=True).stderr
cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1)
(out / 'trust.json').write_text(json.dumps({'path': 't970cam', 'cdhash': cdhash}, indent=2) + '\n')
print('t970cam:', exe, 'cdhash', cdhash)
