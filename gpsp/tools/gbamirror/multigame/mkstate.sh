#!/usr/bin/env bash
# mkstate.sh ROM SAVE OUT.st0 -- a fixture's starting state: the desktop
# core powered on with the battery image in, run --load-at (30) frames, and
# serialized (retro_serialize = the PSP .st0 format).  This is exactly the
# machine gl_host's dev loop starts its pair from, so a script written in
# dev_pair.sh starts from the same place in the PSP session (the harness
# loads the state at frame 30 and holds it, autopilot included, until the
# link forms).  SAVE may be "-" (no battery: a fresh cartridge).
# Deterministic: rerunning it gives the same bytes except the RTC base the
# core stamps at load (rtc_now, the wall clock; a real date, so the session
# never re-anchors it) -- the fixture records the md5 of the copy in use.
set -euo pipefail
ROM="$1"; SAVE="$2"; OUT="$3"
GL="${GL:-$HOME/gbamulti/src/tools/gbamirror/gl_host}"
BIOS="${BIOS:-$HOME/gbalink/bios}"
A=(--rom "$ROM" --bios-dir "$BIOS" --frames 1 --save-state "$OUT"
   --save-state-at 0)
[ "$SAVE" != "-" ] && A+=(--save0 "$SAVE" --save1 "$SAVE")
"$GL" "${A[@]}" | grep -E '^saved state'
md5sum "$OUT"
