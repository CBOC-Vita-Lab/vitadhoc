#!/bin/sh
# Self tests of the Sega core and its host glue (md_host_*) on Linux.
#   MD_TEST_ROMS=<dir>      a tree with the SGDK sample ROMs (sample/**/out/release/rom.bin); without it the ROM tests are SKIPped
#   MD_TEST_SMS_ROMS=<dir>  a tree of Master System / Game Gear / SG-1000 homebrew ROMs; without it those tests are SKIPped
#   MD_TEST_FRAMES=N        frames per ROM (default 1500)
#   MD_TWIN=<binary>        use this md_twin instead of building ../build/x86/md_twin
# No ROM is shipped with this repository; the ROMs are the owner's / public homebrew, fetched by the caller.
# Lines start with PASS / FAIL / SKIP (the gate counts them).
# Copyright CBOC-Vita. The Genesis Plus GX core in ../core keeps its own non-commercial licence (see ../core/PROVENANCE.md).
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TWIN=${MD_TWIN:-$HERE/build/x86/md_twin}
FRAMES=${MD_TEST_FRAMES:-1500}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok()   { echo "PASS  $1"; }
bad()  { echo "FAIL  $1"; fail=1; }
skip() { echo "SKIP  $1"; }

[ -x "$TWIN" ] || make -C "$HERE" -j2 ARCH=x86 >"$T/build.log" 2>&1
[ -x "$TWIN" ] || { tail -5 "$T/build.log"; bad "md_twin build"; exit 1; }

# 0. only md_host_* leaves the relocatable core object
if make -C "$HERE/../core" check TARGET=host B="$T/core" >"$T/check.log" 2>&1; then ok "core object exports only md_host_* ($(grep -c . "$T/check.log") lines of check output)"; else tail -3 "$T/check.log"; bad "exported symbol check"; fi

ROMS=""
if [ -n "$MD_TEST_ROMS" ] && [ -d "$MD_TEST_ROMS" ]; then
	ROMS=$(find "$MD_TEST_ROMS" -path '*/sample/*' -name rom.bin | sort)
fi
nrom=$(echo "$ROMS" | grep -c .)

