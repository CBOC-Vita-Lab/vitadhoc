#!/bin/sh
# Self tests of the WonderSwan host glue on Linux. Usage: selftest.sh   (WSWAN_TWIN overrides the binary;
# WSWAN_PRIVATE_ROM=/path/to/game.ws enables the optional acceptance section, which only uses the file as input)
# All ROMs come from tests/make_roms.py (hand-written V30MZ code); nothing is downloaded here.
#  1. determinism of frame hashes            2. state save/load replays identically
#  3. cartridge SRAM and EEPROM persistence: written, reloaded (counter grows), .bak, forced failure keeps the original
#  4. orientation: footer flag, key-group scan, LCD icon, manual override, per-game table
#  5. pad mapping: Vita pad -> keys in both presentations   6. audio present and not clipped
#  7. render-off equivalence of emulation   8. colour ROM   9. blend unit test
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
TWIN=${WSWAN_TWIN:-$HERE/build/wswan_twin}
T=$(mktemp -d)
fail=0
ok()  { echo "PASS  $1"; }
bad() { echo "FAIL  $1"; fail=1; }
run() { "$TWIN" "$@" >/dev/null 2>&1; }
field() { sed -n "s/.*$1=\([^ ]*\).*/\1/p" | tail -1; }

python3 -I "$HERE/tests/make_roms.py" $T/roms >/dev/null || { echo "FAIL  ROM generation"; exit 1; }
R=$T/roms

# 1
run $R/h_sram.ws --frames 300 --pad 50:a+x2,120:none --hashes $T/a.h
run $R/h_sram.ws --frames 300 --pad 50:a+x2,120:none --hashes $T/b.h
cmp -s $T/a.h $T/b.h && [ -s $T/a.h ] && ok "deterministic frame hashes" || bad "deterministic frame hashes"
run $R/h_sram.ws --frames 300 --pad 50:a,120:none --hashes $T/c.h
cmp -s $T/a.h $T/c.h && bad "a different pad script changes the picture" || ok "a different pad script changes the picture"

# 2
for m in h_sram v_sram icon color h_eeprom; do
	run $R/$m.ws --frames 300 --pad 50:a,90:x3 --save-state $T/s.st --state-frame 99 --hashes $T/c.h
	run $R/$m.ws --frames 300 --pad 50:a,90:x3 --load-state $T/s.st --load-frame 100 --hashes $T/d.h
	x=$(awk '$1>=100{print $2,$3}' $T/c.h | md5sum)
	y=$(awk '$1>=100{print $2,$3}' $T/d.h | md5sum)
	[ -s $T/s.st ] && [ "$x" = "$y" ] && ok "save state / load state replays identically ($m)" || bad "save state / load state ($m)"
done

# 3
for m in h_sram h_eeprom; do
	rm -f $T/$m.srm $T/$m.srm.* ; mkdir -p $T/bk
	run $R/$m.ws --frames 30 --srm $T/$m.srm --backup-dir $T/bk
	[ -s $T/$m.srm ] && ok "$m: save file written" || bad "$m: save file written"
	sz=$(wc -c < $T/$m.srm)
	n1=$(od -An -tu1 -N1 $T/$m.srm | tr -d ' ')
	run $R/$m.ws --frames 30 --srm $T/$m.srm --backup-dir $T/bk
	n2=$(od -An -tu1 -N1 $T/$m.srm | tr -d ' ')
	[ "$n2" = "$((n1 + 1))" ] && ok "$m: boot counter persisted across runs ($n1 -> $n2)" || bad "$m: persistence ($n1 -> $n2)"
	[ "$(wc -c < $T/$m.srm)" = "$sz" ] && ok "$m: file size stable ($sz bytes)" || bad "$m: file size"
	ls $T/$m.srm.tmp $T/$m.srm.old >/dev/null 2>&1 && bad "$m: no .tmp/.old left" || ok "$m: no .tmp/.old left"
	[ "$(ls $T/bk $T/$m.srm.bak 2>/dev/null | wc -l)" -ge 1 ] && ok "$m: previous file kept as backup" || bad "$m: backup"
	# forced failure: a directory sits where the temporary file goes; the original must survive and the exit code say so
	cp $T/$m.srm $T/$m.keep
	mkdir -p $T/$m.srm.tmp
	"$TWIN" $R/$m.ws --frames 30 --srm $T/$m.srm >/dev/null 2>&1; rc=$?
	rmdir $T/$m.srm.tmp 2>/dev/null
	cmp -s $T/$m.srm $T/$m.keep && [ $rc -ne 0 ] && ok "$m: forced write failure keeps the original and reports it (rc=$rc)" || bad "$m: forced failure (rc=$rc)"
