#!/bin/bash
# Proves that frames that are not drawn leave the machine as drawn frames do (fast forward 1 in 3, Steady pacing's half mode 1 in 2):
# per ROM, a run drawing every frame and two runs drawing one frame in 2 and in 3 (nes_twin --render-mod); the sound, the 2 KiB of
# console RAM and the saved state at the last frame must be identical.
#   skip_check.sh TWIN ROMLIST [FRAMES]      ROMLIST: a file with one ROM path a line.  exit 1 on a difference
# Copyright CBOC-Vita. MIT licence.
TWIN=$1; L=$2; F=${3:-900}
[ -x "$TWIN" ] && [ -f "$L" ] || { echo "usage: $0 nes_twin ROMLIST [FRAMES]"; exit 2; }
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
PAD=$(python3 -I -c "
import random
random.seed(11);b=['a','b','start','select','up','down','left','right'];ev=[]
for f in range(20,$F,17):
    n=random.choice([0,1,1,2,3]); ev.append('%d:%s'%(f,'+'.join(random.sample(b,n)) if n else 'none'))
print(','.join(ev))")
same=0; diff=0
while read -r r; do
  for m in 0 2 3; do
    a=""; [ $m -gt 0 ] && a="--render-mod $m"
    rm -f $O/s.$m $O/m.$m $O/w.$m
    nice timeout 120 "$TWIN" "$r" --frames $F --pad "$PAD" --wav $O/w.$m --ram-dump $O/m.$m --save-state $O/s.$m --state-frame $((F-1)) $a >/dev/null 2>&1
  done
  for m in 2 3; do
    if cmp -s $O/w.0 $O/w.$m && cmp -s $O/m.0 $O/m.$m && cmp -s $O/s.0 $O/s.$m; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $(basename "$r") mod$m"; fi
  done
done < "$L"
echo "undrawn = drawn: $same runs, differing: $diff"
[ $diff = 0 ]
