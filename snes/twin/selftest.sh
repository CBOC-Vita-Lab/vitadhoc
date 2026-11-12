#!/bin/sh
# Self tests of the host glue on Linux. Usage: selftest.sh   (ROM root: SNES_ROMS)
#  1. the same ROM and pad script give the same frame hashes twice
#  2. a state saved at frame N and loaded later replays the same frames
#  3. battery RAM: loaded, saved, the previous file kept as .bak
#  4. a ROM with a copier header loads like the headerless file
#  5. audio pacing scenarios S1..S7 (audio_sim.sh)
# Copyright CBOC-Vita. MIT licence.
HERE=$(cd "$(dirname "$0")" && pwd)
TWIN=${TWIN:-$HERE/build/snes_twin}
ROOT=${SNES_ROMS:-/w/snes-testroms/SNES}
T=$(mktemp -d)
fail=0
ok()  { echo "PASS  $1"; }
bad() { echo "FAIL  $1"; fail=1; }

W=$ROOT/PPU/HDMA/WaveHDMA/WaveHDMA.sfc
if [ -f "$W" ]; then
	"$TWIN" "$W" --frames 200 --pad 50:a,60:none --hashes $T/a.h >/dev/null 2>&1
	"$TWIN" "$W" --frames 200 --pad 50:a,60:none --hashes $T/b.h >/dev/null 2>&1
	cmp -s $T/a.h $T/b.h && ok "deterministic frame hashes" || bad "deterministic frame hashes"

	"$TWIN" "$W" --frames 300 --save-state $T/s.st --state-frame 99 --hashes $T/c.h >/dev/null 2>&1
	"$TWIN" "$W" --frames 300 --load-state $T/s.st --state-frame 150 --hashes $T/d.h >/dev/null 2>&1
	x=$(awk '$1>=100 && $1<=199{print $4}' $T/c.h | md5sum)
	y=$(awk '$1>=150 && $1<=249{print $4}' $T/d.h | md5sum)
	[ "$x" = "$y" ] && ok "save state / load state replays identically" || bad "save state / load state"

	# the Vita's non-blocking video fetch only ever returns pictures of the reference run
	"$TWIN" "$W" --frames 200 --hashes $T/r.h >/dev/null 2>&1
	"$TWIN" "$W" --frames 200 --opt ppu_thread=1 --opt apu_thread=1 --nowait --hashes $T/n.h >/dev/null 2>&1
	cut -d' ' -f2- $T/r.h | sort -u > $T/r.s; cut -d' ' -f2- $T/n.h | sort -u > $T/n.s
	[ -s $T/n.s ] && [ -z "$(comm -13 $T/r.s $T/n.s)" ] && ok "non-blocking fetch returns only reference pictures" || bad "non-blocking fetch"

	# copier header: 512 zero bytes in front must not change the picture
	( head -c 512 /dev/zero; cat "$W" ) > $T/hdr.smc
	"$TWIN" $T/hdr.smc --frames 200 --pad 50:a,60:none --hashes $T/e.h >/dev/null 2>&1
	cmp -s $T/a.h $T/e.h && ok "512 byte copier header is skipped" || bad "copier header"
else
	echo "SKIP  ROMs not found in $ROOT"
fi

python3 -I "$HERE/tests/make_sram_rom.py" $T/sram.sfc
"$TWIN" $T/sram.sfc --frames 10 --srm $T/g.srm >/dev/null 2>&1
v1=$(od -An -tu1 -N1 $T/g.srm | tr -d ' ')
"$TWIN" $T/sram.sfc --frames 10 --srm $T/g.srm >/dev/null 2>&1
v2=$(od -An -tu1 -N1 $T/g.srm | tr -d ' ')
b2=$(od -An -tu1 -N1 $T/g.srm.bak 2>/dev/null | tr -d ' ')
[ "$v1" = 1 ] && [ "$v2" = 2 ] && ok "battery RAM saved and loaded again (1 then 2)" || bad "battery RAM ($v1,$v2)"
[ "$b2" = 1 ] && ok "previous save kept as .bak" || bad "backup ($b2)"
[ ! -e $T/g.srm.tmp ] && ok "no .tmp left behind" || bad "tmp file left"
# a save file longer than the cartridge RAM (another emulator's padding) keeps its tail
sz=$(stat -c %s $T/g.srm)
( cat $T/g.srm; printf 'TAIL-KEPT' ) > $T/t.srm
"$TWIN" $T/sram.sfc --frames 10 --srm $T/t.srm >/dev/null 2>&1
v3=$(od -An -tu1 -N1 $T/t.srm | tr -d ' ')
[ "$(tail -c 9 $T/t.srm)" = TAIL-KEPT ] && [ "$(stat -c %s $T/t.srm)" = $((sz + 9)) ] && [ "$v3" = 3 ] \
	&& ok "a longer save file keeps its extra bytes after the RAM" || bad "save tail ($v3)"

