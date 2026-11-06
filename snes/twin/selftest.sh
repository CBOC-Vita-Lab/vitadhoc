#!/bin/sh
# Self tests of the host glue on Linux. Usage: selftest.sh   (ROM root: SNES_ROMS)
#  1. the same ROM and pad script give the same frame hashes twice
#  2. a state saved at frame N and loaded later replays the same frames
#  3. battery RAM: loaded, saved, the previous file kept as .bak
#  4. a ROM with a copier header loads like the headerless file
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
TWIN=${TWIN:-$HERE/build/snes_twin}
ROOT=${SNES_ROMS:-/w/snes-testroms/SNES}
T=$(mktemp -d)
fail=0
ok()  { echo "PASS  $1"; }
bad() { echo "FAIL  $1"; fail=1; }

W=$ROOT/PPU/HDMA/WaveHDMA/WaveHDMA.sfc
if [ -f "$W" ]; then
	"$TWIN" "$W" --frames 200 --pad 50:a,60:none --hashes $T/a.h >/dev/null 2>&1
	"$TWIN" "$W" --frames 200 --pad 50:a,60:none --hashes $T/b.h >/dev/null 2>&1
	cmp -s $T/a.h $T/b.h && ok "deterministic frame hashes" || bad "deterministic frame hashes"

	"$TWIN" "$W" --frames 300 --save-state $T/s.st --state-frame 99 --hashes $T/c.h >/dev/null 2>&1
	"$TWIN" "$W" --frames 300 --load-state $T/s.st --state-frame 150 --hashes $T/d.h >/dev/null 2>&1
	x=$(awk '$1>=100 && $1<=199{print $4}' $T/c.h | md5sum)
	y=$(awk '$1>=150 && $1<=249{print $4}' $T/d.h | md5sum)
	[ "$x" = "$y" ] && ok "save state / load state replays identically" || bad "save state / load state"

	# copier header: 512 zero bytes in front must not change the picture
	( head -c 512 /dev/zero; cat "$W" ) > $T/hdr.smc
	"$TWIN" $T/hdr.smc --frames 200 --pad 50:a,60:none --hashes $T/e.h >/dev/null 2>&1
	cmp -s $T/a.h $T/e.h && ok "512 byte copier header is skipped" || bad "copier header"
else
	echo "SKIP  ROMs not found in $ROOT"
fi

python3 -I "$HERE/tests/make_sram_rom.py" $T/sram.sfc
"$TWIN" $T/sram.sfc --frames 10 --srm $T/g.srm >/dev/null 2>&1
v1=$(od -An -tu1 -N1 $T/g.srm | tr -d ' ')
"$TWIN" $T/sram.sfc --frames 10 --srm $T/g.srm >/dev/null 2>&1
v2=$(od -An -tu1 -N1 $T/g.srm | tr -d ' ')
b2=$(od -An -tu1 -N1 $T/g.srm.bak 2>/dev/null | tr -d ' ')
[ "$v1" = 1 ] && [ "$v2" = 2 ] && ok "battery RAM saved and loaded again (1 then 2)" || bad "battery RAM ($v1,$v2)"
[ "$b2" = 1 ] && ok "previous save kept as .bak" || bad "backup ($b2)"
[ ! -e $T/g.srm.tmp ] && ok "no .tmp left behind" || bad "tmp file left"

rm -rf $T
[ $fail = 0 ] && echo "all passed" || { echo "FAILED"; exit 1; }
