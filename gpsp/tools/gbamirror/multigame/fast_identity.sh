#!/usr/bin/env bash
# fast_identity.sh [OUTDIR] [fixture names...] -- the cable fast path
# (gbalink_core.c gbalink_fast, docs/GBA-LINK-MIRROR.md 21) must not change a
# machine: every multigame fixture on the desktop mirror (gl_host), once as
# built and once with --fast, and the two per-frame hash files (both
# machines, every frame) must be byte-identical, as must the cable counters.
#
#   bash tools/gbamirror/multigame/fast_identity.sh ~/linkspeed/ident
#
# Needs ~/gbamulti/{roms,fx} as the fixtures describe and a built gl_host
# (GL, default beside this script).  Seconds per fixture.  EXTRA_FX: more
# "name:rom:state:host_script:join_script:frames" entries (state "-" = power-on).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HOME/linkspeed/ident}"; shift || true
GL="${GL:-$HERE/../gl_host}"; BIOS="${BIOS:-$HOME/gbalink/bios}"
R="${MG_ROMS:-$HOME/gbamulti/roms}"; X="${MG_FX:-$HOME/gbamulti/fx}"
mkdir -p "$OUT"; rc=0
one() {  # name rom state0 state1 save0 save1 host join frames
  local n=$1 m a
  for m in off fast; do
    a=(--rom "$2" --bios-dir "$BIOS" --ap0 "$7" --ap1 "$8" --frames "$9"
       --hash "$OUT/$n.$m.hash" --evt "$OUT/$n.$m.evt")
    [ "$3" != - ] && a+=(--state0 "$3"); [ "$4" != - ] && a+=(--state1 "$4")
    [ "$5" != - ] && { cp "$5" "$OUT/$n.$m.s0"; a+=(--save0 "$OUT/$n.$m.s0"); }
    [ "$6" != - ] && { cp "$6" "$OUT/$n.$m.s1"; a+=(--save1 "$OUT/$n.$m.s1"); }
    [ $m = fast ] && a+=(--fast)
    "$GL" "${a[@]}" > "$OUT/$n.$m.out" 2>&1
  done
  local l; l=$(wc -l < "$OUT/$n.off.hash")
  if cmp -s "$OUT/$n.off.hash" "$OUT/$n.fast.hash" &&
     cmp -s <(grep -aE '^(slot|bus|guard|sio)' "$OUT/$n.off.out") \
            <(grep -aE '^(slot|bus|guard|sio)' "$OUT/$n.fast.out"); then
    echo "IDENTICAL $n lines=$l $(grep -a '^bus' "$OUT/$n.off.out")"
  else echo "DIFFERENT $n lines=$l"; rc=1; fi
  rm -f "$OUT/$n".*.s0 "$OUT/$n".*.s1
}
if [ $# -gt 0 ]; then FXS=("$@"); else
  FXS=(); for f in "$HERE"/fixtures/*.fixture; do FXS+=("$(basename "$f" .fixture)"); done
fi
for fx in "${FXS[@]}"; do
  HOST_SAVE=-; JOIN_SAVE=-
  # shellcheck disable=SC1090
  . "$HERE/fixtures/$fx.fixture"
  hs=-; js=-
  [ "$HOST_SAVE" != - ] && hs="$X/$HOST_SAVE"; [ "$JOIN_SAVE" != - ] && js="$X/$JOIN_SAVE"
  one "$fx" "$R/$ROM" "$X/$HOST_STATE" "$X/$JOIN_STATE" "$hs" "$js" \
      "$HERE/scripts/$HOST_SCRIPT" "$HERE/scripts/$JOIN_SCRIPT" "$FRAMES"
done
for e in ${EXTRA_FX:-}; do
  IFS=: read -r n rom st h j fr <<< "$e"
  one "$n" "$rom" "$st" "$st" - - "$h" "$j" "$fr"
done
exit $rc
