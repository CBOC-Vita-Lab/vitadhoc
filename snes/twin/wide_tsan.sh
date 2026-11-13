#!/bin/sh
# ThreadSanitizer over the wide view's capture / compose split: compose runs on a real thread (snes_wide_twin --wide-split) while the next
# frame is emulated and captured. One ROM per invocation step, so that every foreground command stays short:
#   make -C snes/twin B=<dir> EXTRA="-fsanitize=thread -g" OPT=-O1 <dir>/snes_wide_twin
#   WIDE_TWIN=<dir>/snes_wide_twin sh snes/twin/wide_tsan.sh OUTDIR FRAMES ROM...
# For each ROM: a run without --wide-split (the single call path) and one with it (and, with MT=1, also with the worker threads
# apu_thread=1,ppu_thread=1), both with TSan; the per-frame wide hashes (--whashes) of the runs must be identical and TSan must print
# no report. Exit 0 when all ROMs pass.   Copyright CBOC-Vita. MIT licence.
OUT=$1; FR=$2; shift 2
[ -n "$OUT" ] && [ -n "$FR" ] && [ -n "$1" ] || { echo "usage: wide_tsan.sh OUTDIR FRAMES ROM..."; exit 2; }
WT=${WIDE_TWIN:?set WIDE_TWIN to the TSan build of snes_wide_twin}
PAD="40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none"
mkdir -p "$OUT" || exit 1
rc=0
for rom in "$@"; do
	n=$(basename "$rom" | tr ' ' '_'); n=${n%.*}
	d=$OUT/$n; rm -rf "$d"; mkdir -p "$d"
	TSAN_OPTIONS="log_path=$d/tsan_single halt_on_error=0" "$WT" "$rom" --frames "$FR" --pad "$PAD" --wide 70 --map $d/single.map --whashes $d/single.wh >/dev/null 2>$d/single.err
	TSAN_OPTIONS="log_path=$d/tsan_split halt_on_error=0" "$WT" "$rom" --frames "$FR" --pad "$PAD" --wide 70 --wide-split --map-poll --map $d/split.map --whashes $d/split.wh >/dev/null 2>$d/split.err
	mt=same
	if [ "$MT" = 1 ]; then
		TSAN_OPTIONS="log_path=$d/tsan_splitmt halt_on_error=0" "$WT" "$rom" --frames "$FR" --pad "$PAD" --wide 70 --wide-split --map-poll --map $d/splitmt.map --opt apu_thread=1 --opt ppu_thread=1 --whashes $d/splitmt.wh >/dev/null 2>$d/splitmt.err
		cmp -s $d/single.wh $d/splitmt.wh || mt=DIFF
	fi
	reports=$(cat "$d"/tsan_* 2>/dev/null | grep -c "WARNING: ThreadSanitizer")
	h=same; cmp -s $d/single.wh $d/split.wh || h=DIFF
	echo "$n: frames $(wc -l < $d/split.wh), single vs split hashes $h, vs split+workers $mt, tsan reports $reports"
	[ "$h" = same ] && [ "$mt" = same ] && [ "$reports" = 0 ] || rc=1
done
exit $rc
