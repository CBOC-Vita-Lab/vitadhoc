#!/bin/sh
# Writes the symbol map for the renamed renderer copy (PPU thread).
#   rcopy.sh OUT.map obj...
# Every global symbol DEFINED by the given objects gets the suffix _r (so a
# second copy of the renderer can be linked beside the first), and the
# variables the renderer reads are redirected to the _R copies that
# core/ppu_mt.cpp defines. Copyright CBOC-Vita. MIT licence.
# RCOPY_SUF=w makes the map of the third copy (the wide view's margin thread): suffix _w, variables *_W.
out=$1; shift
NM=${NM:-nm}
suf=${RCOPY_SUF:-r}
SUF=$(echo "$suf" | tr a-z A-Z)
$NM -g --defined-only "$@" | awk -v s="_$suf" 'NF==3 {print $3" "$3 s}' | sort -u > "$out"
for v in PPU IPPU GFX BG Memory LineData LineMatrixData DirectColourMaps brightness_cap; do
	echo "$v ${v}_$SUF" >> "$out"
done
