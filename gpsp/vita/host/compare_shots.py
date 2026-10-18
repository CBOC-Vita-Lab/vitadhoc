#!/usr/bin/env python3
"""Compare two runs of the host test, screenshot by screenshot.

    python3 compare_shots.py [--subset] <dir a> <dir b> [max channel difference]

Used by `make armhf-check`: the x86-64 run against the same sources built for
32-bit ARM and run under qemu-arm.  The rasteriser is integer arithmetic and
the glyph atlases come from stb_truetype's floats, so the two must agree to
within a rounding step: a screenshot fails when any channel of any pixel
differs by more than the limit (default 2), or when a file is missing.
With --subset, <dir b> may hold only some of the screenshots (a run of a few
test groups): only those are compared.
"""
import os
import sys

from PIL import Image, ImageChops


def main():
    args = [x for x in sys.argv[1:] if x != '--subset']
    subset = len(args) != len(sys.argv) - 1
    a, b = args[0], args[1]
    limit = int(args[2]) if len(args) > 2 else 2
    names = sorted(n for n in os.listdir(a) if n.endswith('.png'))
    other = sorted(n for n in os.listdir(b) if n.endswith('.png'))
    if subset:
        names = [n for n in names if n in other]
    fails = 0
    for n in sorted(set(names) ^ set(other)):
        print('MISSING in one run: %s' % n)
        fails += 1
    same = close = 0
    worst = (0, '')
    for n in names:
        if n not in other:
            continue
        ia = Image.open(os.path.join(a, n)).convert('RGB')
        ib = Image.open(os.path.join(b, n)).convert('RGB')
        if ia.size != ib.size:
            print('SIZE %s: %s vs %s' % (n, ia.size, ib.size))
            fails += 1
            continue
        d = ImageChops.difference(ia, ib)
        if d.getbbox() is None:
            same += 1
            continue
        mx = max(hi for _, hi in d.getextrema())
        r, g, bl = d.split()
        m = ImageChops.lighter(ImageChops.lighter(r, g), bl)
        npx = m.size[0] * m.size[1] - m.histogram()[0]
        if mx > worst[0]:
            worst = (mx, n)
        if mx > limit:
            print('DIFF %s: %d px differ, max channel difference %d, bbox %s'
                  % (n, npx, mx, d.getbbox()))
            fails += 1
        else:
            close += 1
    print('%d screenshots: %d identical, %d within %d, %d failure(s)'
          % (max(len(names), len(other)), same, close, limit, fails))
    if worst[0]:
        print('largest channel difference %d (%s)' % worst)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
