#!/usr/bin/env bash
# fast_stress.sh [OUTDIR] -- stress for the cable fast path (gbalink_fast,
# docs/GBA-LINK-MIRROR.md 21-22) beyond what the hash-identity run covers,
# on the desktop mirror (gl_host):
#
#   for every fixture and every seed, three runs with the SAME seed:
#     off    gbalink_fast = 0
#     fast   gbalink_fast = 1
#     chk    gbalink_fast = 2 (the full path decides; the fast path's
#            answers are compared with it on every scheduler call)
#     lean   gbalink_fast = 3 (the fast path plus the inline hooks)
#   under gl_host --stress SEED: a random run horizon inside the stepping
#   rule's limits for every run, and random extra pumps of either machine
#   (the seed is deterministic, so the three runs see the same schedule).
#   PASS = the three per-frame hash files are byte-identical, the cable
#   counters equal, chk reports mismatch=0/0, and no run ends in a pair
#   error.  Seeds with bit 31 set stress the pumps only (horizons as the
#   rule makes them): those must ALSO hash like the unstressed run.
#
#   bash tools/gbamirror/multigame/fast_stress.sh ~/linkspeed/stress
#
# Environment: SEEDS (default "1 2 3 4 5 6 0x80000007 0x80000008"), FXS
# (fixture names, default all), JOBS (default 12), EXTRA_FX as
# fast_identity.sh ("name:rom:state:host:join:frames", state "-" = power-on),
# GL, BIOS, MG_ROMS, MG_FX.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HOME/linkspeed/stress}"
GL="${GL:-$HERE/../gl_host}"; BIOS="${BIOS:-$HOME/gbalink/bios}"
R="${MG_ROMS:-$HOME/gbamulti/roms}"; X="${MG_FX:-$HOME/gbamulti/fx}"
SEEDS="${SEEDS:-1 2 3 4 5 6 0x80000007 0x80000008}"
mkdir -p "$OUT"; : > "$OUT/jobs.txt"
job() {  # name rom state0 state1 save0 save1 host join frames
  local n=$1 s m
  for s in 0 $SEEDS; do for m in off fast chk lean; do
    echo "$n $s $m $2 $3 $4 $5 $6 $7 $8 $9" >> "$OUT/jobs.txt"
  done; done
}
if [ -n "${FXS:-}" ]; then read -r -a FX <<< "$FXS"; else
  FX=(); for f in "$HERE"/fixtures/*.fixture; do FX+=("$(basename "$f" .fixture)"); done
fi
for fx in "${FX[@]}"; do
  [ "$fx" = sf ] && continue      # its ROM is not in MG_ROMS by default: EXTRA_FX
  HOST_SAVE=-; JOIN_SAVE=-
  # shellcheck disable=SC1090
  . "$HERE/fixtures/$fx.fixture"
  hs=-; js=-
  [ "$HOST_SAVE" != - ] && hs="$X/$HOST_SAVE"; [ "$JOIN_SAVE" != - ] && js="$X/$JOIN_SAVE"
  job "$fx" "$R/$ROM" "$X/$HOST_STATE" "$X/$JOIN_STATE" "$hs" "$js" \
      "$HERE/scripts/$HOST_SCRIPT" "$HERE/scripts/$JOIN_SCRIPT" "$FRAMES"
done
for e in ${EXTRA_FX:-}; do
  IFS=: read -r n rom st h j fr <<< "$e"
  job "$n" "$rom" "$st" "$st" - - "$h" "$j" "$fr"
done
one() {
  read -r n s m rom st0 st1 sv0 sv1 h j fr <<< "$1"
  local t="$OUT/$n.$s.$m" a
  a=(--rom "$rom" --bios-dir "$BIOS" --ap0 "$h" --ap1 "$j" --frames "$fr"
     --hash "$t.hash" --evt "$t.evt")
  [ "$st0" != - ] && a+=(--state0 "$st0"); [ "$st1" != - ] && a+=(--state1 "$st1")
  [ "$sv0" != - ] && { cp "$sv0" "$t.s0"; a+=(--save0 "$t.s0"); }
  [ "$sv1" != - ] && { cp "$sv1" "$t.s1"; a+=(--save1 "$t.s1"); }
  [ "$s" != 0 ] && a+=(--stress "$s")
  [ "$m" = fast ] && a+=(--fast); [ "$m" = chk ] && a+=(--fast2); [ "$m" = lean ] && a+=(--fast3)
  "$GL" "${a[@]}" > "$t.out" 2>&1; echo "rc=$?" >> "$t.out"
  rm -f "$t.s0" "$t.s1" "$t.evt"
}
export -f one; export OUT GL BIOS
xargs -P "${JOBS:-12}" -I{} bash -c 'one "$@"' _ {} < "$OUT/jobs.txt"
rc=0
cnt() { grep -aE '^(slot|bus|guard|sio)' "$1"; }
for n in $(cut -d' ' -f1 "$OUT/jobs.txt" | sort -u); do
  for s in 0 $SEEDS; do
    t="$OUT/$n.$s"; v=PASS; why=""
    for m in off fast chk lean; do
      grep -q '^rc=0' "$t.$m.out" || { v=FAIL; why="$why $m:$(grep -a 'pair error\|unexpected\|^rc=' "$t.$m.out" | tr '\n' ' ')"; }
    done
    cmp -s "$t.off.hash" "$t.fast.hash" || { v=FAIL; why="$why hash(off!=fast)"; }
    cmp -s "$t.off.hash" "$t.chk.hash" || { v=FAIL; why="$why hash(off!=chk)"; }
    cmp -s "$t.off.hash" "$t.lean.hash" || { v=FAIL; why="$why hash(off!=lean)"; }
    cmp -s <(cnt "$t.off.out") <(cnt "$t.lean.out") || { v=FAIL; why="$why counters(lean)"; }
    cmp -s <(cnt "$t.off.out") <(cnt "$t.fast.out") || { v=FAIL; why="$why counters"; }
    grep -aq 'fastchk: checks=[1-9][0-9]*/[1-9][0-9]* mismatch=0/0' "$t.chk.out" || { v=FAIL; why="$why $(grep -a fastchk "$t.chk.out" | tr '\n' ' ')"; }
    # pumps only: the machines of the unstressed run
    case "$s" in 0x8*) cmp -s "$t.off.hash" "$OUT/$n.0.off.hash" || { v=FAIL; why="$why pump-only-changed-the-machines"; } ;; esac
    [ $v = PASS ] || rc=1
    echo "$v $n seed=$s lines=$(wc -l < "$t.off.hash") $(grep -a 'fastchk: checks' "$t.chk.out") $(grep -a '^bus' "$t.off.out" | sed 's/.*switches=/switches=/')$why"
    [ $v = PASS ] && rm -f "$t.fast.hash" "$t.chk.hash" "$t.lean.hash"
  done
done
rm -f "$OUT"/*.hash
exit $rc
