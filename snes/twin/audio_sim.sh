#!/bin/sh
# Audio pacing simulation (virtual time, no audio device): scenarios S1..S7 of frontend/audio_pace.cpp.
#   audio_sim.sh              run the new pacer; exit 1 if a scenario fails; also prints the legacy table
#   audio_sim.sh --md         print the results of both modes as markdown tables (for baseline_results.md)
# Source of the sound: a synthetic tone (always available). With a ROM (SNES_ROMS, or AUDIO_SIM_ROM=file)
# S1 also runs once with the core's own audio.
#   TWIN=build/snes_twin
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
TWIN=${TWIN:-$HERE/build/snes_twin}
ROOT=${SNES_ROMS:-/w/snes-testroms/SNES}
ROM=${AUDIO_SIM_ROM:-$ROOT/HelloWorld/HelloWorld.sfc}
T=$(mktemp -d)
rc=0

"$TWIN" - --audio-sim SD --tone > $T/sd.txt || rc=1
cat $T/sd.txt | sed 's/^SIM SD /detector: /'
"$TWIN" - --audio-sim all --tone > $T/new.txt || rc=1
"$TWIN" - --audio-sim all --tone --legacy > $T/legacy.txt   # fails by design: recorded, not enforced

table() {  # file
	echo "| Scenario | Result | Underruns | Clicks | Fill ms min/avg/max | Ratio ppm min/avg/max | Trims | Detail |"
	echo "|---|---|---:|---:|---|---|---:|---|"
	awk '{
		sc=$2; res=$6; sub("result=","",res)
		for (i=1;i<=NF;i++) { if ($i ~ /^underruns=/) u=substr($i,11); if ($i ~ /^clicks=/) c=substr($i,8);
			if ($i ~ /^fill_ms=/) f=substr($i,9); if ($i ~ /^ratio_ppm=/) r=substr($i,11); if ($i ~ /^trims=/) t=substr($i,7) }
		d=$0; sub(/^[^|]*\| ?/,"",d); gsub(/\|/,"/",d)
		printf "| %s | %s | %s | %s | %s | %s | %s | %s |\n", sc, res, u, c, f, r, t, d }' "$1"
}

if [ "$1" = "--md" ]; then
	echo "#### New pacer"; echo; table $T/new.txt; echo
	echo "#### Legacy (first player's audio)"; echo; table $T/legacy.txt
else
	cat $T/new.txt
	echo "(legacy mode, for comparison; expected to fail several:)"; cat $T/legacy.txt | cut -c1-150
fi

if [ -f "$ROM" ]; then
	"$TWIN" "$ROM" --audio-sim S1 > $T/core.txt 2>/dev/null || rc=1
	cat $T/core.txt
else
	echo "note: no SNES ROM at $ROM, the core-audio run of S1 is skipped (tone runs above are the test)"
fi

grep -q "result=FAIL" $T/new.txt && rc=1
grep -q "result=FAIL" $T/sd.txt && rc=1
rm -rf $T
exit $rc
