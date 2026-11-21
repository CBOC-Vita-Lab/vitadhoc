#!/bin/bash
# State loader fuzz and proof, all cores (docs/TESTING.md, "State loaders").  MIT licence.
#
#   bash tools/statefuzz/statefuzz.sh OUTDIR [SECONDS_PER_ROM]
#
# Builds one harness per core under AddressSanitizer + UBSan (host twins, x86; the GBA core is the interpreter build, because a
# memory checker cannot watch generated code), then for every ROM it finds:
#   --proof   a state loaded at frame N and run on equals the machine that was never interrupted (normal states are unaffected)
#   fuzz      damaged states with their checksums repaired are loaded and run for SECONDS_PER_ROM seconds (default 15):
#             no crash, no hang, no sanitizer report
# ROMs are read from the places the gate uses (never copied anywhere): GATE_MD_ROMS (the SGDK samples tree), GATE_MD_SMS_ROMS,
# GATE_SNES_ROMS, GATE_NES_ROMS, GATE_WS_ROM, GATE_GBA_ROMS, plus the test cartridges under gpsp/vita/hostcore/testrom when built.
# A core with no ROM is reported SKIP.  Cases that crash or hang are saved in OUTDIR/cases-<core> (replay: harness ROM DIR --case FILE).
# Prints "PASS <name>", "FAIL <name>" and "SKIP <name>" lines; the exit status is 1 when anything failed.
# The sanitizers leave three kinds of upstream behaviour unreported (shifts, signed overflow, misaligned access: all defined on the
# targets and common in these cores); everything else aborts.
set -u
OUT=${1:?usage: statefuzz.sh OUTDIR [SECONDS_PER_ROM]}
SECS=${2:-${STATEFUZZ_SECONDS:-15}}
SEED=${STATEFUZZ_SEED:-1}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SAN="-fsanitize=address,undefined -fno-sanitize=shift,signed-integer-overflow,alignment -fwrapv -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1" UBSAN_OPTIONS="print_stacktrace=1:abort_on_error=1"
FAILS=0
CPUN=${STATEFUZZ_JOBS:-2}

say() { echo "$@"; }
fail() { say "FAIL $1"; FAILS=$((FAILS+1)); }
pass() { say "PASS $1"; }
skip() { say "SKIP $1 ($2)"; }

# run one harness on one rom: proof, then fuzz
run_one() { # name harness rom cases extra...
	n=$1; h=$2; r=$3; cd_=$4; shift 4
	mkdir -p "$cd_"
	if "$h" "$r" "$cd_" "$@" --proof >"$OUT/$n.proof.log" 2>&1 </dev/null; then :; else fail "$n proof $(basename "$r" | cut -c1-24)"; tail -4 "$OUT/$n.proof.log"; return; fi
	if "$h" "$r" "$cd_" "$@" --seconds "$SECS" --seed "$SEED" >"$OUT/$n.fuzz.log" 2>&1 </dev/null; then pass "$n $(basename "$r" | cut -c1-24): proof and $SECS s of fuzz, $(grep -o '[0-9]* states' "$OUT/$n.fuzz.log" | tail -1)"; else fail "$n fuzz $(basename "$r" | cut -c1-24)"; grep -m6 "ERROR\|SUMMARY\|#[0-4] \|hang\|crash" "$OUT/$n.fuzz.log"; fi
}

# run_list N DIR "patterns" name harness cases extra...: run_one on up to N ROMs (paths may contain spaces)
run_list() {
	cnt=$1; dir=$2; pats=$3; shift 3
	set -f; firstn "$cnt" "$dir" $pats > "$OUT/.list"; set +f
	while IFS= read -r rom; do [ -n "$rom" ] && run_one "$1" "$2" "$rom" "$3" "${@:4}"; done < "$OUT/.list"
}

firstn() { # N dir patterns...: up to N files below dir matching patterns
	n=$1; d=$2; shift 2
	[ -d "$d" ] || { [ -f "$d" ] && echo "$d"; return; }
	for p in "$@"; do find "$d" -type f -iname "$p" 2>/dev/null; done | sort | head -n "$n"
}

# ---- Sega ----
if [ -n "${GATE_MD_ROMS:-}${GATE_MD_SMS_ROMS:-}" ]; then
	B=$OUT/md-build; mkdir -p "$B"
	if (cd "$ROOT/md" && nice make -C core TARGET=host B="$B" OPT=-O1 EXTRA="$SAN" -j"$CPUN" all >"$OUT/md-build.log" 2>&1 &&
	    cd twin && nice g++ -O1 -g -std=gnu++14 $SAN -I../frontend -o "$OUT/md-fuzz" statefuzz.cpp "$B/mdcore.o" -lm -pthread >>"$OUT/md-build.log" 2>&1); then
		run_list 2 "${GATE_MD_ROMS:-/nonexistent}" "rom.bin *.gen" sega "$OUT/md-fuzz" "$OUT/cases-md" --system md
		for ext in sms gg sg; do run_list 1 "${GATE_MD_SMS_ROMS:-/nonexistent}" "*.$ext" sega-$ext "$OUT/md-fuzz" "$OUT/cases-md" --system $ext; done
	else fail "sega build"; tail -5 "$OUT/md-build.log"; fi
else skip sega "GATE_MD_ROMS / GATE_MD_SMS_ROMS not set"; fi

