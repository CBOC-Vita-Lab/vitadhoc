#!/bin/bash
# Quick response (run-ahead by one frame) on the Super NES: the proof that it changes nothing in the game and shows the next frame.
# Per ROM, one plain run and runs with every frame from frame 10 on run ahead (snes_twin --ahead 10), single thread and with the APU
# and picture threads; the pad presses buttons along the way:
#   - the sound (WAV), the work RAM + battery RAM hash and the machine's digest after every frame (Snes9x's state but one drawing
#     flag, the pads' state, the sprite flags: snes_host_machine_digest) are the plain run's, frame for frame;
#   - every way back gives the machine that was saved (--ahead-verify: the digest before the look-ahead frame and after the way back);
#   - the picture shown at frame n is the plain run's frame n + 1 (but on the first frame run ahead, which follows an undrawn frame as
#     a picture after fast forward does, and where the pad changes: the look-ahead frame has the buttons of frame n; an interlaced
#     picture (448 or 478 lines) also the frame after, as its other field is the one of the picture shown before);
#   - a small cartridge that shows the pad a frame after it reads it (tests/make_latency_rom.py) shows a press 1 frame after it
#     plain and 0 with Quick response;
#   - a conditional cheat fires on the same frame with and without Quick response.
# Without a ROM folder it runs on its own cartridges: the latency one, an SA-1 and a DSP-1 kept busy (tests/make_chip_work_rom.py)
# and one that writes video RAM and a sprite only while a button is held (tests/make_ahead_pad_rom.py: the frames where the
# look-ahead frame guessed the buttons wrong, so what it wrote is not written again by the real frame).
#   ahead_check.sh TWIN [ROMDIR] [FRAMES]       exit 1 on a difference. ROMDIR is searched recursively for .sfc / .smc.
# Copyright CBOC-Vita. MIT licence.
TWIN=$1; D=$2; F=${3:-600}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -x "$TWIN" ] || { echo "usage: $0 snes_twin [ROMDIR] [FRAMES]"; exit 2; }
TWIN=$(cd "$(dirname "$TWIN")" && pwd)/$(basename "$TWIN")
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
python3 -I "$HERE/tests/make_latency_rom.py" $O/latency.sfc || exit 2
python3 -I "$HERE/tests/make_chip_work_rom.py" sa1 $O/sa1work.sfc || exit 2
python3 -I "$HERE/tests/make_chip_work_rom.py" dsp1 $O/dsp1work.sfc || exit 2
python3 -I "$HERE/tests/make_ahead_pad_rom.py" $O/padwork.sfc || exit 2
# the pad: a press every few frames (the changes are skipped in the picture check)
PAD=$(python3 -I -c "
import random
random.seed(11);b=['a','b','x','y','l','r','start','select','up','down','left','right'];ev=[]
for f in range(30,$F,37):
    n=random.choice([0,1,1,2]); ev.append('%d:%s'%(f,'+'.join(random.sample(b,n)) if n else 'none'))
print(','.join(ev))")
CHG=$(echo "$PAD" | tr ',' '\n' | cut -d: -f1 | tr '\n' ' ')
pass=0; fail=0
ok()  { echo "PASS  $1"; pass=$((pass+1)); }
bad() { echo "FAIL  $1"; fail=$((fail+1)); }
roms=("$O/latency.sfc" "$O/sa1work.sfc" "$O/dsp1work.sfc" "$O/padwork.sfc")
if [ -n "$D" ] && [ -d "$D" ]; then
  while IFS= read -r r; do roms+=("$r"); done < <(find -L "$D" -type f \( -name '*.sfc' -o -name '*.smc' \) | sort)
else
  echo "SKIP  no ROM folder: the test's own cartridges only"
fi
for r in "${roms[@]}"; do
  n=$(basename "$r"); n=${n%.*}
  # (run in the ROM's folder: an MSU-1 program finds its data and audio files there)
  ( cd "$(dirname "$r")" && nice "$TWIN" "$r" --frames $F --pad "$PAD" --wav $O/w.p --hashes $O/h.p --digests $O/d.p --peek-sum 2>/dev/null ) | grep ramsum > $O/r.p
  for mode in st mt; do
    x=""; [ $mode = mt ] && x="--opt apu_thread=1 --opt ppu_thread=1"
    ( cd "$(dirname "$r")" && nice "$TWIN" "$r" --frames $F --pad "$PAD" --wav $O/w.a --hashes $O/h.a --digests $O/d.a --peek-sum --ahead 10 --ahead-verify $x 2>/dev/null ) > $O/o.a
    grep ramsum $O/o.a > $O/r.a
    st=$(grep '^ahead:' $O/o.a)
    why=$(echo "$st" | sed 's/.*why=//')
    if [ -n "$why" ]; then echo "SKIP  $n $mode: not run ahead ($why)"; continue; fi
    dd=$(diff <(awk '$1>=10' $O/d.p) <(awk '$1>=10' $O/d.a) | grep -c '^<')
    vb=$(echo "$st" | sed 's/.*differed=\([0-9]*\).*/\1/')
    pics=$(join <(awk '$1>=12{print $1-1, $4}' $O/h.p) <(awk -v F=$F '$1>=11 && $1<F-1{print $1, $4, $3}' $O/h.a) | awk -v chg=" $CHG " '$2!=$3 && index(chg, " " ($1+1) " ")==0 && ($4<=240 || index(chg, " " $1 " ")==0)' | wc -l)
    if cmp -s $O/w.p $O/w.a && cmp -s $O/r.p $O/r.a && [ "$dd" = 0 ] && [ "$vb" = 0 ] && [ "$pics" = 0 ]; then
      ok "$n $mode: same sound, memory and machine every frame; next frame's picture; $(echo "$st" | sed 's/ why=.*//')"
    else
      bad "$n $mode: wav $(cmp -s $O/w.p $O/w.a && echo same || echo DIFF) ram $(cmp -s $O/r.p $O/r.a && echo same || echo DIFF) digests differing $dd ways back differing $vb pictures differing $pics"
    fi
  done
done
# the wide view (snes_wide_twin --wide 70): its picture shown at frame n, margins included, is the one a plain run shows at frame n + 1
# when that run leaves frame 10 undrawn as Quick response's first frame does (the view's learning reacts to one missing frame as it
# does after fast forward); the sound is the same. Single thread, worker threads, and the capture composed on another thread.
WT=${WIDE_TWIN:-$(dirname "$TWIN")/snes_wide_twin}
if [ -x "$WT" ]; then
  mkdir -p $O/wr && python3 "$HERE/tests/make_wide_roms.py" $O/wr >/dev/null 2>&1
  WP="40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none"
  for r in $O/wr/*.sfc; do
    n=$(basename "$r" .sfc)
    for x in "" "--opt apu_thread=1 --opt ppu_thread=1" "--opt apu_thread=1 --opt ppu_thread=1 --wide-split"; do
      nice "$WT" "$r" --frames 300 --pad "$WP" --wide 70 --whashes $O/p.wh --wav $O/p.wav --skip-draw 10 $x >/dev/null 2>&1
      nice "$WT" "$r" --frames 300 --pad "$WP" --wide 70 --whashes $O/a.wh --wav $O/a.wav --ahead 10 $x >/dev/null 2>&1
      d=$(python3 -I -c "
chg=set([int(e.split(':')[0]) for e in '$WP'.split(',')])
def load(p):
    d={}
    for l in open(p):
        t=l.split(); d[int(t[0])]=t[1:]
    return d
p=load('$O/p.wh'); a=load('$O/a.wh')
print(sum(1 for f in a if f>10 and f+1 in p and p[f+1]!=a[f] and (f+1) not in chg) if len(a)>250 and len(p)>250 else 'no pictures')")
      m=${x:-st}; m=${m/--opt apu_thread=1 --opt ppu_thread=1/mt}
      if [ "$d" = 0 ] && cmp -s $O/p.wav $O/a.wav; then ok "wide view $n [$m]: the next frame's wide picture, same sound"; else bad "wide view $n [$m]: pictures differing $d, sound $(cmp -s $O/p.wav $O/a.wav && echo same || echo DIFF)"; fi
    done
  done
else echo "SKIP  wide view: snes_wide_twin not built ($WT)"; fi

# latency: a press shows 1 frame after it plain, 0 with Quick response
lat() { "$TWIN" $O/latency.sfc --frames 160 $1 --hashes $O/n.h >/dev/null 2>&1; "$TWIN" $O/latency.sfc --frames 160 --pad 100:$2 $1 --hashes $O/y.h >/dev/null 2>&1; paste $O/n.h $O/y.h | awk '$4!=$8{print $1-100; exit}'; }
for b in a start; do
  p=$(lat "" $b); a=$(lat "--ahead 10" $b)
  [ "$p" = 1 ] && [ "$a" = 0 ] && ok "latency ($b): $p frame plain, $a with Quick response" || bad "latency ($b): $p frame(s) plain, $a with Quick response"
done
# a conditional cheat (address=condition?byte) fires on the frame its condition first holds: the look-ahead frame sees that frame
# first, and the cheat engine's memory of the code is part of the machine saved (the latency cartridge counts frames at $12)
chk() { "$TWIN" $O/latency.sfc --frames 100 --peek 12 --cheat '7e0012=20?80' --cheats-root $O/ch$1 $2 2>/dev/null | grep '^peek' ; }
p=$(chk p ""); a=$(chk a "--ahead 10"); m=$(chk m "--ahead 10 --opt apu_thread=1 --opt ppu_thread=1")
[ -n "$p" ] && [ "$p" != "$("$TWIN" $O/latency.sfc --frames 100 --peek 12 2>/dev/null | grep '^peek')" ] && [ "$a" = "$p" ] && [ "$m" = "$p" ] \
  && ok "conditional cheat: fires as without Quick response ($p)" || bad "conditional cheat: plain [$p] ahead [$a] ahead mt [$m]"
echo "quick response: $pass passed, $fail failed"
[ $fail = 0 ]
