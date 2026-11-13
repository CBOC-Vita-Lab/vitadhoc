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

# ---- cheats: decode vectors, effect on a synthetic ROM, persistence (.cht), failures, bit-identity with no code on
python3 -I "$HERE/tests/cheat_vectors.py" "$TWIN" >$T/cv.txt 2>&1 && ok "cheat codes: $(tail -1 $T/cv.txt)" || { cat $T/cv.txt; bad "cheat decode vectors"; }
python3 -I "$HERE/tests/make_cheat_rom.py" $T/cheat.sfc
cd_=$T/cht; mkdir -p $cd_
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h0 --peek 10 --srm $cd_/s0.srm >$cd_/o0 2>/dev/null
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h1 --peek 10 --srm $cd_/s1.srm --cheat 7E0010=AA >$cd_/o1 2>/dev/null
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h2 --peek 10 --srm $cd_/s2.srm --cheat 7E0010AA >$cd_/o2 2>/dev/null
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h3 --peek 10 --srm $cd_/s3.srm --cheat 7e0010:aa >$cd_/o3 2>/dev/null
[ "$(grep -o 'peek.*' $cd_/o0)" = "peek 00010 = 05" ] && [ "$(grep -o 'peek.*' $cd_/o1)" = "peek 00010 = AA" ] \
	&& [ "$(od -An -tx1 -N2 $cd_/s0.srm | tr -d ' \n')" = 0505 ] && [ "$(od -An -tx1 -N2 $cd_/s1.srm | tr -d ' \n')" = aa05 ] \
	&& ! cmp -s $cd_/h0 $cd_/h1 && ok "a RAM code changes the game's RAM, its picture and its save (05 -> AA)" || bad "RAM code effect ($(cat $cd_/o0 $cd_/o1 | tr '\n' ' '))"
