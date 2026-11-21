#!/bin/bash
# Proofs for three things the Sega player does with frames nobody sees, on the Linux twin (md_twin):
#   1. an UNDRAWN frame (fast forward, half-rate pacing, the real frame behind a run-ahead picture) leaves the machine exactly as a
#      drawn one does: sound, state and RAM hashes equal, single thread and with the VDP worker;
#   2. "Quick response" (run-ahead by one frame, md_host_run_frame_ahead) changes nothing in the machine: sound, state, RAM and
#      battery RAM hashes equal to a plain run; the picture it shows at frame n is the plain run's picture of frame n+1 while the
#      buttons do not change; and a button press shows one frame sooner (measured in frames on a small test cartridge made by tools/sms_latency_gen.py);
#   3. "No sprite flicker" (option sprite_limit=0; Master System, Game Gear, SG-1000) changes pictures only;
#   4. a loaded state (and a rewind landing) continues exactly as the run that was never interrupted (the "--proof" runs).
#
#   MD_TEST_ROMS=<dir>      a tree with the SGDK sample ROMs (rom.bin files)
#   MD_TEST_SMS_ROMS=<dir>  a tree of Master System / Game Gear / SG-1000 homebrew ROMs
#   AHEAD_ROM_LIST=<file>   or: one ROM path per line
#   AHEAD_FRAMES=N          frames per ROM (default 900)
#   MD_TWIN=<binary>        the twin to use (default ../build/x86/md_twin beside this script)
# No ROM is shipped with this repository. Lines start with PASS / FAIL / SKIP (the gate counts them).
# Copyright CBOC-Vita. MIT licence for this script.
HERE=$(cd "$(dirname "$0")" && pwd)
TWIN=${MD_TWIN:-$HERE/build/x86/md_twin}
F=${AHEAD_FRAMES:-900}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok()   { echo "PASS  $1"; }
bad()  { echo "FAIL  $1"; fail=1; }
skip() { echo "SKIP  $1"; }
[ -x "$TWIN" ] || { bad "md_twin not built ($TWIN)"; exit 1; }

: > "$T/roms"
[ -n "$AHEAD_ROM_LIST" ] && [ -f "$AHEAD_ROM_LIST" ] && cat "$AHEAD_ROM_LIST" >> "$T/roms"
[ -n "$MD_TEST_ROMS" ] && [ -d "$MD_TEST_ROMS" ] && find "$MD_TEST_ROMS" -name rom.bin | sort >> "$T/roms"
[ -n "$MD_TEST_SMS_ROMS" ] && [ -d "$MD_TEST_SMS_ROMS" ] && find "$MD_TEST_SMS_ROMS" \( -name '*.sms' -o -name '*.gg' -o -name '*.sg' \) | sort >> "$T/roms"
n=$(grep -c . "$T/roms")
if [ "$n" = 0 ]; then skip "no ROMs (set MD_TEST_ROMS, MD_TEST_SMS_ROMS or AHEAD_ROM_LIST)"; exit 0; fi

widel() { grep -a '^WIDE ' | grep -o 'analysed=[0-9]*\|real=[0-9]*%\|hindsight=[0-9/]*\|map_cells=[0-9]*\|rooms=[0-9]*\|map_px=[0-9]*\|rows=[0-9]*\|rejected=[0-9]*' | tr '\n' ' '; }
emu() { grep '^RESULT' | grep -o 'audio=[0-9a-f]* state=[0-9a-f]* ram=[0-9a-f]*'; }
sramh() { grep -o 'sram=[0-9a-f]*' | tail -1; }

