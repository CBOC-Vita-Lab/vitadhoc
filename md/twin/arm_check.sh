#!/bin/sh
# ARM validation: md_twin built for armhf (arm-linux-gnueabihf-gcc, static, Cortex-A9 NEON flags, run under qemu-arm) and for
# the host must give identical results on every ROM: picture hash of every frame, sound hash, sample count, picture size and
# the hash of the 68000 / Z80 work RAM at the end. (The saved-state bytes are not compared: that image holds a few structures
# with pointers and is a different size on a 32 bit machine; the state round trip is tested per architecture instead.)
#   md/twin/arm_check.sh [FRAMES]      default 600
#   MD_TEST_ROMS=<dir with sample/**/out/release/rom.bin>   MD_TEST_SMS_ROMS=<dir of .sms/.gg/.sg ROMs>
# Without any ROM tree the run is SKIPped. Final line: armhf vs x86_64 over N ROMs, F frames each: identical X, different Y
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
F=${1:-600}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
command -v arm-linux-gnueabihf-gcc >/dev/null && command -v qemu-arm >/dev/null || { echo "SKIP  needs arm-linux-gnueabihf-gcc and qemu-arm"; exit 0; }
make -C "$HERE" -j2 ARCH=x86 B="$T/x86" >"$T/x.log" 2>&1 && make -C "$HERE" -j2 ARCH=armhf B="$T/armhf" >"$T/a.log" 2>&1 || { tail -5 "$T/x.log" "$T/a.log"; echo "FAIL  build"; exit 1; }
strip() { grep '^RESULT' | sed 's/ state=[0-9a-f]*//; s/ p50=.*//'; }
# Sega CD: the synthetic discs (tools/mkscd.py; no commercial disc or BIOS here) on the test stand-in BIOS: picture, sound and RAM must match, and the reader checks
# (cd_test: TOC, sectors, audio sample for sample on CUE, WAV, Ogg and CHD discs) must pass on the armhf build (the CHD reader decodes big endian audio)
cdiff=0
if command -v python3 >/dev/null; then
	python3 -I "$HERE/tools/mkscd.py" standin "$T/standin.bin" && python3 -I "$HERE/tools/mkscd.py" discs "$T/discs" || { echo "FAIL  Sega CD fixtures"; cdiff=1; }
	make -C "$HERE" -j2 ARCH=armhf B="$T/armhf" "$T/armhf/cd_test" >"$T/c.log" 2>&1 || { tail -3 "$T/c.log"; cdiff=1; }
	qemu-arm -cpu max "$T/armhf/cd_test" "$T/discs" "$T/standin.bin" 2>&1 | grep -q ' 0 failed' || { echo "FAIL  Sega CD reader checks on armhf"; cdiff=1; }
	csame=0
	for d in single.cue multi.cue wavs.cue disc.chd cooked.iso; do
		"$T/x86/md_twin" --disc "$T/discs/$d" --bios "$T/standin.bin" --bios-stand-in --frames $F 2>&1 | strip >"$T/x.out"
		qemu-arm -cpu max "$T/armhf/md_twin" --disc "$T/discs/$d" --bios "$T/standin.bin" --bios-stand-in --frames $F 2>&1 | strip >"$T/a.out"
		if [ -s "$T/x.out" ] && cmp -s "$T/x.out" "$T/a.out"; then csame=$((csame + 1)); else cdiff=1; echo "DIFF  Sega CD disc $d"; fi
	done
	echo "armhf vs x86_64 Sega CD, 5 synthetic discs, $F frames each: identical $csame"
fi
LIST=""
[ -n "$MD_TEST_ROMS" ] && LIST=$(find "$MD_TEST_ROMS" -path '*/sample/*' -name rom.bin | sort)
[ -n "$MD_TEST_SMS_ROMS" ] && LIST="$LIST
$(find "$MD_TEST_SMS_ROMS" -type f \( -name '*.sms' -o -name '*.gg' -o -name '*.sg' -o -name '*.sc' \) | sort)"
LIST=$(echo "$LIST" | grep .)
[ -n "$LIST" ] || { echo "SKIP  no ROMs (set MD_TEST_ROMS / MD_TEST_SMS_ROMS)"; exit $cdiff; }
same=0; diff=0
strip() { grep '^RESULT' | sed 's/ state=[0-9a-f]*//; s/ p50=.*//'; }
for r in $LIST; do
	"$T/x86/md_twin" "$r" --frames $F --pad auto 2>&1 | strip >"$T/x.out"
	qemu-arm -cpu max "$T/armhf/md_twin" "$r" --frames $F --pad auto 2>&1 | strip >"$T/a.out"
	if [ -s "$T/x.out" ] && cmp -s "$T/x.out" "$T/a.out"; then same=$((same + 1)); else diff=$((diff + 1)); echo "DIFF  $r"; fi
done
echo "armhf vs x86_64 over $((same + diff)) ROMs, $F frames each: identical $same, different $diff"
# the same with the wide view on: the picture inside the window, the sound and the RAM as above, and what the wide view decided (how much of the
# margin was real picture, how many lines had only the soft extension, how many scene changes it saw) must not depend on the machine either
wsame=0; wdiff=0
wstrip() { grep -E '^RESULT|^WIDE' | sed 's/ state=[0-9a-f]*//; s/ p50=.*//; s/ analyse=[0-9.]*ms compose=[0-9.]*ms//; s/^WIDE frames=[0-9]* analysed=[0-9]* composed=[0-9]*/WIDE/'; }
for r in $LIST; do
	"$T/x86/md_twin" "$r" --frames $F --pad auto --wide 45 2>&1 | wstrip >"$T/x.out"
	qemu-arm -cpu max "$T/armhf/md_twin" "$r" --frames $F --pad auto --wide 45 2>&1 | wstrip >"$T/a.out"
	if [ -s "$T/x.out" ] && cmp -s "$T/x.out" "$T/a.out"; then wsame=$((wsame + 1)); else wdiff=$((wdiff + 1)); echo "DIFF (wide view)  $r"; fi
done
echo "armhf vs x86_64 with the wide view on, over $((wsame + wdiff)) ROMs, $F frames each: identical $wsame, different $wdiff"
[ $diff = 0 ] && [ $wdiff = 0 ] && [ $cdiff = 0 ]