# ---- SNES ----
if [ -n "${GATE_SNES_ROMS:-}" ]; then
	B=$OUT/snes-build
	if (cd "$ROOT/snes/twin" && nice make B="$B" OPT=-O1 CXX="g++ $SAN" CC="gcc $SAN" -j"$CPUN" "$B/statefuzz" >"$OUT/snes-build.log" 2>&1); then
		run_list 3 "$GATE_SNES_ROMS" "*.smc *.sfc" snes "$B/statefuzz" "$OUT/cases-snes" --warm 400
	else fail "snes build"; tail -5 "$OUT/snes-build.log"; fi
else skip snes "GATE_SNES_ROMS not set"; fi

# ---- NES ----
if [ -n "${GATE_NES_ROMS:-}" ]; then
	B=$OUT/nes-build
	if (cd "$ROOT/nes/twin" && nice make B="$B" OPT=-O1 EXTRA="$SAN -g" -j"$CPUN" "$B/statefuzz" >"$OUT/nes-build.log" 2>&1); then
		run_list 3 "$GATE_NES_ROMS" "*.nes *.unf" nes "$B/statefuzz" "$OUT/cases-nes"
	else fail "nes build"; tail -5 "$OUT/nes-build.log"; fi
else skip nes "GATE_NES_ROMS not set"; fi

# ---- WonderSwan ----
if [ -n "${GATE_WS_ROM:-}" ]; then
	B=$OUT/ws-build
	if (cd "$ROOT/wswan/twin" && nice make B="$B" OPT=-O1 EXTRA="$SAN -g" -j"$CPUN" "$B/statefuzz" >"$OUT/ws-build.log" 2>&1); then
		run_one wswan "$B/statefuzz" "$GATE_WS_ROM" "$OUT/cases-ws"
	else fail "wswan build"; tail -5 "$OUT/ws-build.log"; fi
else skip wswan "GATE_WS_ROM not set"; fi

# ---- Game Boy / Game Boy Color (the test cartridges built by build-testrom-gb) ----
GBS=$(firstn 3 "$ROOT/gpsp/vita/hostcore/testrom" '*.gb' '*.gbc' | head -n 3)
if [ -n "$GBS" ]; then
	B=$OUT/gb-build; mkdir -p "$B"
	GB=$ROOT/gpsp/gbcore
	ok=1
	for f in tgbdual/apu tgbdual/cpu tgbdual/gb tgbdual/lcd tgbdual/mbc tgbdual/rom tgbdual/sgb tgbdual/tgb_shared; do
		nice gcc -I"$GB" -I"$GB/../frontend-common" -O1 -g $SAN -std=gnu11 -fgnu89-inline -fno-strict-aliasing -w -c "$GB/$f.c" -o "$B/$(basename $f).o" >>"$OUT/gb-build.log" 2>&1 || ok=0
	done
	nice gcc -I"$GB" -I"$GB/../frontend-common" -O1 -g $SAN -std=gnu11 -w -c "$GB/gbcore_tgbdual.c" -o "$B/gbcore_tgbdual.o" >>"$OUT/gb-build.log" 2>&1 || ok=0
	nice g++ -I"$GB" -I"$GB/../frontend-common" -O1 -g $SAN -std=gnu++14 -w -c "$GB/tests/statefuzz.cpp" -o "$B/statefuzz.o" >>"$OUT/gb-build.log" 2>&1 || ok=0
	if [ $ok = 1 ] && g++ $SAN -o "$B/gb-fuzz" "$B/statefuzz.o" "$B/gbcore_tgbdual.o" "$B"/apu.o "$B"/cpu.o "$B"/gb.o "$B"/lcd.o "$B"/mbc.o "$B"/rom.o "$B"/sgb.o "$B"/tgb_shared.o -lm -pthread >>"$OUT/gb-build.log" 2>&1; then
		for r in $GBS; do run_one gb "$B/gb-fuzz" "$r" "$OUT/cases-gb"; done
	else fail "gb build"; tail -5 "$OUT/gb-build.log"; fi
else skip gb "no GB test cartridge built (build-testrom-gb needs RGBDS)"; fi

# ---- GBA (interpreter build: a memory checker cannot watch the dynamic recompiler's generated code) ----
GBAT=$ROOT/gpsp/vita/hostcore/testrom/gbadhoc-test.gba
if [ -n "${GATE_GBA_ROMS:-}" ] || [ -f "$GBAT" ]; then
	B=$OUT/gba-build; HC=$ROOT/gpsp/vita/hostcore
	if (cd "$HC" && nice make ARCH=x86 DYNAREC=0 B="$B" EXTRA_CFLAGS="$SAN -g" -j"$CPUN" "$B/libgpsp_a.a" "$B/gbalink/gbalink_bus.o" "$B/gbalink/gbalink_pair.o" "$B/gbalink/gbalink_script.o" >"$OUT/gba-build.log" 2>&1 &&
	    cd statefuzz && nice g++ $SAN -O1 -g -std=gnu++14 -w -I../../.. -I../../../libretro -I../../../libretro/libretro-common/include -o "$OUT/gba-fuzz" statefuzz_gba.cpp "$B/libgpsp_a.a" "$B"/gbalink/*.o -lm -pthread >>"$OUT/gba-build.log" 2>&1); then
		[ -n "${GATE_GBA_ROMS:-}" ] && run_list 2 "$GATE_GBA_ROMS" "*.gba" gba "$OUT/gba-fuzz" "$OUT/cases-gba" --warm 300
		[ -f "$GBAT" ] && run_one gba "$OUT/gba-fuzz" "$GBAT" "$OUT/cases-gba" --warm 300
	else fail "gba build"; tail -5 "$OUT/gba-build.log"; fi
else skip gba "GATE_GBA_ROMS not set and no test cartridge built"; fi

say "statefuzz: $FAILS failed"
[ "$FAILS" = 0 ]
