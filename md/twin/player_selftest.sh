#!/bin/sh
# Tests of the Sega player's own code (md/vita/player.cpp) on Linux with playerkit's software renderer: launch protocol, menu walk with
# screenshots, 10 state slots with thumbnails, Continue, battery RAM writes with the verified backup, RetroArch import, patches, cheats,
# region / PAL pacing, pad models, the look governor, smooth hand-over bit for bit, session.txt, patch CRC, rewind and fast forward, RetroAchievements
# against a fake server on the loopback (these use a loopback port: run the twin suites ALONE).
#   MD_TEST_ROMS=<dir>      tree with the SGDK sample ROMs (sample/**/out/release/rom.bin)
#   MD_TEST_SMS_ROMS=<dir>  tree with Master System homebrew (.sms); a Game Gear run forces the console id gg on one of them
#   GATE_SG_ROM=<file>      an SG-1000 game (public homebrew); else a .sg in MD_TEST_SMS_ROMS; else the cartridge tools/sg_gen.py makes (no ROM needed)
#   GATE_GG_ROM=<file>      a Game Gear game (public homebrew); else a .gg in MD_TEST_SMS_ROMS; else the cartridge tools/sg_gen.py --gg makes: a .gg
#                           file with the Game Gear header, so the extension, not a forced console id, picks the console
#   MD_PLAYER_SHOTS=<dir>   where the screenshots go (default: a temporary folder)
# No ROM is shipped with this repository. Lines start with PASS / FAIL / SKIP.
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
PT=${MD_PLAYER_TWIN:-$HERE/build/x86/player_twin}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
SHOTS=${MD_PLAYER_SHOTS:-$T/shots}
fail=0
[ -x "$PT" ] || make -C "$HERE" -j2 ARCH=x86 >"$T/build.log" 2>&1
[ -x "$PT" ] || { tail -5 "$T/build.log"; echo "FAIL  player_twin build"; exit 1; }
# run NAME ARGS...: PASS when the last "checks, 0 failed" line says so
run() {
	name=$1; shift
	"$PT" "$@" >"$T/out.txt" 2>&1; rc=$?
	line=$(grep -E "checks, [0-9]+ failed" "$T/out.txt" | tail -1)
	n=$(echo "$line" | sed -n 's/.* \([0-9]*\) checks.*/\1/p')
	if [ $rc = 0 ] && echo "$line" | grep -q ", 0 failed"; then echo "PASS  $name ($n checks)"; else echo "FAIL  $name ($line)"; grep FAIL "$T/out.txt" | head -5; fail=1; fi
}
run "picture ceilings, layout and the Game Gear LCD table" ceilings
MD=""; [ -n "$MD_TEST_ROMS" ] && MD=$(find "$MD_TEST_ROMS" -path '*/basics/hello-world/*' -name rom.bin | head -1)
JT=""; [ -n "$MD_TEST_ROMS" ] && JT=$(find "$MD_TEST_ROMS" -path '*/joy-test/*' -name rom.bin | head -1)
SMS=""; GG=""
if [ -n "$MD_TEST_SMS_ROMS" ]; then
	SMS=$(find "$MD_TEST_SMS_ROMS" -name picross.sms | head -1)
	GG=$(find "$MD_TEST_SMS_ROMS" -name baythirddungeon.sms | head -1)
fi
if [ -n "$MD" ]; then
	run "Mega Drive walk (menu, 10 slots, Continue, battery RAM, patch, cheats, session.txt)" test md "$MD" "$SHOTS/md" "$T/w_md"
	run "Mega Drive pad model and the six button pad" pads md "${JT:-$MD}" "$T/p_md"
	run "Mega Drive look governor (scaler tiers, memory, rows)" look md "$MD" "$T/l_md"
	run "Mega Drive region and PAL pacing" pal "$MD"
	run "Mega Drive smooth hand-over on / off, bit for bit" handoff "$MD" "$T/h_md"
	run "Mega Drive patch CRC, rewind 5 s (bit for bit), fast forward 2x / 3x" speed md "$MD" "$T/s_md"
	run "Mega Drive Quick response (run-ahead): Off, On, Auto: the same machine after every refresh" ahead md "$MD" "$T/q_md"
	run "Mega Drive RetroAchievements (fake server: stored token, unlock toast, Hardcore)" ra md "$MD" "$T/a_md"
