#!/bin/bash
# The picture Quick response shows is the next frame's: with no button pressed, the middle of the screenshot at refresh N with it on equals the one
# at refresh N+1 with it off (and the one at N+1 on equals N+2 off).  Prints PASS or FAIL per ROM (and "static" when the ROM's picture does not
# change over those refreshes, which proves nothing).  Needs the twin's --shot.
#   ahead_picture.sh build/x86/gbadhoc_host ROMLIST [AT]      AT: first refresh (default 150);  AHEAD_CONFIG as in ahead_check.sh
# Copyright CBOC-Vita. MIT licence for this script.
TWIN=$1; L=$2; AT=${3:-150}
[ -x "$TWIN" ] && [ -f "$L" ] || { echo "usage: $0 gbadhoc_host ROMLIST [AT]"; exit 2; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fail=0
while read -r rom; do
  [ -f "$rom" ] || continue
  ext="${rom##*.}"
  for m in 0 2; do
    root=$W/r$m; rm -rf $root; mkdir -p $root/roms $root/shots; cp "$rom" $root/roms/rom.$ext; echo rom.$ext > $root/autoboot.txt
    [ -n "$AHEAD_CONFIG" ] && printf '%s\n' "$AHEAD_CONFIG" > $root/config.ini
    shots=""; for f in $(seq $AT $((AT + 3))); do shots="$shots --shot $f:s$f.png"; done
    GBADHOC_AHEAD=$m nice "$TWIN" --root $root --frames $((AT + 5)) --shot-dir $root/shots $shots > $root/out.txt 2>&1
  done
  # the middle of the screenshot only: the ambient glow beside the picture blends over time, so it differs by a frame's history
  res=$(python3 - $W/r0/shots $W/r2/shots $AT <<'PY'
import sys
from PIL import Image
d0, d2, at = sys.argv[1], sys.argv[2], int(sys.argv[3])
def mid(d, f):
    try: return Image.open("%s/s%d.png" % (d, f)).convert("RGB").crop((300, 150, 660, 400)).tobytes()
    except OSError: return None
ok, moving = True, False
for k in range(3):
    a, b = mid(d2, at + k), mid(d0, at + k + 1)
    ok = ok and a is not None and a == b
    moving = moving or mid(d0, at + k) != b
print("ok" if ok else "bad", "moving" if moving else "static")
PY
)
  case "$res" in
    "ok moving") echo "PASS $(basename "$rom")" ;;
    "ok static") echo "PASS $(basename "$rom") (static picture: proves nothing)" ;;
    *) echo "FAIL $(basename "$rom")"; fail=1 ;;
  esac
done < "$L"
[ $fail = 0 ]