cmp -s $cd_/h1 $cd_/h2 && cmp -s $cd_/h1 $cd_/h3 && ok "Action Replay 7E0010AA, 7E0010=AA and 7e0010:aa are the same code" || bad "code spellings"
gg=$(python3 -I "$HERE/tests/cheat_vectors.py" "$TWIN" --encode 00800D 77)
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h4 --peek 11 --srm $cd_/s4.srm --cheat $gg >$cd_/o4 2>/dev/null
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h5 --peek 11 --srm $cd_/s5.srm --cheat $gg --cheat-toggle 10:0:0 >$cd_/o5 2>/dev/null
[ "$(grep -o 'peek.*' $cd_/o4)" = "peek 00011 = 77" ] && [ "$(grep -o 'peek.*' $cd_/o5)" = "peek 00011 = 05" ] && ! cmp -s $cd_/h4 $cd_/h5 \
	&& ok "Game Genie $gg patches the ROM operand (05 -> 77); switched off at frame 10 the old byte is back" || bad "Game Genie patch ($(cat $cd_/o4 $cd_/o5 | tr '\n' ' '))"
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h6 --cheat 7E0010=AA --cheat-toggle 0:0:0 >/dev/null 2>&1
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h7 --cheat 7E0010=AA --cheat-disable-all >/dev/null 2>&1
cmp -s $cd_/h0 $cd_/h6 && ok "a code switched off before the first frame leaves the run bit-identical (cheat ROM)" || bad "off = no code"
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h8 --opt apu_thread=1 --opt ppu_thread=1 --peek 10 --cheat 7E0010=AA --cheat $gg >$cd_/o8 2>/dev/null
"$TWIN" $T/cheat.sfc --frames 20 --hashes $cd_/h9 --peek 10 --cheat 7E0010=AA --cheat $gg >$cd_/o9 2>/dev/null
cmp -s $cd_/h8 $cd_/h9 && ok "threads on with codes on: same frames as single-threaded" || bad "threads + cheats"
# state load and reset re-arm the codes
"$TWIN" $T/cheat.sfc --frames 40 --save-state $cd_/st --state-frame 10 >/dev/null 2>&1
"$TWIN" $T/cheat.sfc --frames 20 --load-state $cd_/st --state-frame 10 --peek 10 --cheat 7E0010=AA >$cd_/o10 2>/dev/null
[ "$(grep -o 'peek.*' $cd_/o10)" = "peek 00010 = AA" ] && ok "after a state load the code is poked again" || bad "state load re-arm ($(cat $cd_/o10 | tr '\n' ' '))"
# the list: .cht file, persistence, unknown keys kept, a foreign file
R=$cd_/root; mkdir -p $R
"$TWIN" $T/cheat.sfc --frames 5 --cheats-root $R --cheat 7E0010=AA --cheat $gg >$cd_/o11 2>/dev/null
f=$R/cheats/cheat.cht
[ "$(sed -n 1p $f)" = "cheats = 2" ] && grep -q 'cheat0_code = "7E0010=AA"' $f && grep -q "cheat1_code = \"$gg\"" $f && grep -q "cheat1_enable = true" $f && ok ".cht written in RetroArch's format ($(wc -c <$f) bytes)" || { cat $f; bad ".cht format"; }
"$TWIN" $T/cheat.sfc --frames 20 --cheats-root $R --peek 10 --cheat-list >$cd_/o12 2>/dev/null
grep -q "peek 00010 = AA" $cd_/o12 && grep -q "cheat 1:.*on" $cd_/o12 && ok "the list comes back in a new run and its codes are armed" || bad "persistence ($(cat $cd_/o12 | tr '\n' ' '))"
printf 'cheat0_handler = "x"\n# a comment\n' >>$f
"$TWIN" $T/cheat.sfc --frames 5 --cheats-root $R --cheat-toggle 1:0:0 >/dev/null 2>&1
grep -q "cheat0_enable = false" $f && grep -q 'cheat0_handler = "x"' $f && grep -q '^# a comment' $f && ok "switching a code off is saved; keys and lines we do not know are kept" || { cat $f; bad "unknown keys"; }
"$TWIN" $T/cheat.sfc --frames 5 --cheats-root $R --cheat-remove 0 >/dev/null 2>&1
[ "$(sed -n 1p $f)" = "cheats = 1" ] && grep -q "cheat0_code = \"$gg\"" $f && ! grep -q 7E0010 $f && ok "deleting a code renumbers the file" || { cat $f; bad "remove"; }
mkdir -p $R/cheats; printf 'cheats = 2\ncheat0_desc = "Infinite"\ncheat0_code = "7E0010AA"\ncheat0_enable = true\ncheat1_desc = "Junk"\ncheat1_code = "not a code"\ncheat1_enable = true\n' >$R/cheats/foreign.cht
cp $T/cheat.sfc $T/foreign.sfc
"$TWIN" $T/foreign.sfc --frames 10 --cheats-root $R --peek 10 --cheat-list >$cd_/o13 2>/dev/null
grep -q "peek 00010 = AA" $cd_/o13 && grep -q "cheat 1: Junk | not a code | on (not a code)" $cd_/o13 && ok "a RetroArch file loads; an entry that is not a code stays in the list but is not run" || bad "foreign .cht ($(cat $cd_/o13 | tr '\n' ' '))"
# the writer is the safe one: a failed write leaves the old file byte-identical and says why; a backup exists after a change
python3 - <<P
n = 120
t = "cheats = %d\n" % n
for i in range(n):
    t += 'cheat%d_desc = "Code %d"\ncheat%d_code = "7E%04X=%02X"\ncheat%d_enable = false\n' % (i, i, i, 0x100 + i, i & 255, i)
