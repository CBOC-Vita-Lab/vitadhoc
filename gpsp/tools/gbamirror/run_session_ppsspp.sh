#!/usr/bin/env bash
# run_session_ppsspp.sh -- a GBA link-cable SESSION between TWO PPSSPP
# instances over PPSSPP's ad-hoc (docs/GBA-LINK-MIRROR.md section 14): each
# console runs both GBAs (the mirror), only buttons cross the radio
# (fe_gblink over fe_gbadual's machines).  The AW2 battle: the host's machine
# is driven by aw2_battle_host.txt on the host console, the join's by
# aw2_battle_join.txt on the join console, both from the owner's AW2 state.
#
#   run_session_ppsspp.sh OUT EBOOT_DIR [options]
#     --delay=N       input delay in frames (default 4)
#     --lat=MS --jit=MS --loss=PCT   emulated radio on both sides
#     --hash-every=N  slot-0 frames between HASH messages (default 0 = the
#                     build's default: 60 before claude/gbalink-hw, 240 after)
#     --nohash        no per-frame gbadual_hash.txt (the cost arm)
#     --frames=N      autoexit_frames (default 30000)
#     --end-after=N   host asks to end N frames after its script is DONE
#     --desync        corrupt nothing: instead the join plays a DIFFERENT
#                     script (aw2_link_join.txt), which must still be
#                     hash-identical (inputs travel) -- a control
#
# Environment: JOIN_EB=DIR gives the join its own EBOOT dir (the hardware
# pairing: host harness64, join harness); MODEL_HOST / MODEL_JOIN = PPSSPP
# PSPModel (default 1 / 1); EXTRA_INI = extra harness lines for both
# consoles (e.g. the smoothness kill switches, GBA-LINK-MIRROR.md 15);
# XVFB_DISPLAYS = "HOST JOIN" display numbers (default "133 134").
#
# Uses two Xvfb displays (XVFB_DISPLAYS) only, inside a private
# namespace (fresh /dev/shm and loopback: PPSSPP_ID and the ad-hoc ports are
# per-namespace).  Run it as:
#   unshare -rmnpf --mount-proc --propagation private bash -c \
#     'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && exec bash run_session_ppsspp.sh ...'
# (run_session_pair.sh wraps that.)
#
# PASS: both exit 0 after gblink_done; every periodic HASH matched on both;
# no desync; equal FINAL hashes; and both consoles' gbadual_hash.txt (both
# machines, every frame) identical over their common prefix, which must
# reach the agreed end frame.
set -uo pipefail
OUT="$1"; EB="$2"; shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
DELAY=4; LAT=0; JIT=0; LOSS=0; HASHN=0; NOHASH=0; FRAMES=30000; ENDAFTER=120
for a in "$@"; do
  case "$a" in
    --delay=*) DELAY="${a#*=}" ;;
    --lat=*) LAT="${a#*=}" ;;
    --jit=*) JIT="${a#*=}" ;;
    --loss=*) LOSS="${a#*=}" ;;
    --hash-every=*) HASHN="${a#*=}" ;;
    --nohash) NOHASH=1 ;;
    --frames=*) FRAMES="${a#*=}" ;;
    --end-after=*) ENDAFTER="${a#*=}" ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
BIOS="${BIOS:-$HOME/gbalink/bios/gba_bios.bin}"
R="${ROMS:-$HOME/gbalink/roms}"
S="${SCRIPTS:-$HOME/gbamirror/runs}"
SB="$HOME/gpsp-e2e/sandboxes/gbasess-$(basename "$OUT")"
read -r XD1 XD2 <<< "${XVFB_DISPLAYS:-133 134}"
JEB="${JOIN_EB:-$EB}"
TIMEOUT_S="${TIMEOUT_S:-1800}"
rm -rf "$OUT" "$SB"; mkdir -p "$OUT"
note() { echo "$*" | tee -a "$OUT/verdict.txt"; }
fail() { note "FAIL: $*"; note "VERDICT FAIL"; exit 1; }

