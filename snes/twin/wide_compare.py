#!/usr/bin/env python3
"""Identity proof for the wide view: the normal picture, the audio and the emulation state must not change with the wide view on.

  wide_compare.py --twin BIN [--base BIN] [--frames 3000] [--mt apu_thread=1,ppu_thread=1] [--roms FILE...|--dir DIR|--list FILE]
                  [--pad SCRIPT] [--margin 70] [--state-every 50] [--jobs 1]

For every ROM it runs (all with the same scripted pad):
  A  base   : the reference binary (snes_twin built from the tree before the wide view), if --base is given
  B  off    : snes_wide_twin with the wide view off, single thread
  C  on     : snes_wide_twin --wide M, single thread
  D  on, mt : the same with the worker threads (default apu_thread=1 ppu_thread=1)
and requires: per-frame hashes of the normal picture identical in A, B, C, D; the audio byte-identical; the state hashes
(every N frames, over the whole saved state) identical in B, C, D; and in C and D the 256 centre columns of every wide frame
identical to the normal picture of that frame (the runner checks that itself, column 8 of --whashes).
Copyright CBOC-Vita. MIT licence.
"""
import argparse, glob, os, subprocess, sys, tempfile, time

PAD = "40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none"

def run(binary, rom, frames, pad, extra, tmp, tag, want_state=True):
    h = os.path.join(tmp, tag + ".h"); w = os.path.join(tmp, tag + ".wav"); s = os.path.join(tmp, tag + ".st"); wh = os.path.join(tmp, tag + ".wh")
    cmd = [binary, rom, "--frames", str(frames), "--pad", pad, "--hashes", h, "--wav", w]
    paths = {"h": h, "w": w}
    if want_state and "wide_twin" in os.path.basename(binary):
        cmd += ["--state-hash", "50", s]; paths["s"] = s
    if "wide_twin" in os.path.basename(binary):
        cmd += ["--whashes", wh]; paths["wh"] = wh
    cmd += extra
    t = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    if r.returncode != 0:
        return None, "exit %d: %s" % (r.returncode, r.stderr.strip()[-160:])
    out = {k: open(p, "rb").read() for k, p in paths.items() if os.path.exists(p)}
    out["stderr"] = r.stderr
    out["secs"] = time.time() - t
    return out, ""

def first_diff(a, b):
    la = a.split(b"\n"); lb = b.split(b"\n")
    return next((i for i in range(min(len(la), len(lb))) if la[i] != lb[i]), min(len(la), len(lb)))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--twin", required=True)
    ap.add_argument("--base", default=None)
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--pad", default=PAD)
    ap.add_argument("--margin", type=int, default=70)
    ap.add_argument("--mt", default="apu_thread=1,ppu_thread=1")
    ap.add_argument("--roms", nargs="*", default=[])
    ap.add_argument("--dir", default=None)
    ap.add_argument("--list", default=None)
    ap.add_argument("--filter", default="")
    ap.add_argument("--no-mt", action="store_true")
    a = ap.parse_args()
    roms = list(a.roms)
    if a.dir:
        for pat in ("*.sfc", "*.smc"):
            roms += sorted(glob.glob(os.path.join(a.dir, "**", pat), recursive=True))
    if a.list:
        roms += [l.strip() for l in open(a.list) if l.strip() and not l.startswith("#")]
    roms = [r for r in roms if a.filter in r]
    mt = []
    for o in a.mt.split(","):
        if o: mt += ["--opt", o]
    tmp = tempfile.mkdtemp(prefix="widecmp")
    ok = bad = skip = 0
    for rom in roms:
        name = os.path.basename(rom)
        runs = {}
        plan = []
        if a.base: plan.append(("A", a.base, []))
        plan += [("B", a.twin, []), ("C", a.twin, ["--wide", str(a.margin)])]
        if not a.no_mt: plan.append(("D", a.twin, ["--wide", str(a.margin)] + mt))
        fail = None
        for tag, binary, extra in plan:
            res, err = run(binary, rom, a.frames, a.pad, extra, tmp, tag)
            if res is None:
                fail = "%s: %s" % (tag, err); break
            runs[tag] = res
        if fail:
            if "load failed" in fail: skip += 1; print("SKIP  %s (%s)" % (name, fail)); continue
            bad += 1; print("FAIL  %s (%s)" % (name, fail)); continue
        why = []
        ref = runs["B"]
        for tag in runs:
            if tag == "B": continue
            r = runs[tag]
            if r["h"] != ref["h"]:
                why.append("%s: frames differ from B (first at %d)" % (tag, first_diff(r["h"], ref["h"])))
            if r["w"] != ref["w"]:
                why.append("%s: audio differs from B" % tag)
            if "s" in r and r["s"] != ref["s"]:
                why.append("%s: state differs from B (first at %d)" % (tag, first_diff(r["s"], ref["s"])))
        for tag in ("C", "D"):
            if tag in runs:
                lines = [l.split() for l in runs[tag]["wh"].decode().splitlines() if l.strip()]
                badc = sum(1 for l in lines if len(l) >= 8 and l[7] != "1")
                avail = sum(1 for l in lines if len(l) >= 8 and l[1] != "0")
                if badc: why.append("%s: %d wide frames whose centre differs from the normal picture" % (tag, badc))
                runs[tag]["avail"] = avail; runs[tag]["nl"] = len(lines)
        if why:
            bad += 1; print("FAIL  %s: %s" % (name, "; ".join(why)))
        else:
            ok += 1
            c = runs["C"]
            print("ok    %-34s frames=%d wide frames available %d/%d  (%.0f s on)" % (name, a.frames, c["avail"], c["nl"], c["secs"]))
        sys.stdout.flush()
        for f in glob.glob(os.path.join(tmp, "*")): os.remove(f)
    print("%d identical, %d different, %d skipped (of %d ROMs, %d frames, margin %d, threads: %s)" % (ok, bad, skip, len(roms), a.frames, a.margin, a.mt))
    os.rmdir(tmp)
    sys.exit(1 if bad else 0)

main()
