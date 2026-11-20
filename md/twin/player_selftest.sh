#!/bin/sh
# Tests of the Sega player's own code (md/vita/player.cpp) on Linux with playerkit's software renderer: launch protocol, menu walk with
# screenshots, 10 state slots with thumbnails, Continue, battery RAM writes with the verified backup, RetroArch import, patches, cheats,
# region / PAL pacing, pad models, the look governor, smooth hand-over bit for bit, session.txt, patch CRC, rewind and fast forward, RetroAchievements
# against a fake server on the loopback (these use a loopback port: run the twin suites ALONE).
#   MD_TEST_ROMS=<dir>      tree with the SGDK sample ROMs (sample/**/out/release/rom.bin)
#   MD_TEST_SMS_ROMS=<dir>  tree with Master System homebrew (.sms); a Game Gear run forces the console id gg on one of them
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
	run "Mega Drive RetroAchievements (fake server: stored token, unlock toast, Hardcore)" ra md "$MD" "$T/a_md"
else echo "SKIP  Mega Drive player tests (MD_TEST_ROMS not set)"; fi
if [ -n "$SMS" ]; then
	run "Master System walk" test sms "$SMS" "$SHOTS/sms" "$T/w_sms"
	run "Master System pad model" pads sms "$SMS" "$T/p_sms"
	run "Master System look governor" look sms "$SMS" "$T/l_sms"
	run "Master System smooth hand-over on / off, bit for bit" handoff "$SMS" "$T/h_sms"
	run "Master System patch CRC, rewind 5 s, fast forward 2x / 3x" speed sms "$SMS" "$T/s_sms"
	run "Master System RetroAchievements" ra sms "$SMS" "$T/a_sms"
else echo "SKIP  Master System player tests (MD_TEST_SMS_ROMS not set)"; fi
if [ -n "$GG" ]; then
	run "Game Gear walk (an SMS ROM with the console id forced to gg)" test gg "$GG" "$SHOTS/gg" "$T/w_gg"
	run "Game Gear pad model" pads gg "$GG" "$T/p_gg"
	run "Game Gear look governor" look gg "$GG" "$T/l_gg"
	run "Game Gear patch CRC, rewind 5 s, fast forward 2x / 3x" speed gg "$GG" "$T/s_gg"
	run "Game Gear RetroAchievements" ra gg "$GG" "$T/a_gg"
else echo "SKIP  Game Gear player tests (MD_TEST_SMS_ROMS not set)"; fi
[ -n "$MD_PLAYER_SHOTS" ] && echo "screenshots: $(find "$SHOTS" -name '*.png' | wc -l) in $SHOTS"
exit $fail