setup() {  # inst role script
  local MS="$SB/inst$1/ppsspp" srv=False eb="$EB" model="${MODEL_HOST:-1}"
  local GAME="$MS/PSP/GAME/gpsp-adhoc"
  [ "$1" = 1 ] && srv=True
  [ "$1" = 2 ] && { eb="$JEB"; model="${MODEL_JOIN:-1}"; }
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
NickName = GBALNK$1
MacAddress = 02:00:00:00:00:0$1
PSPModel = $model
EOI
  cp "$eb/EBOOT.PBP" "$GAME/EBOOT.PBP"
  cp "$eb/gbadhoc_me.prx" "$GAME/" 2>/dev/null
  cp "$BIOS" "$GAME/gba_bios.bin"
  cp "$R/aw2.gba" "$GAME/roms/aw2.gba"
  cp "$R/aw2.st0" "$GAME/roms/aw2.st0"
  cp "$R/aw2.sav" "$GAME/roms/aw2.sav"
  cp "$3" "$GAME/battle.txt"
  {
    echo "rom = aw2.gba"
    echo "script = battle.txt"
    echo "load_state = 1"
    echo "shash = 1"
    echo "heartbeat_s = 5"
    echo "me_mode = 1"
    echo "log_input = 0"
    echo "gba_dual = 1"
    [ "$NOHASH" = 1 ] || echo "gba_dual_hash = 1"
    echo "gblink_role = $2"
    echo "group = GBALNK"
    echo "nick = $2"
    echo "gblink_delay = $DELAY"
    echo "gblink_end_after = $ENDAFTER"
    # 0 = no key (the build default); an ini key read twice keeps the FIRST
    [ "$HASHN" = 0 ] || echo "gblink_hash_every = $HASHN"
    echo "net_latency_ms = $LAT"
    echo "net_jitter_ms = $JIT"
    echo "net_loss_pct = $LOSS"
    echo "autoexit_frames = $FRAMES"
    if [ -n "${EXTRA_INI:-}" ]; then printf '%b\n' "$EXTRA_INI"; fi
  } > "$GAME/.gpsp-harness.ini"
  cp "$GAME/.gpsp-harness.ini" "$OUT/$2.ini"
  printf 'theme = 0\nui_shell = 0\nscale = 1\n' > "$GAME/config.ini"
}
setup 1 host "$S/aw2_battle_host.txt"
setup 2 join "$S/aw2_battle_join.txt"
G1="$SB/inst1/ppsspp/PSP/GAME/gpsp-adhoc"; G2="$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
HL="$G1/log/frontend.log"; JL="$G2/log/frontend.log"
note "session delay=$DELAY lat=${LAT}ms jit=${JIT}ms loss=$LOSS hash_every=$HASHN nohash=$NOHASH frames=$FRAMES eboot=$(md5sum "$EB/EBOOT.PBP" | cut -c1-8) join_eboot=$(md5sum "$JEB/EBOOT.PBP" | cut -c1-8) model=${MODEL_HOST:-1}/${MODEL_JOIN:-1} extra=$(printf '%b' "${EXTRA_INI:-}" | tr '\n' ' ')"

rm -f /dev/shm/PPSSPP_ID
# A private X socket directory: inside the user namespace the shared
# /tmp/.X11-unix (owned by the real root) cannot take a listener.
mount -t tmpfs tmpfs /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
XP=()
for d in $XD1 $XD2; do
  rm -f "/tmp/.X$d-lock"
  Xvfb ":$d" -screen 0 1280x720x24 +extension GLX +render -noreset -nolisten tcp \
    > "$OUT/xvfb$d.log" 2>&1 &
  XP+=($!)
done
sleep 2
launch() {  # inst display -- its own process group, killed by PID below
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
grep -q "^EVT adhoc_up" "$HL" 2>/dev/null || {
  cp "$HL" "$OUT/host.log" 2>/dev/null
  kill "${XP[@]}" 2>/dev/null
  fail "host ad-hoc bring-up never completed"; }
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
# by PID: each launch is its own process group (setsid)
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
[ -f "$OUT/host.log" ] && [ -f "$OUT/join.log" ] || fail "logs missing"
python3 "$HERE/score_session.py" "$OUT" | tee -a "$OUT/verdict.txt"
exit "${PIPESTATUS[0]}"
