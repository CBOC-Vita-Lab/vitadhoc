#!/bin/sh
# ARM validation: nescore.o built for armhf (arm-linux-gnueabihf-gcc, run under qemu-arm) and for the host must give
# byte-identical results (picture hash of every frame, audio hash, RAM hash, battery RAM hash, state size) on the
# synthetic cartridges and, if NES_TESTROMS is set, on every ROM of that tree.
#   twin/arm_check.sh [FRAMES]       (default 240)   needs arm-linux-gnueabihf-gcc and qemu-arm; builds under a temp dir
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
FE=$HERE/../frontend
F=${1:-240}
T=$(mktemp -d)
command -v arm-linux-gnueabihf-gcc >/dev/null && command -v qemu-arm >/dev/null || { echo "SKIP  needs arm-linux-gnueabihf-gcc and qemu-arm"; exit 0; }
make -C "$FE" -j4 TARGET=host B=$T/host check >$T/host.log 2>&1 && make -C "$FE" -j4 TARGET=armhf B=$T/armhf check >$T/arm.log 2>&1 || { tail -5 $T/host.log $T/arm.log; echo "FAIL  build"; exit 1; }
grep "globals defined" $T/host.log $T/arm.log
gcc -O1 -I"$FE" -o $T/oh_x86 "$HERE/tests/obj_hash.c" $T/host/nescore.o -lm || exit 1
arm-linux-gnueabihf-gcc -static -O1 -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -I"$FE" -o $T/oh_arm "$HERE/tests/obj_hash.c" $T/armhf/nescore.o -lm || exit 1
python3 -I "$HERE/tests/make_roms.py" $T/roms >/dev/null || exit 1
list="$T/roms/*.nes"
[ -n "$NES_TESTROMS" ] && list="$list $(find "$NES_TESTROMS" -name '*.nes' | sort | tr '\n' ' ')"
same=0; diff=0
for r in $list; do
	$T/oh_x86 "$r" $F >$T/x.out 2>&1
	qemu-arm -cpu max $T/oh_arm "$r" $F >$T/a.out 2>&1
	if [ -s $T/x.out ] && cmp -s $T/x.out $T/a.out; then same=$((same + 1)); else diff=$((diff + 1)); echo "DIFF  $r"; fi
done
echo "armhf vs x86_64 over $((same + diff)) ROMs, $F frames each: identical $same, different $diff"
rm -rf $T
[ $diff = 0 ]
