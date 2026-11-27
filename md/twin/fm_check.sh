#!/bin/bash
# Proof that the YM2612 on its own thread (md/core/fm_thread.c) changes nothing: sound, picture, machine state and RAM hashes equal to the single
# thread path, on the Linux twin (md_twin), for the SGDK sample ROMs and for the Sega CD stand-in with FM playing. Lines start with PASS / FAIL / SKIP.
#   MD_TEST_ROMS=<dir>   a tree with the SGDK sample ROMs (rom.bin files); without it only the timer shadow self test and the Sega CD part run
#   FMC_FRAMES=N         frames per ROM (default 900)
#   MD_TWIN=<binary>     the twin to use (default build/x86/md_twin beside this script)
#   FMC_LOG=<file>       the census lines (FMW ...) of every run are appended here
# Variants of the worker: --fm-thread (plain), --fm-check (the timer shadow compared with the chip at every join), --fm-stress (a join after every run
# of samples), --fm-inline (the emulating thread always takes the queue over: the fallback path), --fm-tiny (a 24 word queue: the queue full path).
# Copyright CBOC-Vita. MIT licence for this script.
HERE=$(cd "$(dirname "$0")" && pwd)
TWIN=${MD_TWIN:-$HERE/build/x86/md_twin}
F=${FMC_FRAMES:-900}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok()   { echo "PASS  $1"; }
bad()  { echo "FAIL  $1"; fail=1; }
skip() { echo "SKIP  $1"; }
[ -x "$TWIN" ] || { bad "md_twin not built ($TWIN)"; exit 1; }
hs() { grep '^RESULT' | sed 's/ samples=.*//;s/^RESULT [^ ]* //'; }          # video audio state ram
logfm() { [ -n "$FMC_LOG" ] && grep -a '^FMW' >> "$FMC_LOG"; }

# 0. the timer shadow against the real chip on random writes and runs
o=$("$TWIN" /dev/null --fm-selftest 2>&1)
case "$o" in *"differences=0"*) ok "timer shadow equals the chip's timers on 40 random write / run sequences (4000 steps each)";; *) bad "timer shadow self test: $o";; esac

