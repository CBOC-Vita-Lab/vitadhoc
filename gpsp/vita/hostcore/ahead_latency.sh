#!/bin/bash
# Latency in refreshes of a button press, with Quick response off and on, on the small test cartridges ahead_latency_gen.py makes
# (they answer the pad one frame after they read it, as most games do): the A button (cross and circle together: the consoles map them differently) goes down at refresh 100; the screenshots of
# refreshes 96..112 are compared with the one at 96, and the first one that differs is the answer.  Prints
#   LATENCY <console> plain=<n> ahead=<m>   and PASS when ahead = plain - 1, FAIL otherwise.
#   ahead_latency.sh build/x86/gbadhoc_host [WORKDIR]
# Copyright CBOC-Vita. MIT licence for this script.
TWIN=$1; W=${2:-$(mktemp -d)}; Q=${QEMU:-}
[ -x "$TWIN" ] || { echo "usage: $0 gbadhoc_host [WORKDIR]"; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W/roms-gen"; python3 "$HERE/tools/ahead_latency_gen.py" "$W/roms-gen" > /dev/null || exit 2
fail=0
for rom in "$W"/roms-gen/latency.*; do
  ext="${rom##*.}"; res=""
  for m in 0 2; do
    root=$W/r$m-$ext; rm -rf "$root"; mkdir -p "$root/roms" "$root/shots"; cp "$rom" "$root/roms/rom.$ext"; echo "rom.$ext" > "$root/autoboot.txt"
    shots=""; for f in $(seq 96 112); do shots="$shots --shot $f:s$f.png"; done
    GBADHOC_AHEAD=$m nice $Q "$TWIN" --root "$root" --frames 114 --shot-dir "$root/shots" $shots --input "100:cross+circle:40" > "$root/out.txt" 2>&1
    first=-1
    for f in $(seq 97 112); do
      [ -f "$root/shots/s$f.png" ] && ! cmp -s "$root/shots/s96.png" "$root/shots/s$f.png" && { first=$f; break; }
    done
    if [ $first -lt 0 ]; then res="$res none"; else res="$res $((first - 100))"; fi
  done
  set -- $res
  if [ "$1" != none ] && [ "$2" != none ] && [ "$2" -eq $(($1 - 1)) ]; then echo "PASS  LATENCY $ext plain=$1 ahead=$2"; else echo "FAIL  LATENCY $ext plain=$1 ahead=$2"; fail=1; fi
done
exit $fail
