#!/bin/bash
# Proves that frames that are not drawn leave the machine as drawn frames do (fast forward draws 1 in 3, Steady pacing's half mode
# 1 in 2, catching up the frames it owes): per ROM, one run drawing every frame and two runs drawing one frame in 2 and in 3
# (snes_twin --render-mod); the sound, the work RAM + battery RAM hash and the saved state at the last frame must match.
# The state may differ in one byte, PPU.RecomputeClipWindows (a drawing cache that every drawn frame sets before it uses it),
# which is therefore not counted.
#   skip_check.sh TWIN ROMDIR [FRAMES]       exit 1 on a difference
# Copyright CBOC-Vita. MIT licence.
TWIN=$1; D=$2; F=${3:-3000}
[ -x "$TWIN" ] && [ -d "$D" ] || { echo "usage: $0 snes_twin ROMDIR [FRAMES]"; exit 2; }
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
PAD=$(python3 -I -c "
import random
random.seed(7);b=['a','b','x','y','l','r','start','select','up','down','left','right'];ev=[]
for f in range(30,$F,23):
    n=random.choice([0,1,1,2,3]); ev.append('%d:%s'%(f,'+'.join(random.sample(b,n)) if n else 'none'))
print(','.join(ev))")
same=0; diff=0
for r in "$D"/*.smc "$D"/*.sfc; do
  [ -f "$r" ] || continue
  n=$(basename "$r" | md5sum | cut -c1-8)
  for m in 0 2 3; do
    a=""; [ $m -gt 0 ] && a="--render-mod $m"
    nice "$TWIN" "$r" --frames $F --pad "$PAD" --wav $O/w.$m --peek-sum --save-state $O/s.$m --state-frame $((F-1)) $a 2>/dev/null | grep ramsum > $O/r.$m
  done
  for m in 2 3; do
    nb=$(cmp -l $O/s.0 $O/s.$m | wc -l)
    if cmp -s $O/w.0 $O/w.$m && cmp -s $O/r.0 $O/r.$m && [ "$nb" -le 1 ]; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $n mod$m state bytes differing: $nb"; fi
  done
done
echo "undrawn = drawn: $same runs, differing: $diff"
[ $diff = 0 ]