# ---- save safety under forced failures: the original must stay byte-identical, the real reason is reported,
# and the retry after the cause is gone works. (The twin exits 3 when the save failed.)
sv=$T/sv; mkdir -p $sv
mk_orig() {  # a save with content of its own (2 KiB cart RAM, a tail), as another emulator or an earlier session left it
	( head -c 2048 /dev/urandom; printf 'KEEP-THE-TAIL' ) > "$1"
}
chk_untouched() {  # file original-copy
	cmp -s "$1" "$2" && [ ! -e "$1.tmp" ]
}
# 1. RLIMIT_FSIZE: no write may exceed 1 KiB (SIGXFSZ ignored so that write() fails with EFBIG)
mk_orig $sv/f1.srm; cp $sv/f1.srm $sv/f1.orig
( trap '' XFSZ; ulimit -f 2; "$TWIN" $T/sram.sfc --frames 10 --srm $sv/f1.srm >/dev/null 2>$sv/f1.err ); rc=$?
chk_untouched $sv/f1.srm $sv/f1.orig && [ $rc = 3 ] && grep -q "sram save: [23] " $sv/f1.err && grep -q "File too large" $sv/f1.err \
	&& ok "file size limit: save and backup refused, original byte-identical, reason reported ($(sed -n 's/.*sram save: //p' $sv/f1.err | head -1 | cut -c1-70))" \
	|| { cat $sv/f1.err; bad "file size limit (rc $rc)"; }
# 2. the backup folder cannot be made (a plain file is in the way): no backup, so no overwrite; then retried in the same run
mk_orig $sv/f2.srm; cp $sv/f2.srm $sv/f2.orig
echo block > $sv/blk
"$TWIN" $T/sram.sfc --frames 10 --srm $sv/f2.srm --backup-dir $sv/blk/bk >/dev/null 2>$sv/f2.err; rc=$?
chk_untouched $sv/f2.srm $sv/f2.orig && [ $rc = 3 ] && grep -q "sram save: 3 " $sv/f2.err && grep -q "existing file was not touched" $sv/f2.err \
	&& ok "backup folder blocked: save refused (code 3), original byte-identical, reason reported" || { cat $sv/f2.err; bad "backup blocked (rc $rc)"; }
for mode in "" "--srm-async"; do
	mk_orig $sv/f2b.srm; cp $sv/f2b.srm $sv/f2b.orig; rm -rf $sv/blk2 $sv/bk2; echo block > $sv/blk2
	"$TWIN" $T/sram.sfc --frames 10 --srm $sv/f2b.srm --backup-dir $sv/blk2/bk $mode --unblock $sv/blk2 >/dev/null 2>$sv/f2b.err; rc=$?
	bk=$(ls $sv/blk2/bk 2>/dev/null | grep -c 'f2b.srm.*bak$')
	v=$(od -An -tu1 -N1 $sv/f2b.srm | tr -d ' ')
	# the cause is removed after the first failure; the retry makes the backup, then writes
	grep -q "sram save: 3 " $sv/f2b.err && grep -q "(retry): 0" $sv/f2b.err && [ $rc = 0 ] && [ $bk = 1 ] && ! cmp -s $sv/f2b.srm $sv/f2b.orig \
		&& cmp -s $(ls $sv/blk2/bk/f2b.srm*.bak | head -1) $sv/f2b.orig \
		&& ok "failed save is retried and then succeeds, backup is a whole copy of the original ${mode:+(writer-thread calls)}" \
		|| { cat $sv/f2b.err; bad "retry after blocked backup $mode (rc $rc, backups $bk)"; }
done
# 3. a truncated backup of the same save left by an interrupted copy is replaced by a whole one, never trusted
mk_orig $sv/f3.srm; cp $sv/f3.srm $sv/f3.orig; mkdir -p $sv/bk3
"$TWIN" $T/sram.sfc --frames 10 --srm $sv/f3.srm --backup-dir $sv/bk3 >/dev/null 2>&1
b=$(ls $sv/bk3/f3.srm*.bak | head -1)
if [ -n "$b" ] && cmp -s "$b" $sv/f3.orig; then
	truncate -s 700 "$b"; cp $sv/f3.orig $sv/f3.srm
	"$TWIN" $T/sram.sfc --frames 10 --srm $sv/f3.srm --backup-dir $sv/bk3 >/dev/null 2>$sv/f3.err; rc=$?
	cmp -s "$b" $sv/f3.orig && [ $rc = 0 ] && ! cmp -s $sv/f3.srm $sv/f3.orig \
		&& ok "truncated earlier backup is replaced by a whole copy before the save is overwritten" || { cat $sv/f3.err; bad "truncated backup (rc $rc)"; }
	# and a different file sitting under the backup's name is kept, the backup goes to the next name
	echo "someone else's data, not a prefix" > "$b"; cp "$b" $sv/f3.other; cp $sv/f3.orig $sv/f3.srm
	"$TWIN" $T/sram.sfc --frames 10 --srm $sv/f3.srm --backup-dir $sv/bk3 >/dev/null 2>$sv/f3.err; rc=$?
	n=$(ls $sv/bk3/f3.srm*.bak | wc -l)
	cmp -s "$b" $sv/f3.other && [ $rc = 0 ] && [ $n = 2 ] && cmp -s "${b%.bak}.1.bak" $sv/f3.orig \
		&& ok "a different file under the backup's name is never replaced" || { cat $sv/f3.err; bad "name clash (rc $rc, $n files)"; }
