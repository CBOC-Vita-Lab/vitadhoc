#!/bin/bash
# Proves that Quick response (run-ahead by one frame, fe_host.c gb_ahead_frame) leaves the machine exactly as it is without it, for the
# consoles that run through fe_host's gbcore path (Game Boy, Game Boy Color, NES, WonderSwan): per ROM, a run of the app twin with it
# off and a run with it forced on (GBADHOC_AHEAD=2: every frame at normal speed), same pad script; the sound (WAV), the quick save
# (state) and the battery file must be identical, and the run must really have run ahead (the log's ahead_frames line).
#   ahead_check.sh build/x86/gbadhoc_host ROMLIST [FRAMES]    ROMLIST: a file with one ROM path a line.  Exit 1 on a difference.
# Set GBADHOC_AHEAD_SKIP=N to run both with the undrawn-frame pattern of skip_check.sh (GBADHOC_SKIP_MOD) as well.
# Copyright CBOC-Vita. MIT licence for this script.
TWIN=$1; L=$2; F=${3:-400}; Q=${QEMU:-}
[ -x "$TWIN" ] && [ -f "$L" ] || { echo "usage: $0 gbadhoc_host ROMLIST [FRAMES]"; exit 2; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
INPUT="120:A:3,160:B:3,200:START:3,240:A:3,280:RIGHT:40,340:A:3,380:LEFT:30,420:A:3,$((F-60)):SELECT+L:3"
same=0; diff=0; idle=0
while read -r rom; do
  [ -f "$rom" ] || continue
  name=$(basename "$rom"); ext="${name##*.}"
  for m in 0 2; do
    root=$W/$m; rm -rf $root; mkdir -p $root/roms; cp "$rom" "$root/roms/rom.$ext"; echo "rom.$ext" > $root/autoboot.txt
    GBADHOC_AHEAD=$m GBADHOC_SKIP_MOD=${GBADHOC_AHEAD_SKIP:-0} timeout 600 nice $Q "$TWIN" --root $root --frames $F --wav $root/a.wav --input "$INPUT" >$root/out.txt 2>&1
    echo $? > $root/rc
  done
  ok=1
  cmp -s $W/0/rc $W/2/rc || ok=0
  cmp -s $W/0/a.wav $W/2/a.wav || ok=0
  for f in $(cd $W/0; find roms states -type f \( -name '*.sav' -o -name '*.st0' \)); do
    case $f in
      states/gb/*|states/gbc/*) python3 - "$W/0/$f" "$W/2/$f" <<'PY' || ok=0
# a Game Boy state holds the renderer's window line counter (now_win_line), which is counted only while lines are drawn: a frame that is
# not drawn (the real frame behind a run-ahead picture, fast forward) leaves another value, and the next frame sets it again before it reads
# it (lcd_clear_win_count).  It is the one field the states may differ in; the machine's own hash (gbcore_sync_hash) leaves it out too.
import sys
a, b = (bytearray(open(p, 'rb').read()) for p in sys.argv[1:3])
for d in (a, b):
    n = int.from_bytes(d[36:40], 'little') + 40
    d[n + 28:n + 32] = bytes(4)
sys.exit(0 if a == b else 1)
PY
      ;;
      *) cmp -s $W/0/$f $W/2/$f || ok=0 ;;
    esac
  done
  n=$(grep -ao 'ahead_frames n=[0-9]*' $W/2/log/gbadhoc.log 2>/dev/null | tail -1 | cut -d= -f2)
  if [ "${n:-0}" -lt 1 ]; then idle=$((idle+1)); echo "NOTE $name ran no frame ahead"; fi
  if [ $ok = 1 ]; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $name"; fi
done < "$L"
echo "run-ahead = plain: $same ROMs, differing: $diff, never ahead: $idle"
[ $diff = 0 ]