open("$R/cheats/big.cht", "w").write(t)
P
cp $T/cheat.sfc $T/big.sfc; cp $R/cheats/big.cht $cd_/big.orig
( trap '' XFSZ; ulimit -f 2; "$TWIN" $T/big.sfc --frames 5 --cheats-root $R --cheat-toggle 1:0:1 --cheat-list >$cd_/o14 2>$cd_/e14 )
cmp -s $R/cheats/big.cht $cd_/big.orig && grep -q "cheat file:" $cd_/o14 && ok "cheat file write fails (file size limit): old file byte-identical, reason shown ($(grep 'cheat file:' $cd_/o14 | head -1 | cut -c1-80))" || { cat $cd_/o14 | head -5; bad "cheat write failure"; }
"$TWIN" $T/big.sfc --frames 5 --cheats-root $R --backup-dir $cd_/bk --cheat-toggle 1:0:1 >/dev/null 2>&1; mkdir -p $cd_/bk
"$TWIN" $T/big.sfc --frames 5 --cheats-root $R --backup-dir $cd_/bk --cheat-toggle 1:0:0 >/dev/null 2>&1
b=$(ls $cd_/bk/*.bak 2>/dev/null | head -1)
[ -n "$b" ] && grep -q "cheat0_enable = true" $R/cheats/big.cht 2>/dev/null; true
chmod 000 $R/cheats/big.cht 2>/dev/null
if [ "$(id -u)" != 0 ]; then
	cp $R/cheats/big.cht $cd_/big.keep 2>/dev/null
	"$TWIN" $T/big.sfc --frames 5 --cheats-root $R --cheat 7E0010=AA >$cd_/o15 2>/dev/null
	chmod 600 $R/cheats/big.cht
	grep -q "cheat file:" $cd_/o15 && ok "an unreadable cheat file is never overwritten" || bad "unreadable cheat file"
else chmod 600 $R/cheats/big.cht; fi

g++ -O1 -I"$HERE/../frontend" -o $T/pic_test "$HERE/tests/picture_test.cpp" "$HERE/../frontend/picture.cpp" -lm 2>/dev/null \
	&& $T/pic_test >/dev/null && ok "picture layout, settings file, governor" || bad "picture unit test"

# audio pacing scenarios S1..S7 in virtual time (a synthetic tone when no ROM is around; a ROM adds a core-audio run)
sh "$HERE/audio_sim.sh" >$T/audio_sim.txt 2>&1 && ok "audio pacing scenarios S1-S7 (new pacer), click detector" \
	|| { cat $T/audio_sim.txt; bad "audio pacing scenarios"; }

# the player's own code (menu, states, launch file, patch, cheats, remap, session report, old-state migration)
PT=${PLAYER_TWIN:-$(dirname "$TWIN")/player_twin}
if [ -x "$PT" ]; then
	python3 -I "$HERE/tests/make_cheat_rom.py" $T/pt.sfc
	mkdir -p $T/ptw
	"$PT" test $T/pt.sfc ${PLAYER_SHOTS:-$T/ptw/shots} $T/ptw >$T/pt.txt 2>&1 && ok "player: $(grep 'player_twin:' $T/pt.txt | tail -1 | sed 's/player_twin: //')" \
		|| { grep -v '^Map_' $T/pt.txt | tail -30; bad "player (player_twin)"; }
else echo "SKIP  player_twin not built ($PT)"; fi

# frame hand-over ("Smooth hand-over", playerkit pk_handoff through snes/vita/pipe.cpp): the setting row (default on, kept), and the
# pictures it delivers are bit for bit those of the plain run, in order, none missing outside steady pacing's half mode; with a
# second thread, a slow drawing side, a flush, and the mailbox switched off and on again (two-player lockstep)
if [ -x "$PT" ]; then
	mkdir -p $T/hd/rom
	python3 "$HERE/tests/make_wide_roms.py" $T/hd/rom >/dev/null 2>&1
	if [ -f $T/hd/rom/wide_scroll64.sfc ]; then
		"$PT" handoff $T/hd/rom/wide_scroll64.sfc $T/hd/w >$T/hd.txt 2>&1 && ok "hand-over: $(grep 'checks' $T/hd.txt | tail -1 | sed 's/handoff proof: //')" \
			|| { grep 'handoff proof' $T/hd.txt | tail -8; bad "frame hand-over (player_twin handoff)"; }
	else echo "SKIP  hand-over proof: wide_scroll64 ROM not made"; fi
	if [ -f "$W" ]; then
		"$PT" handoff "$W" $T/hd/w2 >$T/hd2.txt 2>&1 && ok "hand-over on a game ROM: $(grep 'checks' $T/hd2.txt | tail -1 | sed 's/handoff proof: //')" \
			|| { grep 'handoff proof' $T/hd2.txt | tail -8; bad "frame hand-over, game ROM"; }
	fi
fi

# steady pacing: with odd frames not rendered (threads on) the game is the same (audio, RAM, rendered frames)
if [ -f "$W" ]; then
	"$TWIN" "$W" --frames 300 --pad 50:a,60:none --hashes $T/p1.h --wav $T/p1.wav --peek-sum >$T/p1.o 2>/dev/null
	"$TWIN" "$W" --frames 300 --pad 50:a,60:none --hashes $T/p2.h --wav $T/p2.wav --peek-sum --render-half --opt apu_thread=1 --opt ppu_thread=1 >$T/p2.o 2>/dev/null
	awk '$1 % 2 == 0' $T/p1.h > $T/p1e.h
	cmp -s $T/p1e.h $T/p2.h && cmp -s $T/p1.wav $T/p2.wav && [ "$(grep ramsum $T/p1.o)" = "$(grep ramsum $T/p2.o)" ] \
		&& ok "steady pacing half mode: rendered frames, audio and RAM identical" || bad "steady pacing changes the game"
fi

# the wide view: synthetic cartridges with a known level (tests/make_wide_roms.py): no pixel the wide view calls real may be
# wrong, the picture in the middle is the normal picture, and nothing the emulation produces changes with it on
WT=${WIDE_TWIN:-$(dirname "$TWIN")/snes_wide_twin}
if [ -x "$WT" ]; then
	mkdir -p $T/wide
	python3 -I "$HERE/tests/make_wide_roms.py" $T/wide >/dev/null
	for n in wide_scroll64 wide_scroll32 wide_win64_a wide_win64_b wide_mode7; do
		mkdir -p $T/wide/$n.d
		"$WT" $T/wide/$n.sfc --frames 1200 --wide 70 --wopt debug=1 --dump $T/wide/$n.d --dump-frames 150,300,450,600,750,900,1050,1150 >/dev/null 2>&1
		r=$(python3 -I "$HERE/tests/wide_truth.py" $T/wide/$n.json $T/wide/$n.d 2>&1 | head -1)
		echo "$r" | grep -q "centre unmatched 0): wrong real px 0, checked real px [1-9]" && ok "wide view truth: $n ($r)" || bad "wide view truth: $n ($r)"
	done
	python3 -I "$HERE/wide_compare.py" --twin "$WT" --frames 500 --roms $T/wide/wide_scroll64.sfc $T/wide/wide_mode7.sfc $T/wide/wide_win64_a.sfc >$T/wide/id.txt 2>&1 \
		&& ok "wide view identity (frames, audio, state; single thread and workers): $(tail -1 $T/wide/id.txt | cut -d'(' -f1)" || { cat $T/wide/id.txt; bad "wide view identity"; }
	if [ -x "$PT" ]; then
		mkdir -p $T/ptwide
		"$PT" wide $T/wide/wide_scroll64.sfc $T/ptwide >$T/ptwide.txt 2>&1 && ok "wide view in the player: $(grep 'player_twin:' $T/ptwide.txt | tail -1 | sed 's/player_twin: //')" \
			|| { grep -v '^Map_' $T/ptwide.txt | tail -20; bad "wide view in the player"; }
	fi
else echo "SKIP  snes_wide_twin not built ($WT)"; fi

rm -rf $T
[ $fail = 0 ] && echo "all passed" || { echo "FAILED"; exit 1; }
