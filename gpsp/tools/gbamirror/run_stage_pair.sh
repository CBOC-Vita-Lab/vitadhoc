#!/usr/bin/env bash
# run_stage_pair.sh -- one arm of the GBA link HARDWARE rig
# (prepare_gbalink_hw_rig.py) in two PPSSPP instances, exactly as hw_loop
# stages it: the stage directory mirrored with its host-/join- prefixes
# (subdirectories included: roms/aw2.gba), the golden saves restored into
# roms/, the handoff keys dropped (they need a real console) and run_id/arm
# appended.  Host = PSPModel 1 (the 3000's 64 MiB), join = PSPModel 0 (the
# 1000's 32 MiB).  Then the same scorers as the hardware logs:
# score_session.py for a mirror arm, score_awlink.py for F.
#
#   run_stage_pair.sh OUT RIG ARM
#
# Environment: XVFB_DISPLAYS="HOST JOIN" (default "133 134"); AWSCORE = path
# of score_awlink.py (arm F); TIMEOUT_S (default 1500).  Run it inside a
# private namespace, as run_session_ppsspp.sh:
#   unshare -rmnpf --mount-proc --propagation private bash -c \
#     'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && exec bash run_stage_pair.sh ...'
set -uo pipefail
OUT="$1"; RIG="$2"; ARM="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SB="$HOME/gpsp-e2e/sandboxes/gbahw-$(basename "$OUT")"
TIMEOUT_S="${TIMEOUT_S:-1500}"
read -r XD1 XD2 <<< "${XVFB_DISPLAYS:-133 134}"
ST="$RIG/stage-$ARM"
[ -d "$ST" ] || { echo "no $ST" >&2; exit 2; }
rm -rf "$OUT" "$SB"; mkdir -p "$OUT"
note() { echo "$*" | tee -a "$OUT/verdict.txt"; }

setup() {  # inst role model
  local MS="$SB/inst$1/ppsspp" srv=False f rel t d
  local GAME="$MS/PSP/GAME/GBADHOC-GBLINK"
  [ "$1" = 1 ] && srv=True
  mkdir -p "$MS/PSP/SYSTEM" "$GAME/roms" "$GAME/log" "$GAME/handoff"
  cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOF
[General]
FirstRun = False
CheckForNewVersion = False

[Graphics]
GraphicsBackend = 3 (VULKAN)
SoftwareRenderer = True

[Network]
EnableWlan = True
EnableAdhocServer = $srv
proAdhocServer = localhost
AdhocServerRelayMode = 2
PortOffset = 10000
EnableUPnP = False

[SystemParam]
NickName = GBAHW$1
MacAddress = 02:00:00:00:00:0$1
PSPModel = $3
EOF
  ( cd "$ST" && find . -type f ) | while read -r rel; do
    rel="${rel#./}"; t="$(basename "$rel")"; d="$(dirname "$rel")"
    case "$t" in
      host-*|join-*) [ "${t%%-*}" = "$2" ] || continue; t="${t#*-}" ;;
    esac
    [ "$d" = . ] && d="" || mkdir -p "$GAME/$d"
    cp "$ST/$rel" "$GAME/${d:+$d/}$t"
  done
  grep -v '^handoff' "$GAME/.gpsp-harness.ini" > "$GAME/h.tmp"
  printf '\nrun_id = ppsspp\narm = %s\n' "$ARM" >> "$GAME/h.tmp"
  mv "$GAME/h.tmp" "$GAME/.gpsp-harness.ini"
  mv "$GAME/CONFIG.INI" "$GAME/config.ini"
  for f in "$RIG/golden/$2-"*; do
    t="$(basename "$f")"; cp "$f" "$GAME/roms/${t#*-}"
  done
  cp "$GAME/.gpsp-harness.ini" "$OUT/$2.ini"
}
setup 1 host 1; setup 2 join 0
G1="$SB/inst1/ppsspp/PSP/GAME/GBADHOC-GBLINK"; G2="$SB/inst2/ppsspp/PSP/GAME/GBADHOC-GBLINK"
HL="$G1/log/frontend.log"; JL="$G2/log/frontend.log"
note "stage $ARM ($ST) host EBOOT $(md5sum < "$G1/EBOOT.PBP" | cut -c1-8) join EBOOT $(md5sum < "$G2/EBOOT.PBP" | cut -c1-8)"

