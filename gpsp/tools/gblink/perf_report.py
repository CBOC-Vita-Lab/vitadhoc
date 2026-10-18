#!/usr/bin/env python3
"""perf_report.py -- GB link smoothness, per arm and console (docs/GB-LINK.md
section 14): what a session's frames cost and why they were late.

    python tools/gblink/perf_report.py <hw_loop --logs dir | log files...>
        [--per-run] [--vs ARM]

Every column comes from telemetry EVERY build emits -- frame_hist, core_prof,
sess_cost, fps, gblink_stats, audio_ring -- so a build from before
claude/gblink-perf compares with one after it.  (perf1's first report mixed
EVT gblink_win figures for the new builds with these for the old ones, and
the two measure differently; hardware run perf1, GB-LINK.md 14.8.)  The
windows are the 600-frame ones while the machines run (gblink_start ..
gblink_done), the first (the session's start) dropped.

  fps        emulated frames a second (EVT fps)
  work       the loop's frame work, ms (sess_cost frame=)
  core       the session step on FRAME steps, ms (core_prof; absorbed waits
             are excluded since claude/gblink-perf)
  ovb/1k     frames whose work exceeded the budget, per 1000 (frame_hist)
  late/1k    frames that missed their vblank, per 1000 (frame_hist)
  >20/1k     frames with 20 ms or more of work (the old hash spike's size)
  stl/1k     display frames lost waiting for the partner's buttons
  stmax      longest run of them
  armin      lowest audio ring level (frames of 32768 Hz; EVT audio_ring)
  starv%     share of output that was silence (EVT audio_ring starved=)
From EVT gblink_win, where a build has it (else '-'):
  hashmx     longest periodic sync hash, ms
  absrb/1k   late inputs waited for and still on time
  und/min    gaps (the ring ran dry) a minute
  starv%     (when the build has no EVT audio_ring: from gblink_win starved=)
  pg/min     32 KiB ROM pages read from the stick a minute, both GBAs
             (a GBA link-cable session whose cart does not fit: the PSP-1000)

--vs ARM: the per-window difference of `core` against ARM, windows matched
by index (every arm of a rig plays the same scripted trade, so window i is
the same scene in every run) -- the least noisy comparison of two builds.
"""
import argparse
import glob
import os
import re
import statistics as st
import sys


