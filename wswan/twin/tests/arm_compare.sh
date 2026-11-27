#!/bin/sh
# Copyright CBOC-Vita. MIT licence.
# The WonderSwan host on x86 against armhf (qemu-arm), on the same inputs, byte for byte:
#   1. the twin (x86 build, armhf build under qemu-arm, and an x86 build with -funsigned-char, which is what ARM's char is)
#      runs soak.py: frame hashes, audio WAV, save file, rewind results, summary line
#   2. api_run (the harness that links the shipped frontend/wswancore.o, the blend and the safe writes) built for x86 and
#      armhf against each architecture's wswancore.o: per refresh picture hashes, audio samples, state hash, save file
# usage: arm_compare.sh WORKDIR [--frames N] [--roms DIR] [--public DIR] [--private FILE]
# Needs: arm-linux-gnueabihf-gcc/g++, qemu-arm (with -L /usr/arm-linux-gnueabihf). Builds go in WORKDIR (never in the repo).
set -e
export TZ=UTC   # the cartridge RTC reads local time; the same zone on every run
HERE=$(cd "$(dirname "$0")" && pwd)
WS=$(cd "$HERE/../.." && pwd)
W=$1; shift
FRAMES=4000; EXTRA_ARGS=""
while [ $# -gt 0 ]; do
	case "$1" in
	--frames) FRAMES=$2; shift 2 ;;
	--roms|--public|--private) EXTRA_ARGS="$EXTRA_ARGS $1 $2"; shift 2 ;;
	*) echo "unknown $1"; exit 2 ;;
	esac
done
mkdir -p "$W"
X=$W/x86; U=$W/x86-uchar; A=$W/armhf
ARMF="-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"
NICE="nice -n 10"

echo "== builds"
$NICE make -C "$WS/twin" -j1 B="$X" OPT=-O2 >"$W/build-x86.log" 2>&1
$NICE make -C "$WS/twin" -j1 B="$U" OPT=-O2 EXTRA=-funsigned-char >"$W/build-uchar.log" 2>&1
$NICE make -C "$WS/twin" -j1 B="$A" OPT=-O2 CC="arm-linux-gnueabihf-gcc $ARMF" CXX="arm-linux-gnueabihf-g++ $ARMF" >"$W/build-armhf.log" 2>&1
$NICE make -C "$WS/frontend" B="$X/fe" >"$W/fe-x86.log" 2>&1
$NICE make -C "$WS/frontend" TARGET=armhf B="$A/fe" >"$W/fe-armhf.log" 2>&1
printf '#!/bin/sh\nexec qemu-arm -L /usr/arm-linux-gnueabihf "%s/wswan_twin" "$@"\n' "$A" > "$W/twin-armhf"
chmod +x "$W/twin-armhf"

echo "== api harness (x86 and armhf, each against its own wswancore.o)"
FE=$WS/frontend
for arch in x86 armhf; do
	if [ $arch = x86 ]; then CC=gcc; CXX=g++; OUT=$X; OBJ=$X/fe/wswancore.o; else CC="arm-linux-gnueabihf-gcc $ARMF"; CXX="arm-linux-gnueabihf-g++ $ARMF"; OUT=$A; OBJ=$A/fe/wswancore.o; fi
	mkdir -p "$OUT/api"
	$CC -O2 -std=gnu99 -Wall -I"$FE" -c -o "$OUT/api/api_run.o" "$HERE/api_run.c"
	$CC -O2 -std=gnu99 -Wall -I"$FE" -c -o "$OUT/api/blend.o" "$FE/blend.c"
	$CC -O2 -std=gnu99 -Wall -I"$FE" -c -o "$OUT/api/layout.o" "$FE/layout.c"
	$CXX -O2 -std=gnu++11 -Wall -I"$FE" -c -o "$OUT/api/safe_io.o" "$FE/safe_io.cpp"
	$CXX -o "$OUT/api/api_run" "$OUT/api/api_run.o" "$OUT/api/blend.o" "$OUT/api/layout.o" "$OUT/api/safe_io.o" "$OBJ" -lm
done
printf '#!/bin/sh\nexec qemu-arm -L /usr/arm-linux-gnueabihf "%s/api/api_run" "$@"\n' "$A" > "$W/bin-api-armhf"
chmod +x "$W/bin-api-armhf"

fail=0
run_soak() { # name twin
	rm -rf "$W/$1"; mkdir -p "$W/$1"
	python3 -I "$HERE/soak.py" --twin "$2" --work "$W/$1" --frames $FRAMES $EXTRA_ARGS > "$W/$1.txt" 2>&1 || fail=1
}
echo "== soak: x86"; run_soak soak-x86 "$X/wswan_twin"
echo "== soak: x86 with unsigned char"; run_soak soak-uchar "$U/wswan_twin"
echo "== soak: armhf under qemu-arm"; run_soak soak-armhf "$W/twin-armhf"
echo "== compare soak outputs"
for other in soak-uchar soak-armhf; do
	if diff -r -x '*.txt' "$W/soak-x86" "$W/$other" >"$W/diff-$other.txt" 2>&1; then echo "identical: x86 vs $other (hashes, audio, saves, rewinds)"; else echo "DIFFERENT: x86 vs $other (see $W/diff-$other.txt)"; fail=1; fi
done
# the summary lines, without the timing fields
sed 's/cpu_ms[^ ]*//g; s/, work [^)]*//' "$W/soak-x86.txt" > "$W/sum-x86.txt"
for other in soak-uchar soak-armhf; do
	sed 's/cpu_ms[^ ]*//g; s/, work [^)]*//' "$W/$other.txt" > "$W/sum-$other.txt"
	cmp -s "$W/sum-x86.txt" "$W/sum-$other.txt" && echo "identical summary: x86 vs $other" || { echo "summary differs: x86 vs $other"; fail=1; }
done

echo "== api harness: x86 and armhf, on the soak's inputs"
fail_api=0
while read -r rom; do
	[ -n "$rom" ] || continue
	nm=$(basename "$rom" .ws)
	rm -rf "$W/api-x86/$nm" "$W/api-armhf/$nm"; mkdir -p "$W/api-x86/$nm" "$W/api-armhf/$nm"
	"$X/api/api_run" "$rom" $FRAMES "$W/api-x86/$nm" >/dev/null || fail=1
	"$W/bin-api-armhf" "$rom" $FRAMES "$W/api-armhf/$nm" >/dev/null || fail=1
	diff -r "$W/api-x86/$nm" "$W/api-armhf/$nm" >/dev/null && echo "identical api_run: $nm" || { echo "DIFFERENT api_run: $nm"; fail=1; }
done < "$W/soak-x86/roms.txt"
echo "arm_compare: $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