rm -f /dev/shm/PPSSPP_ID
mount -t tmpfs tmpfs /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
XP=()
for d in $XD1 $XD2; do
  rm -f "/tmp/.X$d-lock"
  Xvfb ":$d" -screen 0 1280x720x24 +extension GLX +render -noreset -nolisten tcp \
    > "$OUT/xvfb$d.log" 2>&1 &
  XP+=($!)
done
sleep 2
launch() {  # inst display -- its own process group, killed by PGID below
  setsid bash -c 'cd "$1" && shift && exec "$@"' _ "$PPSSPP_DIR/build" \
    env DISPLAY=":$2" XDG_CONFIG_HOME="$SB/inst$1" SDL_AUDIODRIVER=dummy \
    LIBGL_ALWAYS_SOFTWARE=1 timeout $((TIMEOUT_S + 60)) ./PPSSPPSDL \
    --windowed "$SB/inst$1/ppsspp/PSP/GAME/GBADHOC-GBLINK/EBOOT.PBP" \
    > "$OUT/inst$1.emu.log" 2>&1 &
  echo $!
}
T0=$(date +%s)
PID1=$(launch 1 "$XD1")
for _ in $(seq 1 240); do
  grep -q "^EVT adhoc_up\|^EVT net_up\|^EVT adhoc_create" "$HL" 2>/dev/null && break
  kill -0 "$PID1" 2>/dev/null || break
  sleep 1
done
sleep 3
PID2=$(launch 2 "$XD2")
for _ in $(seq 1 "$TIMEOUT_S"); do
  D1=0; D2=0
  grep -q "^EVT exit code=" "$HL" 2>/dev/null && D1=1
  grep -q "^EVT exit code=" "$JL" 2>/dev/null && D2=1
  [ "$D1" = 1 ] && [ "$D2" = 1 ] && break
  if ! kill -0 "$PID1" 2>/dev/null && ! kill -0 "$PID2" 2>/dev/null; then break; fi
  sleep 1
done
sleep 2
kill -TERM -- "-$PID1" "-$PID2" 2>/dev/null || true
sleep 1; kill -KILL -- "-$PID1" "-$PID2" 2>/dev/null || true
wait "$PID1" "$PID2" 2>/dev/null || true
kill "${XP[@]}" 2>/dev/null; wait "${XP[@]}" 2>/dev/null
rm -f "/tmp/.X$XD1-lock" "/tmp/.X$XD2-lock"
note "wall time $(( $(date +%s) - T0 )) s"
for s in host join; do
  G="$G1"; [ "$s" = join ] && G="$G2"
  mkdir -p "$OUT/$s"
  cp -r "$G/log/." "$OUT/$s/" 2>/dev/null
  cp "$G/log/frontend.log" "$OUT/$s.log" 2>/dev/null
done
[ -f "$OUT/host.log" ] && [ -f "$OUT/join.log" ] || { note "FAIL: logs missing"; note "VERDICT FAIL"; exit 1; }
if [ "$ARM" = F ]; then
  python3 "${AWSCORE:?AWSCORE=score_awlink.py}" "$OUT/host.log" "$OUT/join.log" | tee -a "$OUT/verdict.txt"
  rc=${PIPESTATUS[0]}
  [ "$rc" = 0 ] && note "VERDICT PASS" || note "VERDICT FAIL"
  exit "$rc"
fi
python3 "$HERE/score_session.py" "$OUT" | tee -a "$OUT/verdict.txt"
exit "${PIPESTATUS[0]}"
