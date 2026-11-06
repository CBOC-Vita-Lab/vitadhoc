#!/bin/sh
# Writes the symbol map for the renamed renderer copy (PPU thread).
#   rcopy.sh OUT.map obj...
# Every global symbol DEFINED by the given objects gets the suffix _r (so a
# second copy of the renderer can be linked beside the first), and the
# variables the renderer reads are redirected to the _R copies that
# core/ppu_mt.cpp defines. Copyright CBOC-Vita. MIT licence.
out=$1; shift
NM=${NM:-nm}
$NM -g --defined-only "$@" | awk 'NF==3 {print $3" "$3"_r"}' | sort -u > "$out"
for v in PPU IPPU GFX BG Memory LineData LineMatrixData DirectColourMaps brightness_cap; do
	echo "$v ${v}_R" >> "$out"
done
