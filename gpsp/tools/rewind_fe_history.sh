#!/usr/bin/env bash
# fehist.sh BUILD -- Fire Emblem: The Sacred Stones, 6 minutes of menu and map
# mashing: the history held, heartbeat by heartbeat, with the save part of
# permanent (docs/REWIND.md 17), at three sizes of the save read log.
set -uo pipefail
BUILD=$1
W=$HOME/rewind-agent; S=$W/s11; O=$S/fehist; mkdir -p $O
ROM=${FE_ROM:-$HOME/drcorpus/corpus/roms/fire_emblem_the_sacred_stone-005531.gba}
{
  echo "wait 200"
  for i in $(seq 1 145); do
    echo "press START 4"; echo "wait 30"; echo "press A 4"; echo "wait 30"
    case $((i % 4)) in 0) d=UP;; 1) d=RIGHT;; 2) d=DOWN;; *) d=LEFT;; esac
    echo "hold $d 20"; echo "press A 4"; echo "wait 10"
  done
  echo "wait 100000"
} > $O/mash.inputs
one() { # tag extra...
  local tag=$1; shift
  mkdir -p $O/$tag; rm -f $O/$tag/game.sav
  ( cd $W/$BUILD/sdl && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./gpsp_sdl --rom "$ROM" \
      --bios-dir $W/bios --save $O/$tag/game.sav --script $O/mash.inputs --ff --no-pacing \
      --log $O/$tag/fe.log --dump-dir $O/$tag --autoexit ${FE_FRAMES:-14400} --rtc-pin 1700000000 \
      --rewind --rewind-inplace --rewind-dirty 3 --rewind-fast-restore --rewind-clock \
      --rewind-ring 2097152 --rewind-target 20 "$@" > $O/$tag/err.log 2>&1 )
  python3 - $O/$tag/fe.log $tag <<'PY'
import re, sys
av = []; sf = 0; chip = '-'; sv = ''
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'rewind_avail avail_ms=(\d+).*bytes_per_snap=(\d+) last_drop=(\S+) drops: save=(\d+)', line)
    if m: av.append((int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))))
    m = re.search(r'rewind_win .*save_frames=(\d+)', line)
    if m: sf += int(m.group(1))
    m = re.search(r'rewind_save chip=(\S+)', line)
    if m: chip = m.group(1)
    if 'EVT rewind_save ' in line: sv = line.strip()[4:]
late = av[len(av)//4:]
a = sorted(x[0] for x in late)
print('%-10s chip=%s heartbeats=%d save-writing frames=%d  history held (s), last three quarters of the run: '
      'min %.1f  median %.1f  max %.1f; windows under 5 s: %d of %d; bytes/snapshot median %d; save drops %d' % (
      sys.argv[2], chip, len(av), sf, a[0]/1000, a[len(a)//2]/1000, a[-1]/1000,
      sum(1 for x in a if x < 5000), len(a), sorted(x[1] for x in late)[len(late)//2], av[-1][3]))
print('           ' + sv)
print('           history per heartbeat (s):', ' '.join('%.0f' % (x[0]/1000) for x in av))
PY
}
one new &
one log64k --rewind-rlog 65536 &
one log256k --rewind-rlog 262144 &
wait
