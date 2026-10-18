#!/bin/bash
# yieldtest.sh FRAMES CYCLES... -- the yield-invisibility test: slot 0 alone
# through the link run loop, stopped every C cycles, against C = 1e9 (never).
# Extra args after -- go to gl_host (e.g. --no-horizon-event).
FR=$1; shift
CS=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do CS+=("$1"); shift; done; shift
G=$HOME/gbamirror/src/tools/gbamirror/gl_host; R=$HOME/gbalink/roms
ROM=${ROM:-$R/aw2.gba}; ST=${ST:-$R/aw2.st0}; SAV=${SAV:-$R/aw2.sav}; SC=${SC:-aw2_scenes.txt}
COM="--rom $ROM --bios-dir $HOME/gbalink/bios --save0 $SAV --state0 $ST --script0 $SC --frames $FR"
$G $COM --yield-every 1000000000 --hash ref.txt "$@" | tail -1
for c in "${CS[@]}"; do
  timeout 120 $G $COM --yield-every $c --hash y$c.txt "$@" | tail -1
  printf "C=%s: " $c; cmp ref.txt y$c.txt && echo IDENTICAL
done
