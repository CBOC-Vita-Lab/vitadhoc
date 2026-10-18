# The playable release (GBA_LINK=1) boots in PPSSPP: no harness, so it
# sits in the ROM browser until the timeout; the check is that it lives.
N=1
D=$(dirname "$SPEC")
I1_EB=${REL:-/path/to/GBAdhoc/builds/gbalink-ui-out/rel-link}
I1_PLAIN=1
I1_ROM=$HOME/gbamulti/roms/aw2.gba
I1_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I1_UI=$D/empty.ui
TIMEOUT_S=60
