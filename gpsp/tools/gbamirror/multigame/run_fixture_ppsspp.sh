#!/usr/bin/env bash
# run_fixture_ppsspp.sh OUT FIXTURE [options] -- one multigame fixture as a
# GBA link-cable SESSION between TWO PPSSPP consoles over PPSSPP's ad-hoc
# (the AW2 runner, tools/gbamirror/run_session_ppsspp.sh, generalised to any
# game): each console runs both GBAs (the mirror), only buttons cross.
#
#   FIXTURE      fixtures/<name>.fixture (shell: ROM, HOST_SAVE, ...)
#   --lat=MS --jit=MS --loss=PCT   emulated radio, both sides
#   --nohash     no per-frame gbadual_hash.txt (the cost arm)
#   --delay=N    input delay (default 4, the shipping default)
#
# Consoles (the hardware pair, GBA-LINK-MIRROR 15): host = harness64 EBOOT on
# PSPModel 1 (the 3000's 64 MiB), join = harness EBOOT on PSPModel 0 (the
# 1000's 32 MiB, whose ROM cache pages).  Environment: EB64 / EB32 (EBOOT
# dirs, default ~/gbamulti/eb/f64 and f32: this branch's build), MG_ROMS (default ~/gbamulti/roms),
# MG_FX (states + battery images, default ~/gbamulti/fx), XVFB_DISPLAYS
# (default "138 139"), TIMEOUT_S.
#
# Must run inside a private namespace (fresh /dev/shm and loopback; PPSSPP_ID
# and the ad-hoc ports are per namespace) -- run_matrix.sh does:
#   unshare -rmnpf --mount-proc --propagation private bash -c \
#     'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && exec bash run_fixture_ppsspp.sh ...'
# Every process it starts is its own process group and is killed by PID.
#
# Verdict: score_session.py (both exit 0 after gblink_done, every periodic
# HASH matched, equal FINALs, per-frame files identical to the agreed end),
# then score_fixture.py (golden frame, autopilot DONE on both, the fixture's
# own markers, SIO and cost summary).
set -uo pipefail
OUT="$1"; FX="$2"; shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
DELAY=4; LAT=0; JIT=0; LOSS=0; NOHASH=0
for a in "$@"; do
  case "$a" in
    --delay=*) DELAY="${a#*=}" ;;
    --lat=*) LAT="${a#*=}" ;;
    --jit=*) JIT="${a#*=}" ;;
    --loss=*) LOSS="${a#*=}" ;;
    --nohash) NOHASH=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
# fixture defaults, then the fixture
HOST_SAVE=-; JOIN_SAVE=-; END_AFTER=120; EXTRA_INI=""; RTC_SEED=0
# shellcheck disable=SC1090
. "$FX"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
BIOS="${BIOS:-$HOME/gbalink/bios/gba_bios.bin}"
EB64="${EB64:-$HOME/gbamulti/eb/f64}"
EB32="${EB32:-$HOME/gbamulti/eb/f32}"
MR="${MG_ROMS:-$HOME/gbamulti/roms}"
MF="${MG_FX:-$HOME/gbamulti/fx}"
SB="$HOME/gpsp-e2e/sandboxes/gbamulti-$(basename "$OUT")"
read -r XD1 XD2 <<< "${XVFB_DISPLAYS:-138 139}"
TIMEOUT_S="${TIMEOUT_S:-2400}"
rm -rf "$OUT" "$SB"; mkdir -p "$OUT"
note() { echo "$*" | tee -a "$OUT/verdict.txt"; }
fail() { note "FAIL: $*"; note "VERDICT FAIL"; exit 1; }

# the fixture's files must be the ones its golden was made with
chk() {  # file md5
  [ "$2" = "-" ] && return 0
  local m; m=$(md5sum < "$1" 2>/dev/null | cut -c1-32)
  [ "$m" = "$2" ] || fail "$1: md5 $m, fixture wants $2"
}
chk "$MR/$ROM" "$ROM_MD5"
[ "$HOST_SAVE" = - ] || chk "$MF/$HOST_SAVE" "$HOST_SAVE_MD5"
[ "$JOIN_SAVE" = - ] || chk "$MF/$JOIN_SAVE" "$JOIN_SAVE_MD5"
chk "$MF/$HOST_STATE" "$HOST_STATE_MD5"
chk "$MF/$JOIN_STATE" "$JOIN_STATE_MD5"

