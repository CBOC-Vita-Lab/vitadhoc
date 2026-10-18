#!/usr/bin/env python3
"""rewind_test.py -- the rewind determinism tests and the rewind measurements,
on the desktop twin (a REWIND=1 build of sdl/gpsp_sdl), headless.

    python3 tools/rewind_test.py test    --bin sdl/gpsp_sdl --bios DIR \\
            --rom GAME.gba --inputs SCRIPT [--play K] [--work DIR] [-- EXTRA...]
    python3 tools/rewind_test.py fuzz    --bin ... --rom ... --inputs ... \\
            [--play K] [--seeds 1,2,3] [--ticks T]
    python3 tools/rewind_test.py measure --bin ... --rom ... --inputs ... \\
            [--play K] [--from F]

Needs a ROM, so it is not one of tools/run_host_tests.py's suites (those run
without game data; the ROM-free half of this is tools/tests/test_fe_rewind.c,
which is one).  Runs under WSL/Linux with SDL's dummy drivers.  Build with
`make REWIND=1 EXTRA_CFLAGS=-DGPSP_PERF_RIG` (the harness's 1024-step script
limit; the player build's is 256).

test     plays the first K frames of SCRIPT, then rewinds and plays on,
         several times (300 frames back, 45, 7, 1, and once further than the
         history reaches), each time replaying the frames it went back over
         with the SAME inputs (--rewind-redo).  tools/rewind_check.py then
         asserts that every frame shown backwards is the forward frame of
         that number (picture and reversed sound) and that every replayed
         frame leaves the machine in the identical state.  PASS / FAIL.

fuzz     the abuse test.  After K scripted frames the buttons become a fixed
         pseudo-random function of the frame number, and the rewind control
         is driven by a seeded storm of presses (--seeds, default 1,2,3):
         toggling every tick, short taps, holds of seconds, holds into the
         start of the history -- several hundred transitions.  A second run
         of the same game and inputs that never rewinds is the reference:
         EVERY frame the storm run plays must match it (state, picture,
         sound), every frame it shows backwards must be the reference frame,
         and no tick may run more than the burst of frames plus one restore.
         Needs a wait/press/hold script.

measure  no rewinding: plays K frames with snapshots at 2, 6 and 10 frame
         intervals into a ring too large to wrap, and with a 6-frame interval
         into 1, 2 and 4 MiB rings, and prints bytes per second of play, the
         history each ring held at the end, and the time a snapshot took.
         --from F starts recording at frame F (skip boot and menus).

SCRIPT is an autopilot script (frontend-common/fe_autopilot.h).  It is cut
to K frames when it is made only of wait/press/hold/evt; a script with RAM
predicates or a repeat block is used whole (then K is only the --autoexit
safety net and the rewinds come after the script's last step).
"""
import argparse
import binascii
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRESS_GAP = 2          # fe_autopilot.c AP_PRESS_GAP
FPS = 16777216.0 / 280896.0

TAIL = """rewind 300
wait 420
rewind 45
wait 90
rewind 7
wait 30
rewind 1
wait 30
rewind 3000
wait 240
"""


def cut_script(text, frames):
    """First `frames` frames of a wait/press/hold script; None if the script
    has anything else in it (predicates, repeat)."""
    out, used = [], 0
    for raw in text.splitlines():
        line = raw.split('#')[0].split(';')[0].strip()
        if not line:
            continue
        t = line.split()
        if t[0] in ('evt', 'ff', 'dump'):
            continue
        if t[0] == 'wait':
            n = int(t[1], 0)
        elif t[0] == 'hold':
            n = int(t[2], 0)
        elif t[0] == 'press':
            n = (int(t[2], 0) if len(t) > 2 else 2) + PRESS_GAP
        else:
            return None
        if used + n > frames:
            left = frames - used
            if left > 0:
                if t[0] == 'wait':
                    out.append('wait %d' % left)
                else:
                    out.append('hold %s %d' % (t[1], left))
            used = frames
            break
        out.append(line)
        used += n
    if used < frames:
        out.append('wait %d' % (frames - used))
    return '\n'.join(out) + '\n'