# the variants every ROM runs
VARS="--fm-thread|--fm-check|--fm-stress|--fm-inline|--fm-tiny|--fm-check --vdp-thread|--fm-thread --pad auto"
roms=""
[ -n "$MD_TEST_ROMS" ] && [ -d "$MD_TEST_ROMS" ] && roms=$(find "$MD_TEST_ROMS" -name rom.bin -path '*/out/release/*' | sort)
n=$(echo "$roms" | grep -c .)
if [ "$n" -gt 0 ]; then
	same=0; diff=0; used=0; unused=0; stateeq=0; stateneq=0; rt=0; rtbad=0; xl=0; xlbad=0; shadowbad=0; takes=0; fulls=0; ahd=0; ahdbad=0
	for r in $roms; do
		name=$(echo "$r" | sed 's|.*/sample/||;s|/out/.*||')
		for pad in "--pad auto" "--pad none"; do
			base=$("$TWIN" "$r" --frames "$F" $pad 2>&1 | hs)
			[ -n "$base" ] || { bad "$name: no result"; continue; }
			IFS='|' read -ra VV <<< "--fm-thread|--fm-check|--fm-stress|--fm-inline|--fm-tiny|--fm-check --vdp-thread"
			for v in "${VV[@]}"; do
				out=$("$TWIN" "$r" --frames "$F" $pad $v 2>&1)
				h=$(echo "$out" | hs)
				if [ "$h" = "$base" ]; then same=$((same + 1)); else diff=$((diff + 1)); echo "  differs: $name $pad $v"; echo "    base $base"; echo "    got  $h"; fi
				f=$(echo "$out" | grep -a '^FMW')
				echo "$f" | logfm
				case "$f" in *"check_errors=0"*) ;; *) shadowbad=$((shadowbad + 1)); echo "  shadow/check: $name $v: $f";; esac
				case "$f" in *" runs=0 "*|*"worker off"*) unused=$((unused + 1));; *) used=$((used + 1));; esac
				case "$v" in *inline*) t=$(echo "$f" | sed 's/.*takeovers=\([0-9]*\).*/\1/'); [ "${t:-0}" -gt 0 ] && takes=$((takes + 1));; esac
				case "$v" in *tiny*) t=$(echo "$f" | sed 's/.*full_waits=\([0-9]*\).*/\1/'); [ "${t:-0}" -gt 0 ] && fulls=$((fulls + 1));; esac
			done
		done
		# Quick response (run-ahead) with the worker
		a0=$("$TWIN" "$r" --frames 300 --pad auto --ahead 2>&1 | hs); a1=$("$TWIN" "$r" --frames 300 --pad auto --ahead --fm-check 2>&1 | hs)
		[ -n "$a0" ] && [ "$a0" = "$a1" ] && ahd=$((ahd + 1)) || { ahdbad=$((ahdbad + 1)); echo "  run-ahead differs: $name"; }
		# the saved state is the same bytes, and a state saved by one path loads in the other
		"$TWIN" "$r" --frames 400 --pad auto --state-frame 250 --save-state "$T/s1.st" >/dev/null 2>&1
		"$TWIN" "$r" --frames 400 --pad auto --state-frame 250 --save-state "$T/s2.st" --fm-check >/dev/null 2>&1
		if [ -s "$T/s1.st" ] && cmp -s "$T/s1.st" "$T/s2.st"; then stateeq=$((stateeq + 1)); else stateneq=$((stateneq + 1)); echo "  state bytes differ: $name"; fi
		l0=$("$TWIN" "$r" --frames 600 --pad auto --state-frame 250 --load-state "$T/s1.st" 2>&1 | hs)
		l1=$("$TWIN" "$r" --frames 600 --pad auto --state-frame 250 --load-state "$T/s2.st" --fm-check 2>&1 | hs)
		l2=$("$TWIN" "$r" --frames 600 --pad auto --state-frame 250 --load-state "$T/s1.st" --fm-check 2>&1 | hs)
		[ -n "$l0" ] && [ "$l0" = "$l1" ] && [ "$l0" = "$l2" ] && xl=$((xl + 1)) || { xlbad=$((xlbad + 1)); echo "  loaded state continues differently: $name"; }
		c=$("$TWIN" "$r" --state-roundtrip 300 --pad auto --fm-check 2>&1 | grep '^ROUNDTRIP')
		case "$c" in *"load=0 video=SAME state=SAME audio=SAME corrupt_refused=yes"*) rt=$((rt + 1));; *) rtbad=$((rtbad + 1)); echo "  round trip: $name: $c";; esac
	done
	[ $diff = 0 ] && ok "$n ROMs x 2 pad scripts x 6 worker variants: picture, sound, state and RAM hashes equal to the single thread ($same runs, $F frames each)" || bad "$diff worker run(s) differ from the single thread"
	[ $shadowbad = 0 ] && ok "the timer shadow equalled the chip at every join of every run" || bad "$shadowbad run(s) with a timer shadow / check error"
	[ $unused = 0 ] && ok "the worker really ran in all $used runs (FM runs queued)" || bad "$unused run(s) where the worker did not run"
	[ $takes -gt 0 ] && [ $fulls -gt 0 ] && ok "the fallback paths ran: the emulating thread took the queue over ($takes runs), the queue was full ($fulls runs), all identical" || bad "fallback paths not exercised (takeovers $takes, full queue $fulls)"
	[ $ahdbad = 0 ] && ok "Quick response (run-ahead): worker equals single thread on $ahd ROMs" || bad "run-ahead differs on $ahdbad ROM(s)"
	[ $stateneq = 0 ] && ok "save state: the same bytes with the worker as without ($stateeq ROMs)" || bad "state bytes differ on $stateneq ROM(s)"
	[ $xlbad = 0 ] && ok "a state saved by one path loads in the other and runs on identically ($xl ROMs)" || bad "cross loaded state differs on $xlbad ROM(s)"
	[ $rtbad = 0 ] && ok "state round trip identical with the worker ($rt ROMs)" || bad "round trip failed on $rtbad ROM(s)"
	# chip options and regions, on the ROMs that use the FM most
	opd=0; opbad=0; nk=0; nkbad=0
	for r in $(echo "$roms" | grep -i "xgm-player\|sound-test\|sonic\|bad-apple" | head -4); do
		for o in "fm=1" "fm=2" "fm=0 --region eur" "fm=0 --reset-at 200"; do
			set -- $o; op="--opt $1"; shift; extra="$*"
			b=$("$TWIN" "$r" --frames 500 --pad auto $op $extra 2>&1 | hs); w=$("$TWIN" "$r" --frames 500 --pad auto $op $extra --fm-check 2>&1 | hs)
			[ -n "$b" ] && [ "$b" = "$w" ] && opd=$((opd + 1)) || { opbad=$((opbad + 1)); echo "  option run differs: $r $o"; }
		done
		for o in "fm=3" "fm=4"; do
			b=$("$TWIN" "$r" --frames 300 --pad auto --opt $o 2>&1 | hs); w=$("$TWIN" "$r" --frames 300 --pad auto --opt $o --fm-thread 2>&1)
			case "$w" in *"FMW"*"worker off"*) [ "$b" = "$(echo "$w" | hs)" ] && nk=$((nk + 1)) || nkbad=$((nkbad + 1));; *) nkbad=$((nkbad + 1));; esac
		done
	done
	[ $opbad = 0 ] && ok "chip types (ASIC, enhanced), PAL and a mid-run reset: identical with the worker ($opd runs)" || bad "$opbad option run(s) differ"
	[ $nkbad = 0 ] && ok "the Nuked chips run inline when the worker is asked for (no worker, identical, $nk runs)" || bad "Nuked chip with the worker asked for: $nkbad run(s) wrong"
