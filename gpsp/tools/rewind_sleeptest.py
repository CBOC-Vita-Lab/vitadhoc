#!/usr/bin/env python3
"""rewind_sleeptest.py -- THE HISTORY ENDING IN THE MIDDLE OF A REWIND MUST
NOT DAMAGE THE SAVE (docs/REWIND.md 22, review finding 1).

    python3 tools/rewind_sleeptest.py --bin sdl/gpsp_sdl --bios DIR \\
            --rom EMERALD.gba --sav TEST.sav [--work DIR] [--k 0,1,...]
            [--rlog BYTES] [--every N] [--ticks A..B] [--jobs N]
            [--extra "OPTS"] [--expect-damage]

Desktop twin (REWIND=1, -DGPSP_PERF_RIG), headless, WSL/Linux.  The file
given with --sav is only read (a test fixture, never a player's save).

SEVERAL DIFFERENT in-game saves (--saves, default 3), then a sleep in the
middle of a rewind.  Below, "save 2" is the LAST of them.

  S1   Continue, START menu, save                       -> file "save 1"
  S2   S1, then (play on, START menu, save again) x (saves - 1)
                                                        -> file "save 2"
  run  S2, hold the rewind control back across the later saves and into
       save 1; at
       the Nth tick of that rewind the console "sleeps" (--rewind-sleep-tick
       N: what the PSP does at a suspend, then the pool is overwritten, then
       the wake), the control is let go, the game plays on with no input.
       One run for EVERY tick N of the hold, until the history ends.

By the owner's rule (17: "rewind takes back the game, never the save") the
cartridge save after any of that is SAVE 2, the last one the game made:
  - the save file at exit is byte for byte the file of the run that made
    the saves and never rewound (not save 1, and never a mixture of sectors
    of the two);
  - so is the save in memory at the last frame;
  - a cold start from the file loads it (not the picture of a start without
    a save) and does not write it.
One other outcome is right, and is checked against its own reference: a
sleep at tick N must leave what LETTING GO at tick N leaves (a run with
`rewind N` and no sleep), byte for byte.  That differs from save 2 only
where a release lands just before a save the player had already confirmed:
the game then plays into that save again and writes it, whole, from where
it now is -- a new save by the owner's rule (the "again" case of
rewind_savetest.py), not a mixture.
With one save in the history (the first version of this test) the frames a
badly resumed game replays write the bytes that are already there, and the
test cannot see anything.  With TWO saves it cannot either in Pokemon:
the game keeps two copies and writes them in turn, so save 2 does not touch
the sectors save 1 wrote and the replayed tail of save 1 again writes what
is there (--saves 2 shows it: no damage on a build without the fix).  The
third save goes over save 1's sectors.

--expect-damage: for a build WITHOUT the fix: exit 0 only if at least one
sleep left a file that is not save 2 (the test reproduces the defect).
--rlog: the read log's size (17.2).  The product's 131072 holds one Emerald
save, so the history ends in the tail of save 1; 524288 reaches all of it.

Exit 0 = PASS."""
import argparse
import concurrent.futures as cf
import os
import shlex
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rewind_savetest as st  # noqa: E402

SECTOR = 4096


def clusters(frames):
    """Runs of save-writing frames no more than QUIET apart."""
    out = []
    for f in frames:
        if out and f - out[-1][1] <= st.QUIET:
            out[-1][1] = f
        else:
            out.append([f, f])
    return out