def run(a, tag, script_text, extra, autoexit):
    work = os.path.join(a.work, tag)
    os.makedirs(work, exist_ok=True)
    spath = os.path.join(work, 'script.inputs')
    with open(spath, 'w') as f:
        f.write(script_text)
    sav = os.path.join(work, 'game.sav')
    if os.path.exists(sav):
        os.remove(sav)
    log = os.path.join(work, 'frontend.log')
    cmd = [os.path.abspath(a.bin), '--rom', os.path.abspath(a.rom),
           '--bios-dir', os.path.abspath(a.bios), '--save', sav,
           '--script', spath, '--ff', '--no-pacing', '--log', log,
           '--dump-dir', work, '--autoexit', str(autoexit)] + extra + a.extra
    if a.state:
        cmd += ['--state', os.path.abspath(a.state)]
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    with open(os.path.join(work, 'err.log'), 'w') as err:
        rc = subprocess.call(cmd, env=env, stderr=err, stdout=err,
                             cwd=os.path.dirname(os.path.abspath(a.bin)), timeout=1800)
    evt = {}
    for line in open(log, errors='replace'):
        m = re.match(r'EVT (rewind_\w+|core_prof) (.*)', line)
        if m:
            evt[m.group(1)] = dict(kv.split('=', 1) for kv in m.group(2).split() if '=' in kv)
    return rc, work, evt


def max_step(a):
    """--rewind-step K among the extra arguments: ticks go back up to K."""
    if '--rewind-stride' in a.extra:
        return a.extra[a.extra.index('--rewind-stride') + 1]
    if '--rewind-step' in a.extra:
        return a.extra[a.extra.index('--rewind-step') + 1]
    return '1'


def burst_of(a):
    if '--rewind-burst' in a.extra:
        return int(a.extra[a.extra.index('--rewind-burst') + 1])
    return 2


def tick_verdict(evt, burst):
    """The bounded-work assertion, from the build's own counters."""
    t = evt.get('rewind_tick', {})
    u = evt.get('rewind_use', {})
    ok = (int(t.get('frames_max', 99)) <= burst and
          int(t.get('restores_max', 99)) <= 1 and
          int(t.get('anomalies', 1)) == 0 and int(t.get('selfcheck', 1)) == 0 and
          # a re-run frame read save bytes the first pass had not read
          int(evt.get('rewind_save', {}).get('unlogged', 0)) == 0)
    eng = t.get('engage_us', '0/0').split('/')
    n_eng = max(int(u.get('engages', 0)), 1)
    core = evt.get('core_prof', {}).get('core', '0/0/0').split('/')
    print('ticks: %s rewind ticks, mean %s us, worst %s us (inside the rewind '
          'core %s us); a normal frame here: mean %s us, worst %s us; most '
          'work in one tick: %s frames + %s restore (bound %d + 1); first '
          'tick of a press: mean %.0f us, worst %s us; longest picture hold '
          'after a release: %s ticks; anomalies=%s selfcheck=%s -> %s' % (
              t.get('n'), t.get('mean_us'), t.get('host_max_us'),
              t.get('core_max_us'), core[0], core[-1], t.get('frames_max'),
              t.get('restores_max'), burst, int(eng[0]) / n_eng, eng[1],
              u.get('catchup_run_max'), t.get('anomalies'), t.get('selfcheck'),
              'ok' if ok else 'FAIL'))
    return ok


def use_line(evt):
    u = evt.get('rewind_use', {})
    v = evt.get('rewind_save', {})
    return ('presses=%s shown=%s stall=%s limit=%s catchup=%s restores=%s '
            'replayed=%s | save: write_frames=%s busy_frames=%s '
            'read_bytes=%s rerun_served=%s unlogged=%s refused=%s '
            'rlog_trims=%s rlog_resets=%s snaps=%s+%s snap_max=%s gaps=%s' % (
                u.get('engages'), u.get('show'), u.get('stall'), u.get('limit'),
                u.get('catchup'), u.get('restores'), u.get('replayed'),
                v.get('write_frames'), v.get('busy_frames'), v.get('read_bytes'),
                v.get('rerun_served'), v.get('unlogged'),
                v.get('rerun_writes_refused'), v.get('rlog_trims'),
                v.get('rlog_resets'), u.get('snaps_back'), u.get('snaps_fwd'),
                u.get('snap_max'), v.get('gaps')))


