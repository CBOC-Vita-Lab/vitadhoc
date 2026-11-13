#!/bin/sh
# Golden composed wide frames of the twin: writes one '--wide-hashes' file per run into OUTDIR, over the scenarios of
# selftest section 13 (the synthetic cartridges with the wide view on, with the scout, the first pass, the second pass with
# the learned map, the map reused in another play).  Two runs of this script must give identical directories:
#   wide_golden.sh OUTDIR          (NES_TWIN=... overrides nes/twin/build/nes_twin; WG_EXTRA="--wide-split" adds twin options to every run)
#   diff -r OLD NEW && echo IDENTICAL
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
TWIN=${NES_TWIN:-$HERE/build/nes_twin}
OUT=$1
[ -n "$OUT" ] || { echo "usage: wide_golden.sh OUTDIR"; exit 2; }
mkdir -p "$OUT" || exit 1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 -I "$HERE/tests/make_roms.py" $T/roms >/dev/null || { echo "ROM generation failed"; exit 1; }
python3 -I "$HERE/tests/scroll_roms.py" $T/sr >/dev/null || { echo "scrolling ROM generation failed"; exit 1; }
R=$T/roms
WPAD="40:start,60:none,100:a+right,140:b,200:left+up,260:none,300:right"
fail=0
# 1. the synthetic cartridges of section 13: wide on, and wide on + scout
for m in $R/nrom.nes $R/mmc1.nes $R/mmc3.nes $R/gun.nes $T/sr/scroll_vert.nes $T/sr/scroll_mmc3.nes $T/sr/scroll_chrram.nes $T/sr/scroll_4way.nes $T/sr/single_screen.nes; do
	n=$(basename "$m" .nes)
	"$TWIN" "$m" --frames 400 --pad $WPAD --wide 84 $WG_EXTRA --wide-report --wide-hashes "$OUT/$n.on.wh" >/dev/null 2>&1 || fail=1
	"$TWIN" "$m" --frames 400 --pad $WPAD --wide 84 $WG_EXTRA --scout 3 --wide-hashes "$OUT/$n.scout.wh" >/dev/null 2>&1 || fail=1
done
# 2. truth scenarios: first pass (map saved), second pass (map loaded), scout, and the map reused in another play
truth() {   # name frames pad [extra]
	n=$1; fr=$2; pad=$3; shift 3
	"$TWIN" $T/sr/$n.nes --frames $fr --pad $pad --wide 84 $WG_EXTRA --wide-truth $T/sr/$n --wide-hashes "$OUT/$n.p1.wh" --wide-map-save $T/$n.map "$@" >/dev/null 2>&1 || fail=1
	"$TWIN" $T/sr/$n.nes --frames $fr --pad $pad --wide 84 $WG_EXTRA --wide-truth $T/sr/$n --wide-hashes "$OUT/$n.p2.wh" --wide-map-load $T/$n.map "$@" >/dev/null 2>&1 || fail=1
	"$TWIN" $T/sr/$n.nes --frames $fr --pad $pad --wide 84 $WG_EXTRA --wide-truth $T/sr/$n --wide-hashes "$OUT/$n.s2.wh" --scout 2 "$@" >/dev/null 2>&1 || fail=1
}
truth scroll_vert 900 10:right
truth scroll_vert_nomask 900 10:right
truth scroll_mmc3 900 10:right
truth scroll_chrram 900 10:right
truth scroll_4way 1800 10:right,430:down,960:up
truth single_screen 400 100:start,103:none,250:start,253:none
"$TWIN" $T/sr/scroll_vert.nes --frames 1200 --pad "10:right,200:none,260:right,400:left,520:right,700:none,720:right" --wide 84 $WG_EXTRA \
	--wide-truth $T/sr/scroll_vert --wide-hashes "$OUT/scroll_vert.reuse.wh" --wide-map-load $T/scroll_vert.map >/dev/null 2>&1 || fail=1
if [ -n "$NES_TESTROMS" ] && [ -d "$NES_TESTROMS" ]; then
	# the public test ROMs that load: wide on, one hash file each (names with spaces are read line by line)
	n=0
	find "$NES_TESTROMS" -iname '*.nes' | sort > $T/wlist
	while IFS= read -r r; do
		"$TWIN" "$r" --info >/dev/null 2>&1 || continue
		n=$((n + 1))
		"$TWIN" "$r" --frames 400 --wide 84 $WG_EXTRA --wide-hashes "$OUT/public_$n.wh" >/dev/null 2>&1 || fail=1
	done < $T/wlist
fi
echo "wide golden: $(ls "$OUT" | wc -l) files in $OUT, run errors: $fail"
exit $fail
