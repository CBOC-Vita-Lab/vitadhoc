#!/bin/sh
# The wide view's margin strip cache under change: random writes (between frames and between lines), the VRAM write counter across 2^32, state load,
# console reset, margin change and the margin thread joining and leaving, each against a reference run of the same twin with the cache and the fast
# pixel paths off (wide_cache=0, fast_pixels=0), and with every cache hit checked against a fresh draw (wide_cache_verify=1, "differing" must be 0).
#   WIDE_TWIN=<snes_wide_twin> sh snes/twin/wide_cache_proof.sh OUTDIR FRAMES ROM...
# Per scenario the wide frame hashes (the whole wide picture, and a flag that its centre is the normal picture) and the state hashes of the two runs must be identical. The scenario list is SCEN (default: all).
# NEG_TWIN=<snes_wide_twin built with EXTRA=-DWCACHE_NEGATIVE_CONTROL=7> (the 32 bit counter of the first version): the "wrap" scenario must then FAIL
# (differing > 0 or a different picture): the proof that the scenario would have caught it. Exit 0 when everything passes.   Copyright CBOC-Vita. MIT licence.
OUT=$1; FR=$2; shift 2
[ -n "$OUT" ] && [ -n "$FR" ] && [ -n "$1" ] || { echo "usage: wide_cache_proof.sh OUTDIR FRAMES ROM..."; exit 2; }
WT=${WIDE_TWIN:?set WIDE_TWIN to snes_wide_twin}
SCEN=${SCEN:-"fuzz fuzzmt wrap reload reset margin toggle ladder"}
PAD="40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none"
MT="--opt apu_thread=1 --opt ppu_thread=1"
WRAP=4294960000
mkdir -p "$OUT" || exit 1
rc=0
# The picture of a fuzzed run holds bytes the core never draws (the rows below an overscan or interlaced picture it was not asked to draw) and they follow the
# memory layout and the allocation history of the process, so the hashes of the plain frames (--hashes, kept in the output folder) are not compared in a fuzzed
# run; the wide frame hashes are, and so are the state hashes (the whole saved state). The runs are made with address space randomisation off where that works.
NOASLR=""; setarch "$(uname -m)" -R true 2>/dev/null && NOASLR="setarch $(uname -m) -R"
# run TAG TWIN ROM args...  -> $OUT/TAG.{h,wh,st,out}
run() {
	t=$1; tw=$2; rom=$3; shift 3
	$NOASLR "$tw" "$rom" --frames "$FR" --pad "$PAD" --hashes "$OUT/$t.h" --whashes "$OUT/$t.wh" --state-hash 50 "$OUT/$t.st" --summary "$@" >"$OUT/$t.out" 2>"$OUT/$t.err"
}
# scen NAME ROM args... : reference vs cache on + verify
scen() {
	n=$1; rom=$2; shift 2
	run ref "$WT" "$rom" "$@" --opt wide_cache=0 --opt fast_pixels=0
	run on "$WT" "$rom" "$@" --opt wide_cache_verify=1
	same=same
	for e in wh st; do cmp -s "$OUT/ref.$e" "$OUT/on.$e" || same=DIFF; done
	wl=$(grep '^wcache:' "$OUT/on.out")
	d=$(echo "$wl" | sed 's/.*differing=//')
	hit=$(echo "$wl" | sed 's/.*lines_from_cache=\([0-9]*\).*/\1/')
	st=ok; { [ "$same" = same ] && [ "$d" = 0 ]; } || st=FAIL
	echo "$st  $n: wide pictures / state $same, strip cache lines served $hit, differing from a fresh draw $d"
	[ "$st" = ok ] || rc=1
}
want() { case " $SCEN " in *" $1 "*) return 0;; esac; return 1; }
i=0; negtot=0
for rom in "$@"; do
	i=$((i+1)); echo "== ROM $i"
	W="--wide 96"; M="--wide 41"
	want fuzz && { scen "fuzz 3 (writes between frames and lines)" "$rom" $W --fuzz 3 --fuzz-mid; scen "fuzz 1003 (nearly every frame)" "$rom" $W --fuzz 1003 --fuzz-mid; }
	want fuzzmt && scen "fuzz 1003 with the worker threads, margin 41" "$rom" $M --fuzz 1003 --fuzz-mid $MT
	want wrap && {
		scen "VRAM write counter starts at $WRAP (crosses 2^32), fuzz 1003" "$rom" $W --fuzz 1003 --fuzz-mid --vseq $WRAP
		v=$(grep '^vseq:' "$OUT/on.out" | sed 's/vseq: //')
		[ "${v:-0}" -gt 4294967296 ] 2>/dev/null && echo "ok    wrap: counter ended at $v (passed 2^32)" || { echo "FAIL  wrap: counter ended at $v (did not pass 2^32: raise FRAMES)"; rc=1; }
		if [ -n "$NEG_TWIN" ]; then
			run neg "$NEG_TWIN" "$rom" $W --fuzz 1003 --fuzz-mid --vseq $WRAP --opt wide_cache_verify=1
			dn=$(grep '^wcache:' "$OUT/neg.out" | sed 's/.*differing=//')
			echo "      wrap negative control (32 bit counter) on this ROM: $dn lines differ from a fresh draw"
			negtot=$((negtot + ${dn:-0}))
		fi
	}
	want reload && scen "state load every 90 frames, fuzz 5" "$rom" $W --fuzz 5 --reload 90
	want reset && scen "console reset at frame 200, fuzz 7" "$rom" $W --fuzz 7 --reset-at 200
	want margin && scen "margin changes every 50 frames, fuzz 9" "$rom" $W --fuzz 9 --margin-cycle 50
	want toggle && scen "view off / on and margin thread leaving / joining every 40 frames (with threads)" "$rom" $W --fuzz 11 --wide-toggle 40 $MT
	want ladder && {
		run lad "$WT" "$rom" $W --wopt policy=1 --wopt cost_us=1 --wopt cost_frames=5 --pressure 120,300 --fuzz 13 --opt wide_cache_verify=1 $MT
		wl=$(grep '^wcache:' "$OUT/lad.out"); d=$(echo "$wl" | sed 's/.*differing=//')
		st=ok; [ "$d" = 0 ] || st=FAIL
		echo "$st  cost ladder rung changes (pressure, floor and back), fuzz 13, threads: differing from a fresh draw $d ($(grep '^ladder:' "$OUT/lad.out"))"
		[ "$st" = ok ] || rc=1
	}
done
if [ -n "$NEG_TWIN" ] && want wrap; then
	[ "$negtot" -gt 0 ] && echo "ok    wrap negative control: the 32 bit counter is caught ($negtot lines in all)" || { echo "FAIL  wrap negative control: not caught on these ROMs (the scenario would not notice a wrapping counter)"; rc=1; }
fi
exit $rc
