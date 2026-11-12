#!/bin/sh
# Wide view identity over the public test ROMs (NES_TESTROMS = a clone of christopherpow/nes-test-roms):
# frames, RAM, saved state, audio and battery RAM must be byte identical with the wide view off, on, and on + scout,
# and the centre of the wide picture must be the original picture. Usage: NES_TESTROMS=<tree> twin/wide_public.sh
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
TWIN=${NES_TWIN:-$HERE/build/nes_twin}
[ -d "$NES_TESTROMS" ] || { echo "set NES_TESTROMS"; exit 2; }
T=$(mktemp -d)
WPAD="40:start,60:none,100:a+right,140:b,200:left+up,260:none,300:right"
find "$NES_TESTROMS" -iname '*.nes' | sort > $T/list
: > $T/res
while IFS= read -r r; do
	if ! "$TWIN" "$r" --info >/dev/null 2>&1; then echo "noload $r" >> $T/res; continue; fi
	rm -f $T/*.srm
	"$TWIN" "$r" --frames 400 --pad $WPAD --hashes $T/h0 --ram-dump $T/r0 --save-state $T/s0 --state-frame 399 --wav $T/w0.wav --srm $T/0.srm >/dev/null 2>&1
	res=same
	for mode in "" "--scout 3"; do
		rm -f $T/1.srm
		"$TWIN" "$r" --frames 400 --pad $WPAD --wide 84 $mode --wide-report --hashes $T/h1 --ram-dump $T/r1 --save-state $T/s1 --state-frame 399 --wav $T/w1.wav --srm $T/1.srm >$T/out 2>&1
		cmp -s $T/h0 $T/h1 && cmp -s $T/r0 $T/r1 && cmp -s $T/s0 $T/s1 && cmp -s $T/w0.wav $T/w1.wav && [ -s $T/s0 ] \
			&& { [ ! -e $T/0.srm ] || cmp -s $T/0.srm $T/1.srm; } && grep -q "^wide-centre: differing_px=0 " $T/out || res="differs($mode)"
	done
	echo "$res $r" >> $T/res
	[ "$res" = same ] || echo "DIFF  $r ($res)"
	grep '^scout:' $T/out | sed "s|^|  $(basename "$r"): |" | grep -v 'snapshots=0 ' || true
done < $T/list
n=$(grep -c '^same' $T/res); b=$(grep -c '^differs' $T/res); nl=$(grep -c '^noload' $T/res)
echo "wide view off / on / on + scout over $((n + b)) public test ROMs: identical $n, different $b ($nl of $(wc -l < $T/list) files do not load)"
rm -rf $T
[ "$b" = 0 ]
