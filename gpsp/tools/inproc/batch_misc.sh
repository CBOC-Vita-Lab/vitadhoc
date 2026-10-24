#!/usr/bin/env bash
# batch_misc.sh <eboot dir> <out dir>: fallbacks, the menu's own quit, themes, fade-in.
ED="$1"; export OUT="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
r() { tag=$1; setup=$2; shift 2; ( env "$@" bash "$HERE/run142.sh" "$tag" "$ED" "$HERE/$setup" 600 >/dev/null 2>&1; echo "$tag: $(grep -m1 '^EVT exit' "$OUT/$tag/frontend.log")" ) & }
r kill setup_inproc.sh CFG='quick_return = 0' ROMS=aw2.gba CYCLES=1 FRAMES=200 AUTOEXIT=300
r fakenet setup_inproc.sh EXTRA='inproc_fake_net = 1' ROMS=aw2.gba CYCLES=1 FRAMES=200 AUTOEXIT=300
r menu setup_rbrowse.sh THEME=0 SHELL_UI=1 SCRIPT='60 press CROSS\n130 press DOWN\n150 press CROSS\n230 press CROSS' CYCLES=2 FRAMES=200 AUTOEXIT=240 EXTRA='inproc_via_menu = 1\nret_dumps = 1'
wait
r light setup_rbrowse.sh THEME=1 SHELL_UI=0 SCRIPT='60 press CROSS\n200 press CROSS' CYCLES=1 FRAMES=200 AUTOEXIT=240 EXTRA='inproc_via_menu = 1\nret_dumps = 1'
r fadein setup_rbrowse.sh THEME=0 SHELL_UI=1 SCRIPT='60 press CROSS' CYCLES=0 FRAMES=200 AUTOEXIT=60 EXTRA='ret_fade_in = 1\nret_dumps = 1'
r gbc setup_rbrowse.sh THEME=0 SHELL_UI=1 CONSOLE=2 SCRIPT='60 press CROSS\n200 press CROSS' CYCLES=1 FRAMES=200 AUTOEXIT=240 EXTRA='inproc_via_menu = 1\nret_dumps = 1'
wait
