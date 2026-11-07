#!/bin/sh
# Safety check for the renamed renderer copy: lists symbols the copy still takes
# from the live build that could read the live PPU state. Usage:
#   rcopy_check.sh build_dir          (the Makefiles' build/ folder)
# Prints the undefined symbols of build/rcopy/*.o that are defined in other
# objects as data or as weak (inline/template) code. Data that is meant to be
# shared (Settings, tables) is expected; any weak function listed here must be
# looked at, because the linker would pick the live copy's code. Copyright CBOC-Vita. MIT.
B=${1:-build}; NM=${NM:-nm}
$NM -u "$B"/rcopy/*.o | awk 'NF==2 {print $2}' | sort -u > /tmp/rcopy_u.$$
$NM -g --defined-only $(ls "$B"/core/*.o "$B"/core/apu/*.o "$B"/core/apu/bapu/*/*.o 2>/dev/null) | awk 'NF==3 && $2 ~ /[WVDBRC]/ {print $2" "$3}' | sort -k2 -u > /tmp/rcopy_d.$$
awk 'NR==FNR {u[$1]=1; next} ($2 in u) {print $1, $2}' /tmp/rcopy_u.$$ /tmp/rcopy_d.$$
rm -f /tmp/rcopy_u.$$ /tmp/rcopy_d.$$
