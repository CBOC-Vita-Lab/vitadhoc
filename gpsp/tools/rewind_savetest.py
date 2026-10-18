#!/usr/bin/env python3
"""rewind_savetest.py -- THE SAVE IS PERMANENT UNDER REWIND (docs/REWIND.md
section 17): a rewind takes back the game, never the cartridge save.

    python3 tools/rewind_savetest.py --bin sdl/gpsp_sdl --bios DIR \\
            --rom EMERALD.gba --sav OWNER.sav [--work DIR] [--k 0,1,...]
            [--jobs N] [--skip-window] [--mutants]

Desktop twin (a REWIND=1 build with -DGPSP_PERF_RIG), headless, WSL/Linux.
Pokemon Emerald with a real save file (a COPY: the file given is only read):
Continue, open the START menu, move down K entries, confirm until the game
has saved (the first K of --k that saves is used).  With S = the script up
to and including that save, and W the frames in which the game wrote it:

  across   S, hold the control until the history ends (before the save;
           with 8 MiB of history that is near power-on), let go.
           - the save in memory after every re-run frame and at the resume
             is the save the game wrote (rewind_check: changed_by_rewind=0);
           - the FILE: size, modification time and CRC are the same when
             play resumes as when the control went down, and at exit the
             file is the one the run that never rewound left;
           - the rewind did go back over the save (writes refused > 0), and
             no re-run frame read a save byte the first pass had not read.
  again    the same, then the game saves AGAIN from the frame landed on.
           By the owner's rule that is a NEW save, of the game as it is
           after the rewind, so the file of the run that never rewound is
           not its reference (that one saved a later moment of the game).
           The reference is a run that NEVER REWINDS and is given, frame for
           frame, the buttons the rewound run ended up playing
           (rewind_eqscript.py), from the same file:
           - the resulting file is byte for byte that run's file, and the
             machine at the last frame is that run's;
           - started cold (a new process) the game loads it (not the
             picture of a start without a save) and does not write it.
  window   one run per frame t from just before W to just after W + N: S,
           hold exactly until t is on screen, let go.  Play must never
           continue from inside the save (rewind_check: inside=0,
           chip_busy=0; no landing from the first busy frame to the frame
           before the last write: the last write's own frame and the ones
           after it have the finished save, which is the save there is).
           The frames played after the landing are the first pass's -- or,
           where a restore had to drop the dynarec's RAM translation cache
           (docs/REWIND.md 16.3, 20.2), those of a run that never rewound
           and whose cache is dropped at that same frame, picture and sound
           to the sample (rewind_check --ref-after-landing).
  kill     `across`, killed (no flush, no close) at three moments of the
           rewind: the file is the one the game wrote, whole.
  mutants  (--mutants) the same runs with one rule switched off must FAIL:
           re-run frames write the save; a rewind may stop anywhere.

Prints one line per check and PASS / FAIL.
"""
import argparse
import binascii
import concurrent.futures
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rewind_eqscript  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

OPTS = ['--rewind', '--rewind-inplace', '--rewind-dirty', '3', '--rewind-verify',
        '--rewind-fast-restore', '--rewind-clock', '--rtc-pin', '1700000000',
        '--rewind-ring', str(8 << 20), '--rewind-max-frames', '16384']
QUIET = 60


def boot_script():
    out = ['wait 300']
    for _ in range(30):
        out += ['press A 4', 'wait 40']
    for _ in range(3):
        out += ['press B 4', 'wait 30']
    return out


def again_script(k):
    out = ['press START 4', 'wait 40']
    for _ in range(k):
        out += ['press DOWN 4', 'wait 12']
    for _ in range(8):
        out += ['press A 4', 'wait 70']
    out += ['wait 500']
    return out


def crc_file(path):
    if not os.path.exists(path):
        return None
    return '%08x' % (binascii.crc32(open(path, 'rb').read()) & 0xffffffff)