if [ "$nrom" -gt 0 ]; then
	# 1. every ROM loads and runs FRAMES frames; 2. a second run is identical; 3. state round trip
	runok=0; runbad=0; detok=0; detbad=0; rtok=0; rtbad=0; refok=0; refbad=0
	for r in $ROMS; do
		a=$("$TWIN" "$r" --frames "$FRAMES" 2>&1 | grep '^RESULT')
		b=$("$TWIN" "$r" --frames "$FRAMES" 2>&1 | grep '^RESULT')
		case "$a" in *" frames=$FRAMES "*) runok=$((runok + 1));; *) runbad=$((runbad + 1)); echo "  no run: $r";; esac
		# the timing fields differ run to run: compare the hashes only
		ha=$(echo "$a" | sed 's/ samples=.*//'); hb=$(echo "$b" | sed 's/ samples=.*//')
		[ -n "$ha" ] && [ "$ha" = "$hb" ] && detok=$((detok + 1)) || { detbad=$((detbad + 1)); echo "  differs: $r"; }
		c=$("$TWIN" "$r" --state-roundtrip 2>&1 | grep '^ROUNDTRIP')
		case "$c" in *"load=0 video=SAME state=SAME audio=SAME"*) rtok=$((rtok + 1));; *) rtbad=$((rtbad + 1)); echo "  round trip: $r: $c";; esac
		case "$c" in *"corrupt_refused=yes"*) refok=$((refok + 1));; *) refbad=$((refbad + 1));; esac
	done
	[ $runbad = 0 ] && ok "$runok Mega Drive ROMs load and run $FRAMES frames" || bad "ROM runs ($runok ok, $runbad failed)"
	[ $detbad = 0 ] && ok "two runs identical (picture, sound and state hashes) on $detok ROMs" || bad "determinism ($detok same, $detbad different)"
	[ $rtbad = 0 ] && ok "state round trip identical (picture, sound, state) on $rtok ROMs" || bad "state round trip ($rtok same, $rtbad different)"
	[ $refbad = 0 ] && ok "a state with a flipped byte is refused on $refok ROMs" || bad "corrupt state ($refbad accepted)"

	pick() { echo "$ROMS" | grep "$1" | head -1; }
	HW=$(pick hello-world); JOY=$(pick joy-test); FS=$(pick flash-save); SND=$(pick sound-test)

	# 4. seeded randomness and the soft reset
	s1=$("$TWIN" "$SND" --frames 400 --reset-at 200 --seed 1 | grep -o 'state=[0-9a-f]*')
	s2=$("$TWIN" "$SND" --frames 400 --reset-at 200 --seed 1 | grep -o 'state=[0-9a-f]*')
	s3=$("$TWIN" "$SND" --frames 400 --reset-at 200 --seed 2 | grep -o 'state=[0-9a-f]*')
	[ -n "$s1" ] && [ "$s1" = "$s2" ] && [ "$s1" != "$s3" ] && ok "soft reset: same seed gives the same machine, another seed another power-on phase" || bad "seed / soft reset ($s1 $s2 $s3)"

	# 5. battery RAM
	rm -f "$T/fs.srm"
	"$TWIN" "$FS" --frames 900 --pad "60:a,90:none,120:b,150:none,180:start,200:none" --srm "$T/fs.srm" >/dev/null
	[ -s "$T/fs.srm" ] && ok "save RAM written ($(stat -c %s "$T/fs.srm") bytes, raw .srm)" || bad "save RAM"
	cp "$T/fs.srm" "$T/fs2.srm"
	"$TWIN" "$FS" --frames 5 --srm "$T/fs2.srm" >/dev/null
	cmp -s "$T/fs.srm" "$T/fs2.srm" && ok "save RAM loaded back unchanged" || bad "save RAM reload"

	# 6. region and 50 Hz
	pal=$("$TWIN" "$HW" --frames 3 --region eur --info | grep -o 'pal=[01] fps=[0-9.]*' | head -1)
	ntsc=$("$TWIN" "$HW" --frames 3 --region usa --info | grep -o 'pal=[01] fps=[0-9.]*' | head -1)
	jpp=$("$TWIN" "$HW" --frames 3 --region jppal --info | grep -o 'pal=[01] fps=[0-9.]*' | head -1)
	jpn=$("$TWIN" "$HW" --frames 3 --region jp --info | grep -o 'pal=[01] fps=[0-9.]*' | head -1)
	[ "$pal" = "pal=1 fps=49.7015" ] && [ "$ntsc" = "pal=0 fps=59.9227" ] && [ "$jpp" = "pal=1 fps=49.7015" ] && [ "$jpn" = "pal=0 fps=59.9227" ] \
		&& ok "regions: Europe and Japan PAL run at 49.7015 Hz, USA and Japan NTSC at 59.9227 Hz" || bad "regions ($pal / $ntsc / $jpp / $jpn)"
	h50=$("$TWIN" "$HW" --frames 200 --region eur | grep -o 'samples=[0-9]*')
	h60=$("$TWIN" "$HW" --frames 200 --region usa | grep -o 'samples=[0-9]*')
	n50=${h50#samples=}; n60=${h60#samples=}
	# 200 frames at 48 kHz: 193152 samples at 49.7015 Hz, 160206 at 59.9227 Hz (within 0.5 percent)
	if [ -n "$n50" ] && [ -n "$n60" ] && [ $((n50 * 1000 / 193152)) -ge 995 ] && [ $((n50 * 1000 / 193152)) -le 1005 ] && [ $((n60 * 1000 / 160206)) -ge 995 ] && [ $((n60 * 1000 / 160206)) -le 1005 ]; then
		ok "audio sample count per frame follows 48 kHz at 49.70 / 59.92 Hz ($n50, $n60 samples in 200 frames)"
	else bad "sample count ($n50, $n60)"; fi

	# 7. pads
	p3=$("$TWIN" "$JOY" --pad3 --frames 120 --pad "30:x+y+z+mode" | grep -o 'video=[0-9a-f]*')
	p6=$("$TWIN" "$JOY" --pad6 --frames 120 --pad "30:x+y+z+mode" | grep -o 'video=[0-9a-f]*')
	p6b=$("$TWIN" "$JOY" --pad6 --frames 120 --pad "30:x+y+z+mode" | grep -o 'video=[0-9a-f]*')
	[ -n "$p3" ] && [ "$p3" != "$p6" ] && [ "$p6" = "$p6b" ] && ok "6 button pad shows X Y Z Mode, 3 button pad does not" || bad "pad types ($p3 / $p6)"

	# 8. a .smd (interleaved) file gives the same machine as the .bin
	python3 -I - "$HW" "$T/hw.bin" "$T/hw.smd" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
d += b"\0" * (-len(d) % 16384)
open(sys.argv[2], "wb").write(d)
out = bytearray(512)
out[0] = len(d) // 16384; out[1] = 3; out[8] = 0xAA; out[9] = 0xBB; out[10] = 6
for i in range(0, len(d), 16384):
    blk = d[i:i + 16384]
    out += blk[1::2] + blk[0::2]
open(sys.argv[3], "wb").write(bytes(out))
PY
	b1=$("$TWIN" "$T/hw.bin" --frames 300 | sed 's/ samples=.*//; s/RESULT [^ ]* //')
	b2=$("$TWIN" "$T/hw.smd" --frames 300 | sed 's/ samples=.*//; s/RESULT [^ ]* //')
	[ -n "$b1" ] && [ "$b1" = "$b2" ] && ok "Super Magic Drive (.smd) file de-interleaves to the same machine as .bin" || bad "smd ($b1 / $b2)"

	# 9. cheats
	if [ -f "$REPO/gpsp/vita/tests/cheats/sega_vectors.txt" ]; then
		SMS1=""
		[ -n "$MD_TEST_SMS_ROMS" ] && SMS1=$(find "$MD_TEST_SMS_ROMS" -name '*.sms' | sort | head -1)
		python3 -I "$HERE/cheat_vectors.py" "$TWIN" "$REPO/gpsp/vita/tests/cheats/sega_vectors.txt" "$HW" $SMS1 || fail=1
		[ -z "$SMS1" ] && :
	else skip "cheat vectors (gpsp/vita/tests/cheats/sega_vectors.txt not present)"; fi
	"$TWIN" "$HW" --frames 3 --cheat "FFF000:0042" --peek FFF001 --peek FFF000 | grep -q 'PEEK FFF001=42' && ok "work RAM cheat written every frame (a 00VV patch lands in the other byte of the word, as Genesis Plus GX)" || bad "RAM cheat"
else
	skip "Mega Drive ROM tests (set MD_TEST_ROMS to a tree with the SGDK sample ROMs)"
fi

# 10. Master System / Game Gear / SG-1000 homebrew (own ROM tree)
SMSL=""
[ -n "$MD_TEST_SMS_ROMS" ] && [ -d "$MD_TEST_SMS_ROMS" ] && SMSL=$(find "$MD_TEST_SMS_ROMS" -type f \( -name '*.sms' -o -name '*.gg' -o -name '*.sg' -o -name '*.sc' \) | sort)
nsms=$(echo "$SMSL" | grep -c .)
if [ "$nsms" -gt 0 ]; then
	sok=0; sbad=0
	for r in $SMSL; do
		a=$("$TWIN" "$r" --frames 600 --state-roundtrip 2>&1 | grep -E '^ROUNDTRIP|^LOADFAIL')
		case "$a" in *"load=0 video=SAME state=SAME audio=SAME corrupt_refused=yes"*) sok=$((sok + 1));; *) sbad=$((sbad + 1)); echo "  $r: $a";; esac
	done
	[ $sbad = 0 ] && ok "$sok 8 bit ROMs boot, run 600 frames and round trip the state exactly" || bad "8 bit ROMs ($sok ok, $sbad failed)"
	f1=$(echo "$SMSL" | grep -i "\.sms$" | head -1); [ -n "$f1" ] || f1=$(echo "$SMSL" | head -1)
	i1=$("$TWIN" "$f1" --frames 3 --info | grep '^video' | head -1)
	case "$i1" in *"256x192 native 256x192"*"left_mask="*) ok "Master System picture info: 256x192 native, left column blank flag reported";; *) bad "SMS picture info ($i1)";; esac
	i2=$("$TWIN" "$f1" --system gg --frames 3 --info | grep '^video' | head -1)
	case "$i2" in *"160x144 native 160x144"*"gg_window=1"*) ok "Game Gear window 160x144 reported (forced on a Master System image: smoke test only)";; *) bad "GG picture info ($i2)";; esac
	ngg=$(echo "$SMSL" | grep -ci '\.gg$'); nsg=$(echo "$SMSL" | grep -ciE '\.(sg|sc)$')
	[ "$ngg" = 0 ] && skip "real Game Gear ROMs (none in the tree)"
	[ "$nsg" = 0 ] && skip "real SG-1000 ROMs (none in the tree)"
