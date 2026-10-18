#!/usr/bin/env bash
# det_matrix.sh BIN ROMDIR FIXDIR REFDIR OUT -- the link session's
# determinism under bad radios and stall absorption (docs/GB-LINK.md 14.6).
#
#   BIN     tools/gblink/build.sh output (sesssim)
#   ROMDIR  red.gb (the cartridge FIXDIR was made on) and crystal.gbc
#   FIXDIR  make_fixtures.sh output (gen1-*, gen2-*.sav, gen2-*.st)
#   REFDIR  make_references.sh output from the same fixtures
#
# Every fixture (Gen 1 trade, Gen 2 trade, Gen 1 battle, Gen 2 live link) x
# five simulated radios (clean; jitter 6 and 20 frames; 3 % and 5 % of the
# ordered channel resent late, holding everything behind it, the latter also
# losing 5 % of bulk-lane datagrams) x the smoothness policies off/on (stall
# absorption + resend requests) = 40 two-console sessions.  Each must end
# DONE on both
# consoles with equal final hashes (sesssim exit 0) and cartridge RAM equal
# to the one-process reference (the first 32 KiB: Crystal's clock trailer is
# stamped at the end frame, which a wall-clock end request moves).  Then
# every periodic sync hash of a fixture must be the same at the same frame
# in all ten of its runs, and the live link -- whose end comes from the
# script, not the clock -- must end on one frame:hash.  Exit 0 = all hold.
set -uo pipefail
B="$1"; ROMS="$2"; FIX="$3"; REF="$4"; OUT="$5"
S="$(cd "$(dirname "$0")" && pwd)/scripts"
rm -rf "$OUT"; mkdir -p "$OUT"
pass=0; fail=0
run() {  # name rom fixture script reference extra radio absorb
  local name=$1 rom=$2 fx=$3 ap=$4 ref=$5 extra=$6 radio=$7 ab=$8 tag rc fin
  tag="$name-$(echo "$radio" | tr ' =' '_-')-ab$ab"
  # shellcheck disable=SC2086
  "$B/sesssim" rom_h="$ROMS/$rom" rom_g="$ROMS/$rom" \
    sav_h="$FIX/$fx-a.sav" sav_g="$FIX/$fx-b.sav" \
    ap_h="$S/${ap}_host.ap" ap_g="$S/${ap}_guest.ap" delay=4 rtc=1790500000 \
    max=60000 bulk=1037 batch=154 $extra $radio absorb="$ab" nack="$ab" \
    hashlog="$OUT/$tag.hashes" out_h="$OUT/$tag-a.sav" out_g="$OUT/$tag-b.sav" \
    > "$OUT/$tag.log" 2>&1
  rc=$?
  fin=$(grep -h "gblink_done" "$OUT/$tag.log" | head -1 |
        grep -o "frame=[0-9]* hash=[0-9a-f]*")
  if [ $rc = 0 ] && cmp -s -n 32768 "$OUT/$tag-a.sav" "$REF/$ref-d4-a.sav" &&
     cmp -s -n 32768 "$OUT/$tag-b.sav" "$REF/$ref-d4-b.sav"; then
    pass=$((pass + 1)); echo "PASS $tag $fin"
  else
    fail=$((fail + 1)); echo "FAIL $tag rc=$rc $fin"
  fi
  echo "$name $fin" >> "$OUT/finals.txt"
}
for radio in "lat=2" "lat=3 jitter=6" "lat=1 jitter=20" \
             "lat=2 arq_loss=3 arq_rto=4" \
             "lat=2 jitter=4 arq_loss=5 arq_rto=6 bulk_loss=5"; do
  for ab in 0 1; do
    run gen1 red.gb gen1 gen1 gen1 "" "$radio" $ab
    run gen2 crystal.gbc gen2 gen2 gen2 "" "$radio" $ab
    run battle red.gb gen1 gen1_battle battle "" "$radio" $ab
    run live crystal.gbc gen2 gen2_live live \
        "st_h=$FIX/gen2-a.st st_g=$FIX/gen2-b.st" "$radio" $ab
  done
done
echo "sessions: $pass PASS, $fail FAIL"
bad=0
for name in gen1 gen2 battle live; do
  n=$(cat "$OUT/$name"-*.hashes | sort -u | awk '{print $1}' | sort | uniq -d | wc -l)
  t=$(cat "$OUT/$name"-*.hashes | awk '{print $1}' | sort -u | wc -l)
  echo "$name: $t frames hashed across its 10 runs, $n with two different hashes"
  [ "$n" = 0 ] || bad=1
done
live=$(grep '^live ' "$OUT/finals.txt" | sort -u | wc -l)
echo "live link end frame:hash: $live distinct over 10 runs ($(grep '^live ' "$OUT/finals.txt" | sort -u | cut -d' ' -f2-))"
[ "$fail" = 0 ] && [ "$bad" = 0 ] && [ "$live" = 1 ]
