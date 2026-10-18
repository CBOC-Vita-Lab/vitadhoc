# setup: in-process return cycles.  env: ROMS (a|b|..) CYCLES FRAMES AUTOEXIT EXTRA (ini lines, \n-escaped) STATES (1 = stage .st0/.sav for aw2/emerald)
cp "$FX/aw2.gba" "$GAME/roms/aw2.gba"
cp "$FX/emerald.gba" "$GAME/roms/emerald.gba"
cp "$FX/aw2.sav" "$GAME/roms/aw2.sav"
cp "$FX/emerald_3000.sav" "$GAME/roms/emerald.sav"
if [ "${STATES:-0}" = 1 ]; then
  cp "$FX/aw2.st0" "$GAME/roms/aw2.st0"
  cp "$FX/emerald_3000.st0" "$GAME/roms/emerald.st0"
  cp ~/inproc-fx/red.gb.st0 ~/inproc-fx/crystal.gbc.st0 "$GAME/roms/"
fi
[ -n "${SCRIPTF:-}" ] && cp "$SCRIPTF" "$GAME/script.txt"
cp ~/inproc-fx/red.gb "$GAME/roms/red.gb"
cp ~/inproc-fx/crystal.gbc "$GAME/roms/crystal.gbc"
[ -n "${HNS:-}" ] && cp "$FX/hns.gba" "$GAME/roms/hns.gba" && cp "$FX/hns.sav" "$GAME/roms/hns.sav" && cp "$FX/heart_soul_heavy.st0" "$GAME/roms/hns.st0"
{
  echo "inproc_roms = ${ROMS:-aw2.gba|emerald.gba|red.gb|crystal.gbc}"
  echo "inproc_cycles = ${CYCLES:-4}"
  echo "inproc_frames = ${FRAMES:-240}"
  echo "autoexit_frames = ${AUTOEXIT:-300}"
  echo "heartbeat_s = 5"
  echo "log_input = 0"
  [ -n "${EXTRA:-}" ] && printf '%b\n' "$EXTRA"
} > "$GAME/.gpsp-harness.ini"
printf 'group = GPSP07\ntheme = 0\nui_shell = 1\nconsole = 0\n' > "$GAME/config.ini"
[ -n "${CFG:-}" ] && printf '%b\n' "$CFG" >> "$GAME/config.ini"
[ -n "${GBSAV:-}" ] && cp "$GBSAV"/*.gb*.sav "$GAME/roms/" 2>/dev/null
true
