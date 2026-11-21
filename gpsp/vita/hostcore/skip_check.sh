#!/bin/bash
# Proves that frames that are not drawn leave the machine as drawn frames do, for the consoles of the app (GBA, Game Boy, Game Boy
# Color, WonderSwan, NES): per ROM, a run of the app twin drawing every frame and two runs with GBADHOC_SKIP_MOD=2 and =3 (plat_host.c:
# after every emulated frame the next one is undrawn unless its number divides by N: Steady pacing's half mode, fast forward);
# the sound, the quick save (state) and the battery file must be identical.
#   skip_check.sh build/x86/gbadhoc_host ROMLIST [FRAMES]     ROMLIST: a file with one ROM path a line.  exit 1 on a difference
# Copyright CBOC-Vita. MIT licence.
TWIN=$1; L=$2; F=${3:-400}; Q=${QEMU:-}
[ -x "$TWIN" ] && [ -f "$L" ] || { echo "usage: $0 gbadhoc_host ROMLIST [FRAMES]"; exit 2; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
INPUT="120:A:3,160:B:3,200:START:3,240:A:3,280:RIGHT:40,340:A:3,380:LEFT:30,420:A:3,$((F-60)):SELECT+L:3"
same=0; diff=0
while read -r rom; do
  name=$(basename "$rom"); ext="${name##*.}"
  for m in 0 2 3; do
    root=$W/$m; rm -rf $root; mkdir -p $root/roms; cp "$rom" "$root/roms/rom.$ext"; echo "rom.$ext" > $root/autoboot.txt
    GBADHOC_SKIP_MOD=$m timeout 300 nice $Q "$TWIN" --root $root --frames $F --wav $root/a.wav --input "$INPUT" >$root/out.txt 2>&1
    echo $? > $root/rc
  done
  [ -n "$(cd $W/0; find states -name '*.st0' 2>/dev/null)" ] || echo "note: $name saved no state"
  for m in 2 3; do
    ok=1
    cmp -s $W/0/rc $W/$m/rc || ok=0
    cmp -s $W/0/a.wav $W/$m/a.wav || ok=0
    for f in $(cd $W/0; find roms states -type f \( -name '*.sav' -o -name '*.st0' \)); do cmp -s $W/0/$f $W/$m/$f || ok=0; done
    if [ $ok = 1 ]; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $name mod$m"; fi
  done
done < "$L"
echo "undrawn = drawn: $same runs, differing: $diff"
[ $diff = 0 ]
