#!/usr/bin/env python3
"""hot_budget.py ORDER NM [--strict] -- does the pinned HOT core still fit?

  ORDER  psp/layout/hot_order.txt (its header records the hot budget the
         order was generated for; the hot functions are the lines before
         "# ---- warm tail")
  NM     psp-nm -S -n of the ELF just linked

The hot order is generated for the function sizes of one build
(tools/layout/regen.sh).  When a hot function grows, everything linked behind
it moves and the hot core no longer fits the bytes it was budgeted
(docs/GBA-LINK-MIRROR.md 24: update_gba +688 bytes cost the link pair 8,400
I-cache misses a frame).  This prints the core's size today against the
budget and each function that grew; exit 1 with --strict when it is over.
tools/build.sh runs it on every LAYOUT_PIN=1 build (HOT_BUDGET_STRICT=1 to
make an overrun fail the build).
"""
import re
import sys


def main():
    order, nm = sys.argv[1], sys.argv[2]
    strict = '--strict' in sys.argv
    budget = None
    hot = []
    for l in open(order):
        m = re.search(r'hot budget (\d+) bytes', l)
        if m:
            budget = int(m.group(1))
        if l.startswith('# ---- warm tail'):
            break
        f = l.split('#', 1)[0].split()
        if len(f) >= 3 and not f[0].startswith('@'):
            hot.append((f[0], int(f[2])))
    size = {}
    for l in open(nm, errors='replace'):
        f = l.split()
        if len(f) == 4 and f[2] in 'TtWw' and f[3] not in size:
            size[f[3]] = int(f[1], 16)
    if budget is None or not hot:
        print('hot core: no budget recorded in %s' % order)
        return 0
    # the last hot function is the head-hot one (update_scanline): only its
    # head was budgeted, so it is compared by growth, not counted whole
    then = sum(s for _, s in hot[:-1])
    now = sum(size.get(n, s) for n, s in hot[:-1])
    grew = [(n, s, size[n]) for n, s in hot if n in size and size[n] != s]
    over = now - then
    print('hot core: %d bytes now, %d when the order was generated (budget %d): %s'
          % (now, then, budget, 'OK' if over <= 0 else 'OVER by %d bytes' % over))
    for n, a, b in grew:
        print('  %-40s %6d -> %6d (%+d)' % (n, a, b, b - a))
    if over > 0:
        print('  regenerate the order for this build: tools/layout/regen.sh, then rebuild')
        return 1 if strict else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