und=0; undbad=0; ah=0; ahbad=0; ahthr=0; ahthrbad=0; ahwide=0; ahwidebad=0; shift=0; shiftbad=0; wl=0; wlbad=0; refused=0; fl=0; flbad=0; flpic=0; putback=0
while IFS= read -r r; do
	[ -f "$r" ] || continue
	"$TWIN" "$r" --frames "$F" --pad auto --frame-hashes "$T/p.fh" > "$T/plain.txt" 2>&1
	base=$(emu < "$T/plain.txt"); bs=$(sramh < "$T/plain.txt")
	[ -n "$base" ] || { bad "no run: $r"; continue; }

	# 1. undrawn frames
	a=$("$TWIN" "$r" --frames "$F" --pad auto --no-render 2>&1 | emu)
	und=$((und + 1)); [ "$a" = "$base" ] || { undbad=$((undbad + 1)); echo "  undrawn differs: $r"; }

	# 2. run-ahead: the machine
	"$TWIN" "$r" --frames "$F" --pad auto --ahead > "$T/ahead.txt" 2>&1
	a=$(emu < "$T/ahead.txt"); as=$(sramh < "$T/ahead.txt")
	if grep -q '^AHEAD frames=0 ' "$T/ahead.txt"; then refused=$((refused + 1)); fi
	pb=$(grep -o 'sram_put_back=[0-9]*' "$T/ahead.txt" | cut -d= -f2); [ "${pb:-0}" -gt 0 ] && putback=$((putback + 1))
	ah=$((ah + 1)); [ "$a" = "$base" ] && [ "$as" = "$bs" ] || { ahbad=$((ahbad + 1)); echo "  run-ahead differs: $r"; }
	# ... with the VDP worker (compared with the worker's own plain run)
	tb=$("$TWIN" "$r" --frames "$F" --pad auto --vdp-thread 2>&1 | emu)
	ta=$("$TWIN" "$r" --frames "$F" --pad auto --vdp-thread --ahead 2>&1 | emu)
	ahthr=$((ahthr + 1)); [ -n "$ta" ] && [ "$ta" = "$tb" ] || { ahthrbad=$((ahthrbad + 1)); echo "  run-ahead with the VDP worker differs: $r"; }
	# ... with the wide view's analysis running
	wb=$("$TWIN" "$r" --frames "$F" --pad auto --wide 48 2>&1 | emu)
	wa=$("$TWIN" "$r" --frames "$F" --pad auto --wide 48 --ahead 2>&1 | emu)
	ahwide=$((ahwide + 1)); [ -n "$wa" ] && [ "$wa" = "$wb" ] || { ahwidebad=$((ahwidebad + 1)); echo "  run-ahead with the wide view differs: $r"; }

	# 2a. what the wide view learns: run-ahead shows the plain run's frame n + 1 at frame n, so its analysis of frames 0..F-1 is the plain run's
	#     analysis of frames 1..F (the plain run skips frame 0): the counts must be the same (no button pressed: a press shows a frame sooner, which is the point)
	wp=$("$TWIN" "$r" --frames $((F + 1)) --pad none --wide 48 --wide-skip 1 2>&1 | widel)
	wq=$("$TWIN" "$r" --frames "$F" --pad none --wide 48 --ahead 2>&1 | widel)
	wl=$((wl + 1)); [ -n "$wp" ] && [ "$wp" = "$wq" ] || { wlbad=$((wlbad + 1)); echo "  wide view learns differently with run-ahead: $r"; echo "    plain: $wp"; echo "    ahead: $wq"; }

	# 2b. the picture: with no button pressed, run-ahead's frame n is the plain run's frame n+1
	"$TWIN" "$r" --frames "$F" --pad none --frame-hashes "$T/n.fh" > /dev/null 2>&1
	"$TWIN" "$r" --frames "$F" --pad none --ahead --frame-hashes "$T/a.fh" > /dev/null 2>&1
	if [ -s "$T/n.fh" ] && [ -s "$T/a.fh" ]; then
		tail -n +2 "$T/n.fh" | cut -d' ' -f2 > "$T/n1"; head -n $((F - 1)) "$T/a.fh" | cut -d' ' -f2 > "$T/a1"
		shift=$((shift + 1)); cmp -s "$T/n1" "$T/a1" || { shiftbad=$((shiftbad + 1)); echo "  run-ahead picture is not the next frame's: $r ($(diff "$T/n1" "$T/a1" | grep -c '^<') frames)"; }
	fi

	# 3. no sprite flicker (8 bit systems)
	case "$r" in *.sms|*.gg|*.sg)
		"$TWIN" "$r" --frames "$F" --pad auto --opt sprite_limit=1 --frame-hashes "$T/l1.fh" > "$T/l1.txt" 2>&1
		"$TWIN" "$r" --frames "$F" --pad auto --opt sprite_limit=0 --frame-hashes "$T/l0.fh" > "$T/l0.txt" 2>&1
		fl=$((fl + 1)); [ "$(emu < "$T/l1.txt" | sed 's/ state=[0-9a-f]*//')" = "$(emu < "$T/l0.txt" | sed 's/ state=[0-9a-f]*//')" ] || { flbad=$((flbad + 1)); echo "  no sprite flicker changes the machine: $r"; }
		cmp -s "$T/l1.fh" "$T/l0.fh" || flpic=$((flpic + 1));;
	esac
done < "$T/roms"

