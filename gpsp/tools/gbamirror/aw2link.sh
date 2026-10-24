#!/bin/bash
# aw2link.sh OUT [gl_host args...] -- the AW2 linked run: both machines from
# the owner's state, slot 0 driven by aw2_link_host.txt, slot 1 by
# aw2_link_join.txt (the design-P probe scripts), dumps every 300 frames.
OUT=$1; shift
G=$HOME/gbamirror/src/tools/gbamirror/gl_host; R=$HOME/gbalink/roms
S=${SCRIPTS:-$HOME/gbamirror/runs}
# lookahead pinned to the value the published results used (default is now half a frame)
set -- --lookahead 2464 "$@"
rm -rf "$OUT"; mkdir -p "$OUT"
$G --rom $R/aw2.gba --bios-dir $HOME/gbalink/bios --save0 $R/aw2.sav --save1 $R/aw2.sav \
   --state0 $R/aw2.st0 --state1 $R/aw2.st0 --script0 $S/aw2_link_host.txt \
   --script1 $S/aw2_link_join.txt --frames ${FRAMES:-9000} --hash "$OUT/hash.txt" \
   --dump-dir "$OUT" --dump-every ${DUMP_EVERY:-300} "$@" > "$OUT/out.txt" 2>&1
echo "exit=$?"; tail -5 "$OUT/out.txt"
