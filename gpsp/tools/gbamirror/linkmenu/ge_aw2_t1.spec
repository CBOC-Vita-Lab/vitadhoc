# GE comparison walk (docs/GBA-LINK-UI.md): one console, the design's
# backdrop injected (ui_backdrop), theme 1.
N=1
D=$(dirname "$SPEC")
I1_EB=${EB64:-/path/to/GBAdhoc/builds/gbalink-ui-out/u1-h64}
I1_ROM=$HOME/gbamulti/roms/aw2.gba
I1_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I1_STATE=$HOME/gbamulti/fx/aw2.st0
I1_SAVE=$HOME/gbamulti/fx/aw2.sav
I1_THEME=1
I1_CONSOLE=0
I1_BACKDROP=${FX:-/path/to/GBAdhoc/builds/gbalink-ui-out/fx}/aw2.565
I1_UI=$D/ge_aw2.ui
I1_CFG='standby = 0\nfilter = 2'
TIMEOUT_S=300