BASE="${ROM%.gba}"
setup() {  # inst role script save state
  local MS="$SB/inst$1/ppsspp" srv=False eb="$EB64" model=1
  local GAME="$MS/PSP/GAME/gpsp-adhoc"
  [ "$1" = 1 ] && srv=True
  [ "$1" = 2 ] && { eb="$EB32"; model=0; }
  mkdir -p "$MS/PSP/SYSTEM" "$GAME/roms" "$GAME/log"
  cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOI
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
NickName = GBAMUL$1
MacAddress = 02:00:00:00:00:0$1
PSPModel = $model
EOI
  cp "$eb/EBOOT.PBP" "$GAME/EBOOT.PBP"
  cp "$eb/gbadhoc_me.prx" "$GAME/" 2>/dev/null
  cp "$BIOS" "$GAME/gba_bios.bin"
  cp "$MR/$ROM" "$GAME/roms/$ROM"
  cp "$MF/$5" "$GAME/roms/$BASE.st0"
  [ "$4" = - ] || cp "$MF/$4" "$GAME/roms/$BASE.sav"
  cp "$HERE/scripts/$3" "$GAME/link.txt"
  {
    echo "rom = $ROM"
    echo "script = link.txt"
    echo "load_state = 1"
    echo "heartbeat_s = 5"
    echo "me_mode = 1"
    echo "log_input = 0"
    echo "gba_dual = 1"
    [ "$NOHASH" = 1 ] || echo "gba_dual_hash = 1"
    echo "gblink_role = $2"
    echo "group = GBAMUL"
    echo "nick = $2"
    echo "gblink_delay = $DELAY"
    echo "gblink_end_after = $END_AFTER"
    [ "$RTC_SEED" = 0 ] || echo "gblink_rtc_seed = $RTC_SEED"
    echo "net_latency_ms = $LAT"
    echo "net_jitter_ms = $JIT"
    echo "net_loss_pct = $LOSS"
    echo "autoexit_frames = $FRAMES"
    if [ -n "$EXTRA_INI" ]; then printf '%b\n' "$EXTRA_INI"; fi
    # MG_EXTRA_INI (environment): keys for every fixture of a matrix run,
    # e.g. 'gba_dual_late = 1' (GBA-LINK-MIRROR.md 19)
    if [ -n "${MG_EXTRA_INI:-}" ]; then printf '%b\n' "$MG_EXTRA_INI"; fi
    # MG_EXTRA_INI_HOST / MG_EXTRA_INI_JOIN: keys for ONE console only (the
    # mismatch arms: a switch both consoles must agree on, set on one)
    if [ "$2" = host ] && [ -n "${MG_EXTRA_INI_HOST:-}" ]; then printf '%b\n' "$MG_EXTRA_INI_HOST"; fi
    if [ "$2" = join ] && [ -n "${MG_EXTRA_INI_JOIN:-}" ]; then printf '%b\n' "$MG_EXTRA_INI_JOIN"; fi
  } > "$GAME/.gpsp-harness.ini"
  cp "$GAME/.gpsp-harness.ini" "$OUT/$2.ini"
  printf 'theme = 0\nui_shell = 0\nscale = 1\n' > "$GAME/config.ini"
}
setup 1 host "$HOST_SCRIPT" "$HOST_SAVE" "$HOST_STATE"
setup 2 join "$JOIN_SCRIPT" "$JOIN_SAVE" "$JOIN_STATE"
G1="$SB/inst1/ppsspp/PSP/GAME/gpsp-adhoc"; G2="$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
HL="$G1/log/frontend.log"; JL="$G2/log/frontend.log"
note "fixture $NAME delay=$DELAY lat=${LAT}ms jit=${JIT}ms loss=$LOSS nohash=$NOHASH frames=$FRAMES eboot64=$(md5sum < "$EB64/EBOOT.PBP" | cut -c1-8) eboot32=$(md5sum < "$EB32/EBOOT.PBP" | cut -c1-8)"

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
launch() {  # inst display
  setsid bash -c 'cd "$1" && shift && exec "$@"' _ "$PPSSPP_DIR/build" \
    env DISPLAY=":$2" XDG_CONFIG_HOME="$SB/inst$1" SDL_AUDIODRIVER=dummy \
    LIBGL_ALWAYS_SOFTWARE=1 timeout $((TIMEOUT_S + 60)) ./PPSSPPSDL \
    --windowed "$SB/inst$1/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" \
    > "$OUT/inst$1.emu.log" 2>&1 &
  echo $!
}
T0=$(date +%s)
PID1=$(launch 1 "$XD1")
for _ in $(seq 1 240); do
  grep -q "^EVT adhoc_up" "$HL" 2>/dev/null && break
  kill -0 "$PID1" 2>/dev/null || break
  sleep 1
done
if ! grep -q "^EVT adhoc_up" "$HL" 2>/dev/null; then
  cp "$HL" "$OUT/host.log" 2>/dev/null
  kill -TERM -- "-$PID1" 2>/dev/null; sleep 1; kill -KILL -- "-$PID1" 2>/dev/null
  kill "${XP[@]}" 2>/dev/null
  fail "host ad-hoc bring-up never completed (PPSSPP rig worn out? run the AW2 control)"
fi
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
rm -rf "$SB"
[ -f "$OUT/host.log" ] && [ -f "$OUT/join.log" ] || fail "logs missing"
python3 "$HERE/../score_session.py" "$OUT" > "$OUT/score_session.txt"
R1=$?
cat "$OUT/score_session.txt" >> "$OUT/verdict.txt"
python3 "$HERE/score_fixture.py" "$OUT" "$FX" "$R1" | tee -a "$OUT/verdict.txt"
exit "${PIPESTATUS[0]}"