def kv(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def first(v):
    return int(v.split('/')[0])


def second(v):
    return int(v.split('/')[1])


def parse(path):
    """-> info, windows (dicts), gblink_win records"""
    run = False
    info = {'model': '?', 'arm': '?', 'role': '?'}
    wins, gw, cur = [], [], {}
    prev_st = 0
    with open(path, errors='replace') as fh:
        for ln in fh:
            if ln.startswith('EVT psp_model'):
                info['model'] = kv(ln).get('name', '?')
            elif ln.startswith('EVT rig run_id'):
                info['arm'] = kv(ln).get('arm', '?')
            elif ln.startswith('EVT gblink_begin'):
                info['role'] = kv(ln).get('role', '?')
            elif ln.startswith('EVT gblink_start '):
                run = True
                continue
            elif (ln.startswith('EVT gblink_done') or
                  ln.startswith('EVT gblink_failed') or
                  ln.startswith('EVT gblink_mute on state=3')):
                # not gblink_end_at: the last window's lines straddle it in
                # either order, and dropping it dropped the trade's busiest
                # window from some runs and not others (perf1 G: 67 vs 158
                # over budget per 1000 for one binary)
                run = False
            if not run:
                if ln.startswith('EVT gblink_stats'):
                    prev_st = int(kv(ln).get('stalls', 0))
                continue
            if ln.startswith('EVT gblink_win '):
                gw.append(kv(ln))
            elif ln.startswith('EVT frame_hist'):
                d = kv(ln)
                b = [int(x) for x in d['b'].split(',')]
                cur.update(fwin=int(d['win']), ovb=int(d['ovb']),
                           late=int(d['late']), over20=sum(b[6:]))
            elif ln.startswith('EVT sess_cost'):
                cur['work'] = first(kv(ln)['frame'])
            elif ln.startswith('EVT core_prof'):
                cur['core'] = first(kv(ln)['core'])
            elif ln.startswith('EVT fps '):
                cur['fps'] = float(kv(ln)['emu'])
            elif ln.startswith('EVT audio_ring'):
                d = kv(ln)
                cur.update(ar_min=int(d['min']), starved=int(d['starved']))
            elif ln.startswith('EVT gblink_stats'):
                d = kv(ln)
                s = int(d['stalls'])
                cur['stalls'] = s - prev_st
                prev_st = s
                cur['streak'] = int(d['streak_max'])
                if 'fwin' in cur and 'core' in cur:
                    wins.append(cur)
                cur = {}
    return info, wins[1:] if len(wins) > 2 else wins, \
        gw[1:] if len(gw) > 2 else gw


def summarize(wins, gw):
    if not wins:
        return None
    fr = sum(x['fwin'] for x in wins) or 1
    secs = sum(x['fwin'] / max(x.get('fps', 59.7), 1.0) for x in wins) or 1.0
    ring = any('ar_min' in x for x in wins)
    out = dict(
        fps=st.mean(x.get('fps', 0.0) for x in wins),
        work=st.mean(x.get('work', 0) for x in wins) / 1000,
        core=st.mean(x['core'] for x in wins) / 1000,
        ovb=1000.0 * sum(x['ovb'] for x in wins) / fr,
        late=1000.0 * sum(x['late'] for x in wins) / fr,
        over20=1000.0 * sum(x['over20'] for x in wins) / fr,
        stalls=1000.0 * sum(x.get('stalls', 0) for x in wins) / fr,
        stmax=max(x.get('streak', 0) for x in wins),
        armin=min(x.get('ar_min', 0) for x in wins) if ring else None,
        starved=(100.0 * sum(x.get('starved', 0) for x in wins) / (secs * 44100)
                 if ring else None),
        hashmx=None, absorbed=None, under=None, pg=None)
    if gw:
        gfr = sum(int(x['frames']) for x in gw) or 1
        gsecs = sum(int(x['dt_ms']) for x in gw) / 1000.0 or 1.0
        out.update(hashmx=max(second(x['hash_us']) for x in gw) / 1000,
                   absorbed=1000.0 * sum(int(x['absorbed']) for x in gw) / gfr,
                   under=60.0 * sum(int(x['under']) for x in gw) / gsecs)
        if not ring and all('starved' in x for x in gw):
            out['starved'] = (100.0 * sum(int(x['starved']) for x in gw) /
                              (gsecs * 44100))
        if all('pg' in x for x in gw):
            out['pg'] = 60.0 * sum(first(x['pg']) + second(x['pg'])
                                   for x in gw) / gsecs
    return out


COLS = ('fps', 'work', 'core', 'ovb', 'late', 'over20', 'stalls', 'stmax',
        'armin', 'starved', 'hashmx', 'absorbed', 'under', 'pg')
FMT = {'fps': '%5.1f', 'work': '%5.2f', 'core': '%5.2f', 'ovb': '%6.1f',
       'late': '%6.1f', 'over20': '%6.1f', 'stalls': '%6.2f', 'stmax': '%5d',
       'armin': '%5d', 'starved': '%6.2f', 'hashmx': '%6.2f',
       'absorbed': '%6.1f', 'under': '%6.1f', 'pg': '%6.1f'}
HEAD = ('  fps  work  core ovb/1k late/1k >20/1k stl/1k stmax armin starv% '
        'hashmx absrb/1k und/min pg/min')


def fmt(s):
    out = []
    for c in COLS:
        v = s.get(c)
        w = len(FMT[c] % 0)
        out.append('-'.rjust(w) if v is None else FMT[c] % v)
    return ' '.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--per-run', action='store_true')
    ap.add_argument('--vs', help='window-matched core difference against '
                    'this arm')
    a = ap.parse_args()
    files = []
    for p in a.paths:
        if os.path.isdir(p):
            files += sorted(glob.glob(os.path.join(p, 'auto*-*-*.log')))
        else:
            files.append(p)
    groups = {}
    for f in files:
        m = re.match(r'auto(\d+)-(\w+)-(host|join)\.log$', os.path.basename(f))
        info, wins, gw = parse(f)
        s = summarize(wins, gw)
        if not s:
            continue
        arm = m.group(2) if m else info['arm']
        role = m.group(3) if m else info['role']
        groups.setdefault((arm, role, info['model']), []).append(
            (os.path.basename(f), s, wins))
    print('arm role model    runs ' + HEAD)
    for (arm, role, model), runs in sorted(groups.items()):
        if a.per_run:
            for name, s, _w in runs:
                print('%-3s %-4s %-8s %-4s %s   %s' % (arm, role, model, '1',
                                                    fmt(s), name))
        mean = {}
        for c in COLS:
            vals = [s[c] for _n, s, _w in runs if s.get(c) is not None]
            if not vals:
                mean[c] = None
            elif c in ('stmax', 'armin'):
                mean[c] = max(vals) if c == 'stmax' else min(vals)
            else:
                mean[c] = st.mean(vals)
        print('%-3s %-4s %-8s %4d %s' % (arm, role, model, len(runs), fmt(mean)))
    if a.vs:
        print('\nwindow-matched core (core_prof) against arm %s, us; '
              'mean / median over matched windows' % a.vs)
        for (arm, role, model), runs in sorted(groups.items()):
            ref = [r for (ar, ro, _m), rr in groups.items()
                   if ar == a.vs and ro == role for r in rr]
            if arm == a.vs or not ref:
                continue
            n = min(min(len(w) for _n, _s, w in runs),
                    min(len(w) for _n, _s, w in ref))
            d = [st.mean(w[i]['core'] for _n, _s, w in runs) -
                 st.mean(w[i]['core'] for _n, _s, w in ref) for i in range(n)]
            if d:
                print('%-3s %-4s %+6.0f / %+6.0f  over %d windows' % (
                    arm, role, st.mean(d), st.median(d), n))
    return 0


if __name__ == '__main__':
    sys.exit(main())
