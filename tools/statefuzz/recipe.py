#!/usr/bin/env python3
"""Make a regression recipe from a base state and a case that crashed or hung a state loader.  MIT licence.

    harness ROM DIR --base-out base.st          # the machine's own state at the warm point (what the fuzzer mutates)
    python3 tools/statefuzz/recipe.py base.st crash-md9-178.st > name.recipe
    harness ROM DIR --case name.recipe          # loads base + recipe, runs frames: must load or be refused, never crash or hang

A recipe is text: "SIZE n" when the size differs, then one line "OFFSET HEXBYTES" per run of changed bytes (checksum bytes are
bytes like any other).  It holds only the damage, never a game's state, so it can be committed; the base is made by the harness from a
test ROM that is public (see tools/statefuzz/regress/README.md).  A case that differs from the base in more than --max bytes is refused:
it is a state of another game or point, not a small damage.
"""
import sys


def main(argv):
    mx = 4096
    args = []
    i = 1
    while i < len(argv):
        if argv[i] == "--max":
            mx = int(argv[i + 1]); i += 2
        else:
            args.append(argv[i]); i += 1
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    base = open(args[0], "rb").read()
    case = open(args[1], "rb").read()
    out = ["# regression recipe: base state of the harness' warm point + these bytes"]
    if len(case) != len(base):
        out.append("SIZE %d" % len(case))
    changed = 0
    i = 0
    n = len(case)
    while i < n:
        if i < len(base) and case[i] == base[i]:
            i += 1
            continue
        j = i
        # join runs separated by fewer than 8 equal bytes
        while j < n:
            k = j
            while k < n and k < len(base) and case[k] == base[k] and k - j < 8:
                k += 1
            if k < n and (k >= len(base) or case[k] != base[k]):
                j = k + 1
            else:
                break
        out.append("%d %s" % (i, case[i:j].hex()))
        changed += j - i
        i = j
    if changed > mx:
        sys.stderr.write("%d bytes differ (limit %d): not a small damage of this base\n" % (changed, mx))
        return 1
    sys.stdout.write("\n".join(out) + "\n")
    sys.stderr.write("%d bytes in %d lines\n" % (changed, len(out) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