else
	skip "Master System / Game Gear / SG-1000 ROM tests (set MD_TEST_SMS_ROMS)"
fi

# 11. the wide view changes nothing the console does: with it on (the VDP draws strips beside the picture, the analysis reads the VDP's memory,
# the Game Gear draws its 256 wide band) the picture inside the original window, the sound, the machine state and the work RAM are bit for bit
# the same as with it off, with the VDP worker thread too (the strips are drawn in the worker's copy of the renderer)
WALL="$ROMS
$SMSL"
nw=$(echo "$WALL" | grep -c .)
if [ "$nw" -gt 0 ] && [ -n "$(echo "$WALL" | head -c 1)" ]; then
	WF=$FRAMES; [ "$WF" -gt 600 ] && WF=600
	wok=0; wbad=0; wcov=0
	for r in $WALL; do
		[ -f "$r" ] || continue
		base=$("$TWIN" "$r" --frames "$WF" --pad auto 2>&1 | grep '^RESULT' | sed 's/ samples=.*//')
		w1=$("$TWIN" "$r" --frames "$WF" --pad auto --wide 45 2>&1 | grep '^RESULT' | sed 's/ samples=.*//')
		w2=$("$TWIN" "$r" --frames "$WF" --pad auto --wide 45 --vdp-thread 2>&1 | grep '^RESULT' | sed 's/ samples=.*//')
		if [ -n "$base" ] && [ "$base" = "$w1" ] && [ "$base" = "$w2" ]; then wok=$((wok + 1)); else wbad=$((wbad + 1)); echo "  wide view changes the game: $r"; echo "    off:  $base"; echo "    on:   $w1"; echo "    on+worker: $w2"; fi
	done
	[ $wbad = 0 ] && ok "wide view on or off: picture in the original window, sound, state and RAM identical on $wok ROMs (with and without the VDP worker thread)" || bad "wide view identity ($wok same, $wbad different)"
	# 11b. what the wide view decides does not depend on the thread that analyses (the emulating thread, or the VDP worker while the emulating thread
	# goes on), and the worker's strip cache gives the strips a fresh draw gives (--wide-cache-check draws every cached strip again and compares)
	dok=0; dbad=0; cdiff=0; ccopied=0
	for r in $WALL; do
		[ -f "$r" ] || continue
		d1=$("$TWIN" "$r" --frames "$WF" --pad auto --wide 45 2>&1 | grep '^WIDE ' | sed 's/ analyse=[0-9.]*ms compose=[0-9.]*ms//')
		o2=$("$TWIN" "$r" --frames "$WF" --pad auto --wide 45 --vdp-thread --wide-cache-check 2>&1)
		d2=$(echo "$o2" | grep '^WIDE ' | sed 's/ analyse=[0-9.]*ms compose=[0-9.]*ms//')
		cc=$(echo "$o2" | grep '^WIDECACHE' | sed 's/.*copied=\([0-9]*\).*differed=\([0-9]*\).*/\1 \2/')
		[ -n "$cc" ] && { ccopied=$((ccopied + ${cc% *})); cdiff=$((cdiff + ${cc#* })); }
		if [ -n "$d1" ] && [ "$d1" = "$d2" ]; then dok=$((dok + 1)); else dbad=$((dbad + 1)); echo "  wide view decides differently on the worker thread: $r"; echo "    $d1"; echo "    $d2"; fi
	done
	[ $dbad = 0 ] && ok "wide view: the same decisions (real pixels, learned map, scenes) whichever thread analyses, on $dok ROMs" || bad "wide view decisions by thread ($dok same, $dbad different)"
	[ $cdiff = 0 ] && ok "wide view: the worker's strip cache equals a fresh draw ($ccopied cached line strips checked, 0 different)" || bad "strip cache ($cdiff of $ccopied cached strips differ from a fresh draw)"
else
	skip "wide view identity (needs MD_TEST_ROMS or MD_TEST_SMS_ROMS)"
fi

# 12. the learned picture is true: test cartridges made here (tools/sms_scroll_gen.py: a world of known tiles that scrolls right, left, right, far left;
# Master System with and without the masked first columns, Game Gear) and every margin pixel the wide view shows as real picture compared with the
# world (tools/wide_truth.py). Mega Drive: on the scrolling samples every learned cell that was shown beside the screen is compared with the cell
# the game shows there when the screen gets to it (--wide-check).
if command -v python3 >/dev/null; then
	TT="$T/truth"; mkdir -p "$TT"
	tbad=0; tline=""
	for v in "sms:" "smsnb:--noblank" "gg:--gg"; do
		nm=${v%%:*}; fl=${v#*:}
		ext=sms; [ "$nm" = gg ] && ext=gg
		gfl=$fl
		python3 -I "$HERE/tools/sms_scroll_gen.py" "$TT/scroll_$nm.$ext" $gfl || tbad=1
		o=$(python3 -I "$HERE/tools/wide_truth.py" "$TWIN" "$TT/scroll_$nm.$ext" "$TT/out_$nm" $fl 2>&1 | grep '^TRUTH') || true
		echo "  $o"
		case "$o" in *" wrong=0 "*"wrong=0") ;; *) tbad=1;; esac
		case "$o" in *" checked=0 "*|"") tbad=1;; esac
	done
	[ $tbad = 0 ] && ok "learned picture of the 8 bit consoles: every real margin pixel equals the test cartridge's world (3 cartridges)" || bad "learned picture truth (8 bit)"
	# Mega Drive strips (tools/md_scroll_gen.py: plane A scrolling out and back, per 2-cell vertical scroll with the columns at the edges standing
	# still and, half the time, differing at the left; a window plane over the top lines with plane B behind it): every real margin pixel
	# against the planes' worlds, with the VDP on the emulating thread and on its worker
	python3 -I "$HERE/tools/md_scroll_gen.py" make "$TT/md_scroll.md" || tbad=1
	m1=$(python3 -I "$HERE/tools/md_scroll_gen.py" check "$TWIN" "$TT/md_scroll.md" "$TT/out_md" 2>&1 | grep '^MDTRUTH')
	m2=$(python3 -I "$HERE/tools/md_scroll_gen.py" check "$TWIN" "$TT/md_scroll.md" "$TT/out_md" --vdp-thread 2>&1 | grep '^MDTRUTH')
	echo "  $m1"
	case "$m1" in *" wrong=0 strips_beside_split_columns=0 "*) [ "$m1" = "$m2" ] && case "$m1" in *"checked=0 "*) bad "Mega Drive strip truth (nothing checked)";; *"beside the window lines 0)"*) bad "Mega Drive strip truth (no strip beside the window lines)";; *) ok "Mega Drive strips: every real margin pixel equals the test cartridge's planes, beside window lines and per 2-cell vertical scroll lines too (the same with the VDP worker)";; esac || bad "Mega Drive strip truth differs with the VDP worker";; *) bad "Mega Drive strip truth";; esac
	if [ -n "$ROMS" ]; then
		mshown=0; mseen=0; mwrong=0
		for r in $ROMS; do
			o=$("$TWIN" "$r" --frames 3000 --pad auto --wide 51 --wide-check 2>&1 | grep '^WIDECHECK' | sed 's/.*shown=\([0-9]*\) seen later=\([0-9]*\) wrong=\([0-9]*\).*/\1 \2 \3/')
			[ -n "$o" ] || continue
			set -- $o
			mshown=$((mshown + $1)); mseen=$((mseen + $2)); mwrong=$((mwrong + $3))
		done
		[ $mwrong = 0 ] && [ $mseen -gt 0 ] && ok "learned picture of the Mega Drive: $mshown learned cells shown beside the screen, $mseen of them reached by the screen later, 0 were a different cell" || bad "learned picture truth (Mega Drive: shown $mshown, seen $mseen, wrong $mwrong)"
	fi
else
	skip "learned picture truth (needs python3)"
fi

[ $fail = 0 ]
