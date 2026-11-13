#!/usr/bin/env bash
# Regression: instance B's image bounds and boot copies under the sanitizers.
#
#   tools/tests/run_gbadual_image_test.sh
#
# Builds a stand-in core with -fsanitize=address,undefined, partially links
# it with vita/gba_instance_b.ld and renames it gbab_* as corebuild.mk does,
# then
#   1. check_instance_b.sh accepts it, and refuses a link made without the
#      script and a doctored one (the build's assertion),
#   2. test_gbadual_image takes the boot copies over it: no sanitizer report,
#      every byte restored, every inconsistent marker set refused,
#   3. the OLD copy (a plain memcpy over the range; --naive) is reported as a
#      global-buffer-overflow -- the failure that was seen in a sanitized core
#      build and not in a plain one.  If this stops being reported the
#      sanitizer or the layout changed and step 2 proves less.
# Run from run_tests.sh too (vita/hostcore).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
GP=$(cd "$HERE/../.." && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
fail=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }

gcc -std=gnu99 -O1 -g $SAN -c -o "$W/core.o" "$HERE/gbadual_image_core.c" || exit 1
ld -r -d -T "$GP/vita/gba_instance_b.ld" -o "$W/b.tmp" "$W/core.o" || exit 1
CHK="sh $GP/vita/check_instance_b.sh nm readelf"
$CHK "$W/b.tmp" > "$W/chk.txt" 2>&1 && ok "check_instance_b accepts the scripted link: $(cat "$W/chk.txt" | sed 's/.*: //')" || { bad "check_instance_b refused the scripted link"; cat "$W/chk.txt"; }
ld -r -o "$W/noscript.o" "$W/core.o"
$CHK "$W/noscript.o" > /dev/null 2>&1 && bad "check_instance_b accepted a link without the script" || ok "check_instance_b refuses a link without the script"
# a script whose pristine room is one byte short must be refused
sed 's/\. += SIZEOF(\.data);/. += SIZEOF(.data) - 1;/' "$GP/vita/gba_instance_b.ld" > "$W/short.ld"
ld -r -d -T "$W/short.ld" -o "$W/short.o" "$W/core.o" &&
$CHK "$W/short.o" > /dev/null 2>&1 && bad "check_instance_b accepted a short pristine room" || ok "check_instance_b refuses a short pristine room"

nm -g --defined-only "$W/b.tmp" | awk 'NF == 3 { print $3 " gbab_" $3 }' > "$W/syms"
objcopy --redefine-syms="$W/syms" "$W/b.tmp" "$W/b.o"
gcc -std=gnu99 -O1 -g $SAN -no-pie -o "$W/t" "$HERE/test_gbadual_image.c" "$W/b.o" || exit 1
"$W/t" > "$W/t.txt" 2>&1 && ok "boot copies under ASan/UBSan: $(tail -1 "$W/t.txt")" || { bad "boot copies"; cat "$W/t.txt"; }
"$W/t" --naive > "$W/n.txt" 2>&1
if grep -q "global-buffer-overflow" "$W/n.txt" && grep -q "memcpy" "$W/n.txt"; then
  ok "plain memcpy over B's .data is reported by ASan (the original failure, reproduced)"
else
  bad "plain memcpy over B's .data was not reported -- the test no longer proves anything"; head -5 "$W/n.txt"
fi
[ $fail = 0 ] && echo "gbadual_image: PASS" || echo "gbadual_image: FAIL"
exit $fail
