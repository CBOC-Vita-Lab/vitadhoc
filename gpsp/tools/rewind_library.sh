#!/usr/bin/env bash
# lib.sh BUILD PLAY [extra] -- rewind_test.py test on every corpus ROM
set -uo pipefail
BUILD=$1; PLAY=$2; shift 2
W=$HOME/rewind-agent; S=$W/s9; O=$S/lib_${BUILD}_$PLAY; mkdir -p $O
ALL="--rewind-inplace --rewind-dirty 3 --rewind-fast-restore --rewind-clock --rewind-burst 4 --rewind-verify --rtc-pin 1700000000 $*"
one() {
  rom=$1; n=$(basename "$rom" .gba)
  python3 $S/tools/rewind_test.py test --bin $W/$BUILD/sdl/gpsp_sdl --bios $W/bios --rom "$rom" \
    --inputs $W/work/in/mash.inputs --play $PLAY --work $O/$n --allow-cut 16 -- $ALL > $O/$n.log 2>&1
  echo "$n $(grep -o 'shown_backwards=[0-9]* (bad [0-9]*)' $O/$n.log) $(grep -o 'compared=[0-9]* (bad [0-9]*)' $O/$n.log | head -1) $(grep -o 'rerun_for_rewind=[0-9]* (bad [0-9]*)' $O/$n.log) $(grep -o 'cut_moved=[0-9]*' $O/$n.log) $(grep -o 'gone_back_over=[0-9]*' $O/$n.log) $(grep -c '^PASS' $O/$n.log)P/$(grep -c '^FAIL' $O/$n.log)F"
  rm -rf $O/$n
}
export -f one; export W S O BUILD PLAY ALL
ls $HOME/drcorpus/corpus/roms/*.gba | xargs -P 10 -I{} bash -c 'one "{}"' | sort
