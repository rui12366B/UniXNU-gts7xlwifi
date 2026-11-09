#!/bin/zsh
# Read-only capture of how Android records from the built-in microphones (for the T970 capture path): the full mixer (tinymix) idle
# and while recording, the open capture PCMs' hw_params, and the kernel log lines of the codec macros / DMICs / CDC DMA ports.
# Needs root via KernelSU (`su -c`). Writes nothing on the tablet (no mixer or register writes).
# Output: deploy/gts7xlwifi-20261030/logs/android-mic/<stamp>/ on this Mac. Vendor data: keep it out of the repo.
#   usage: scripts/t970-android-mic-dump.sh [adb serial]
set -u
ROOT=${0:A:h:h:h}
OUT=$ROOT/deploy/gts7xlwifi-20261030/logs/android-mic/$(date +%Y%m%d-%H%M%S)
ADB=(adb)
[ $# -ge 1 ] && ADB=(adb -s "$1")
mkdir -p "$OUT"
"${ADB[@]}" get-state >/dev/null 2>&1 || {echo "no adb device"; exit 1}
"${ADB[@]}" shell su -c id 2>&1 | tee "$OUT/root.txt"
grep -q 'uid=0' "$OUT/root.txt" || {echo "su did not give root: grant Shell root in the KernelSU manager, then run again"; exit 1}

read -r -d '' REMOTE <<'EOF'
echo "== tinymix"; tinymix -D 0 2>&1
echo "== asound pcm"; cat /proc/asound/pcm
for f in /proc/asound/card*/pcm*c/sub0/hw_params /proc/asound/card*/pcm*c/sub0/status /proc/asound/card*/pcm*p/sub0/hw_params; do
  v=$(cat $f 2>/dev/null); case "$v" in closed|"") ;; *) echo "== $f"; echo "$v" ;; esac
done
echo "== dmesg"; dmesg 2>/dev/null | grep -i -e va_macro -e tx_macro -e dmic -e cdc_dma -e 'afe_port_start' -e 'msm_pcm_routing' -e 'adm_open' -e 'q6asm' -e bolero -e micb | tail -400
EOF

pass() {
  echo "dumping: $1"
  "${ADB[@]}" shell "su -c sh" <<<"$REMOTE" > "$OUT/$1.txt" 2>&1
  echo "  -> $OUT/$1.txt ($(wc -l < "$OUT/$1.txt") lines)"
}
pass idle
echo
echo "Now start a recording on the tablet (Samsung Voice Recorder, or the camera in video mode), keep it recording, then press Enter."
read -r _
pass recording
echo "Stop the recording now. Done: $OUT"
