#!/bin/sh
# Dynamic instruction count of the library per frame under qemu-arm.
# qemu -one-insn-per-tb -d exec,nochain prints one `Trace` line per executed instruction;
# the log is piped (never stored).  Two runs of different length cancel the start-up
# cost; a run with the library calls skipped (arg 3 = 1) is the harness baseline.
#   usage: count_insns.sh [binary] [scene] [short] [long]
BIN=${1:-build/arm_bench}
SCENE=${2:-0}
S=${3:-20}
L=${4:-60}
count() { qemu-arm -one-insn-per-tb -d exec,nochain -D /dev/stdout "$BIN" "$1" "$SCENE" "$2" 2>/dev/null | grep -c '^Trace'; }
A=$(count $S 0); B=$(count $L 0)
C=$(count $S 1); D=$(count $L 1)
N=$((L - S))
FULL=$(( (B - A) / N )); BASE=$(( (D - C) / N ))
echo "scene $SCENE: instructions per frame: full $FULL, harness only $BASE, library $(( FULL - BASE ))"