def cmd_test(a):
    text = open(a.inputs).read().replace('\r', '')
    body = cut_script(text, a.play)
    whole = body is None
    if whole:
        body = text if text.endswith('\n') else text + '\n'
    rc, work, evt = run(a, 'test', body + TAIL,
                        ['--rewind', '--rewind-redo', '--rewind-hash',
                         os.path.join(a.work, 'test', 'hash.txt')],
                        a.play * 4 + 20000 if whole else a.play + 6000)
    print('run: exit=%d script=%s %s' % (
        rc, 'whole' if whole else 'cut@%d' % a.play, use_line(evt)))
    if rc != 0 or 'rewind_tick' not in evt:
        print('FAIL (the run did not complete; see %s)' % work)
        return 1
    ok = tick_verdict(evt, burst_of(a))
    rc = subprocess.call([sys.executable, os.path.join(HERE, 'rewind_check.py'),
                          os.path.join(work, 'hash.txt'), '--min-redo', str(a.min_redo),
                          '--max-step', max_step(a), '--allow-cut', str(a.allow_cut),
                              '--allow-timing', str(a.allow_timing)])
    if rc == 0 and not ok:
        print('FAIL (per-tick work bound)')
        return 1
    return rc


def tap_storm(seed, ticks, steps):
    """A seeded pattern of presses of the rewind control: at most `steps`
    script steps covering about `ticks` display ticks.  Every-other-tick
    toggling, short taps, holds of seconds, and holds that run into the start
    of the history; releases are longer than holds on average so the game
    still advances."""
    r = random.Random(seed)
    out, used, held = [], 0, False
    while len(out) < steps - 80 and used < ticks:
        k = r.random()
        if k < 0.10:
            # a burst of single-tick toggles (the "DJ")
            n = r.randint(10, 40)
            lean = r.choice(((1, 1), (1, 2), (2, 1)))
            for _ in range(n):
                out.append('rewind %d' % lean[0])
                out.append('wait %d' % lean[1])
                used += lean[0] + lean[1]
            held = False
            continue
        held = not held
        if k < 0.45:
            n = 1
        elif k < 0.75:
            n = r.randint(1, 10)
        elif k < 0.97:
            n = r.randint(1, 150 if held else 400)
        else:
            n = 2500 if held else r.randint(1, 400)
        out.append(('rewind %d' if held else 'wait %d') % n)
        used += n
    out.append('wait 400')
    return '\n'.join(out) + '\n', used + 400


def cmd_fuzz(a):
    text = open(a.inputs).read().replace('\r', '')
    body = cut_script(text, a.play)
    if body is None:
        print('fuzz needs a plain wait/press/hold script')
        return 2
    worst = 0
    for seed in [int(x) for x in a.seeds.split(',')]:
        storm, ticks = tap_storm(seed, a.ticks, 1000 - body.count('\n'))
        # --rtc-pin: the cartridge clock's base is otherwise the second the
        # game was loaded, which differs between the two runs
        fz = ['--rewind', '--rewind-redo', '--rewind-fuzz', str(a.play + 1), str(seed),
              '--rtc-pin', '1700000000']
        # the truth: the same game and inputs, never rewound
        rc0, w0, _ = run(a, 'ref_%d' % seed, body + 'wait %d\n' % ticks,
                         fz + ['--rewind-hash',
                               os.path.join(a.work, 'ref_%d' % seed, 'hash.txt')],
                         a.play + ticks + 100)
        rc1, w1, e1 = run(a, 'fuzz_%d' % seed, body + storm,
                          fz + ['--rewind-hash',
                                os.path.join(a.work, 'fuzz_%d' % seed, 'hash.txt')],
                          a.play + ticks + 100)
        print('seed %d: %d script steps of the control over %d ticks; %s' % (
            seed, storm.count('\n'), ticks, use_line(e1)))
        if rc0 != 0 or rc1 != 0 or 'rewind_tick' not in e1:
            print('FAIL (a run did not complete; see %s)' % w1)
            worst = 1
            continue
        ok = tick_verdict(e1, burst_of(a))
        rc = subprocess.call([sys.executable, os.path.join(HERE, 'rewind_check.py'),
                              os.path.join(w1, 'hash.txt'), '--ref',
                              os.path.join(w0, 'hash.txt'), '--min-redo', '300',
                              '--max-step', max_step(a), '--allow-cut', str(a.allow_cut),
                              '--allow-timing', str(a.allow_timing)])
        if rc != 0 or not ok:
            worst = 1
        # The save file itself.  The two runs do not end on the same frame
        # (the one that rewinds has played fewer), and a game that writes
        # its save all the time has a different save on every frame: so each
        # file is held against the save IN MEMORY after the last frame that
        # run played -- which the checker above has just held against the
        # clean run's save of that same frame.
        for w, who in ((w0, 'clean run'), (w1, 'storm run')):
            last = None
            for line in open(os.path.join(w, 'hash.txt')):
                if line.startswith('F '):
                    last = line.split()
            sav = os.path.join(w, 'game.sav')
            have = os.path.exists(sav)
            crc = '%08x' % (binascii.crc32(open(sav, 'rb').read()) & 0xffffffff) \
                if have else None
            if have and last and crc != last[7]:
                print('FAIL: %s: the save file (%s) is not the save in memory after '
                      'its last frame %s (%s)' % (who, crc, last[1], last[7]))
                worst = 1
            else:
                print('save file, %s: %s' % (who, 'the save in memory after the last '
                      'frame, %s (%s)' % (last[1], crc) if have else 'none written'))
        if not a.keep:
            for w in (w0, w1):
                os.remove(os.path.join(w, 'hash.txt'))
    print('fuzz: %s' % ('PASS' if worst == 0 else 'FAIL'))
    return worst


