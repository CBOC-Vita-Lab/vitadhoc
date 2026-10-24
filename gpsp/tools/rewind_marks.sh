#!/usr/bin/env bash
# marks.sh BUILD -- how many stores per frame the rewind's page marks ride
# on, for the two fixtures that read +2.1 % on hardware with rewind off.
# BUILD: a desktop twin built with REWIND=1 REWIND_COUNT=1 (and
# EXTRA_CFLAGS=-DREWIND_COUNT for the frontend).  Run with the interpreter:
# every store then comes through the counted C macros, and the guest does
# the same stores whichever CPU core runs it.
set -uo pipefail
B=${1:-c1}
S=$(dirname "$0")
W=$HOME/rewind-agent
O=$W/cnt; rm -rf "$O"; mkdir -p "$O"
cp ~/drcorpus/fx/unbound.sav "$O/u.sav"
cd "$W/$B/sdl"
run() { # tag rom save script frames [state]
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./gpsp_sdl --rom "$2" --bios-dir "$W/bios" \
    --save "$3" --script "$4" --ff --no-pacing --log "$O/$1.log" --dump-dir "$O" \
    --autoexit "$5" ${6:+--state "$6"} --rewind --rewind-inplace --rewind-dirty 3 \
    --option gpsp_drc=disabled > "$O/$1.err" 2>&1
}
run nfs ~/nfsu2/nfsu2.gba "$O/n.sav" "$S/nfsu2_race.inputs" 5200 &
run unb ~/drcorpus/fx/unbound.gba "$O/u.sav" ~/drcorpus/fx/battle.txt 4500 ~/drcorpus/fx/unbound_double_medium.st0 &
wait
for t in nfs unb; do echo "== $t"; grep -h rewind_marks "$O/$t.log" | cut -c1-120; done
