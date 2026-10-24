#!/bin/bash
# run_soak_x.sh -- the PSP-3000 follow-up to run_soak.sh (docs/INPROC-SOAK.md):
# arm X (jit_small = 1, so H&S is resident on real hardware; make_stages.py
# --only X) then Z (the rig's own files back) with STOP.  Wake cycle stages X,
# cycle 1 collects X and stages Z + STOP: the console parks.
cd /path/to/GBAdhoc/builds/inproc-soak-worktree || exit 9
O=../inproc-soak-out; ST=$O/stage/3000; L=$O/logs/3000x
mkdir -p $L
echo "=== soak 3000 X start $(date +%H:%M:%S) pid $$ ===" >> $L/loop.log
python -u tools/rig/hw_loop.py --host F: --join "" --appdir GBADHOC-GBLINK \
  --arms X=$ST/X,Z=$ST/Z --order XZ --runs 1 --golden $ST/golden --logs $L \
  --keep-going --timeout 3600 --window-min-left 45 >> $L/loop.log 2>&1
echo "=== soak 3000 X hw_loop exit=$? $(date +%H:%M:%S) ===" >> $L/loop.log