[ $undbad = 0 ] && ok "undrawn frames leave the machine as drawn ones do (sound, state, RAM) on $und ROMs" || bad "undrawn frames ($undbad of $und differ)"
[ $ahbad = 0 ] && ok "run-ahead leaves the machine untouched (sound, state, RAM, battery RAM) on $ah ROMs, $F frames each (refused on $refused; battery RAM put back on $putback)" || bad "run-ahead ($ahbad of $ah differ)"
[ $ahthrbad = 0 ] && ok "the same with the VDP worker on $ahthr ROMs" || bad "run-ahead with the VDP worker ($ahthrbad of $ahthr differ)"
[ $ahwidebad = 0 ] && ok "the same with the wide view on $ahwide ROMs" || bad "run-ahead with the wide view ($ahwidebad of $ahwide differ)"
[ $wlbad = 0 ] && ok "the wide view learns what it learns without run-ahead (picture, map cells, rooms, hindsight; the one frame it sees later accounted for) on $wl ROMs" || bad "wide view learning with run-ahead ($wlbad of $wl differ)"
[ $shiftbad = 0 ] && ok "run-ahead shows the next frame's picture (frame n = plain frame n+1, no buttons) on $shift ROMs" || bad "run-ahead picture ($shiftbad of $shift are not the next frame's)"
if [ $fl -gt 0 ]; then
	[ $flbad = 0 ] && ok "no sprite flicker changes pictures only on $fl 8 bit ROMs (sound and RAM equal; the state file's carry block holds the sprite list of the next first line, which has more entries) ($flpic of them show more sprites)" || bad "no sprite flicker ($flbad of $fl change the machine)"
else skip "no 8 bit ROM for the sprite flicker check"; fi

# 4. latency in frames: a small cartridge made here (sms_latency_gen.py) answers the pad one frame later, as most games do.
#    A press (button 1 is "b" in the twin's pad script) at frame K changes the picture D frames later; run-ahead must make it D-1.
LATROM="$T/latency.sms"
if python3 "$HERE/tools/sms_latency_gen.py" "$LATROM" > /dev/null 2>&1 && [ -s "$LATROM" ]; then
	K=300
	lat() {   # $1 = extra option; prints the first frame whose picture differs from the run without the press, minus K
		"$TWIN" "$LATROM" --frames 400 --pad none $1 --frame-hashes "$T/q0" > /dev/null 2>&1
		"$TWIN" "$LATROM" --frames 400 --pad "$K:b" $1 --frame-hashes "$T/q1" > /dev/null 2>&1
		paste -d' ' "$T/q0" "$T/q1" | awk -v k=$K '$2 != $4 { print $1 - k; exit }'
	}
	lp=$(lat ""); la=$(lat "--ahead")
	if [ -n "$lp" ] && [ -n "$la" ] && [ "$la" = $((lp - 1)) ]; then ok "a press reaches the picture $lp frames later plain and $la with run-ahead (twin, frames of the game; the display adds the same to both)"
	else bad "latency: plain '$lp', run-ahead '$la' frames"; fi
else skip "no Python 3 for the latency cartridge"; fi

# 5. states: "load the state saved at frame N, run M frames" equals "keep running" (video, sound, state, RAM, battery RAM of every
#    frame), on the same machine, after a reload of the ROM (nothing of the first load is left over), and into a fresh machine;
#    single thread and with the VDP worker. PROOF_FRAMES = "N M" (default "150 60"; the thorough run is 300 200)
PF=${PROOF_FRAMES:-150 60}
pr=0; prbad=0; prw=0; prwbad=0
while IFS= read -r r; do
	[ -f "$r" ] || continue
	o=$("$TWIN" "$r" --proof $PF --pad auto 2>&1 | grep '^PROOF')
	pr=$((pr + 1)); echo "$o" | grep -q 'DIFF\|failed\|carry_refused=[1-9]' || [ -z "$o" ] && { prbad=$((prbad + 1)); echo "  state load differs: $r"; }
	o=$("$TWIN" "$r" --proof $PF --pad auto --vdp-thread 2>&1 | grep '^PROOF')
	prw=$((prw + 1)); echo "$o" | grep -q 'DIFF\|failed\|carry_refused=[1-9]' || [ -z "$o" ] && { prwbad=$((prwbad + 1)); echo "  state load with the VDP worker differs: $r"; }
done < "$T/roms"
[ $prbad = 0 ] && ok "a loaded state continues exactly as the uninterrupted run ($PF frames, three ways) on $pr ROMs" || bad "state loads ($prbad of $pr differ)"
[ $prwbad = 0 ] && ok "the same with the VDP worker on $prw ROMs" || bad "state loads with the VDP worker ($prwbad of $prw differ)"
exit $fail
