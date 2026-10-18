#!/usr/bin/env bash
# run_matrix.sh [OUTDIR] [fixture names...] -- THE command: every multigame
# fixture x {loopback, 15 ms +-5 ms jitter, 3 % loss} as a two-PPSSPP GBA
# link-cable session, plus one cost arm per fixture (loopback, no per-frame
# hash), then the table (matrix_report.py).
#
#   bash tools/gbamirror/multigame/run_matrix.sh                 # all
#   bash tools/gbamirror/multigame/run_matrix.sh ~/gbamulti/m1 sa1 mt
#
# Fixtures: fixtures/<name>.fixture; the AW2 fixture runs FIRST as the
# known-good control, and if the control's loopback run fails the matrix
# stops: after ~40 launch/kill cycles PPSSPP stops initialising and every
# game "fails" (docs/GBA-LINK-MIRROR.md 16).  CONDS overrides the
# conditions (space-separated of: loop jitter loss cost).
#
# Xvfb displays :138 and :139 only (XVFB_DISPLAYS), one session at a time,
# each inside its own private namespace; only processes this script started
# are ever killed, by PID.  WSL/Linux.  Needs ~/gbamulti/{roms,fx,eb} as the
# fixtures describe (md5-checked by run_fixture_ppsspp.sh).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUTD="${1:-$HOME/gbamulti/matrix-$(date +%Y%m%d-%H%M)}"; shift || true
if [ $# -gt 0 ]; then FXS=("$@"); else
  FXS=(); for f in "$HERE"/fixtures/*.fixture; do FXS+=("$(basename "$f" .fixture)"); done
fi
# the control first, always
# (CONTROL=0 skips it: development runs only; CONTROL=end runs only the
# end-of-matrix control, for adding fixtures to a directory whose start
# control already passed)
if [ "${CONTROL:-1}" = 1 ]; then ORD=(aw2); else ORD=(); fi
for f in "${FXS[@]}"; do [ "$f" = aw2 ] || ORD+=("$f"); done
CONDS="${CONDS:-loop jitter loss cost}"
mkdir -p "$OUTD"
export XVFB_DISPLAYS="${XVFB_DISPLAYS:-138 139}"
args_of() {
  case "$1" in
    loop) echo "" ;;
    jitter) echo "--lat=15 --jit=5" ;;
    loss) echo "--loss=3" ;;
    cost) echo "--nohash" ;;
  esac
}
run_one() {  # fixture cond [output tag]
  local fx="$1" c="$2" o="$OUTD/${3:-$1-$2}" a
  a=$(args_of "$c")
  echo "== $(date +%T) $fx $c"
  unshare -rmnpf --mount-proc --propagation private bash -c \
    "mount -t tmpfs tmpfs /dev/shm && ip link set lo up && exec bash '$HERE/run_fixture_ppsspp.sh' '$o' '$HERE/fixtures/$fx.fixture' $a" \
    > "$o.out" 2>&1 < /dev/null
  grep -E '^RESULT|^VERDICT|^FAIL' "$o.out" | tail -4
}
for fx in "${ORD[@]}"; do
  [ -f "$HERE/fixtures/$fx.fixture" ] || { echo "no fixture $fx"; continue; }
  for c in $CONDS; do
    run_one "$fx" "$c"
    if [ "$fx" = aw2 ] && [ "$c" = loop ] && ! grep -q 'verdict=PASS' "$OUTD/aw2-loop.out"; then
      echo "CONTROL FAILED: the AW2 loopback session did not pass -- the PPSSPP rig"
      echo "is suspect (worn out after many launches?), not the fixtures. Stopping."
      python3 "$HERE/matrix_report.py" "$OUTD"
      exit 3
    fi
  done
done
# the control again at the end: a rig that wore out DURING the matrix shows
# here, not as a game failure
if [ "${CONTROL:-1}" != 0 ]; then
  run_one aw2 loop control_end
  grep -q 'verdict=PASS' "$OUTD/control_end.out" ||
    echo "END CONTROL FAILED: suspect the PPSSPP rig for any late failure"
fi
python3 "$HERE/matrix_report.py" "$OUTD" | tee "$OUTD/MATRIX.txt"
