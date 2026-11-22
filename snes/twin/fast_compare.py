#!/usr/bin/env python3
"""Identity proof for the pixel fast paths and the wide view's margin strip cache: a build with them must give the pictures, the audio and the
emulation state of a build without them.

  fast_compare.py --old BIN --new BIN [--frames 3000] [--margins 96,41] [--dir DIR | --roms FILE...] [--mt apu_thread=1,ppu_thread=1] [--pad SCRIPT]
                  [--fuzz 3,1003] [--fuzz-frames 1000] [--no-fuzz-mid]

BIN is a snes_wide_twin built from the tree before the change (--old) and from the tree with it (--new). For every ROM and margin M:
  ref  : old, wide view on with margin M (single thread)
  on   : new, defaults (fast pixel paths and strip cache on)
  mt   : new, defaults with the worker threads (the margin thread draws the strips)
  nc   : new with the strip cache off (wide_cache=0)
  nf   : new with the fast pixel paths off (fast_pixels=0)
  floor: old and new at the cost ladder's floor (centre only), for the normal picture
and requires, for each run against ref: the per-frame hashes of the normal picture, the audio, the state hashes (every 50 frames, the whole saved
state) and the per-frame hashes of the whole wide picture all equal. Also run without the wide view (new against old).

--fuzz SEEDS (default 3,1003; "" = none): random register, VRAM, CGRAM and OAM writes (wide_fuzz.cpp: Mode 7 repeat modes and flips, direct colour, clip
windows, $2100 / $2106 / $2133 bits, OAM priority rotation), between frames and (unless --no-fuzz-mid) between lines. Those games are not the ones OLD was
built for, so the reference is NEW itself with the strip cache off and the per-pixel reference paths (wide_cache=0, fast_pixels=0): per ROM, seed and margin
that reference against the defaults, the worker threads, cache off, fast paths off, and a run with wide_cache_verify=1 whose "differing" count must be 0.
Copyright CBOC-Vita. MIT licence.
"""
import argparse, glob, os, subprocess, sys, tempfile, time

# The pictures of a fuzzed run hold bytes the core never draws (rows below an overscan or interlaced picture it was not asked to draw); they follow the memory
# layout and the allocation history of the process, so the plain frame hashes of fuzzed runs are not compared (the wide frame hashes, the audio and the state are), and
# the runs are made with address space randomisation off where that works (setarch -R).
NOASLR = []
try:
    if subprocess.run(["setarch", os.uname().machine, "-R", "true"], capture_output=True).returncode == 0:
        NOASLR = ["setarch", os.uname().machine, "-R"]
except OSError:
    pass

PAD = "40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none"

def run(binary, rom, frames, pad, extra, tmp, tag):
    h = os.path.join(tmp, tag + ".h"); w = os.path.join(tmp, tag + ".wav"); s = os.path.join(tmp, tag + ".st"); wh = os.path.join(tmp, tag + ".wh")
    cmd = NOASLR + [binary, rom, "--frames", str(frames), "--pad", pad, "--hashes", h, "--wav", w, "--state-hash", "50", s, "--whashes", wh] + extra
    t = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    if r.returncode != 0:
        return None, "exit %d: %s" % (r.returncode, r.stderr.strip()[-160:])
    out = {k: open(p, "rb").read() for k, p in (("h", h), ("w", w), ("s", s), ("wh", wh)) if os.path.exists(p)}
    out["stdout"] = r.stdout
    out["secs"] = time.time() - t
    return out, ""

