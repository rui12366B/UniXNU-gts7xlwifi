#!/bin/zsh
# Read-only dump of the speaker amplifiers' live state on the tablet's Android (root via KernelSU, `su -c`), so T970Audio.cpp can copy
# the working configuration: the CS35L41 regmap registers (debugfs), ALSA cards/PCM hw_params, the mixer controls (tinymix, listing
# only), the amplifiers' device-tree nodes, ASoC debugfs (components, DAIs) and the kernel log lines about the amplifiers.
# Two passes: idle, then while Android plays audio (the script waits for Enter in between).
# Writes nothing to the tablet except mounting debugfs at /sys/kernel/debug if it is not mounted. No register or mixer writes.
# Output: deploy/gts7xlwifi-20261030/logs/android-audio/<stamp>/ on this Mac. Vendor data: keep it out of the repo.
#   usage: scripts/t970-android-audio-dump.sh [adb serial]
set -u
ROOT=${0:A:h:h:h}
OUT=$ROOT/deploy/gts7xlwifi-20261030/logs/android-audio/$(date +%Y%m%d-%H%M%S)
ADB=(adb)
[ $# -ge 1 ] && ADB=(adb -s "$1")
mkdir -p "$OUT"

"${ADB[@]}" get-state >/dev/null 2>&1 || {echo "no adb device"; exit 1}
"${ADB[@]}" shell su -c id 2>&1 | tee "$OUT/root.txt"
grep -q 'uid=0' "$OUT/root.txt" || {echo "su did not give root: grant Shell root in the KernelSU manager, then run again"; exit 1}

# The part that runs on the tablet as root (sh); every command only reads.
read -r -d '' REMOTE <<'EOF'
D=/sys/kernel/debug
[ -d $D/regmap ] || mount -t debugfs none $D 2>/dev/null
echo "== getprop"; getprop ro.product.model; getprop ro.product.device; getprop ro.build.fingerprint
echo "== regmap list"; ls $D/regmap 2>&1
for r in $D/regmap/*; do
  n=$(cat $r/name 2>/dev/null)
  case "$(basename $r) $n" in
    *cs35l4*|*-0040*|*-0041*|*-0042*|*-0043*|*cirrus*)
      echo "== regmap $(basename $r) name=$n"
      cat $r/registers 2>&1 ;;
  esac
done
echo "== asound cards"; cat /proc/asound/cards; echo "== asound pcm"; cat /proc/asound/pcm
for f in /proc/asound/card*/pcm*p/sub0/hw_params /proc/asound/card*/pcm*p/sub0/status; do
  v=$(cat $f 2>/dev/null); case "$v" in closed|"") ;; *) echo "== $f"; echo "$v" ;; esac
done
echo "== tinymix"; (command -v tinymix >/dev/null && tinymix -D 0 2>&1) || (ls /vendor/bin /system/bin | grep -i mix)
echo "== asoc components"; cat $D/asoc/components 2>&1; echo "== asoc dais"; cat $D/asoc/dais 2>&1
echo "== dt cs35l41 nodes"
for n in $(find /proc/device-tree -type d -name 'cs35l41*' 2>/dev/null); do
  echo "-- $n"
  for p in $(find $n -type f); do echo "$p: $(od -An -tx1 -v $p | tr -s ' \n' ' ' | cut -c1-400)"; done
done
echo "== dmesg"; dmesg 2>/dev/null | grep -i -e cs35l4 -e cirrus -e 'tdm' -e 'afe_' | tail -300
EOF

pass() {
  echo "dumping: $1"
  "${ADB[@]}" shell "su -c sh" <<<"$REMOTE" > "$OUT/$1.txt" 2>&1
  echo "  -> $OUT/$1.txt ($(wc -l < "$OUT/$1.txt") lines)"
}
pass idle
echo
echo "Now play audio on the tablet (music or a ringtone, normal volume, keep it playing), then press Enter."
read -r _
pass playing
echo "Done: $OUT"
