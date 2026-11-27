#!/usr/bin/env bash
# run_video_kernel_tests.sh -- the exact-equivalence tests of the renderer's
# vector kernels (video_merge.h: colour-effect merges, video_tile.h: tile and
# sprite rows) and the OBJ X sign extension (video_obj.h) on x86 (the scalar
# arm / branch-free code) and on armhf under qemu-arm (NEON, plus the scalar
# code built for ARM).
#
#   tools/tests/run_video_kernel_tests.sh [quick]
#
# Needs g++, and arm-linux-gnueabihf-g++ + qemu-arm for the ARM half (skipped
# with a note when they are missing).  Builds into a temporary directory.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
Q=${1:-}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fail=0
for t in video_merge video_tile; do
  g++ -O2 -Wall -Wextra -o "$W/$t.x86" "$HERE/test_$t.cc" && "$W/$t.x86" $Q || fail=1
done
# The OBJ X sign extension (video_obj.h, used by video.cc): all 65536 attr1
# values under UBSan, and a negative control -- the original expression must
# be caught, or the check is not testing the shift.
gcc -O2 -Wall -Wextra -fsanitize=undefined -fno-sanitize-recover=all \
  -o "$W/vo.x86" "$HERE/test_video_obj.c" && "$W/vo.x86" || fail=1
gcc -O2 -DOBJ_X_ORIGINAL -fsanitize=undefined -fno-sanitize-recover=all \
  -o "$W/vo.orig" "$HERE/test_video_obj.c" && ! "$W/vo.orig" 2>/dev/null || {
  echo "negative control: the original OBJ X expression was not caught"; fail=1; }
if command -v arm-linux-gnueabihf-g++ >/dev/null && command -v qemu-arm >/dev/null; then
  F="-O2 -Wall -Wextra -static -marm -march=armv7-a -mfpu=neon -mfloat-abi=hard"
  for t in video_merge video_tile; do
    arm-linux-gnueabihf-g++ $F -o "$W/$t.neon" "$HERE/test_$t.cc" && qemu-arm "$W/$t.neon" $Q || fail=1
  done
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -static -o "$W/vo.arm" "$HERE/test_video_obj.c" &&
    qemu-arm "$W/vo.arm" || fail=1
  arm-linux-gnueabihf-g++ $F -DVIDEO_MERGE_NO_VEC -o "$W/vm.arms" "$HERE/test_video_merge.cc" &&
    qemu-arm "$W/vm.arms" $Q || fail=1
else
  echo "(no arm-linux-gnueabihf-g++ / qemu-arm: the NEON half was not run)"
fi
[ $fail = 0 ] && echo "ALL OK" || { echo "FAILED"; exit 1; }