def first_diff(a, b):
    la = a.split(b"\n"); lb = b.split(b"\n")
    return next((i for i in range(min(len(la), len(lb))) if la[i] != lb[i]), min(len(la), len(lb)))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--pad", default=PAD)
    ap.add_argument("--margins", default="96,41")
    ap.add_argument("--mt", default="apu_thread=1,ppu_thread=1")
    ap.add_argument("--roms", nargs="*", default=[])
    ap.add_argument("--dir", default=None)
    ap.add_argument("--filter", default="")
    ap.add_argument("--no-mt", action="store_true")
    ap.add_argument("--fuzz", default="3,1003")
    ap.add_argument("--fuzz-frames", type=int, default=1000)
    ap.add_argument("--no-fuzz-mid", action="store_true")
    a = ap.parse_args()
    roms = list(a.roms)
    if a.dir:
        for pat in ("*.sfc", "*.smc"):
            roms += sorted(glob.glob(os.path.join(a.dir, "**", pat), recursive=True))
    roms = [r for r in roms if a.filter in r]
    mt = []
    for o in a.mt.split(","):
        if o: mt += ["--opt", o]
    floor = ["--wopt", "policy=1", "--wopt", "cost_us=1"]
    tmp = tempfile.mkdtemp(prefix="fastcmp")
    ok = bad = 0
    for ri, rom in enumerate(roms):
        tag = "r%d" % ri         # ROM names are not printed: the report names ROMs by their number in the list
        plan = [("nowide_old", a.old, []), ("nowide_new", a.new, []), ("floor_old", a.old, ["--wide", "96"] + floor), ("floor_new", a.new, ["--wide", "96"] + floor)]
        for m in [int(x) for x in a.margins.split(",") if x]:
            plan += [("ref%d" % m, a.old, ["--wide", str(m)]), ("on%d" % m, a.new, ["--wide", str(m)]),
                     ("nc%d" % m, a.new, ["--wide", str(m), "--opt", "wide_cache=0"]), ("nf%d" % m, a.new, ["--wide", str(m), "--opt", "fast_pixels=0"])]
            if not a.no_mt:
                plan.append(("mt%d" % m, a.new, ["--wide", str(m)] + mt))
        fz = []        # (seed, margin) of the fuzz runs
        fmid = [] if a.no_fuzz_mid else ["--fuzz-mid"]
        for sd in [x for x in a.fuzz.split(",") if x]:
            for m in [int(x) for x in a.margins.split(",") if x]:
                fx = ["--wide", str(m), "--fuzz", sd] + fmid
                fz.append((sd, m))
                plan += [("fref%s_%d" % (sd, m), a.new, fx + ["--opt", "wide_cache=0", "--opt", "fast_pixels=0"]),
                         ("fon%s_%d" % (sd, m), a.new, fx),
                         ("fnc%s_%d" % (sd, m), a.new, fx + ["--opt", "wide_cache=0"]),
                         ("fnf%s_%d" % (sd, m), a.new, fx + ["--opt", "fast_pixels=0"]),
                         ("fvf%s_%d" % (sd, m), a.new, fx + ["--opt", "wide_cache_verify=1", "--summary"])]
                if not a.no_mt:
                    plan.append(("fmt%s_%d" % (sd, m), a.new, fx + mt))
        runs = {}; fail = None; secs = 0
        for t, binary, extra in plan:
            res, err = run(binary, rom, a.fuzz_frames if t.startswith(("fref", "fon", "fnc", "fnf", "fvf", "fmt")) else a.frames, a.pad, extra, tmp, t)
            if res is None:
                fail = "%s: %s" % (t, err); break
            runs[t] = res; secs += res["secs"]
        if fail:
            bad += 1; print("FAIL  #%d (%s)" % (ri, fail)); continue
        why = []
        pairs = [("nowide_new", "nowide_old"), ("floor_new", "floor_old")]
        for m in [int(x) for x in a.margins.split(",") if x]:
            pairs += [("on%d" % m, "ref%d" % m), ("nc%d" % m, "ref%d" % m), ("nf%d" % m, "ref%d" % m)]
            if not a.no_mt: pairs.append(("mt%d" % m, "ref%d" % m))
        for sd, m in fz:
            for k in ("on", "nc", "nf", "vf") + (() if a.no_mt else ("mt",)):
                pairs.append(("f%s%s_%d" % (k, sd, m), "fref%s_%d" % (sd, m)))
            o = runs["fvf%s_%d" % (sd, m)]["stdout"]
            wl = [l for l in o.splitlines() if l.startswith("wcache:")]
            if not wl or "differing=0" not in wl[0]: why.append("fuzz %s margin %d: strip cache check: %s" % (sd, m, wl[0] if wl else "no wcache line"))
            if fmid and not any(l.startswith("fuzzmid: lines=") and "lines=0 " not in l and "writes=0" not in l for l in o.splitlines()):
                why.append("fuzz %s margin %d: no write between lines happened (this twin has no CBOC_TEST_LINE_HOOK core?)" % (sd, m))
        for x, y in pairs:
            rx, ry = runs[x], runs[y]
            for k, label in (("h", "frames"), ("w", "audio"), ("s", "state"), ("wh", "wide frames")):
                if k == "h" and x.startswith(("fref", "fon", "fnc", "fnf", "fvf", "fmt")):
                    continue        # plain frame hashes of a fuzzed run: see NOASLR above (the wide frame hashes carry the centre check)
                if k in rx and k in ry and rx[k] != ry[k]:
                    why.append("%s vs %s: %s differ (first at line %d)" % (x, y, label, first_diff(rx[k], ry[k])))
        for t, r in runs.items():
            if "wh" in r:
                lines = [l.split() for l in r["wh"].decode().splitlines() if l.strip()]
                badc = sum(1 for l in lines if len(l) >= 8 and l[7] != "1")
                if badc: why.append("%s: %d wide frames whose centre differs from the normal picture" % (t, badc))
        if why:
            bad += 1; print("FAIL  #%d: %s" % (ri, "; ".join(why[:6])))
        else:
            ok += 1; print("ok    #%d  %d runs identical (%d frames, %.0f s)" % (ri, len(plan), a.frames, secs))
        sys.stdout.flush()
        for f in glob.glob(os.path.join(tmp, "*")): os.remove(f)
    print("%d ROMs identical, %d different (of %d, %d frames, margins %s, threads: %s)" % (ok, bad, len(roms), a.frames, a.margins, "off" if a.no_mt else a.mt))
    os.rmdir(tmp)
    sys.exit(1 if bad else 0)

main()
