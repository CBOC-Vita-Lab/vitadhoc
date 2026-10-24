#!/usr/bin/env python3
"""rewind_eqscript.py HASH.txt -- the input script of the game a rewound run
ENDED UP playing: the keys of every frame of its final timeline (first-pass
frames up to each landing, then the frames played from there), as a script
for a run that never rewinds.  Used by rewind_savetest.py ("again"): a save
made after a rewind must be the save that game would have written."""
import sys

NAMES = [(0x100, 'A'), (0x001, 'B'), (0x008, 'START'), (0x004, 'SELECT'),
         (0x020, 'DOWN'), (0x010, 'UP'), (0x040, 'LEFT'), (0x080, 'RIGHT'),
         (0x200, 'X'), (0x002, 'Y'), (0x400, 'L'), (0x800, 'R')]


def timeline(path):
    """{frame: keys} of the final timeline, and its last frame."""
    keys = {}
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == 'F':
            keys[int(t[1])] = int(t[6], 16)
        elif t[0] == 'E':
            to = int(t[1])
            for f in [f for f in keys if f > to]:
                del keys[f]
    return keys


def script(keys, first=None, last=None):
    fs = sorted(keys)
    first = fs[0] if first is None else first
    last = fs[-1] if last is None else last
    out, f = [], first
    # frames before the first logged one had no keys
    if first > 1:
        out.append('wait %d' % (first - 1))
    while f <= last:
        k = keys.get(f, 0)
        g = f
        while g + 1 <= last and keys.get(g + 1, 0) == k:
            g += 1
        n = g - f + 1
        if k == 0:
            # the script engine spends two frames more without keys after a
            # press than the wait says (measured; the self-check in
            # rewind_savetest.py holds this to the frame)
            if out and out[-1].startswith('press'):
                if n < 3:
                    raise SystemExit('gap of %d frames after a press at %d' % (n, f))
                n -= 2
            out.append('wait %d' % n)
        else:
            if out and out[-1].startswith('press'):
                raise SystemExit('two presses with no gap at frame %d' % f)
            unknown = k & ~sum(b for b, _ in NAMES)
            if unknown:
                raise SystemExit('unknown key bits %04x at frame %d' % (k, f))
            out.append('press %s %d' % ('+'.join(nm for b, nm in NAMES if k & b), n))
        f = g + 1
    return out


BITS = dict((nm, b) for b, nm in NAMES)


def simulate(lines, start=1):
    """{frame: keys} the script engine produces for wait / press lines, the
    first line beginning at frame `start` (a press of n frames is followed
    by two frames without keys before the next line begins)."""
    keys, f = {}, start
    for line in lines:
        t = line.split()
        if t[0] == 'wait':
            f += int(t[1])
        elif t[0] == 'press':
            k = sum(BITS[x] for x in t[1].split('+'))
            for g in range(f, f + int(t[2])):
                keys[g] = k
            f += int(t[2]) + 2
        elif t[0] != 'evt':
            raise SystemExit('simulate: ' + line)
    return keys, f


def logged(path):
    """[(frame, keys)] of every F record, in file order, and the landings
    (index into that list, frame landed on)."""
    rec, lands = [], []
    for line in open(path):
        t = line.split()
        if t and t[0] == 'F':
            rec.append((int(t[1]), int(t[6], 16)))
        elif t and t[0] == 'E':
            lands.append((len(rec), int(t[1])))
    return rec, lands


def equivalent(path, before, after):
    """The script of a run that never rewinds and plays what the run of
    `path` ended up playing: `before` (script lines up to the rewind) for
    the frames up to the landing, then `after` (the lines after the rewind)
    placed where the log shows its first key.  The log has no record for
    some frames (the history's barriers), so the keys come from the script,
    and every record there is must agree.  One landing only."""
    rec, lands = logged(path)
    if len(lands) != 1:
        raise SystemExit('equivalent: %d landings' % len(lands))
    cut, land = lands[0]
    kb, _ = simulate(before)
    for f, k in rec[:cut]:
        if kb.get(f, 0) != k:
            raise SystemExit('equivalent: frame %d played %04x, script says %04x'
                             % (f, k, kb.get(f, 0)))
    ka0, _ = simulate(after, 0)
    first_log = min(f for f, k in rec[cut:] if k)
    shift = first_log - min(ka0)
    ka = dict((f + shift, k) for f, k in ka0.items())
    for f, k in rec[cut:]:
        if ka.get(f, 0) != k:
            raise SystemExit('equivalent: frame %d played %04x after the '
                             'landing, script says %04x' % (f, k, ka.get(f, 0)))
    keys = dict((f, k) for f, k in kb.items() if f <= land)
    keys.update(ka)
    last = max(f for f, _ in rec[cut:])
    for f in range(1, last + 1):
        keys.setdefault(f, 0)
    return script(keys, 1, last), land, last


if __name__ == '__main__':
    print(chr(10).join(script(timeline(sys.argv[1]))))
