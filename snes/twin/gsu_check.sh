#!/bin/bash
# Proves the fast Super FX core (core/fxfast.cpp, block cache) is the reference interpreter in every result: for every Super FX
# ROM in ROMDIR, one run with --opt gsu_fast=0 and one with gsu_fast=1, at the hardware clock and at a low clock (a small
# per-line budget makes the interpreter stop inside blocks far more often), with a pad script that moves the game along.
# The picture hashes of every frame, the sound, the work RAM + battery RAM hash and the saved state at the last frame must be
# identical. ROMs that are not Super FX carts are skipped (their line in the log says so).
#   gsu_check.sh TWIN ROMDIR [FRAMES]       exit 1 on a difference, 2 when no Super FX ROM was found
# Copyright CBOC-Vita. MIT licence.
TWIN=$1; D=$2; F=${3:-3000}
[ -x "$TWIN" ] && [ -d "$D" ] || { echo "usage: $0 snes_twin ROMDIR [FRAMES]"; exit 2; }
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
PAD=$(python3 -I -c "
import random
random.seed(11);b=['a','b','x','y','l','r','start','select','up','down','left','right'];ev=[]
for f in range(30,$F,19):
    n=random.choice([0,1,1,2]); ev.append('%d:%s'%(f,'+'.join(random.sample(b,n)) if n else 'none'))
print(','.join(ev))")
same=0; diff=0; skipped=0
for r in "$D"/*.smc "$D"/*.sfc; do
  [ -f "$r" ] || continue
  n=$(basename "$r" | md5sum | cut -c1-8)
  nice "$TWIN" "$r" --frames 2 2>&1 | grep -q SuperFX || { skipped=$((skipped+1)); continue; }
  for clk in 100 35; do
    for m in 0 1; do
      nice "$TWIN" "$r" --frames $F --pad "$PAD" --opt superfx_clock=$clk --opt gsu_fast=$m --hashes $O/h.$m --wav $O/w.$m \
          --peek-sum --save-state $O/s.$m --state-frame $((F-1)) 2>/dev/null | grep ramsum > $O/r.$m
    done
    if cmp -s $O/h.0 $O/h.1 && cmp -s $O/w.0 $O/w.1 && cmp -s $O/r.0 $O/r.1 && cmp -s $O/s.0 $O/s.1 && [ -s $O/r.0 ]; then
      same=$((same+1))
    else
      diff=$((diff+1)); echo "DIFF $n clock $clk"
    fi
  done
done
echo "gsu fast = reference: $same runs, differing: $diff (non Super FX ROMs skipped: $skipped)"
[ $diff = 0 ] && [ $same -gt 0 ] || { [ $diff = 0 ] && exit 2; exit 1; }