else echo "SKIP  Mega Drive player tests (MD_TEST_ROMS not set)"; fi
if [ -n "$SMS" ]; then
	run "Master System walk" test sms "$SMS" "$SHOTS/sms" "$T/w_sms"
	run "Master System pad model" pads sms "$SMS" "$T/p_sms"
	run "Master System look governor" look sms "$SMS" "$T/l_sms"
	run "Master System smooth hand-over on / off, bit for bit" handoff "$SMS" "$T/h_sms"
	run "Master System patch CRC, rewind 5 s, fast forward 2x / 3x" speed sms "$SMS" "$T/s_sms"
	run "Master System Quick response (run-ahead)" ahead sms "$SMS" "$T/q_sms"
	run "Master System RetroAchievements" ra sms "$SMS" "$T/a_sms"
else echo "SKIP  Master System player tests (MD_TEST_SMS_ROMS not set)"; fi
if [ -n "$GG" ]; then
	run "Game Gear walk (an SMS ROM with the console id forced to gg)" test gg "$GG" "$SHOTS/gg" "$T/w_gg"
	run "Game Gear pad model" pads gg "$GG" "$T/p_gg"
	run "Game Gear look governor" look gg "$GG" "$T/l_gg"
	run "Game Gear patch CRC, rewind 5 s, fast forward 2x / 3x" speed gg "$GG" "$T/s_gg"
	run "Game Gear Quick response (run-ahead)" ahead gg "$GG" "$T/q_gg"
	run "Game Gear RetroAchievements" ra gg "$GG" "$T/a_gg"
else echo "SKIP  Game Gear (forced id) player tests (MD_TEST_SMS_ROMS not set)"; fi
# SG-1000 and Game Gear as their own files (.sg, .gg): a public homebrew when given, else the cartridges made by tools/sg_gen.py
SGF=${GATE_SG_ROM:-}; [ -z "$SGF" ] && [ -n "$MD_TEST_SMS_ROMS" ] && SGF=$(find "$MD_TEST_SMS_ROMS" -name '*.sg' | head -1)
GGF=${GATE_GG_ROM:-}; [ -z "$GGF" ] && [ -n "$MD_TEST_SMS_ROMS" ] && GGF=$(find "$MD_TEST_SMS_ROMS" -name '*.gg' | head -1)
SGSRC="a given ROM"; GGSRC="a given ROM"
if command -v python3 >/dev/null; then
	[ -z "$SGF" ] && python3 -I "$HERE/tools/sg_gen.py" "$T/gen.sg" >/dev/null 2>&1 && SGF="$T/gen.sg" && SGSRC="the generated cartridge"
	[ -z "$GGF" ] && python3 -I "$HERE/tools/sg_gen.py" "$T/gen.gg" --gg >/dev/null 2>&1 && GGF="$T/gen.gg" && GGSRC="the generated cartridge"
fi
if [ -n "$SGF" ]; then
	run "SG-1000 walk ($SGSRC)" test sg "$SGF" "$SHOTS/sg" "$T/w_sg"
	run "SG-1000 pad model" pads sg "$SGF" "$T/p_sg"
	run "SG-1000 look governor" look sg "$SGF" "$T/l_sg"
	run "SG-1000 smooth hand-over on / off, bit for bit" handoff "$SGF" "$T/h_sg"
	run "SG-1000 patch CRC, rewind 5 s, fast forward 2x / 3x" speed sg "$SGF" "$T/s_sg"
	run "SG-1000 Quick response (run-ahead)" ahead sg "$SGF" "$T/q_sg"
	run "SG-1000 RetroAchievements" ra sg "$SGF" "$T/a_sg"
