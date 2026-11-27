#!/bin/sh
# Proves that the exact speed-ups of the core give the same result as not having them.
#   exact_check.sh [rom dir ...]      (default: SNES_ROMS, UNDIS_ROMS; set FRAMES=3000 for the longer run)
# Builds the twin twice from the same sources, in a temporary folder:
#   ref: -DCBOC_NO_WAI_SKIP -DCBOC_NO_SA1_REPLAY (every WAI pass and every SA-1 instruction is interpreted)
#   opt: as shipped
# and compares frame hashes and the audio of every ROM bit for bit (mt_compare.py --ref). Exit 1 on a difference.
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
T=${EXACT_TMP:-$(mktemp -d)}
J=${JOBS:-2}
make -C "$HERE" -j$J B="$T/ref" EXTRA="-DCBOC_NO_WAI_SKIP -DCBOC_NO_SA1_REPLAY" >"$T/ref.log" 2>&1 || { tail "$T/ref.log"; exit 2; }
make -C "$HERE" -j$J B="$T/opt" >"$T/opt.log" 2>&1 || { tail "$T/opt.log"; exit 2; }
if [ $# -gt 0 ]; then
	exec python3 -I "$HERE/mt_compare.py" --ref "$T/ref/snes_twin" --twin "$T/opt/snes_twin" --mt "" --frames "${FRAMES:-600}" --roms "$@"
fi
exec python3 -I "$HERE/mt_compare.py" --ref "$T/ref/snes_twin" --twin "$T/opt/snes_twin" --mt "" --frames "${FRAMES:-600}"