def run(a, tag, lines, extra, autoexit, env=None, sav=None, no_sav=False):
    d = os.path.join(a.work, tag)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    with open(os.path.join(d, 's.inputs'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    dst = os.path.join(d, 'game.sav')
    if not no_sav:
        shutil.copyfile(sav or a.sav, dst)
    cmd = [os.path.abspath(a.bin), '--rom', os.path.abspath(a.rom),
           '--bios-dir', os.path.abspath(a.bios), '--save', dst,
           '--script', os.path.join(d, 's.inputs'), '--ff', '--no-pacing',
           '--log', os.path.join(d, 'fe.log'), '--dump-dir', d,
           '--autoexit', str(autoexit),
           '--rewind-hash', os.path.join(d, 'hash.txt')] + OPTS + extra
    e = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    e.update(env or {})
    with open(os.path.join(d, 'err.log'), 'w') as err:
        rc = subprocess.call(cmd, env=e, stdout=err, stderr=err,
                             cwd=os.path.dirname(os.path.abspath(a.bin)),
                             timeout=1800)
    return rc, d


def facts(d):
    """What a run's hash log says: frames (F records by frame, last run of
    each), the frame on screen when each rewind was let go, where play
    resumed, the last F record."""
    fr, lands, last, cur = {}, [], None, None
    top = None
    for line in open(os.path.join(d, 'hash.txt')):
        t = line.split()
        if not t:
            continue
        if t[0] == 'F':
            fr[int(t[1])] = t
            last = t
            cur = int(t[1])
            top = None
        elif t[0] in ('R', 'S'):
            if top is None:
                top = cur
            cur = int(t[1])
        elif t[0] == 'E':
            lands.append(dict(top=top, screen=cur, land=int(t[1]),
                              busy=t[2] if len(t) > 2 else '?',
                              save=t[3] if len(t) > 3 else None))
            cur = int(t[1])
    return fr, lands, last


def evt(d, name):
    out = []
    for line in open(os.path.join(d, 'fe.log'), errors='replace'):
        if line.startswith('EVT ' + name + ' '):
            out.append(dict(x.split('=', 1) for x in line.split()[2:] if '=' in x))
    return out


def check(d, extra=()):
    p = subprocess.run([sys.executable, os.path.join(HERE, 'rewind_check.py'),
                        os.path.join(d, 'hash.txt'), '--new-timeline',
                        '--allow-timing', '-1', '--save-quiet', str(QUIET)] + list(extra),
                       stdout=subprocess.PIPE, universal_newlines=True)
    return p.returncode == 0, p.stdout.strip().splitlines()


class Tally:
    def __init__(self):
        self.n = self.bad = 0

    def say(self, ok, what):
        self.n += 1
        self.bad += 0 if ok else 1
        print('  %-4s %s' % ('ok' if ok else 'FAIL', what))
        sys.stdout.flush()
        return ok


def last_video(d):
    _, _, last = facts(d)
    return last[3] if last else None


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--bin', required=True)
    p.add_argument('--bios', required=True)
    p.add_argument('--rom', required=True)
    p.add_argument('--sav', required=True)
    p.add_argument('--work', default='rewind_savetest')
    p.add_argument('--k', default='0,1,2,3,4,5,6,7')
    p.add_argument('--jobs', type=int, default=12)
    p.add_argument('--skip-window', action='store_true')
    p.add_argument('--mutants', action='store_true')
    a = p.parse_args()
    a.work = os.path.abspath(a.work)
    a.sav = os.path.abspath(a.sav)
    t = Tally()
    os.makedirs(a.work, exist_ok=True)
    src_crc = crc_file(a.sav)

    # ---- the run that never rewinds --------------------------------------
    k = S = r = None
    for k in [int(x) for x in a.k.split(',')]:
        S = boot_script() + again_script(k)
        rc, r = run(a, 'ref', S + ['evt saved', 'wait 600'], [], 12000)
        fr, _, last = facts(r)
        wrote = sorted(f for f in fr if f - 1 in fr and fr[f][7] != fr[f - 1][7])
        if rc == 0 and wrote:
            break
        print('  K=%d: the game did not save' % k)
    else:
        print('rewind_savetest: no K saved\nFAIL')
        return 1
    busy = sorted(f for f in fr if len(fr[f]) > 9 and fr[f][9] != '0')
    w0, w1 = wrote[0], wrote[-1]
    b0 = min(f for f in busy if f >= w0 - QUIET)
    c0, c1 = fr[w0 - 1][7], last[7]
    ref_file = os.path.join(r, 'game.sav')
    ref_file_crc = crc_file(ref_file)
    reads_boot = int(fr[w0 - 1][8])
    reads_all = int(last[8])
    print('K=%d: the game wrote its save in %d frames, %d..%d (chip busy in '
          '%d frames from %d on); save in memory %s -> %s; file %s; save '
          'bytes read by the game: %d before the save, %d during and after'
          % (k, len(wrote), w0, w1, len([f for f in busy if f >= b0]), b0, c0,
             c1, ref_file_crc, reads_boot, reads_all - reads_boot))
    hold = ['evt saved', 'wait 200', 'rewind 5000', 'wait 120']

    # ---- across ----------------------------------------------------------
    rc, d = run(a, 'across', S + hold, [], 20000)
    ok, out = check(d)
    for line in out:
        print('       ' + line)
    t.say(rc == 0 and ok, 'across: rewind_check passes (the save in memory was '
          'never changed by a re-run frame or a resume)')
    _, lands, last = facts(d)
    L = lands[0]['land'] if lands else -1
    t.say(lands and L < w0 and lands[0]['top'] > w1,
          'across: the rewind began at frame %s, after the save, and play '
          'resumed at %d, before it (%d..%d)' % (lands[0]['top'] if lands else '-', L, w0, w1))
    t.say(lands and all(x['save'] == c1 for x in lands) and last[7] == c1,
          'across: the save in memory at the resume and at the end is the one '
          'the game wrote (%s)' % c1)
    sf = evt(d, 'rewind_savefile')
    beg = [x for x in sf if x.get('when') == 'begin']
    end = [x for x in sf if x.get('when') == 'end']
    t.say(len(beg) == 1 and len(end) == 1 and
          all(beg[0].get(q) == end[0].get(q) for q in ('size', 'mtime_ns', 'crc'))
          and beg[0].get('crc') == ref_file_crc,
          'across: the file when the control went down and when play resumed: '
          'size %s / %s, mtime %s / %s, crc %s / %s (the never-rewound run\'s: %s)'
          % tuple([(beg[0] if beg else {}).get('size'), (end[0] if end else {}).get('size'),
                   (beg[0] if beg else {}).get('mtime_ns'), (end[0] if end else {}).get('mtime_ns'),
                   (beg[0] if beg else {}).get('crc'), (end[0] if end else {}).get('crc'),
                   ref_file_crc]))
    t.say(crc_file(os.path.join(d, 'game.sav')) == ref_file_crc,
          'across: the file at exit is the never-rewound run\'s (%s)'
          % crc_file(os.path.join(d, 'game.sav')))
    sv = (evt(d, 'rewind_save') or [{}])[-1]
    t.say(int(sv.get('rerun_writes_refused', 0)) > 0 and sv.get('unlogged') == '0'
          and int(sv.get('rerun_served', 0)) > 0,
          'across: re-run frames met %s save writes (all refused), were served '
          '%s save bytes from the read log, %s reads had nothing logged; log '
          'cut the history %s+%s times' % (
              sv.get('rerun_writes_refused'), sv.get('rerun_served'),
              sv.get('unlogged'), sv.get('rlog_trims'), sv.get('rlog_resets')))
    t.say(crc_file(a.sav) == src_crc, 'the save file given was not touched')
    top = lands[0]['top'] if lands else None

    # ---- again -----------------------------------------------------------
    rc, d2 = run(a, 'again', S + hold + again_script(k) + ['wait 600'], [], 30000)
    ok, out = check(d2)
    fr2, lands2, last2 = facts(d2)
    wrote2 = [f for f in sorted(fr2) if f > lands2[0]['land'] + 1 and f - 1 in fr2
              and fr2[f][7] != fr2[f - 1][7]] if lands2 else []
    t.say(rc == 0 and ok and wrote2,
          'again: after landing at %s the game saved again (%d frames wrote), '
          'rewind_check passes' % (lands2[0]['land'] if lands2 else '-', len(wrote2)))
    again_file = os.path.join(d2, 'game.sav')
    t.say(crc_file(again_file) == '%08x' % int(last2[7], 16),
          'again: the file at exit is the save in memory at the last frame '
          '(%s / %s)' % (crc_file(again_file), last2[7]))
    # the run that never rewinds and plays what `again` ended up playing
    after = ['wait 120'] + again_script(k) + ['wait 600']
    try:
        eq_lines, land_a, last_a = rewind_eqscript.equivalent(
            os.path.join(d2, 'hash.txt'), S, after)
        rc, de = run(a, 'again_eq', eq_lines + ['wait 5'], [], 30000)
        fre, le, _ = facts(de)
        eq_file = os.path.join(de, 'game.sav')
        wrote_e = [f for f in sorted(fre) if f - 1 in fre and fre[f][7] != fre[f - 1][7]]
        same_m = last_a in fre and fre[last_a][2:9] == last2[2:9]
        t.say(rc == 0 and not le and wrote_e and
              open(eq_file, 'rb').read() == open(again_file, 'rb').read() and same_m,
              'again: a run that never rewound and played the same buttons '
              '(frames 1..%d of the first pass, then the %d frames played '
              'after the landing) wrote its save in frames %s..%s and left '
              'the same file, byte for byte (%s / %s); the machine, picture '
              'and sound of frame %d are the same: %s'
              % (land_a, last_a - land_a, wrote_e[0] if wrote_e else '-',
                 wrote_e[-1] if wrote_e else '-', crc_file(again_file),
                 crc_file(eq_file), last_a, same_m))
    except SystemExit as e:
        t.say(False, 'again: the equivalent never-rewound run could not be '
              'built (%s)' % e)
    cold = boot_script() + ['wait 300']
    _, ca = run(a, 'cold_again', cold, [], 8000, sav=again_file)
    _, cr = run(a, 'cold_ref', cold, [], 8000, sav=ref_file)
    _, cn = run(a, 'cold_none', cold, [], 8000, no_sav=True)
    va, vr, vn = last_video(ca), last_video(cr), last_video(cn)
    fa, _, _ = facts(ca)
    wrote_cold = [f for f in sorted(fa) if f - 1 in fa and fa[f][7] != fa[f - 1][7]]
    t.say(va is not None and va != vn and not wrote_cold,
          'again: a cold start from the resulting file loads it (picture %s, '
          'not the one without a save, %s) and wrote the save in %d frames. '
          '(For information: the first save, made at frame %d against %s for '
          'this one, cold-starts to %s: %s.)'
          % (va, vn, len(wrote_cold), w0, wrote2[0] if wrote2 else '-', vr,
             'the same picture' if va == vr else 'another picture, as it may'))

    # ---- kill ------------------------------------------------------------
    for tick in (3, 400, 1500):
        rc, dk = run(a, 'kill%d' % tick, S + hold, [], 20000,
                     env={'RW_KILL_TICK': str(tick)})
        t.say(rc == 9 and crc_file(os.path.join(dk, 'game.sav')) == ref_file_crc,
              'kill at rewind tick %d (exit %d): the file is %s, the save the '
              'game wrote' % (tick, rc, crc_file(os.path.join(dk, 'game.sav'))))

    # ---- window ----------------------------------------------------------
    def win_one(tf, extra=(), tag='win'):
        n = top - tf
        rc, dw = run(a, '%s_%d' % (tag, tf),
                     S + ['evt saved', 'wait 200', 'rewind %d' % n, 'wait 90'],
                     list(extra), 20000)
        ok, out = check(dw)
        _, ld, _ = facts(dw)
        res = [tf, rc, ok, ld[0] if ld else None, out[-2] if len(out) > 1 else '', dw]
        if ok and rc == 0:
            shutil.rmtree(dw, ignore_errors=True)
        return res

    twins = {}

    def twin_of(x):
        """A release whose forward frames are not the first pass's: is the
        run, from its landing on, the never-rewound game whose RAM
        translation cache is dropped at the first frame the last restore
        re-ran differently?  Returns that frame, or None."""
        dw = x[5]
        q = []
        for line in open(os.path.join(dw, 'hash.txt')):
            u = line.split()
            if u and u[0] == 'Q':
                f = int(u[1])
                q.append((f, f in fr and fr[f][2] != u[2]))
            elif u and u[0] == 'E':
                break
        i = len(q)
        while i > 0 and q[i - 1][1]:
            i -= 1
        if i == len(q):
            return None
        cut = q[i][0]
        if cut not in twins:
            _, dt = run(a, 'twin_%d' % cut, S + ['evt saved', 'wait 600'],
                        ['--flush-ram-at', str(cut), '0'], 12000)
            twins[cut] = dt
        ok2, _ = check(dw, ['--ref-after-landing',
                            os.path.join(twins[cut], 'hash.txt')])
        if ok2:
            shutil.rmtree(dw, ignore_errors=True)
        return cut if ok2 else None

    frames = list(range(b0 - 3, w1 + QUIET + 4))
    if not a.skip_window and top:
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            res = list(ex.map(win_one, frames))
        tw = []
        for x in res:
            if x[1] == 0 and not x[2]:
                c = twin_of(x)
                if c is not None:
                    x[2] = True
                    tw.append((x[0], c))
        bad = [x for x in res if not (x[1] == 0 and x[2])]
        moved = [x for x in res if x[3] and x[3]['land'] != x[3]['screen']]
        inside = [x for x in res if x[3] and b0 <= x[3]['land'] < w1]
        t.say(not bad and moved and not inside,
              'window: %d releases, on screen frames %d..%d; %d were moved '
              '(to %s); landings inside the save, %d..%d: %d; failed: %s; '
              'played on as the never-rewound game whose RAM translation '
              'cache is dropped at frame C (release:C): %s'
              % (len(res), min(x[3]['screen'] for x in res if x[3]),
                 max(x[3]['screen'] for x in res if x[3]), len(moved),
                 sorted(set(x[3]['land'] for x in moved)), b0, w1 - 1,
                 len(inside), [x[0] for x in bad][:6],
                 ' '.join('%d:%d' % y for y in tw) or 'none'))

    # ---- mutants ---------------------------------------------------------
    if a.mutants and top:
        rc, dm = run(a, 'mut_write', S + hold, ['--rewind-sav-mutant', '2'], 20000)
        ok, out = check(dm)
        t.say(not ok, 'mutant "re-run frames write the save": rewind_check fails '
              '(%s)' % (out[-2].strip() if len(out) > 1 else '-')[:110])
        # (the mutant "re-run frames read the live save" does not show in
        # Emerald: what it reads back while saving is what it has just
        # written.  Fire Emblem shows it: tools/rewind_all.sh with
        # RW_EXTRA="--rewind-sav-mutant 1".)
        mid = [w0 + 5, (w0 + w1) // 2, w1 - 5]
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            res = list(ex.map(lambda f: win_one(f, ['--rewind-sav-mutant', '4'], 'mutwin'), mid))
        t.say(all(not x[2] for x in res),
              'mutant "a rewind may stop anywhere": releases at %s landed at '
              '%s and rewind_check fails for %d of %d'
              % (mid, [x[3]['land'] if x[3] else None for x in res],
                 sum(1 for x in res if not x[2]), len(res)))

    print('rewind_savetest: K=%d, %d checks, %d failed' % (k, t.n, t.bad))
    print('PASS' if t.bad == 0 else 'FAIL')
    return 0 if t.bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
