#!/usr/bin/env python3
"""dr_corpus_fields.py HOME A B [--only substr] -- for a switch that is NOT
speed-only: per ROM, WHICH parts of the machine differ between two corpus
variants, when first, and for how many frames (dr_corpus.py prints only the
first difference).

Fields (dr_host --hash --fbhash): a audio hash, n samples, r registers, pc,
i IWRAM, e EWRAM, io I/O registers, p palette, o OAM, v VRAM, fb the picture.
Columns: first frame the field differs / number of frames it differs
(of the frames both runs have).  "io only" = nothing but the I/O block ever
differs (the timer counters' snapshot at the frame's end).
"""
import os
import re
import sys

F = ["a", "n", "r", "pc", "i", "e", "io", "p", "o", "v", "fb"]


def load(p):
    rows = {}
    for l in open(p, errors="replace"):
        m = re.match(r"f=(\d+)", l)
        if not m:
            continue
        d = dict(t.split("=", 1) for t in l.split() if "=" in t)
        rows[int(m.group(1))] = d
    return rows


def main():
    home, va, vb = sys.argv[1:4]
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else ""
    ra, rb = os.path.join(home, "runs", va), os.path.join(home, "runs", vb)
    print("%-36s %6s  %s" % ("rom", "frames", "  ".join("%-9s" % f for f in ["fb", "a", "e", "i", "v", "o", "p", "r", "io"])))
    tot = {k: 0 for k in ("same", "io", "state", "audio", "pic")}
    for slug in sorted(os.listdir(ra)):
        if only and only not in slug:
            continue
        pa, pb = os.path.join(ra, slug, "hash.txt"), os.path.join(rb, slug, "hash.txt")
        if not (os.path.isfile(pa) and os.path.isfile(pb)):
            continue
        A, B = load(pa), load(pb)
        fr = sorted(set(A) & set(B))
        first = {f: None for f in F}
        cnt = {f: 0 for f in F}
        for f in fr:
            for k in F:
                if A[f].get(k) != B[f].get(k):
                    cnt[k] += 1
                    if first[k] is None:
                        first[k] = f
        cols = []
        for k in ["fb", "a", "e", "i", "v", "o", "p", "r", "io"]:
            cols.append("%-9s" % ("-" if first[k] is None else "%d/%d" % (first[k], cnt[k])))
        if all(first[k] is None for k in F):
            cls = "same"
        elif all(first[k] is None for k in F if k != "io"):
            cls = "io"
        elif first["fb"] is not None:
            cls = "pic"
        elif first["a"] is not None or first["n"] is not None:
            cls = "audio"
        else:
            cls = "state"
        tot[cls] += 1
        print("%-36s %6d  %s  [%s]" % (slug[:36], len(fr), "  ".join(cols), cls))
    print("identical %(same)d | only the I/O block %(io)d | internal state, same picture and sound %(state)d | "
          "sound differs, same picture %(audio)d | picture differs %(pic)d" % tot)


if __name__ == "__main__":
    main()