def cmd_measure(a):
    text = open(a.inputs).read().replace('\r', '')
    body = cut_script(text, a.play)
    if body is None:
        body = text
    rows = []
    for interval, ring_mib in ((2, 64), (6, 64), (10, 64), (6, 1), (6, 2), (6, 4)):
        rc, work, evt = run(a, 'm_%d_%d' % (interval, ring_mib), body,
                            ['--rewind', '--rewind-at', str(a.start),
                             '--rewind-interval', str(interval),
                             '--rewind-ring', str(ring_mib << 20),
                             '--rewind-max-frames', '65536'], a.play)
        s = evt.get('rewind_stats')
        if not s:
            print('measure: run failed (%s)' % work)
            return 1
        snaps = int(s['snaps'])
        byt = int(s['bytes'])
        g_sum, g_max = (int(x) for x in s['gather_us'].split('/'))
        p_sum, p_max = (int(x) for x in s['pack_us'].split('/'))
        rows.append(dict(interval=interval, ring=ring_mib, snaps=snaps, bytes=byt,
                         max=int(s['max']), evicted=int(s['evicted']),
                         hist=int(s['history_frames']),
                         gather=g_sum / max(snaps, 1), gather_max=g_max,
                         pack=p_sum / max(snaps, 1), pack_max=p_max))
    name = os.path.basename(a.rom)
    print('%s: frames %d..%d' % (name, a.start, a.play))
    for r in rows[:3]:
        frames = r['snaps'] * r['interval']
        bps = r['bytes'] / (frames / FPS) if frames else 0
        print('  interval %2d: %6.1f KB/s  mean delta %6d B  max %6d B  -> '
              '1/2/4 MiB = %5.1f / %5.1f / %5.1f s   gather %.0f us (max %d)  '
              'pack %.0f us (max %d)' % (
                  r['interval'], bps / 1024, r['bytes'] / max(r['snaps'], 1), r['max'],
                  (1 << 20) / bps if bps else 0, (2 << 20) / bps if bps else 0,
                  (4 << 20) / bps if bps else 0, r['gather'], r['gather_max'],
                  r['pack'], r['pack_max']))
    for r in rows[3:]:
        print('  interval  6, %d MiB ring as run: history at the end %.1f s '
              '(%d frames, %d deltas dropped)' % (
                  r['ring'], r['hist'] / FPS, r['hist'], r['evicted']))
    return 0


def main():
    argv = sys.argv[1:]
    extra = []
    if '--' in argv:
        i = argv.index('--')
        extra = argv[i + 1:]
        argv = argv[:i]
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=('test', 'fuzz', 'measure'))
    p.add_argument('--bin', required=True)
    p.add_argument('--bios', required=True)
    p.add_argument('--rom', required=True)
    p.add_argument('--inputs', required=True)
    p.add_argument('--state', default=None)
    p.add_argument('--play', type=int, default=4000)
    p.add_argument('--from', dest='start', type=int, default=600)
    p.add_argument('--work', default='rewind_work')
    p.add_argument('--seeds', default='1,2,3')
    p.add_argument('--ticks', type=int, default=12000)
    p.add_argument('--keep', action='store_true', help='keep the hash logs')
    p.add_argument('--min-redo', type=int, default=300,
                   help='test: frames that must have been run again')
    p.add_argument('--allow-timing', type=int, default=0,
                   help='rewind_check.py --allow-timing')
    p.add_argument('--allow-cut', type=int, default=0,
                   help='re-run frames whose state (not save) may differ: '
                        'the frame cut, see rewind_check.py')
    a = p.parse_args(argv)
    a.extra = extra
    return {'test': cmd_test, 'fuzz': cmd_fuzz, 'measure': cmd_measure}[a.mode](a)


if __name__ == '__main__':
    sys.exit(main())