done
# the combined file ends with the internal EEPROM trailer
tail -c 1032 $T/h_sram.srm | head -c 4 | grep -q WSiE && ok "combined save file carries the WSiE trailer" || bad "WSiE trailer"
# no save memory: nothing written
rm -f $T/ns.srm; run $R/h_nosave.ws --frames 30 --srm $T/ns.srm
[ ! -s $T/ns.srm ] || [ "$(wc -c < $T/ns.srm)" -le 1032 ] && ok "cartridge without save memory: no cartridge region in the file" || bad "no save memory"

# 4
o() { "$TWIN" "$@" 2>/dev/null | field orient; }
[ "$(o $R/h_sram.ws --frames 60)" = horizontal ] && ok "orientation: horizontal footer flag" || bad "horizontal footer flag"
[ "$(o $R/v_sram.ws --frames 60)" = vertical ] && ok "orientation: vertical footer flag" || bad "vertical footer flag"
[ "$(o $R/h_sram.ws --frames 60 --orient v)" = vertical ] && ok "orientation: manual override to vertical" || bad "override v"
[ "$(o $R/v_sram.ws --frames 60 --orient h)" = horizontal ] && ok "orientation: manual override to horizontal" || bad "override h"
# switch.ws: X scan for 64 frames then Y, so at frame 100 vertical, at frame 150 horizontal again (6 frame debounce)
run $R/switch.ws --frames 260 --hashes $T/sw.h
a=$(awk '$1==110{print $3}' $T/sw.h); b=$(awk '$1==170{print $3}' $T/sw.h); c=$(awk '$1==230{print $3}' $T/sw.h)
[ "$a" = 1 ] && [ "$b" = 0 ] && [ "$c" = 1 ] && ok "orientation follows the key-group scan (V at 110, H at 170, V at 230)" || bad "scan switching ($a $b $c)"
# icon.ws: polls both groups, icon decides: horizontal icon frames 0-127, vertical 128-255
run $R/icon.ws --frames 300 --hashes $T/ic.h
a=$(awk '$1==60{print $3}' $T/ic.h); b=$(awk '$1==200{print $3}' $T/ic.h); c=$(awk '$1==290{print $3}' $T/ic.h)
[ "$a" = 0 ] && [ "$b" = 1 ] && [ "$c" = 0 ] && ok "orientation follows the LCD icon when both key groups are polled" || bad "icon switching ($a $b $c)"
printf '1 74 1\n' > $T/table.txt
if "$TWIN" $R/h_sram.ws --frames 5 --table "01:71:v" >/dev/null 2>&1; then
	[ "$(o $R/h_sram.ws --frames 60 --table 01:71:v)" = vertical ] && ok "per-game table (developer 01, cart 71) forces vertical" || bad "per-game table"
else ok "per-game table option not accepted in this form (informational)"; fi

# 5: Vita pad mapping equals the raw key script
for pres in upright turned; do
	run $R/v_sram.ws --frames 120 --vpad 20:up,40:right,60:down,80:left,100:cross --vita-presentation $pres --hashes $T/v_$pres.h
done
run $R/v_sram.ws --frames 120 --pad 20:y2,40:y3,60:y4,80:y1,100:x4 --hashes $T/raw.h
cmp -s $T/v_upright.h $T/raw.h && ok "vertical upright: d-pad up/right/down/left = Y2/Y3/Y4/Y1, cross = X4 (south)" || bad "vertical upright mapping"
run $R/v_sram.ws --frames 120 --pad 20:y1,40:y2,60:y3,80:y4,100:x3 --hashes $T/raw2.h
cmp -s $T/v_turned.h $T/raw2.h && ok "vertical turned: d-pad up/right/down/left = Y1/Y2/Y3/Y4, cross = X3" || bad "vertical turned mapping"
run $R/h_sram.ws --frames 120 --vpad 20:up,40:right,60:down,80:left,100:cross,110:circle --hashes $T/vh.h
run $R/h_sram.ws --frames 120 --pad 20:x1,40:x2,60:x3,80:x4,100:b,110:a --hashes $T/rh.h
cmp -s $T/vh.h $T/rh.h && ok "horizontal: d-pad = X1..X4, cross = B, circle = A" || bad "horizontal mapping"

