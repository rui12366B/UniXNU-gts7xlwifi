#!/usr/bin/env python3
"""Add the audio server plug-in to a GPU addon and its CDHash to a trust bundle, writing new files (the inputs stay as they are).

  t970-audio-pack.py ADDON_IN ADDON_OUT TRUST_IN TRUST_OUT [HALDIR]

HALDIR is scripts/build-t970-audio-hal.py's output (default .cache/audio-hal): its stage/ directory goes into the addon under audio/
(the kernel writes it to /private/var/ngpu/audio and mounts it over /System/Library/Audio/Plug-Ins/HAL, gpu-addon=1), and its
trust.json CDHash is appended to the trust bundle as one more owner module (UniXNU scripts/trust-append-module.py), as UniXNU-sheng's
scripts/pack27-gpu.sh does. Uses UniXNU's scripts/addon-add-dir.py and trust-append-module.py from ../sources/UniXNU-next."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT.parent / 'sources/UniXNU-next/scripts'
if len(sys.argv) not in (5, 6):
    sys.exit(__doc__)
addon_in, addon_out, trust_in, trust_out = (Path(a).resolve() for a in sys.argv[1:5])
hal = Path(sys.argv[5]).resolve() if len(sys.argv) == 6 else ROOT / '.cache/audio-hal'
cdhash = json.loads((hal / 'trust.json').read_text())['cdhash']
if not (hal / 'stage/T970Audio.driver/T970Audio').is_file():
    sys.exit('No built plug-in in ' + str(hal / 'stage'))
subprocess.run([sys.executable, CORE / 'addon-add-dir.py', addon_in, addon_out, 'audio', hal / 'stage'], check=True)
subprocess.run([sys.executable, CORE / 'trust-append-module.py', trust_in, trust_out, cdhash], check=True)
print(f'plug-in CDHash {cdhash}')
