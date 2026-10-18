#!/usr/bin/env bash
# rewind_player.sh ARM GAME SCRIPT FRAMES
#
# The rewind in the state a PLAYER's build is in at every GBA game load: a
# link-cable room can be hosted (gba_cable set, instance B loaded late or at
# boot), no session is running.  The harness never used to be in that state,
# and fe_host_rewind_blocked() refused the rewind in it for every game
# (2026-10-10): it tested "can host a session" where it meant "a session is
# running".  ARM is a harness build with GBA_LINK=1 and REWIND=1.
#
# Two runs, B loaded late (the player's build) and B loaded at boot, each
# with the scripted rewinds of SCRIPT replayed and checked.  PASS needs, in
# both: the cable to be available, the rewind never denied, frames actually
# shown backwards, and tools/rewind_check.py clean.
set -uo pipefail
ARM=${1:?arm}; GAME=${2:?game}; SCRIPT=${3:?script}; FRAMES=${4:?frames}
T=$(cd "$(dirname "$0")" && pwd)
W=$HOME/rewind-agent
rc=0
for late in 1 0; do
  tag=player_late$late
  FF=1 bash "$T/rewind_ppsspp.sh" $tag "$ARM" "$GAME" "$SCRIPT" "$FRAMES" \
    "rewind = 1" "rewind_product = 1" "rewind_verify = 1" "rewind_hash = 1" \
    "rewind_redo = 1" "link_avail = 1" "gba_dual_late = $late" >/dev/null
  L=$W/pp2/$tag/frontend.log
  ok=1
  grep -q "gba_cable ready" "$L" || { echo "$tag: the cable was not made available (not the player's state)"; ok=0; }
  grep -q "rewind_denied" "$L" && { echo "$tag: $(grep -m1 rewind_denied "$L")"; ok=0; }
  shown=$(grep -o "rewind_use engages=[0-9]* show=[0-9]*" "$L" | grep -o "[0-9]*$")
  [ "${shown:-0}" -gt 0 ] || { echo "$tag: nothing was shown backwards"; ok=0; }
  python3 "$T/rewind_check.py" "$W/pp2/$tag/rewind_hash.txt" --min-redo 300 --max-step 12 | tail -n 2
  [ "${PIPESTATUS[0]}" = 0 ] || ok=0
  grep -h "gba_cable ready\|rewind_tick" "$L" | cut -c1-200
  [ $ok = 1 ] && echo "$tag: PASS (shown=$shown)" || { echo "$tag: FAIL"; rc=1; }
done
exit $rc