def sectors(data, a, b):
    """Of the file's 4 KiB sectors: how many are save 2's, save 1's (and not
    save 2's), neither."""
    n2 = n1 = nx = 0
    for o in range(0, max(len(data), len(b)), SECTOR):
        s = data[o:o + SECTOR]
        if s == b[o:o + SECTOR]:
            n2 += 1
        elif s == a[o:o + SECTOR]:
            n1 += 1
        else:
            nx += 1
    return n2, n1, nx


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--bin', required=True)
    p.add_argument('--bios', required=True)
    p.add_argument('--rom', required=True)
    p.add_argument('--sav', required=True)
    p.add_argument('--work', default='sleeptest-work')
    p.add_argument('--k', default='0,1,2,3,4,5')
    p.add_argument('--rlog', type=int, default=524288)
    p.add_argument('--every', type=int, default=1)
    p.add_argument('--saves', type=int, default=3)
    p.add_argument('--ticks', default='', help='A..B: only these ticks')
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    p.add_argument('--extra', default='')
    p.add_argument('--expect-damage', action='store_true')
    p.add_argument('--keep', action='store_true')
    a = p.parse_args()
    a.work = os.path.abspath(a.work)
    a.sav = os.path.abspath(a.sav)
    os.makedirs(a.work, exist_ok=True)
    src_crc = st.crc_file(a.sav)
    rl = ['--rewind-rlog', str(a.rlog)]
    extra = shlex.split(a.extra)

    # ---- save 1 ------------------------------------------------------------
    S1 = r1 = None
    for k in [int(x) for x in a.k.split(',')]:
        S1 = st.boot_script() + st.again_script(k)
        rc, r1 = st.run(a, 'ref1', S1 + ['wait 300'], rl, 12000)
        fr, _, _ = st.facts(r1)
        if rc == 0 and [f for f in fr if f - 1 in fr and fr[f][7] != fr[f - 1][7]]:
            break
    else:
        print('rewind_sleeptest: the game did not save\nFAIL')
        return 1
    # ---- the later saves (the START menu remembers its cursor: try a few
    # ways of getting to "Save" again) ---------------------------------------
    S2, r2, cl, fr, last = S1, r1, None, None, None
    for i in range(2, a.saves + 1):
        for k2 in (0, k, 1, 2, 3):
            Sn = S2 + ['wait 200'] + st.again_script(k2)
            rc, rn = st.run(a, 'ref%d' % i, Sn + ['evt saved', 'wait 600'], rl,
                            10000 * i)
            fr, _, last = st.facts(rn)
            wrote = sorted(f for f in fr if f - 1 in fr and fr[f][7] != fr[f - 1][7])
            cl = clusters(wrote)
            if rc == 0 and len(cl) == i:
                S2, r2 = Sn, rn
                break
        else:
            print('rewind_sleeptest: the game did not save %d times\nFAIL' % i)
            return 1
    sav1 = open(os.path.join(r1, 'game.sav'), 'rb').read()
    sav2 = open(os.path.join(r2, 'game.sav'), 'rb').read()
    crc1, crc2 = st.crc_file(os.path.join(r1, 'game.sav')), st.crc_file(os.path.join(r2, 'game.sav'))
    mem2 = last[7]
    n2, n1, nx = sectors(sav1, sav1, sav2)
    print('%d saves, written in frames %s; file after save 1: %s, after save '
          '%d: %s (in memory %s); those two differ in %d of %d sectors'
          % (a.saves, ', '.join('%d..%d' % tuple(c) for c in cl), crc1,
             a.saves, crc2, mem2, n1 + nx, n2 + n1 + nx))
    if crc1 == crc2:
        print('the saves are the same file: this test proves nothing\nFAIL')
        return 1

    # ---- how long is the hold ----------------------------------------------
    def hold(n):
        return ['evt saved', 'wait 200', 'rewind %d' % n, 'wait 600']
    rc, h = st.run(a, 'hold', S2 + hold(3000 * a.saves), rl + extra, 60000)
    g = st.evt(h, 'rewind_gesture')
    _, lands, _ = st.facts(h)
    if rc != 0 or not g or not lands:
        print('rewind_sleeptest: the hold without a sleep failed (rc=%d)\nFAIL' % rc)
        return 1
    T = int(g[0]['ticks']) - int(g[0]['limit']) - int(g[0]['catchup']) + 4
    top, floor = lands[0]['top'], lands[0]['land']
    reach = 'all of save 1' if floor < cl[0][0] else (
        'the tail of save 1 (from frame %d)' % floor if floor < cl[0][1]
        else 'NOT save 1')
    print('held from frame %d, the history ends at frame %d (%s): %d ticks; '
          'without a sleep the file at exit is %s'
          % (top, floor, reach, T, st.crc_file(os.path.join(h, 'game.sav'))))
    if floor >= cl[0][1]:
        print('the rewind does not reach save 1: this test proves nothing\nFAIL')
        return 1
    if st.crc_file(os.path.join(h, 'game.sav')) != crc2:
        print('the hold WITHOUT a sleep changed the save\nFAIL')
        return 1

    # ---- one sleep per tick --------------------------------------------------
    ticks = list(range(1, T + 1, a.every))
    if a.ticks:
        lo, hi = [int(x) for x in a.ticks.split('..')]
        ticks = [n for n in ticks if lo <= n <= hi]

    def one(n):
        rc, d = st.run(a, 't%d' % n, S2 + hold(T + 40),
                       rl + ['--rewind-sleep-tick', str(n)] + extra, 60000)
        _, _, l2 = st.facts(d)
        sl = st.evt(d, 'rewind_sleep_test')
        f = os.path.join(d, 'game.sav')
        data = open(f, 'rb').read() if os.path.exists(f) else b''
        log = open(os.path.join(d, 'fe.log'), errors='replace').read()
        res = dict(n=n, rc=rc, data=None, crc=st.crc_file(f),
                   mem=l2[7] if l2 else None,
                   mach=int(sl[0]['mach']) if sl else -1,
                   now=int(sl[0]['now']) if sl else -1,
                   landed='rewind_land ' in log,
                   reset='rewind_unvetted_reset' in log)
        res['ok'] = rc == 0 and bool(sl) and data == sav2 and res['mem'] == mem2
        res['again'] = False
        if not res['ok'] and rc == 0 and sl and data != sav2:
            # Not save 2.  The one other thing it may be: what LETTING GO at
            # this tick leaves.  (A release that lands just before a save
            # the player had already confirmed plays into that save again:
            # the game saves, whole, from where it now is.  By the owner's
            # rule that is a new save, not damage.)
            rc3, d3 = st.run(a, 'rel%d' % n, S2 + hold(n), rl + extra, 60000)
            _, _, l3 = st.facts(d3)
            f3 = os.path.join(d3, 'game.sav')
            if (rc3 == 0 and os.path.exists(f3) and l3 and
                    open(f3, 'rb').read() == data and l3[7] == res['mem']):
                res['ok'] = res['again'] = True
            shutil.rmtree(d3, ignore_errors=True)
        if not res['ok']:
            res['data'] = data
            if a.keep:
                return res
        shutil.rmtree(d, ignore_errors=True)
        return res

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(one, ticks))
    bad = [x for x in rows if not x['ok']]
    kinds = {}
    for x in bad:
        if x['rc'] != 0 or x['mach'] < 0:
            key = 'run failed (rc=%d, slept=%d)' % (x['rc'], x['mach'] >= 0)
        elif x['data'] == sav1:
            key = 'file is SAVE 1, whole (%s)' % crc1
        elif x['data'] == sav2:
            key = 'file is the last save but the save in memory is %s' % x['mem']
        else:
            key = 'file %s is a MIXTURE: %d sectors of the LAST save, %d of save 1, ' \
                  '%d of neither' % ((x['crc'],) + sectors(x['data'], sav1, sav2))
        kinds.setdefault(key, []).append(x)
    nosave = None
    for key, xs in sorted(kinds.items(), key=lambda kv: kv[1][0]['n']):
        cold = ''
        if xs[0]['data'] and xs[0]['data'] not in (sav1, sav2):
            # does the game at least load it?
            tmp = os.path.join(a.work, 'mix.sav')
            open(tmp, 'wb').write(xs[0]['data'])
            if nosave is None:
                _, cn = st.run(a, 'cold_none', st.boot_script() + ['wait 300'],
                               [], 8000, no_sav=True)
                nosave = st.last_video(cn)
            _, cm = st.run(a, 'cold_mix', st.boot_script() + ['wait 300'], [],
                           8000, sav=tmp)
            cold = '; a cold start %s' % (
                'loads a save from it' if st.last_video(cm) != nosave
                else 'does NOT load it')
        ms = sorted(x['mach'] for x in xs)
        print('  %4d sleeps: %s; machine stood at frames %d..%d, ticks %d..%d%s'
              % (len(xs), key, ms[0], ms[-1], xs[0]['n'], xs[-1]['n'], cold))
    inside = [x for x in rows if cl[0][0] <= x['mach'] < cl[0][1]]
    print('%d sleeps, one at every %s tick of the hold (ticks %d..%d; the '
          'machine stood at frames %d..%d, inside save 1\'s write sequence in '
          '%d of them); landed first: %d, reset as the last resort: %d; '
          'the game saved again after the wake exactly as it does when the '
          'control is let go at that tick: %d; the save is damaged after %d'
          % (len(rows), 'single' if a.every == 1 else '%dth' % a.every,
             ticks[0], ticks[-1],
             min(x['mach'] for x in rows if x['mach'] >= 0),
             max(x['mach'] for x in rows), len(inside),
             sum(1 for x in rows if x['landed']),
             sum(1 for x in rows if x['reset']),
             sum(1 for x in rows if x['again']), len(bad)))
    if not inside:
        print('no sleep found the machine inside save 1: this test proves '
              'nothing\nFAIL')
        return 1

    # ---- the cold start of save 2 -------------------------------------------
    cold = st.boot_script() + ['wait 300']
    _, c2 = st.run(a, 'cold_2', cold, [], 8000, sav=os.path.join(r2, 'game.sav'))
    if nosave is None:
        _, cn = st.run(a, 'cold_none', cold, [], 8000, no_sav=True)
        nosave = st.last_video(cn)
    cold_ok = (st.last_video(c2) is not None and st.last_video(c2) != nosave and
               st.crc_file(os.path.join(c2, 'game.sav')) == crc2)
    print('a cold start from the last save loads it and leaves it: %s' % cold_ok)
    print('the save file given was not touched: %s' % (st.crc_file(a.sav) == src_crc))
    if a.expect_damage:
        ok = len(bad) > 0
        print('EXPECTED DAMAGE (a build without the fix): %s'
              % ('the save was damaged, the test sees the defect' if ok
                 else 'NO sleep damaged the save: the defect is not reproduced'))
    else:
        ok = not bad and cold_ok and st.crc_file(a.sav) == src_crc
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
