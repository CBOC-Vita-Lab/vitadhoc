#!/usr/bin/env python3
"""pair_misses.py RUN.misses.txt PSP.nm A_JIT_HEX [--load 0x08804000] [--top N]

Who takes the I-cache misses (pair_sim.sh writes misses.txt for the FIRST
layout of its layouts file): per PSP function, and for the translation
caches per instance and area (the emitter's stubs = the first `--stubs`
bytes of a ROM cache, default 11248; translated blocks = the rest).
Misses per frame and x 215.3 ns.
"""
import argparse
import bisect
import collections


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('misses')
    ap.add_argument('nm')
    ap.add_argument('ajit')
    ap.add_argument('--load', default='0x08804000')
    ap.add_argument('--stubs', type=int, default=11248)
    ap.add_argument('--top', type=int, default=30)
    a = ap.parse_args()
    load = int(a.load, 16)
    syms = []
    named = {}
    for l in open(a.nm, errors='replace'):
        f = l.split()
        if len(f) == 4 and f[2] in 'TtWw':
            syms.append((int(f[0], 16) + load, int(f[1], 16), f[3]))
        if len(f) >= 3:
            try:
                named[f[-1]] = int(f[0], 16) + load
            except ValueError:
                pass
    syms.sort()
    ad = [s[0] for s in syms]
    aj = int(a.ajit, 16)
    regions = [('A rom cache', aj, 10 << 20), ('A ram cache', aj + (10 << 20), 512 << 10)]
    if 'gbab_rom_translation_cache_static' in named:
        regions += [('B rom cache', named['gbab_rom_translation_cache_static'], 2 << 20),
                    ('B ram cache', named['gbab_ram_translation_cache_static'], 512 << 10)]
    frames = 1
    by = collections.Counter()
    for l in open(a.misses):
        if l.startswith('#'):
            frames = int(l.split()[2]) or 1
            continue
        x, n = l.split()
        x = int(x, 16)
        n = int(n)
        name = None
        for rn, lo, size in regions:
            if lo - 64 <= x < lo + size:
                off = x - lo
                name = '%s: %s' % (rn, 'stubs' if 'rom' in rn and off < a.stubs else 'blocks')
                break
        if name is None:
            i = bisect.bisect_right(ad, x + 63) - 1
            name = syms[i][2] if i >= 0 and x < syms[i][0] + syms[i][1] + 64 else '?'
        by[name] += n
    tot = sum(by.values())
    print('%d misses/frame = %.2f ms' % (tot / frames, tot / frames * 215.3e-6))
    for k, v in by.most_common(a.top):
        print('  %8.0f %5.1f%%  %s' % (v / frames, 100.0 * v / tot, k))


if __name__ == '__main__':
    main()
