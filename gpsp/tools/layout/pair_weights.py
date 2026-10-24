#!/usr/bin/env python3
"""pair_weights.py -- profile weights for gen_layout.py from PAIR runs
(pair_sim.sh ... - : counts.txt + run.info), in profile_weights.txt's format.

  pair_weights.py --home ~/linkspeed/drprof --twin VARIANT --psp-nm psp.nm
                  --runs RUN ... [--merge tools/layout/profile_weights.txt]
                  [--scale S] --out FILE

Each run is normalised to 1.0 of its placed static executions and summed,
as gen_layout.py weights does for the solo fixtures.  Instance B's functions
(gbab_*, and the second copy of every static function) are folded into the
name of the function they are copies of: gen_layout.py places B's hot path
in the same order as A's, after it.  --merge adds an existing weights file
(the solo profile) so one order serves both; --scale multiplies the pair's
share first.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp_map  # noqa: E402
from pair_layouts import split_instances  # noqa: E402


def fold(n):
    if n.endswith('@B'):
        n = n[:-2]
    return n[5:] if n.startswith('gbab_') else n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--home', required=True)
    ap.add_argument('--twin', required=True)
    ap.add_argument('--psp-nm', required=True)
    ap.add_argument('--runs', nargs='+', required=True)
    ap.add_argument('--merge')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    home = os.path.expanduser(a.home)
    tsyms = lp_map.Syms(split_instances(lp_map.read_nm(os.path.join(home, 'bin', 'dr_pair_%s.nm' % a.twin))))
    psyms = lp_map.Syms(split_instances(lp_map.read_nm(a.psp_nm)))
    excluded = lp_map.members_of(os.path.join(home, 'bin', 'dr_pair_%s.members' % a.twin),
                                 lp_map.RENDERER_OBJS)
    total = collections.Counter()
    for run in a.runs:
        d = os.path.join(home, 'runs', run)
        ranges = []
        for tok in open(os.path.join(d, 'run.info')).read().split():
            if tok.startswith('jit='):
                for part in tok.split(','):
                    lo, hi = part[4:].split(':')
                    ranges.append((int(lo, 16), int(hi, 16)))
        counts, _ = lp_map.read_counts(os.path.join(d, 'counts.txt'))
        static = sorted(x for x in counts if not any(lo <= x < hi for lo, hi in ranges))
        placed, pairs, lost = lp_map.place_static(tsyms, psyms, excluded, static)
        per = collections.Counter()
        for ad in placed:
            per[fold(pairs[tsyms.at(ad)[2]][2])] += counts[ad]
        tot = sum(per.values()) or 1
        for k, v in per.items():
            total[k] += a.scale * v / tot
    if a.merge:
        for l in open(a.merge):
            l = l.split('#', 1)[0].split()
            if len(l) == 2:
                total[l[0]] += float(l[1])
    with open(a.out, 'w', newline='\n') as fh:
        fh.write('# pair_weights.py: %s%s\n' % (' '.join(a.runs),
                 (' + ' + os.path.basename(a.merge)) if a.merge else ''))
        for k, v in sorted(total.items(), key=lambda kv: (-kv[1], kv[0])):
            if v >= 1e-6:
                fh.write('%s %.7f\n' % (k, v))
    print('wrote %s (%d functions)' % (a.out, len(total)))


if __name__ == '__main__':
    main()
