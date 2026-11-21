#!/bin/bash
# What Quick response costs, in host instructions per emulated frame (callgrind, steady state, inside fe_host_run_frame): a window of FRAMES frames from frame 150 with it off
# and with it forced on (GBADHOC_AHEAD=2).  Prints  COST <rom> plain=<M instr/frame> ahead=<M instr/frame> ratio=<x>.  Needs valgrind.
#   ahead_cost.sh build/x86/gbadhoc_host ROM [FRAMES]
# Copyright CBOC-Vita. MIT licence for this script.
TWIN=$1; rom=$2; N=${3:-60}
[ -x "$TWIN" ] && [ -f "$rom" ] || { echo "usage: $0 gbadhoc_host ROM [FRAMES]"; exit 2; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
ext="${rom##*.}"; res=""
for m in 0 2; do
  root=$W/r$m; mkdir -p $root/roms; cp "$rom" $root/roms/rom.$ext; echo rom.$ext > $root/autoboot.txt
  probe=; [ $m = 2 ] && [ "$ext" = gba ] && probe=1   # the GBA has no run-ahead: its probe does the state copies and the second frame
  env GBADHOC_AHEAD=$m ${probe:+GBADHOC_AHEAD_GBA_PROBE=1} GBADHOC_CG_FROM=${CGFROM:-150} nice valgrind --tool=callgrind --smc-check=all --instr-atstart=no --callgrind-out-file=$W/cg$m.out \
    "$TWIN" --root $root --frames $((${CGFROM:-150} + N)) --input "120:A:3,160:B:3,200:START:3" > $root/out.txt 2>&1
  # inclusive instructions of the emulation entry point (the twin's own picture work is left out)
  ca=$(callgrind_annotate --inclusive=yes $W/cg$m.out 2>/dev/null)
  ir=$(echo "$ca" | grep -a 'fe_host.c:fe_host_run_frame \[' | head -1 | awk '{gsub(",","",$1); print $1}')
  nf=$(echo "$ca" | grep -a '=> .*fe_host.c:fe_host_run_frame (' | head -1 | sed 's/.*(\([0-9]*\)x).*/\1/')
  if [ "$ext" = gba ]; then
    # translated code runs outside any call the profiler sees: everything except the picture work (frame_present) is the emulation
    tot=$(echo "$ca" | grep -a 'PROGRAM TOTALS' | head -1 | awk '{gsub(",","",$1); print $1}')
    pr=$(echo "$ca" | grep -a 'main_vita.c:frame_present \[' | head -1 | awk '{gsub(",","",$1); print $1}')
    res="$res $(python3 -c "print(round((${tot:-0}-${pr:-0})/$N/1e6,3))")"
  else
    res="$res $(python3 -c "print(round(${ir:-0}/${nf:-1}/1e6,3))")"
  fi
done
set -- $res
echo "COST $(basename "$rom") plain=$1 ahead=$2 ratio=$(python3 -c "print(round($2/($1 or 1),2))")  (host M instructions per frame in fe_host_run_frame)"
