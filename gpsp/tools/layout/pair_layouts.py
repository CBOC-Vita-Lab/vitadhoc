#!/usr/bin/env python3
"""pair_layouts.py -- layouts file for pair_sim.sh: PSP builds of the GBA link
pair, both core instances placed (docs/GBA-LINK-MIRROR.md 24).

  pair_layouts.py --home ~/linkspeed/drprof --twin VARIANT --prof RUN --out NAME
                  --elf name=psp.nm@A_JIT_BASE_HEX ...

For each PSP build: the static map (twin address -> PSP address by function
name, lp_map.py; instance B's gbab_* functions are in both binaries), the
PSP-only per-frame path, and three translation-cache bases: instance A's
(from the console's `EVT jit_cache base=`), and instance B's ROM and RAM
caches, which are static arrays in the module (gbab_*_translation_cache_static
in the nm + the module's load base).  A PRX's nm is relative; --load (default
0x08804000, what `EVT gbadual_jit b_base=` confirms on a PSP-3000) is added.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp_map  # noqa: E402


def sym_addr(nm, name):
    for ln in open(nm, errors='replace'):
        f = ln.split()
        if f and f[-1] == name:
            return int(f[0], 16)
    raise SystemExit('%s: no symbol %s' % (nm, name))


def split_instances(rows):
    """Static functions exist twice in a pair binary (instance B's objects are
    the same code with only the GLOBAL names prefixed gbab_).  Tag each copy
    @B when the nearest uniquely named symbol is a gbab_ one, so the two
    copies pair up across binaries whatever order the linker put them in."""
    import bisect
    import collections
    cnt = collections.Counter(r[2] for r in rows)
    uniq = sorted((r[0], r[2]) for r in rows if cnt[r[2]] == 1)
    ua = [u[0] for u in uniq]
    out = []
    for a, sz, n in rows:
        if cnt[n] == 2 and uniq:
            i = bisect.bisect_left(ua, a)
            cand = [uniq[j] for j in (i - 1, i) if 0 <= j < len(uniq)]
            near = min(cand, key=lambda u: abs(u[0] - a))
            if near[1].startswith('gbab_'):
                n = n + '@B'
        out.append((a, sz, n))
    return out


def _report(name, counts, static, placed, tsyms):
    tot = sum(counts[x] for x in static)
    instr = sum(counts[t] for t in placed)
    lw = {}
    for x in static:
        if x not in placed:
            t = tsyms.at(x)
            n = t[2] if t else '?'
            lw[n] = lw.get(n, 0) + counts[x]
    top = sorted(lw.items(), key=lambda kv: -kv[1])[:5]
    print('%s: placed %.1f %% of static executions; not placed: %s'
          % (name, 100.0 * instr / max(tot, 1),
             ', '.join('%s %.1f%%' % (n, 100.0 * v / max(tot, 1)) for n, v in top)),
          file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--home', required=True)
    ap.add_argument('--twin', required=True)
    ap.add_argument('--prof', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--elf', action='append', required=True)
    ap.add_argument('--load', default='0x08804000')
    ap.add_argument('--frame-path')
    ap.add_argument('--solo', action='store_true',
                    help='a single-player run (SOLO=1 pair_sim.sh): dr_host twin, no instance B')
    ap.add_argument('--tier', default='large', choices=('large', 'small'),
                    help='large: instance A at the heap base given after @ (3000/Go); '
                         'small: A in its static arrays (the PSP-1000 tier)')
    ap.add_argument('--pin-a', type=lambda x: int(x, 0), default=None,
                    help='A offset mod 8 KiB (session pins); default: as linked / as given')
    ap.add_argument('--pin-b', type=lambda x: int(x, 0), default=None)
    ap.add_argument('--tag', default='', help='suffix for the layout names')
    ap.add_argument('--append', action='store_true', help='add to an existing layouts file')
    ap.add_argument('--sweep', type=lambda x: int(x, 0), default=0,
                    help='also every (A offset, B offset) mod 8 KiB in this step: what '
                         'gba_link_pin_{a,b}_* choose at run time')
    a = ap.parse_args()
    home = os.path.expanduser(a.home)
    load = int(a.load, 16)
    out = os.path.join(home, 'sim', a.out)
    os.makedirs(out, exist_ok=True)
    prof = os.path.join(home, 'runs', a.prof)
    info = open(os.path.join(prof, 'run.info')).read()
    ranges = []
    for tok in info.split():
        if tok.startswith('jit=') or ',jit=' in tok:
            for part in tok.split(','):
                lo, hi = part[4:].split(':')
                ranges.append((int(lo, 16), int(hi, 16)))
    tnm = os.path.join(home, 'bin', '%s_%s.nm' % ('dr_host' if a.solo else 'dr_pair', a.twin))
    tmem = os.path.join(home, 'bin', '%s_%s.members' % ('dr_host' if a.solo else 'dr_pair', a.twin))
    tsyms = lp_map.Syms(split_instances(lp_map.read_nm(tnm)))
    excluded = lp_map.members_of(tmem, lp_map.RENDERER_OBJS)
    counts, _ = lp_map.read_counts(os.path.join(prof, 'counts.txt'))
    static = sorted(x for x in counts
                    if not any(lo <= x < hi for lo, hi in ranges))
    rows = []
    for spec in a.elf:
        name, rest = spec.split('=', 1)
        path, _, base = rest.partition('@')
        psyms = lp_map.Syms(split_instances(lp_map.read_nm(path)))
        placed, pairs, lost = lp_map.place_static(tsyms, psyms, excluded, static)
        mp = os.path.join(out, '%s.map' % name)
        with open(mp, 'w') as fh:
            for t in sorted(placed):
                fh.write('%x %x\n' % (t, placed[t] + load))
        fp = '-'
        if a.frame_path:
            fp = os.path.join(out, '%s.frame' % name)
            with open(fp, 'w') as fh:
                for x in lp_map.frame_lines(psyms, lp_map.read_frame_path(a.frame_path)):
                    fh.write('%x\n' % (x + load))
        if a.tier == 'small':
            ja = (sym_addr(path, 'rom_translation_cache_static') + load) & ~63
        else:
            ja = int(base or '9635500', 16) & ~63
        if a.pin_a is not None:
            ja = (ja & ~8191) + (a.pin_a & 8191 & ~63)
        name = name + a.tag
        if a.solo:
            rows.append('%s %s %x %s' % (name, mp.replace(home, '/w'), ja,
                                         fp.replace(home, '/w') if fp != '-' else '-'))
            for oa in range(0, 8192, a.sweep) if a.sweep else ():
                rows.append('%s@a%04x %s %x %s' % (name, oa, mp.replace(home, '/w'),
                                                   (ja & ~8191) + oa,
                                                   fp.replace(home, '/w') if fp != '-' else '-'))
            _report(name, counts, static, placed, tsyms)
            continue
        jb = (sym_addr(path, 'gbab_rom_translation_cache_static') + load) & ~63
        jc = (sym_addr(path, 'gbab_ram_translation_cache_static') + load) & ~63
        if a.pin_b is not None:
            jb = (jb & ~8191) + (a.pin_b & 8191 & ~63)
            jc = (jc & ~8191) + (a.pin_b & 8191 & ~63)
        rows.append('%s %s %x %s %x %x' % (name, mp.replace(home, '/w'), ja,
                                           fp.replace(home, '/w') if fp != '-' else '-',
                                           jb, jc))
        if a.sweep:
            for oa in range(0, 8192, a.sweep):
                for ob in range(0, 8192, a.sweep):
                    rows.append('%s@a%04x_b%04x %s %x %s %x %x' % (
                        name, oa, ob, mp.replace(home, '/w'), (ja & ~8191) + oa,
                        fp.replace(home, '/w') if fp != '-' else '-',
                        (jb & ~8191) + ob, (jc & ~8191) + ob))
        _report(name, counts, static, placed, tsyms)
    lay = os.path.join(out, 'pair.lay')
    with open(lay, 'a' if a.append else 'w') as fh:
        fh.write('\n'.join(rows) + '\n')
    print(lay.replace(home, '/w'))


if __name__ == '__main__':
    main()
