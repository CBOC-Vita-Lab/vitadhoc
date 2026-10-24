# setup (PPSSPP smoke of a hardware arm, tools/inproc/hw): stages the SAME
# arm dir hw_loop copies onto a card.  env: ARMDIR (the arm), S1DIR (the
# install arm: EBOOT-independent ROMs), CYC (optional inproc_cycles override).
# The role prefix is stripped and the handoff keys dropped (no USB here).
mkdir -p "$GAME/roms/soak"
cp "$S1DIR"/roms/soak/*.gba "$S1DIR"/roms/soak/*.gb "$S1DIR"/roms/soak/*.gbc "$GAME/roms/soak/"
( cd "$ARMDIR" && find . -type f ! -name '*-.gpsp-harness.ini' ! -name 'EBOOT.PBP' ! -name '*.prx' | while read -r f; do mkdir -p "$GAME/$(dirname "$f")"; cp "$f" "$GAME/$f"; done )
grep -v '^handoff' "$ARMDIR"/*-.gpsp-harness.ini > "$GAME/.gpsp-harness.ini"
[ -n "${CYC:-}" ] && sed -i "s/^inproc_cycles = .*/inproc_cycles = $CYC/" "$GAME/.gpsp-harness.ini"
# the card's other GBA entries (the link rig's), so the browser lists what the card lists
cp "$FX/aw2.gba" "$GAME/roms/aw2.gba"; cp "$FX/aw2.st0" "$GAME/roms/aw2.st0"; cp "$FX/aw2.gba" "$GAME/roms/msc.gba"
mv "$GAME/CONFIG.INI" "$GAME/config.ini" 2>/dev/null
true
