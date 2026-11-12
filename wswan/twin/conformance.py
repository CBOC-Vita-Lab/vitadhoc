#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Optional conformance run of the WonderSwan host glue on public test ROMs. Nothing is vendored or downloaded by this script.

  WSWAN_TESTSRC=<dir with clones> conformance.py [--out DIR] [--frames N]

WSWAN_TESTSRC is a directory holding clones of (any subset)
  https://github.com/FluBBaOfWard/WSTimingTest   (nasm; timingtest.asm  -> timingtest.ws)
  https://github.com/FluBBaOfWard/WSHWTest       (nasm; WSHWTest.asm    -> WSHWTest.wsc)
and, as ready-built ROMs, any .ws / .wsc / .pc2 files found anywhere below it (for example ROMs you built from
https://github.com/asiekierka/ws-test-suite with the Wonderful toolchain, which needs its own install).
These tests report on the screen only (no memory protocol like blargg's NES tests), so the script runs every ROM
headless, saves a PNG per ROM into --out (default ./conformance_out) after a scripted menu press where one is known, and
prints one line per ROM: loaded, frames run, final frame hash, audio peak. Reading the PNG is the verdict.
Needs nasm for the two clones above. Exit status is 0 whatever the results.
"""
import os, subprocess, sys, shutil

here = os.path.dirname(os.path.abspath(__file__))
twin = os.environ.get("WSWAN_TWIN", os.path.join(here, "build", "wswan_twin"))
src = os.environ.get("WSWAN_TESTSRC", "")
out, frames = "conformance_out", 1500
a = sys.argv[1:]
while a:
    k = a.pop(0)
    if k == "--out": out = a.pop(0)
    elif k == "--frames": frames = int(a.pop(0))
    else: print(__doc__); sys.exit(2)
if not src or not os.path.isdir(src):
    print("WSWAN_TESTSRC is not set (or not a directory): nothing to run. See wswan/README.md."); sys.exit(0)
os.makedirs(out, exist_ok=True)

roms = []   # (name, path, pad script)
build = [("WSTimingTest", "timingtest.asm", "timingtest.ws", ""),
         ("WSHWTest", "WSHWTest.asm", "WSHWTest.wsc", "100:down,110:none,130:a,140:none")]   # menu: "Test All"
for d, asm, rom, pad in build:
    p = os.path.join(src, d)
    if os.path.isfile(os.path.join(p, asm)):
        if not shutil.which("nasm"): print("nasm not found: skipping", d); continue
        dst = os.path.join(out, rom)
        r = subprocess.run(["nasm", "-f", "bin", "-o", dst, asm], cwd=p, capture_output=True, text=True)
        if r.returncode: print("nasm failed for", d, r.stderr[-200:]); continue
        roms.append((rom, dst, pad))
for root, _, files in os.walk(src):
    for f in sorted(files):
        if f.lower().endswith((".ws", ".wsc", ".pc2")) and os.path.join(root, f) not in [r[1] for r in roms]:
            roms.append((f, os.path.join(root, f), ""))

for name, path, pad in roms:
    png = os.path.join(out, name + ".png")
    cmd = [twin, path, "--frames", str(frames), "--shot", "%d:%s" % (frames - 2, png), "--shot-scale", "2", "--quiet"]
    if pad: cmd += ["--pad", pad]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=300)
        line = (p.stdout.strip().splitlines() or ["(no output)"])[-1]
        print("%-28s rc=%d %s" % (name, p.returncode, line[:140]))
    except subprocess.TimeoutExpired:
        print("%-28s timeout" % name)
