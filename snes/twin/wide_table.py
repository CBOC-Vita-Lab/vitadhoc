#!/usr/bin/env python3
"""Per game table of the wide view: coverage, Auto per side, fallback reasons, compose cost, where the cost ladder settles.

  wide_table.py --twin snes_wide_twin --dir ROMDIR [--margin 41] [--frames 3000] [--scale 800] [--hudcheck] [--filter TEXT]

Per ROM it runs the twin (all with the owner's scripted pad):
  A  --wide M --summary                    (Auto on, ladder off): real share of the margin pixels, per side ambient frames, line reasons, compose time
  B  A with --wopt hud_excl=0              (the coverage without excluding status bar lines from Auto)
  C  A with --wopt policy=1 --wopt cost_scale=S   (the cost ladder; S percent stands in for a slower machine): the rung it settles on
  H  (--hudcheck) --wopt auto=0 with hud_excl 1 and 0: the composed frames must be identical (hud_excl only changes Auto's decision,
     never a pixel of the picture)
Prints a markdown table.  Copyright CBOC-Vita. MIT licence.
"""
import argparse, os, re, subprocess, sys, tempfile

PAD = "40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none"

def run(twin, rom, frames, margin, extra, whash=None):
    cmd = [twin, rom, "--frames", str(frames), "--pad", PAD, "--wide", str(margin), "--summary"] + extra
    if whash: cmd += ["--whashes", whash]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
    return r.returncode, r.stdout

def parse(out):
    d = {}
    m = re.search(r"wide: frames=(\d+) available=(\d+) mean real px L=([\d.]+) R=([\d.]+), adapter (\d+) us/frame", out)
    if m: d["frames"], d["avail"], d["L"], d["R"], d["us"] = int(m[1]), int(m[2]), float(m[3]), float(m[4]), int(m[5])
    m = re.search(r"censuspx: (\d+) (\d+) (\d+) (\d+) soft (\d+) reveal (\d+)", out)
    if m:
        tm, ln, bd, un, sf, rv = map(int, m.groups())
        tot = tm + ln + bd + un + sf + rv
        d["real"] = 100.0 * (tm + ln + bd + rv) / tot if tot else 0
        d["soft"] = 100.0 * sf / tot if tot else 0
    m = re.search(r"sides: ambient left (\d+) right (\d+) of (\d+)", out)
    if m: d["amb"] = (100.0 * int(m[1]) / max(1, int(m[3])), 100.0 * int(m[2]) / max(1, int(m[3])))
    m = re.search(r"ladder: rung=(\d+) chip=(\d+) compose_us=(\d+) rung_changes=(\d+)", out)
    if m: d["rung"], d["chip"], d["cus"], d["rch"] = map(int, m.groups())
    m = re.search(r"^reasons:(.*)$", out, re.M)
    d["reasons"] = " ".join(m[1].replace(" of ", " ").split(";")[:3]).strip() if m else ""
    return d

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--twin", required=True); ap.add_argument("--dir", required=True)
    ap.add_argument("--margin", type=int, default=41); ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--scale", type=int, default=800); ap.add_argument("--hudcheck", action="store_true")
    ap.add_argument("--filter", default="")
    a = ap.parse_args()
    roms = sorted(f for f in os.listdir(a.dir) if f.lower().endswith((".smc", ".sfc")) and a.filter.lower() in f.lower())
    print("| game | chip | real share %% (hud_excl 1 / 0) | soft %% | Auto ambient frames L / R %% | main limits of the margin (lines) | compose us/frame (x86, full margin) | ladder rung at x%d (steps) | hud_excl pixels |" % a.scale)
    print("|---|---|---|---|---|---|---|---|---|")
    for g in roms:
        rom = os.path.join(a.dir, g)
        rc, o1 = run(a.twin, rom, a.frames, a.margin, [])
        _, o0 = run(a.twin, rom, a.frames, a.margin, ["--wopt", "hud_excl=0"])
        _, oc = run(a.twin, rom, a.frames, a.margin, ["--wopt", "policy=1", "--wopt", "cost_scale=%d" % a.scale])
        d1, d0, dc = parse(o1), parse(o0), parse(oc)
        hc = "-"
        if a.hudcheck:
            with tempfile.TemporaryDirectory() as t:
                run(a.twin, rom, a.frames, a.margin, ["--wopt", "auto=0"], t + "/a")
                run(a.twin, rom, a.frames, a.margin, ["--wopt", "auto=0", "--wopt", "hud_excl=0"], t + "/b")
                hc = "identical" if open(t + "/a", "rb").read() == open(t + "/b", "rb").read() else "DIFFERENT"
        chip = {0: "-", 1: "Super FX", 2: "SA-1"}.get(dc.get("chip", 0), "?")
        print("| %s | %s | %.1f / %.1f | %.1f | %.0f / %.0f | %s | %d | %d (%d) | %s |" % (
            os.path.splitext(g)[0], chip, d1.get("real", 0), d0.get("real", 0), d1.get("soft", 0), *d1.get("amb", (0, 0)), d1.get("reasons", ""),
            d1.get("us", 0), dc.get("rung", -1), dc.get("rch", -1), hc))
        sys.stdout.flush()

main()
