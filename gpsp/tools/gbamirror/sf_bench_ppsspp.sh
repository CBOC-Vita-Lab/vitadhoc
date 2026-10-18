#!/usr/bin/env bash
# sf_bench_ppsspp.sh TAG EBOOT_DIR OUTROOT HOSTSCRIPT JOINSCRIPT STATE|- [ini lines...]
# The one-console Street Fighter dual bench in one headless PPSSPP (derived
# from ppsspp_run.sh; private namespace, WSL): the harness ini is the
# owner's hardware bench (gba_dual = 1, gba_dual_autopilot, lookahead
# 140448) plus the ini lines given.  STATE "-" stages no state and sets no
# load_state: WITHOUT "gba_dual_cold = 1" that run is ONE machine (EVT
# gbadual_idle), which is what the 2026-10-10 hardware bench measured
# (docs/GBA-LINK-MIRROR.md 21).  OUTROOT must be absolute.
#   sf_bench_ppsspp.sh solo eb ~/out h.txt j.txt -
#   sf_bench_ppsspp.sh pair eb ~/out h.txt j.txt - "gba_dual_cold = 1"
#   sf_bench_ppsspp.sh fast eb ~/out h.txt j.txt - "gba_dual_cold = 1" "gba_link_fast = 1"
# Read: EVT gbadual_start / gbadual_idle, EVT core_prof core=<us>/..., and
# log/gbadual_hash.txt with "gba_dual_hash = 1".  PPSSPP has no second core,
# so machine A is CPU-rendered here: compare arms, not absolute times.
set -uo pipefail
TAG=$1; EB=$2; OUTROOT=$3; SC=$4; PSC=$5; ST=${6:--}; shift 6 || true
ROM=${ROM:-$HOME/sfdev/sf.gba}
BIOS=${BIOS:-$HOME/gbalink/bios/gba_bios.bin}
PPSSPP_DIR=${PPSSPP_DIR:-$HOME/ppsspp}
TIMEOUT=${TIMEOUT:-1500}
OUT="$OUTROOT/$TAG"
SB="$HOME/gpsp-e2e/sandboxes/linkspeed-$TAG"
MS="$SB/ppsspp"; GAME="$MS/PSP/GAME/gpsp-adhoc"
RB=$(basename "$ROM"); STEM="${RB%.*}"
rm -rf "$SB" "$OUT"; mkdir -p "$MS/PSP/SYSTEM" "$GAME/log" "$GAME/roms" "$OUT"
cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOI
[General]
FirstRun = False
CheckForNewVersion = False
[Graphics]
GraphicsBackend = 3 (VULKAN)
[Network]
EnableWlan = False
[SystemParam]
NickName = GBALINK
EOI
cp "$EB/EBOOT.PBP" "$GAME/"; cp "$EB/gbadhoc_me.prx" "$GAME/" 2>/dev/null || true
cp "$BIOS" "$GAME/gba_bios.bin"
cp "$ROM" "$GAME/roms/$RB"
cp "$SC" "$GAME/script.txt"; cp "$PSC" "$GAME/sf_join.txt"
{ echo "rom = $RB"; echo "script = script.txt"
  echo "heartbeat_s = 5"; echo "autoexit_frames = ${AUTOEXIT:-8600}"
  echo "me_mode = 1"; echo "ff = 0"; echo "host = 0"; echo "join = 0"
  echo "gba_dual = 1"; echo "gba_dual_autopilot = sf_join.txt"
  echo "gba_dual_lookahead = 140448"
  if [ "$ST" != "-" ]; then cp "$ST" "$GAME/roms/$STEM.st0"; echo "load_state = 1"; fi
  for k in "$@"; do echo "$k"; done; } > "$GAME/.gpsp-harness.ini"
cp "$GAME/.gpsp-harness.ini" "$OUT/harness.ini"
SOFT="$SB/softgpu.ini"; printf '[Graphics]\nSoftwareRenderer = True\n' > "$SOFT"
EVT="$GAME/log/frontend.log"
( cd "$PPSSPP_DIR/build" && XDG_CONFIG_HOME="$SB" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  SOFT="$SOFT" GAME="$GAME" TO="$TIMEOUT" \
  unshare -rmnpf --mount-proc --propagation private bash -c 'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && exec timeout $TO xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" ./PPSSPPSDL --windowed --appendconfig="$SOFT" "$GAME/EBOOT.PBP"' > "$OUT/emu.log" 2>&1 ) &
PID=$!
for _ in $(seq 1 "$TIMEOUT"); do
  grep -q "^EVT exit" "$EVT" 2>/dev/null && break
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
done
sleep 2
P1="sandboxes/linkspeed-"; pkill -TERM -f "$P1$TAG/" 2>/dev/null; sleep 1; pkill -KILL -f "$P1$TAG/" 2>/dev/null
wait "$PID" 2>/dev/null
cp -r "$GAME/log/." "$OUT/" 2>/dev/null
rm -rf "$SB"
echo "$TAG: $(grep -m1 '^EVT exit' "$OUT/frontend.log" 2>/dev/null)"
