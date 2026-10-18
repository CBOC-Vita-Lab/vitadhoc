#!/usr/bin/env bash
# smoke.sh <model 1000|3000> <eboot dir> <out dir> <display>: PPSSPP smoke of the
# hardware arms (S1 cut to CYC cycles, U in full, the four fresh boots), one
# after another on one private-namespace Xvfb display.
M="$1"; ED="$2"; export OUT="$3"; export DISP="$4"
HERE="$(cd "$(dirname "$0")" && pwd)"
ST="$HERE/../../../../inproc-soak-out/stage/$M"
mkdir -p "$OUT"
r() { tag=$1; shift; env "$@" ARMDIR="$ST/$tag" S1DIR="$ST/S1" bash "$HERE/run_disp.sh" "$tag" "$ED" "$HERE/setup_arm.sh" "${TO:-1500}" >/dev/null 2>&1
      echo "$tag: $(grep -m1 '^EVT exit' "$OUT/$tag/frontend.log" 2>/dev/null) returns=$(grep -c '^EVT inproc_return' "$OUT/$tag/frontend.log" 2>/dev/null) refused=$(grep -c '^EVT inproc_refused' "$OUT/$tag/frontend.log" 2>/dev/null)"; }
r U
r S1 CYC=${CYC:-10}
r Fr
r Fe
r Fc
r Fh
echo SMOKEDONE
