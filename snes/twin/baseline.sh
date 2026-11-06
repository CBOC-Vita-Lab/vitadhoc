#!/bin/sh
# Baseline table: fps (CPU time, profiling off) and the time split (profiling on)
# per test ROM. Usage: baseline.sh [rom-root] > table.md
#   SNES_ROMS=/path  default /w/snes-testroms/SNES      TWIN=build/snes_twin
# Copyright CBOC-Vita. MIT licence.
HERE=$(dirname "$0")
ROOT=${1:-${SNES_ROMS:-/w/snes-testroms/SNES}}
TWIN=${TWIN:-$HERE/build/snes_twin}
UNDIS=${UNDIS_ROMS:-/w/snes-testroms/snes-test-roms/bin}   # built from undisbeliever/snes-test-roms; paths written @undis/...
echo "| Test ROM | fps (no profiling) | ms/frame | CPU 65816 | APU | PPU | chip | frontend | PPU flushes/frame | APU port accesses/frame |"
echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
grep -v "^#" "${ROMLIST:-$HERE/baseline_roms.txt}" | while IFS='|' read -r label path frames pad; do
	case "$path" in @undis/*) full="$UNDIS/${path#@undis/}" ;; @own/*) full="${OWN_ROMS:-/w/snes-roms}/${path#@own/}" ;; @chip/*) full="${CHIP_ROMS:-/w/snes-out/chip}/${path#@chip/}" ;; *) full="$ROOT/$path" ;; esac
	[ -f "$full" ] || { echo "| $label | missing | | | | | | | | |"; continue; }
	set -- "$full" --frames "$frames"
	[ -n "$pad" ] && set -- "$@" --pad "$pad"
	# best of 3 for the unprofiled number: the machine may be shared
	best=0
	for i in 1 2 3; do
		f=$("$TWIN" "$@" 2>/dev/null | sed -n 's/.* = \([0-9.]*\) fps.*/\1/p')
		best=$(echo "$f $best" | awk '{print ($1>$2)?$1:$2}')
	done
	line=$("$TWIN" "$@" --summary 2>/dev/null | tail -1)
	cpu=$(echo "$line" | sed -n 's/.*cpu= *\([0-9.]*\)%.*/\1/p')
	apu=$(echo "$line" | sed -n 's/.*apu= *\([0-9.]*\)%.*/\1/p')
	ppu=$(echo "$line" | sed -n 's/.*ppu= *\([0-9.]*\)%.*/\1/p')
	chip=$(echo "$line" | sed -n 's/.*chip= *\([0-9.]*\)%.*/\1/p')
	front=$(echo "$line" | sed -n 's/.*front= *\([0-9.]*\)%.*/\1/p')
	flush=$(echo "$line" | sed -n 's/.*ppu_flush\/fr=\([0-9.]*\).*/\1/p')
	port=$(echo "$line" | sed -n 's/.*apu_port\/fr=\([0-9.]*\).*/\1/p')
	ms=$(echo "$best" | awk '{printf "%.2f", 1000/$1}')
	echo "| $label | $best | $ms | $cpu% | $apu% | $ppu% | $chip% | $front% | $flush | $port |"
done
