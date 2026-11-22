#!/bin/sh
# ThreadSanitizer over the wide view's capture / compose split: compose runs on a real thread (snes_wide_twin --wide-split) while the next
# frame is emulated and captured. One ROM per invocation step, so that every foreground command stays short:
#   make -C snes/twin B=<dir> EXTRA="-fsanitize=thread -g" OPT=-O1 <dir>/snes_wide_twin
#   WIDE_TWIN=<dir>/snes_wide_twin sh snes/twin/wide_tsan.sh OUTDIR FRAMES ROM...
# For each ROM: a run without --wide-split (the single call path) and one with it (and, with MT=1, also with the worker threads
# apu_thread=1,ppu_thread=1), both with TSan; the per-frame wide hashes (--whashes) of the runs must be identical and TSan must print
# no report. Exit 0 when all ROMs pass.
# LADDER=1: the cost ladder runs too (--wopt policy=1 cost_us=1 cost_frames=5: it reaches the centre only floor at once, so the capture / compose
# threads and the core's flag are exercised); the frame at which the floor starts differs between the paths, so only the TSan reports are judged.   Copyright CBOC-Vita. MIT licence.
OUT=$1; FR=$2; shift 2
[ -n "$OUT" ] && [ -n "$FR" ] && [ -n "$1" ] || { echo "usage: wide_tsan.sh OUTDIR FRAMES ROM..."; exit 2; }
WT=${WIDE_TWIN:?set WIDE_TWIN to the TSan build of snes_wide_twin}
LW=""; [ "$LADDER" = 1 ] && LW="--wopt policy=1 --wopt cost_us=1 --wopt cost_frames=5"
PAD="40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none"
mkdir -p "$OUT" || exit 1
rc=0
for rom in "$@"; do
	n=$(basename "$rom" | tr ' ' '_'); n=${n%.*}
	d=$OUT/$n; rm -rf "$d"; mkdir -p "$d"
	TSAN_OPTIONS="log_path=$d/tsan_single halt_on_error=0" "$WT" "$rom" --frames "$FR" --pad "$PAD" --wide 70 $LW --map $d/single.map --whashes $d/single.wh >/dev/null 2>$d/single.err
	TSAN_OPTIONS="log_path=$d/tsan_split halt_on_error=0" "$WT" "$rom" --frames "$FR" --pad "$PAD" --wide 70 $LW --wide-split --map-poll --map $d/split.map --whashes $d/split.wh >/dev/null 2>$d/split.err
	mt=same
	if [ "$MT" = 1 ]; then
		TSAN_OPTIONS="log_path=$d/tsan_splitmt halt_on_error=0" "$WT" "$rom" --frames "$FR" --pad "$PAD" --wide 70 $LW --wide-split --map-poll --map $d/splitmt.map --opt apu_thread=1 --opt ppu_thread=1 --whashes $d/splitmt.wh >/dev/null 2>$d/splitmt.err
		cmp -s $d/single.wh $d/splitmt.wh || mt=DIFF
	fi
	reports=$(cat "$d"/tsan_* 2>/dev/null | grep -c "WARNING: ThreadSanitizer")
	h=same; [ "$LADDER" = 1 ] || cmp -s $d/single.wh $d/split.wh || h=DIFF
	[ "$LADDER" = 1 ] && mt=same
	echo "$n: frames $(wc -l < $d/split.wh), single vs split hashes $h, vs split+workers $mt, tsan reports $reports"
	[ "$h" = same ] && [ "$mt" = same ] && [ "$reports" = 0 ] || rc=1
done
# FUZZ=1: also random register / VRAM / OAM writes between frames and lines (wide_fuzz.cpp) with the strip cache checked against a fresh draw, with
# the worker threads and the margin thread, once per ROM: TSan must print no report and the run must report 0 differing lines (needs the hooked core of snes_wide_twin).
if [ "$FUZZ" = 1 ]; then
	for rom in "$@"; do
		n=$(basename "$rom" | tr ' ' '_'); n=${n%.*}
		d=$OUT/$n; mkdir -p "$d"
		TSAN_OPTIONS="log_path=$d/tsan_fuzz halt_on_error=0" "$WT" "$rom" --frames "$FR" --pad "$PAD" --wide 70 --fuzz 1003 --fuzz-mid --wide-toggle 53 --opt wide_cache_verify=1 --opt apu_thread=1 --opt ppu_thread=1 --wide-split --map-poll --summary >$d/fuzz.out 2>$d/fuzz.err
		dif=$(grep '^wcache:' $d/fuzz.out | sed 's/.*differing=//')
		reports=$(cat "$d"/tsan_fuzz* 2>/dev/null | grep -c "WARNING: ThreadSanitizer")
		echo "$n: fuzz + strip cache check with threads: lines differing from a fresh draw ${dif:-none}, tsan reports $reports"
		[ "$dif" = 0 ] && [ "$reports" = 0 ] || rc=1
	done
fi
exit $rc
