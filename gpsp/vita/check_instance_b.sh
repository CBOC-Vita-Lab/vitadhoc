#!/bin/sh
# check_instance_b.sh NM READELF FILE [PREFIX] -- the sizes instance B's boot copies
# (frontend-common/fe_gbadual_image.h) rely on, checked on the partial link
# of B (gba_instance_b.ld) before its globals are renamed (the build), or on
# the renamed object with PREFIX=gbab_ (hostcore/run_tests.sh):
#
#   bdata_end - bdata_start         == the size of the .data section
#   bdata_pristine_end - pristine   == the same (the copy's room)
#   bregion_start .. bregion_lend   inside .bss, below bdata_pristine
#   bdata_pristine_end              inside .bss (the copy fits)
#
# A partial link made without the script, or a script that no longer places
# what the code copies, stops the build here instead of corrupting memory
# at boot.  (fe_gbadual_boot checks the same markers again at run time.)
set -eu
NM=$1; READELF=$2; F=$3; PFX=${4:-}
fail() { echo "check_instance_b: $F: $*" >&2; exit 1; }
sym() {
  v=$("$NM" "$F" | awk -v n="$PFX$1" '$3 == n { print $1; exit }')
  [ -n "$v" ] || fail "symbol $PFX$1 missing (partial link without gba_instance_b.ld?)"
  echo $((0x$v))
}
sec() {   # size of section $1, from the section table
  v=$("$READELF" -SW "$F" | sed -E 's/^ *\[ *[0-9]+\] *//' |
      awk -v n="$1" '$1 == n { print $5; exit }')
  [ -n "$v" ] || fail "section $1 missing"
  echo $((0x$v))
}
ds=$(sym bdata_start); de=$(sym bdata_end)
pr=$(sym bdata_pristine); pe=$(sym bdata_pristine_end)
bs=$(sym bregion_start); bl=$(sym bregion_lend)
dsz=$(sec .data); bsz=$(sec .bss)
[ "$ds" -eq 0 ] || fail "bdata_start is $ds, not the start of .data"
[ $((de - ds)) -eq "$dsz" ] ||
  fail "bdata_end - bdata_start = $((de - ds)) but .data is $dsz bytes"
[ $((pe - pr)) -eq "$dsz" ] ||
  fail "the pristine copy's room is $((pe - pr)) bytes, .data is $dsz"
[ "$bs" -eq 0 ] || fail "bregion_start is $bs, not the start of .bss"
[ "$bl" -le "$pr" ] || fail "the zeroed range ends at $bl, past the pristine copy at $pr"
[ "$pe" -le "$bsz" ] || fail "the pristine copy ends at $pe, .bss is only $bsz bytes"
echo "check_instance_b: .data $dsz, zeroed $((bl - bs)), pristine copy $pr..$pe of .bss $bsz: ok"
