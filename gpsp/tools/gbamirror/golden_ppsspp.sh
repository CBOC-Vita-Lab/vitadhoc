#!/usr/bin/env bash
# golden_ppsspp.sh STAGE OUT [ARMS...] -- the PPSSPP golden for a mirrig
# stage: each arm run once from the EXACT staged files (stage/base: EBOOT,
# PRX, BIOS, ROM, save, state, scripts) as app GBADHOC-GBAMIRROR, with the
# arm's ini minus the handoff keys (mirrig.py ini).  WSL; STAGE is a Windows
# path under /mnt/c.  PSPMODEL=1 runs PPSSPP's 64 MiB model (default: the
# PSP-1000's 32 MiB; the guest hashes do not depend on it).
set -uo pipefail
STAGE=$1; OUT=$2; shift 2
ARMS=${*:-M-DUAL M-DUAL10K M-SOLO}
D=$(cd "$(dirname "$0")" && pwd)
PPSSPP_DIR=${PPSSPP_DIR:-$HOME/ppsspp}
TIMEOUT=${TIMEOUT:-1500}
mkdir -p "$OUT"
run_arm() {
  local arm=$1 SB="$HOME/gpsp-e2e/sandboxes/gbamirror-$(basename "$OUT")-$1" O="$OUT/$1"
  local MS="$SB/ppsspp" GAME="$SB/ppsspp/PSP/GAME/GBADHOC-GBAMIRROR"
  rm -rf "$SB" "$O"; mkdir -p "$MS/PSP/SYSTEM" "$MS/PSP/GAME" "$O"
  cp -r "$STAGE/base" "$GAME"; mkdir -p "$GAME/log" "$GAME/handoff"
  printf '[General]\nFirstRun = False\nCheckForNewVersion = False\n[Graphics]\nGraphicsBackend = 3 (VULKAN)\n[Network]\nEnableWlan = False\n[SystemParam]\nNickName = GBAMIRROR\nPSPModel = %s\n' "${PSPMODEL:-0}" > "$MS/PSP/SYSTEM/ppsspp.ini"
  python3 "$D/mirrig.py" ini --arm "$arm" | tr -d '\r' > "$GAME/.gpsp-harness.ini"
  cp "$GAME/.gpsp-harness.ini" "$O/harness.ini"
  printf '[Graphics]\nSoftwareRenderer = True\n' > "$SB/softgpu.ini"
  ( cd "$PPSSPP_DIR/build" && XDG_CONFIG_HOME="$SB" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    SOFT="$SB/softgpu.ini" GAME="$GAME" TO="$TIMEOUT" \
    unshare -rmnpf --mount-proc --propagation private bash -c 'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && exec timeout $TO xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" ./PPSSPPSDL --windowed --appendconfig="$SOFT" "$GAME/EBOOT.PBP"' > "$O/emu.log" 2>&1 ) &
  local PID=$!
  for _ in $(seq 1 "$TIMEOUT"); do
    grep -q "^EVT exit" "$GAME/log/frontend.log" 2>/dev/null && break
    kill -0 "$PID" 2>/dev/null || break
    sleep 1
  done
  sleep 2
  pkill -TERM -f "$SB/" 2>/dev/null; sleep 1
  pkill -KILL -f "$SB/" 2>/dev/null
  wait "$PID" 2>/dev/null
  cp -r "$GAME/log/." "$O/" 2>/dev/null
  echo "$arm: $(grep -m1 '^EVT exit' "$O/frontend.log" 2>/dev/null) shash=$(grep -c '^f=' "$O/shash.txt" 2>/dev/null)"
}
for a in $ARMS; do run_arm "$a" & sleep 3; done
wait
