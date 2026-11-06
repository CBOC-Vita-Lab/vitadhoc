#!/usr/bin/env python3
"""Run every test ROM with the threads off and on and compare frame hashes and audio.

  mt_compare.py [--mt apu_thread=1,ppu_thread=1] [--frames 300] [--ref BIN] [--twin BIN]
                [--list FILE | --roms DIR...] [--jobs N] [--keep]

Frame hashes (--hashes: one fnv1a64 of the picture per frame, with size) and the
whole audio (--wav, compared by file) must be byte-identical. A pad script that
presses start / a / directions is applied to every ROM so that demos and games
that wait for input move.
Copyright CBOC-Vita. MIT licence.
"""
import argparse, glob, hashlib, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
PAD = "40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none"

def run(binary, rom, frames, opts, tmp, tag):
    h = os.path.join(tmp, tag + ".h"); w = os.path.join(tmp, tag + ".wav")
    cmd = [binary, rom, "--frames", str(frames), "--pad", PAD, "--hashes", h, "--wav", w]
    for o in opts:
        cmd += ["--opt", o]
    t = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return None, None, "timeout"
    if r.returncode != 0:
        return None, None, "exit %d: %s" % (r.returncode, r.stderr.strip()[-120:])
    return open(h, "rb").read(), open(w, "rb").read(), ""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mt", default="apu_thread=1,ppu_thread=1")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--twin", default=os.path.join(HERE, "build", "snes_twin"))
    ap.add_argument("--ref", default=None, help="binary for the reference run (default: --twin with no options)")
    ap.add_argument("--list", default=None)
    ap.add_argument("--roms", nargs="*", default=None)
    ap.add_argument("--filter", default="")
    a = ap.parse_args()
    roms = []
    if a.list:
        roms = [l.strip() for l in open(a.list) if l.strip() and not l.startswith("#")]
    else:
        roots = a.roms or [os.environ.get("SNES_ROMS", "/w/snes-testroms/SNES"),
                           os.environ.get("UNDIS_ROMS", "/w/snes-testroms/snes-test-roms/bin"),
                           os.environ.get("CHIP_ROMS", "/w/snes-out/chip"),
                           os.environ.get("OWN_ROMS", "/w/snes-roms")]
        for r in roots:
            roms += sorted(glob.glob(os.path.join(r, "**", "*.sfc"), recursive=True) + glob.glob(os.path.join(r, "**", "*.smc"), recursive=True))
    roms = [r for r in roms if a.filter in r]
    ref = a.ref or a.twin
    mt = [o for o in a.mt.split(",") if o]
    tmp = tempfile.mkdtemp(prefix="mtcmp")
    ok = bad = skip = 0
    for rom in roms:
        h1, w1, e1 = run(ref, rom, a.frames, [], tmp, "ref")
        if h1 is None:
            skip += 1; print("SKIP  %s (%s)" % (rom, e1)); continue
        h2, w2, e2 = run(a.twin, rom, a.frames, mt, tmp, "mt")
        if h2 is None:
            bad += 1; print("FAIL  %s (threaded run: %s)" % (rom, e2)); continue
        why = []
        if h1 != h2:
            l1 = h1.split(b"\n"); l2 = h2.split(b"\n")
            first = next((i for i in range(min(len(l1), len(l2))) if l1[i] != l2[i]), -1)
            why.append("frames differ (first at %d)" % first)
        if w1 != w2:
            why.append("audio differs (%d vs %d bytes)" % (len(w1), len(w2)))
        if why:
            bad += 1; print("FAIL  %s: %s" % (rom, "; ".join(why)))
        else:
            ok += 1
            if os.environ.get("VERBOSE"): print("ok    %s" % rom)
    print("%d identical, %d different, %d skipped (of %d ROMs, %d frames, threads: %s)" % (ok, bad, skip, len(roms), a.frames, a.mt))
    for f in glob.glob(os.path.join(tmp, "*")): os.remove(f)
    os.rmdir(tmp)
    sys.exit(1 if bad else 0)

main()