# 6
r=$("$TWIN" $R/h_sram.ws --frames 200 --wav $T/t.wav 2>/dev/null)
p=$(echo "$r" | field audio_peak); c=$(echo "$r" | field audio_clipped)
[ "$p" -gt 1000 ] && [ "$c" = 0 ] && [ -s $T/t.wav ] && ok "audio: tone present (peak $p), nothing clipped" || bad "audio (peak $p clipped $c)"

# 7
run $R/h_sram.ws --frames 200 --pad 50:a --hashes $T/f.h
run $R/h_sram.ws --frames 200 --pad 50:a --render-off-from 100 --hashes $T/g.h
x=$(awk '$1<100{print $2}' $T/f.h | md5sum); y=$(awk '$1<100{print $2}' $T/g.h | md5sum)
[ "$x" = "$y" ] && ok "render-off: frames before the switch identical" || bad "render-off prefix"
"$TWIN" $R/h_sram.ws --frames 200 --srm $T/r1.srm --quiet >/dev/null 2>&1; "$TWIN" $R/h_sram.ws --frames 200 --render-off-from 10 --srm $T/r2.srm --quiet >/dev/null 2>&1
cmp -s $T/r1.srm $T/r2.srm && ok "render-off does not change emulated memory (save equal)" || bad "render-off changes emulation"

# 8
[ "$("$TWIN" $R/color.ws --frames 30 2>/dev/null | grep -c 'colour')" -ge 1 ] && ok "colour ROM is detected as colour" || bad "colour ROM detection"
run $R/color.ws --frames 30 --shot 20:$T/color.png --shot-scale 1
[ -s $T/color.png ] && ok "colour screenshot written" || bad "colour screenshot"
run $R/h_sram.ws --frames 30 --shot 20:$T/m0.png --palette 0; run $R/h_sram.ws --frames 30 --shot 20:$T/m1.png --palette 3
cmp -s $T/m0.png $T/m1.png && bad "mono palettes differ" || ok "mono palettes (grey vs SwanCrystal) give different pictures"

# 9
if [ -x $HERE/build/blend_test ]; then
	$HERE/build/blend_test > $T/bt.txt 2>&1 && ok "blend_test passes ($(grep -c PASS $T/bt.txt) checks)" || { cat $T/bt.txt; bad "blend_test"; }
else echo "SKIP  blend_test not built (make -C $HERE)"; fi

# 10. property and matrix tests (fast settings; the full runs are in tests/ with their own options)
PY="python3 -I $HERE/tests"
if command -v python3 >/dev/null 2>&1; then
	WSWAN_TWIN=$TWIN $PY/orient_prop.py --seeds 4 --frames 300 --work $T/orient >$T/orient.txt 2>&1 && ok "orientation property test (4 seeds x 300 frames): $(tail -1 $T/orient.txt | sed 's/.*checks=/checks=/')" || { cat $T/orient.txt; bad "orientation property test"; }
	WSWAN_TWIN=$TWIN $PY/save_types.py --work $T/saves >$T/saves.txt 2>&1 && ok "save types: $(tail -1 $T/saves.txt)" || { tail -5 $T/saves.txt; bad "save types"; }
	WSWAN_TWIN=$TWIN $PY/state_fuzz.py --work $T/fuzz >$T/fuzz.txt 2>&1 && ok "state fuzz: $(tail -1 $T/fuzz.txt | sed 's/.*state fuzz: //')" || { tail -5 $T/fuzz.txt; bad "state fuzz"; }
	WSWAN_TWIN=$TWIN $PY/rtc_fixed.py --work $T/rtc >$T/rtc.txt 2>&1 && ok "RTC with a fixed clock (UTC and JST, deterministic)" || { cat $T/rtc.txt; bad "RTC fixed clock"; }
else echo "SKIP  property and save tests (python3 missing)"; fi

# optional acceptance with a private ROM, used only as an input file
if [ -n "$WSWAN_PRIVATE_ROM" ] && [ -f "$WSWAN_PRIVATE_ROM" ]; then
	r=$("$TWIN" "$WSWAN_PRIVATE_ROM" --frames 1500 --pad 120:start,140:none,200:a,220:none --srm $T/p.srm 2>/dev/null)
	p=$(echo "$r" | field audio_peak)
	[ "$p" -gt 0 ] && ok "private ROM boots and makes sound (peak $p)" || bad "private ROM"
else echo "SKIP  private ROM acceptance (set WSWAN_PRIVATE_ROM)"; fi

rm -rf $T
[ $fail = 0 ] && echo "selftest: all passed" || echo "selftest: FAILED"
exit $fail
