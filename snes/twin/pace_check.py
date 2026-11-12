#!/usr/bin/env python3
"""pace_check.py --twin BIN --roms DIR [--frames N] [--expect JSON] [--filter S]

Steady pacing must not change the game. For every ROM in DIR (the files are only read, never copied or
printed) this runs the twin twice:
  1. the normal run (single thread): frame hashes, audio, a hash of work RAM + battery RAM at the end;
  2. steady pacing's half mode with both worker threads on (--render-half: odd frames take the core's
     render-off path, exactly what the Vita player does in half mode).
It checks: the same audio, the same RAM at the end, and every rendered (even) frame identical to the same
frame of the normal run. With --expect (a bench.py result file of an earlier build) it also checks that the
normal run's frame-hash file and audio are byte-identical to the recorded ones (md5), i.e. that the player
work did not change the emulation with the menu never opened.
Copyright CBOC-Vita. MIT licence."""
import argparse, glob, hashlib, json, os, re, subprocess, sys, tempfile

def padscript(frames):
    ev = []
    for k in range(0, frames, 120):
        ev += [(k + 10, "start"), (k + 15, "none"), (k + 30, "a"), (k + 34, "none"),
               (k + 60, "right"), (k + 90, "b+right"), (k + 100, "none")]
    return ",".join("%d:%s" % e for e in ev if e[0] < frames)

def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()

ap = argparse.ArgumentParser()
ap.add_argument("--twin", required=True); ap.add_argument("--roms", required=True)
ap.add_argument("--frames", type=int, default=3000); ap.add_argument("--expect", default=None)
ap.add_argument("--filter", default=""); ap.add_argument("--ext", default="*.smc")
a = ap.parse_args()
exp = json.load(open(a.expect)) if a.expect else {}
pad = padscript(a.frames)
tmp = tempfile.mkdtemp(prefix="pacechk_")
bad = 0; n = 0
for rom in sorted(glob.glob(os.path.join(a.roms, a.ext))):
    name = os.path.basename(rom).rsplit(".", 1)[0]
    if a.filter and a.filter not in name: continue
    n += 1
    h1, w1, h2, w2 = [os.path.join(tmp, x) for x in ("h1", "w1", "h2", "w2")]
    p1 = subprocess.run([a.twin, rom, "--frames", str(a.frames), "--pad", pad, "--hashes", h1, "--wav", w1, "--peek-sum"], capture_output=True, text=True)
    p2 = subprocess.run([a.twin, rom, "--frames", str(a.frames), "--pad", pad, "--hashes", h2, "--wav", w2, "--peek-sum", "--render-half",
                         "--opt", "apu_thread=1", "--opt", "ppu_thread=1"], capture_output=True, text=True)
    if p1.returncode or p2.returncode:
        print("%-28s ERROR (twin exit %d/%d)" % (name, p1.returncode, p2.returncode)); bad += 1; continue
    r1 = re.search(r"ramsum ([0-9a-f]+)", p1.stdout); r2 = re.search(r"ramsum ([0-9a-f]+)", p2.stdout)
    full = {int(l.split()[0]): l for l in open(h1).read().split("\n") if l}
    half = [l for l in open(h2).read().split("\n") if l]
    frames_ok = len(half) == (a.frames + 1) // 2 and all(full.get(int(l.split()[0])) == l for l in half)
    audio_ok = md5(w1) == md5(w2)
    ram_ok = bool(r1 and r2 and r1.group(1) == r2.group(1))
    e = exp.get(name, {}).get("st", {})
    rec = ""
    if e:
        rec_ok = e.get("frames") == md5(h1) and e.get("audio") == md5(w1)
        rec = " recorded=%s" % ("same" if rec_ok else "DIFFERENT")
        if not rec_ok: bad += 1
    ok = frames_ok and audio_ok and ram_ok
    if not ok: bad += 1
    print("%-28s half-mode: rendered frames %s, audio %s, RAM %s%s" % (name, "same" if frames_ok else "DIFF", "same" if audio_ok else "DIFF",
          "same" if ram_ok else "DIFF", rec), flush=True)
print("%d ROMs, %d problems" % (n, bad))
sys.exit(1 if bad else 0)
