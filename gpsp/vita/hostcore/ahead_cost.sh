#!/bin/bash
# What Quick response costs, in host instructions per emulated frame (callgrind, steady state): a window of FRAMES frames from frame 150 with it off
# and with it forced on (GBADHOC_AHEAD=2).  Prints  COST <rom> plain=<M instr/frame> ahead=<M instr/frame> ratio=<x>.  Needs valgrind.
#   ahead_cost.sh build/x86/gbadhoc_host ROM [FRAMES]
# Copyright CBOC-Vita. MIT licence for this script.
TWIN=$1; rom=$2; N=${3:-60}
[ -x "$TWIN" ] && [ -f "$rom" ] || { echo "usage: $0 gbadhoc_host ROM [FRAMES]"; exit 2; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
ext="${rom##*.}"; res=""
for m in 0 2; do
  root=$W/r$m; mkdir -p $root/roms; cp "$rom" $root/roms/rom.$ext; echo rom.$ext > $root/autoboot.txt
  GBADHOC_AHEAD=$m GBADHOC_CG_FROM=150 nice valgrind --tool=callgrind --instr-atstart=no --callgrind-out-file=$W/cg$m.out \
    "$TWIN" --root $root --frames $((150 + N)) --input "120:A:3,160:B:3,200:START:3" > $root/out.txt 2>&1
  ir=$(grep -a '^summary:' $W/cg$m.out | awk '{print $2}')
  res="$res $(python3 -c "print(round($ir/$N/1e6,3))")"
done
set -- $res
echo "COST $(basename "$rom") plain=$1 ahead=$2 ratio=$(python3 -c "print(round($2/$1,2))")  (host M instructions per frame, ${N}-frame window)"