else bad "backup folder run did not make a backup"; fi
# 4. the temporary file cannot be created (a folder has its name)
mk_orig $sv/f4.srm; cp $sv/f4.srm $sv/f4.orig; mkdir $sv/f4.srm.tmp
"$TWIN" $T/sram.sfc --frames 10 --srm $sv/f4.srm >/dev/null 2>$sv/f4.err; rc=$?
cmp -s $sv/f4.srm $sv/f4.orig && [ $rc = 3 ] && grep -q "sram save: 1 " $sv/f4.err \
	&& ok "temporary file blocked: save refused (code 1), original byte-identical" || { cat $sv/f4.err; bad "tmp blocked (rc $rc)"; }
# 5. the .bak (twin) is not trusted either: a truncated one is rebuilt, and a good second run still keeps the older save
mk_orig $sv/f5.srm; cp $sv/f5.srm $sv/f5.orig; head -c 100 /dev/urandom > $sv/f5.srm.bak
"$TWIN" $T/sram.sfc --frames 10 --srm $sv/f5.srm >/dev/null 2>$sv/f5.err; rc=$?
cmp -s $sv/f5.srm.bak $sv/f5.orig && [ $rc = 0 ] && ok "stale .bak replaced by a verified copy of the save before it was overwritten" || bad "stale bak (rc $rc)"
# 6. a save file the program cannot read is not overwritten (no backup possible)
mk_orig $sv/f6.srm; cp $sv/f6.srm $sv/f6.orig; mkdir -p $sv/f6dir
if [ "$(id -u)" != 0 ]; then
	chmod 000 $sv/f6.srm
	"$TWIN" $T/sram.sfc --frames 10 --srm $sv/f6.srm >/dev/null 2>$sv/f6.err; rc=$?
	chmod 600 $sv/f6.srm
	cmp -s $sv/f6.srm $sv/f6.orig && [ $rc = 3 ] && ok "unreadable save is not overwritten" || bad "unreadable save (rc $rc)"
fi

# pad 2 and the multitap: our own pad-reading cartridge copies $4218-$421F into battery RAM
python3 -I "$HERE/tests/make_pad_rom.py" $T/pad.sfc
"$TWIN" $T/pad.sfc --frames 30 --pad 5:a --pad2 5:b+y --srm $T/p1.srm >/dev/null 2>&1
"$TWIN" $T/pad.sfc --frames 30 --opt multitap=1 --pad 5:a --pad2 5:b+y --pad3 5:start --pad4 5:x --pad5 5:select --srm $T/p2.srm >/dev/null 2>&1
a=$(od -An -tx1 -N9 $T/p1.srm | tr -d ' \n'); b=$(od -An -tx1 -N9 $T/p2.srm | tr -d ' \n')
[ "$(echo $a | cut -c3-4)" = 80 ] && [ "$(echo $a | cut -c9-10)" = c0 ] && ok "pad 1 and pad 2 reach the game ($a)" || bad "pads ($a)"
[ "$(echo $b | cut -c9-10)" = c0 ] && [ "$b" != "$a" ] && ok "multitap pads reach the game ($b)" || bad "multitap ($b)"

g++ -O1 -I"$HERE/../frontend" -o $T/pic_test "$HERE/tests/picture_test.cpp" "$HERE/../frontend/picture.cpp" -lm 2>/dev/null \
	&& $T/pic_test >/dev/null && ok "picture layout, settings file, governor" || bad "picture unit test"

# audio pacing scenarios S1..S7 in virtual time (a synthetic tone when no ROM is around; a ROM adds a core-audio run)
sh "$HERE/audio_sim.sh" >$T/audio_sim.txt 2>&1 && ok "audio pacing scenarios S1-S7 (new pacer), click detector" \
	|| { cat $T/audio_sim.txt; bad "audio pacing scenarios"; }

rm -rf $T
[ $fail = 0 ] && echo "all passed" || { echo "FAILED"; exit 1; }
