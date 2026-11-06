#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Screenshot conformance: run every ROM that has a reference PNG beside it
(the demo author's own screenshot) and compare the last frame, in RGB555.

  conformance.py [rom-root] [--frames N] [--filter TEXT] [--keep DIR]

Needs Pillow. The ROMs are not in this repository (see snes/README.md).
Exit status 0 even with mismatches: the list is the result.
"""
import os, subprocess, sys, tempfile
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
twin = os.environ.get("TWIN", os.path.join(here, "build", "snes_twin"))
args = sys.argv[1:]
root = "/w/snes-testroms/SNES"
frames, filt, keep = 300, "", None
opts = []
while args:
    a = args.pop(0)
    if a == "--frames": frames = int(args.pop(0))
    elif a == "--filter": filt = args.pop(0)
    elif a == "--keep": keep = args.pop(0)
    elif a == "--opt": opts += ["--opt", args.pop(0)]
    else: root = a

def q(im):
    return [(r >> 3, g >> 3, b >> 3) for r, g, b in im.convert("RGB").getdata()]

def close(x, y):
    # Snes9x maps full brightness to 30 of 31 on some channels: allow one step
    return abs(x[0] - y[0]) <= 1 and abs(x[1] - y[1]) <= 1 and abs(x[2] - y[2]) <= 1

total = same = 0
rows = []
tmp = tempfile.mkdtemp()
for d, _, files in sorted(os.walk(root)):
    for f in sorted(files):
        if not f.lower().endswith(".sfc") or filt not in os.path.join(d, f):
            continue
        ref = os.path.join(d, f[:-4] + ".png")
        if not os.path.exists(ref):
            alt = [x for x in files if x.lower().endswith(".png")]
            if len(alt) != 1: continue
            ref = os.path.join(d, alt[0])
        shot = os.path.join(tmp, "s_")
        subprocess.run([twin, os.path.join(d, f), "--frames", str(frames), "--shots", str(frames - 1),
                        "--shot-prefix", shot] + opts, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = shot + "%05d.png" % (frames - 1)
        if not os.path.exists(out):
            rows.append((f, "NO-OUTPUT", 1.0)); total += 1; continue
        a, b = Image.open(out), Image.open(ref)
        total += 1
        if a.size != b.size:
            rows.append((f, "SIZE %s vs %s" % (a.size, b.size), 1.0)); continue
        qa, qb = q(a), q(b)
        diff = sum(1 for x, y in zip(qa, qb) if not close(x, y)) / len(qa)
        if diff == 0:
            same += 1
        else:
            rows.append((f, "DIFF", diff))
            if keep:
                os.makedirs(keep, exist_ok=True)
                os.replace(out, os.path.join(keep, f[:-4] + ".png"))
        if os.path.exists(out): os.remove(out)
for f, why, diff in rows:
    print("%-38s %-10s %5.1f%% of pixels differ" % (f, why, diff * 100))
print("%d of %d match the reference screenshot (RGB555, one step per channel allowed, frame %d)" % (same, total, frames - 1))
