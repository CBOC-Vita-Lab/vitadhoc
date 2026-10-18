#!/usr/bin/env bash
# batch_x20.sh <eboot dir> <out dir>: 20 in-process returns over five games (GBA/GB/GBC,
# states loaded), census at every step, frame hashes per session; plus a fresh boot
# of each game for the identity reference.
ED="$1"; export OUT="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
X='shash = 1\nvhash = 1\nme_mode = 1\nload_state = 1'
mkdir -p "$OUT"
run() { tag=$1; shift; ( env "$@" bash "$HERE/run142.sh" "$tag" "$ED" "$HERE/setup_inproc.sh" 1500 >/dev/null 2>&1; echo "$tag: $(grep -m1 '^EVT exit' "$OUT/$tag/frontend.log")" ) & }
run X20 CFG='menu_button = home' STATES=1 HNS=1 ROMS='aw2.gba|red.gb|emerald.gba|crystal.gbc|hns.gba' CYCLES=20 FRAMES=300 AUTOEXIT=301 EXTRA="$X"
run F-aw2 STATES=1 ROMS=aw2.gba CYCLES=0 AUTOEXIT=301 EXTRA="$X"
run F-red STATES=1 ROMS=red.gb CYCLES=0 AUTOEXIT=301 EXTRA="$X"
wait
run F-emerald STATES=1 ROMS=emerald.gba CYCLES=0 AUTOEXIT=301 EXTRA="$X"
run F-crystal STATES=1 ROMS=crystal.gbc CYCLES=0 AUTOEXIT=301 EXTRA="$X"
run F-hns STATES=1 HNS=1 ROMS=hns.gba CYCLES=0 AUTOEXIT=301 EXTRA="$X"
wait
