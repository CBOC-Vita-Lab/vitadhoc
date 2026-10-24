#!/bin/bash
# bench_ppsspp.sh EBOOT_DIR OUT -- the AW2 linked session in PPSSPP on ONE
# console (no network): slot 0 is the harness's own machine driven by the
# autopilot (aw2_link_host.txt, delayed 30 frames for the frame-30 state
# autoload), slot 1 the mirror partner driven by aw2_link_join.txt.
# Arms: solo (no partner), dual x2 (determinism), dual at lookahead 10000,
# dual_own (the partner loads its own ROM copy instead of sharing A's).
EB=$1; OUT=$2; mkdir -p "$OUT/ini"
D=$(cd "$(dirname "$0")" && pwd); R=$HOME/gbalink/roms; S=$HOME/gbamirror/runs
{ echo "wait 30"; cat "$S/aw2_link_host.txt"; } > "$OUT/host30.txt"
FR=${FRAMES:-9100}
ini() { local n=$1; shift; { for k in "$@"; do echo "$k"; done; } > "$OUT/ini/$n.ini"; }
ini solo
ini dual    "gba_dual = 1" "gba_dual_script = partner.txt" "gba_dual_hash = 1"
ini dual2   "gba_dual = 1" "gba_dual_script = partner.txt" "gba_dual_hash = 1"
ini dual10k "gba_dual = 1" "gba_dual_script = partner.txt" "gba_dual_hash = 1" "gba_dual_lookahead = 10000"
ini dual_own "gba_dual = 1" "gba_dual_script = partner.txt" "gba_dual_hash = 1" "gba_dual_own_rom = 1"
for n in ${ARMS:-solo dual dual2 dual10k}; do
  AUTOEXIT=$FR "$D/ppsspp_run.sh" "$n" "$EB" "$R/aw2.gba" "$R/aw2.st0" "$R/aw2.sav" \
     "$OUT/host30.txt" "$OUT" "$OUT/ini/$n.ini" "$S/aw2_link_join.txt" &
  sleep 3
done
wait
