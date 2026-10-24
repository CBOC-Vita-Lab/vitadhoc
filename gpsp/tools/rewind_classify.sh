#!/usr/bin/env bash
# rewind_classify.sh BUILD PLAY ROMGLOB... -- for each game: run the scripted
# rewind test with the per-frame log kept and say WHAT differs first, by
# kind: R = a picture/sound shown backwards, Q = the machine a re-run frame
# leaves, F = a frame replayed with the same inputs (the forward game after a
# rewind: the release-blocking kind).  WSL, the desktop twin.
set -uo pipefail
BUILD=$1; PLAY=$2; shift 2
W=$HOME/rewind-agent; S=$W/s9; O=$S/classify; mkdir -p $O
ALL="--rewind-inplace --rewind-dirty 3 --rewind-fast-restore --rewind-clock --rewind-burst 4 --rewind-verify --rtc-pin 1700000000 ${RW_EXTRA:-}"
for g in "$@"; do
  rom=$(ls $HOME/drcorpus/corpus/roms/$g*.gba | head -1); n=$(basename "$rom" .gba | cut -c1-28)
  python3 $S/tools/rewind_test.py test --bin $W/$BUILD/sdl/gpsp_sdl --bios $W/bios --rom "$rom" \
    --inputs $W/work/in/mash.inputs --play $PLAY --work $O/$n --keep -- $ALL > $O/$n.log 2>&1
  python3 - $O/$n/test/hash.txt "$n" <<'PY'
import sys
first = {}; latest = {}; cur = None
kinds = {}
names = ("state", "video", "audio", "audio_n", "input", "save")
for line in open(sys.argv[1]):
    t = line.split()
    if not t: continue
    if t[0] == 'F':
        f = int(t[1]); rec = tuple(t[2:8])
        if f in first and first[f] != rec:
            d = ",".join(n for n, a, b in zip(names, first[f], rec) if a != b)
            k = kinds.setdefault('F', [0, f, d, set()]); k[0] += 1; k[3].add(d)
        first.setdefault(f, rec); latest[f] = rec
    elif t[0] == 'R' and int(t[1]) in first:
        f = int(t[1]); v, a, an = first[f][1:4]
        d = []
        if t[3] != 'silent':
            if v != t[2]: d.append('video')
            if (a, an) != (t[3], t[4]): d.append('audio')
        if d:
            k = kinds.setdefault('R', [0, f, ",".join(d), set()]); k[0] += 1; k[3].add(",".join(d))
    elif t[0] == 'Q' and int(t[1]) in latest:
        f = int(t[1]); d = []
        if latest[f][0] != t[2]: d.append('state')
        if len(t) > 3 and latest[f][5] != t[3]: d.append('save')
        if d:
            k = kinds.setdefault('Q', [0, f, ",".join(d), set()]); k[0] += 1; k[3].add(",".join(d))
out = ["%s: %d (first at frame %d: %s; kinds %s)" % (k, v[0], v[1], v[2], "/".join(sorted(v[3])))
       for k, v in sorted(kinds.items())]
print("%-28s %s" % (sys.argv[2], " | ".join(out) if out else "clean"))
PY
done
