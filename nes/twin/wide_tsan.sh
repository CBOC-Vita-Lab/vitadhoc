#!/bin/sh
# ThreadSanitizer over the whole wide_golden suite, one cartridge per invocation so that each fits in a short run:
#   make -C nes/twin B=build_tsan EXTRA="-fsanitize=thread -g" OPT=-O1
#   sh nes/twin/wide_tsan.sh OUTDIR            runs every cartridge one after another (in the foreground), split mode on
#   sh nes/twin/wide_tsan.sh OUTDIR scroll_vert  runs one
# Each invocation is wide_golden.sh with WG_ONLY=<cartridge> WG_EXTRA=--wide-split and NES_TWIN=build_tsan/nes_twin, its output
# hashes are compared with wide_golden_sha256.txt (the single-call baseline) and TSan's stderr goes to OUTDIR/<name>.tsan.
# Exit 0 when every cartridge finished, matched its baseline hashes and TSan printed no report.
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1; ONLY=$2
[ -n "$OUT" ] || { echo "usage: wide_tsan.sh OUTDIR [cartridge]"; exit 2; }
mkdir -p "$OUT" || exit 1
ALL="nrom mmc1 mmc3 gun scroll_vert scroll_vert_nomask scroll_mmc3 scroll_chrram scroll_4way single_screen"
[ -n "$ONLY" ] && ALL=$ONLY
rc=0
for n in $ALL; do
	d=$OUT/$n; rm -rf "$d"; mkdir -p "$d"
	WG_ONLY=$n WG_EXTRA="--wide-split" NES_TWIN=${NES_TWIN:-$HERE/build_tsan/nes_twin} TSAN_OPTIONS="log_path=$d/tsan halt_on_error=0" sh "$HERE/wide_golden.sh" "$d" > "$d/run.log" 2>&1
	e=$?
	reports=$(cat "$d"/tsan.* 2>/dev/null | grep -c "WARNING: ThreadSanitizer")
	bad=0
	for f in "$d"/*.wh; do
		b=$(basename "$f")
		want=$(grep " $b\$" "$HERE/wide_golden_sha256.txt" | cut -d' ' -f1)
		have=$(sha256sum "$f" | cut -d' ' -f1)
		[ "$want" = "$have" ] || { bad=$((bad + 1)); echo "  $n: $b differs from the baseline"; }
	done
	echo "$n: exit $e, files $(ls "$d"/*.wh 2>/dev/null | wc -l), tsan reports $reports, hash mismatches $bad"
	[ "$e" = 0 ] && [ "$reports" = 0 ] && [ "$bad" = 0 ] || rc=1
done
exit $rc