else echo "SKIP  SG-1000 player tests (no ROM and no python3 for the generator)"; fi
if [ -n "$GGF" ]; then
	run "Game Gear walk, a real .gg ($GGSRC)" test gg "$GGF" "$SHOTS/gg_file" "$T/w_ggf"
	run "Game Gear pad model, a real .gg" pads gg "$GGF" "$T/p_ggf"
	run "Game Gear look governor, a real .gg" look gg "$GGF" "$T/l_ggf"
	run "Game Gear smooth hand-over, a real .gg" handoff "$GGF" "$T/h_ggf"
	run "Game Gear patch CRC, rewind 5 s, fast forward 2x / 3x, a real .gg" speed gg "$GGF" "$T/s_ggf"
	run "Game Gear Quick response, a real .gg" ahead gg "$GGF" "$T/q_ggf"
	run "Game Gear RetroAchievements, a real .gg" ra gg "$GGF" "$T/a_ggf"
else echo "SKIP  Game Gear .gg player tests (no ROM and no python3 for the generator)"; fi
# Sega CD: the same player on a synthetic disc and the test-only stand-in BIOS (tools/mkscd.py; no ROM, no BIOS, no disc of anyone's is used), so these
# run without MD_TEST_ROMS. The disc has no patch, no RetroArch import and no thumbnail of a cartridge: the walk expects that.
if command -v python3 >/dev/null; then
	SC="$T/scd"; mkdir -p "$SC"
	if python3 -I "$HERE/tools/mkscd.py" standin "$SC/standin.bin" >/dev/null 2>&1 && python3 -I "$HERE/tools/mkscd.py" discs "$SC" >/dev/null 2>&1; then
		SD="$SC/single.cue"
		run "Sega CD walk (menu, 10 slots, Continue, backup RAM and cartridge in one save, cheats, session.txt)" test scd "$SD" "$SHOTS/scd" "$T/w_scd"
		run "Sega CD pad model" pads scd "$SD" "$T/p_scd"
		run "Sega CD look governor" look scd "$SD" "$T/l_scd"
		run "Sega CD rewind 5 s (bit for bit, one capture in four frames), fast forward 2x / 3x" speed scd "$SD" "$T/s_scd"
		run "Sega CD Quick response (run-ahead)" ahead scd "$SD" "$T/q_scd"
		run "Sega CD RetroAchievements (fake server)" ra scd "$SD" "$T/a_scd"
		run "Sega CD costs (state size, rewind ring, capture time)" costs scd "$SD"
	else echo "FAIL  Sega CD player tests (mkscd.py could not write the fixtures)"; fail=1; fi
else echo "SKIP  Sega CD player tests (python3 not found)"; fi
# the wide view through the picture pipe (direct drawing and copies, composed on a second thread) against composing at once, bit for bit
WP=${MD_WIDE_PIPE_PROOF:-$HERE/build/x86/wide_pipe_proof}
[ -x "$WP" ] || make -C "$HERE" -j2 ARCH=x86 build/x86/wide_pipe_proof >"$T/build2.log" 2>&1
wpone() {
	name=$1; shift
	out=$("$WP" "$@" --frames 400 2>&1 | tail -1)
	case "$out" in *"mismatches=0 "*"empty=0") echo "PASS  $name ($out)";; *) echo "FAIL  $name ($out)"; fail=1;; esac
}
if [ -x "$WP" ]; then
	if [ -n "$MD" ]; then
		MDG=$(find "$MD_TEST_ROMS" -path '*/game/platformer/*' -name rom.bin | head -1)
		[ -n "$MDG" ] && wpone "Mega Drive wide view through the picture pipe" "$MDG"
	fi
	[ -n "$SMS" ] && wpone "Master System wide view through the picture pipe" "$SMS" --margin 41
	[ -n "$GG" ] && wpone "Game Gear wide band through the picture pipe (an SMS ROM forced to gg)" "$GG" --system gg
else echo "SKIP  wide view through the picture pipe (wide_pipe_proof not built)"; fi
[ -n "$MD_PLAYER_SHOTS" ] && echo "screenshots: $(find "$SHOTS" -name '*.png' | wc -l) in $SHOTS"
exit $fail
