#!/usr/bin/env bash
# dev_pair.sh -- the fast loop for writing a multigame fixture: the desktop
# mirror (gl_host, x86 dynarec) runs both machines with the REAL autopilot
# engine on each (--ap0/--ap1, RAM predicates read each slot's own machine),
# dumping both screens.  x86 and the PSP's MIPS dynarec are different
# engines (GBA-LINK-FEASIBILITY: AW2 diverges at f3132), so a fixture is
# only DONE when it passes in two PPSSPP consoles (run_matrix.sh); this
# loop is for finding the menus.
#
#   dev_pair.sh OUT ROM HOST_SCRIPT JOIN_SCRIPT FRAMES [gl_host args...]
#
# Environment: SAVE0/SAVE1 battery images, STATE0/STATE1 states (default:
# none -- power-on, the fixture's state is made by mkstate.sh the same way),
# DUMP_EVERY (default 120), GL (gl_host path).
set -uo pipefail
OUT="$1"; ROM="$2"; H="$3"; J="$4"; FR="$5"; shift 5
GL="${GL:-$HOME/gbamulti/src/tools/gbamirror/gl_host}"
BIOS="${BIOS:-$HOME/gbalink/bios}"
rm -rf "$OUT"; mkdir -p "$OUT/d"
A=(--rom "$ROM" --bios-dir "$BIOS" --ap0 "$H" --ap1 "$J" --frames "$FR"
   --hash "$OUT/hash.txt" --evt "$OUT/evt.txt" --dump-dir "$OUT/d"
   --dump-every "${DUMP_EVERY:-120}")
[ -n "${SAVE0:-}" ] && A+=(--save0 "$SAVE0")
[ -n "${SAVE1:-}" ] && A+=(--save1 "$SAVE1")
[ -n "${STATE0:-}" ] && A+=(--state0 "$STATE0")
[ -n "${STATE1:-}" ] && A+=(--state1 "$STATE1")
"$GL" "${A[@]}" "$@" > "$OUT/out.txt" 2>&1
echo "exit=$?"
tail -12 "$OUT/out.txt"
grep -v '^EVT heartbeat' "$OUT/evt.txt" 2>/dev/null | tail -25
