# Save States page GE walk (docs/UI-OVERLAY.md section 13): gb, theme 0.
N=1
D=$(dirname "$SPEC")
FX=${FX:-/path/to/GBAdhoc/builds/savestate-ui-out/fx}
OUTUI=/tmp/ss_ge_gb_t0.ui; sed 's/SCENE/gb/' $D/ge_gbx.ui > $OUTUI
I1_EB=${EB:-/path/to/GBAdhoc/builds/savestate-ui-out/s1-h64}
I1_ROM=$HOME/gbdual/roms/kirby.gb
I1_ROMNAME="Kirby's Dream Land.gb"
I1_THEME=0
I1_CONSOLE=1
I1_BACKDROP=$FX/gb/backdrop.565
I1_UI=$OUTUI
I1_STAGE=$D/stage.sh
export SSFX=$FX/gb SSSUF='.gb'
I1_CFG='standby = 0'
TIMEOUT_S=300