else
	skip "Mega Drive ROM runs (set MD_TEST_ROMS to a tree with the SGDK sample ROMs)"
fi

# Sega CD: the stand-in BIOS with the main CPU playing the YM2612 (mkscd.py standin-fm): FM, PCM and CD audio mixed
if command -v python3 >/dev/null; then
	CD="$T/cd"; mkdir -p "$CD"
	if python3 -I "$HERE/tools/mkscd.py" standin-fm "$CD/fm.bin" && python3 -I "$HERE/tools/mkscd.py" discs "$CD/discs" >/dev/null 2>&1; then
		cdbad=0; cdn=0
		for disc in single.cue disc.chd multi.cue; do
			base=$("$TWIN" --disc "$CD/discs/$disc" --bios "$CD/fm.bin" --bios-stand-in --frames 900 2>&1 | hs)
			[ -n "$base" ] || { bad "Sega CD $disc: no result"; continue; }
			for v in "--fm-thread" "--fm-check" "--fm-stress" "--fm-inline" "--fm-tiny" "--fm-check --vdp-thread --cd-thread"; do
				out=$("$TWIN" --disc "$CD/discs/$disc" --bios "$CD/fm.bin" --bios-stand-in --frames 900 $v 2>&1)
				echo "$out" | logfm
				f=$(echo "$out" | grep -a '^FMW')
				if [ "$(echo "$out" | hs)" = "$base" ]; then cdn=$((cdn + 1)); else cdbad=$((cdbad + 1)); echo "  Sega CD $disc $v differs"; fi
				case "$f" in *"check_errors=0"*) ;; *) cdbad=$((cdbad + 1)); echo "  Sega CD $disc $v: $f";; esac
			done
		done
		plain=$("$TWIN" --disc "$CD/discs/single.cue" --bios "$CD/fm.bin" --bios-stand-in --frames 900 --fm-check 2>&1 | grep -a '^FMW')
		case "$plain" in *" writes=0 "*|*"worker off"*) bad "Sega CD stand-in did not play the FM ($plain)";; *) [ $cdbad = 0 ] && ok "Sega CD (FM from the main CPU, PCM, CD audio): worker equals single thread on 3 discs x 6 variants ($cdn runs); $(echo "$plain" | sed 's/.*\(writes=[0-9]*\).*\(runs=[0-9]*\).*/\1 \2/')" || bad "Sega CD: $cdbad difference(s)";; esac
		c=$("$TWIN" --disc "$CD/discs/single.cue" --bios "$CD/fm.bin" --bios-stand-in --state-roundtrip 600 --fm-check 2>&1 | grep '^ROUNDTRIP')
		case "$c" in *"load=0 video=SAME state=SAME audio=SAME corrupt_refused=yes"*) ok "Sega CD: state round trip identical with the worker";; *) bad "Sega CD round trip: $c";; esac
		a0=$("$TWIN" --disc "$CD/discs/single.cue" --bios "$CD/fm.bin" --bios-stand-in --frames 300 --ahead 2>&1 | hs); a1=$("$TWIN" --disc "$CD/discs/single.cue" --bios "$CD/fm.bin" --bios-stand-in --frames 300 --ahead --fm-check 2>&1 | hs)
		[ -n "$a0" ] && [ "$a0" = "$a1" ] && ok "Sega CD: run-ahead identical with the worker" || bad "Sega CD run-ahead differs ($a0 / $a1)"
	else bad "Sega CD fixtures (mkscd.py standin-fm)"; fi
else
	skip "Sega CD (needs python3)"
fi

[ $fail = 0 ]
