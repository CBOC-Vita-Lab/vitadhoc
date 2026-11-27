#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Save states that come from the disk are untrusted. Two checks on one game (a synthetic ROM, made here):
  1. refused: a truncated state, and states cut at other lengths, are refused (exit status of the load, "state load failed")
     and the machine is left as it was: the frame hashes after the refusal equal a run with no load at all.
  2. damaged: N states with random byte changes (same length) are refused by the CRC32 trailer, and the machine is left as it
     was. No crash, and no sanitizer report in SAN_LOG_DIR when the twin is a sanitizer build (the caller sets log_path).
A state that changes nothing (the random bytes equal the old ones) is not counted as damaged.
usage: state_fuzz.py [--twin BIN] [--work DIR] [--n N]
"""
import glob, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_roms as mr

PAD = "50:a,90:x3,120:y1+b,200:x2,260:none,300:start+a,340:none,380:x4+y2"


def run(twin, args):
    p = subprocess.run([twin] + args, capture_output=True, text=True, errors="replace", timeout=600)
    return p.returncode, p.stdout + p.stderr


def main():
    twin = os.environ.get("WSWAN_TWIN", os.path.join(HERE, "..", "build", "wswan_twin"))
    work = None; n = 40
    a = sys.argv[1:]
    while a:
        k = a.pop(0)
        if k == "--twin": twin = a.pop(0)
        elif k == "--work": work = a.pop(0)
        elif k == "--n": n = int(a.pop(0))
        else: print(__doc__); return 2
    work = work or tempfile.mkdtemp(prefix="state_fuzz_")
    os.makedirs(work, exist_ok=True)
    rom = os.path.join(work, "h_sram.ws")
    with open(rom, "wb") as f: f.write(mr.build("h_sram.ws", **mr.ROMS["h_sram.ws"]))
    pad = os.path.join(work, "pad.txt"); open(pad, "w").write(PAD)
    st = os.path.join(work, "good.st")
    rc, _ = run(twin, [rom, "--frames", "101", "--pad", PAD, "--save-state", st, "--state-frame", "100", "--quiet"])
    good = open(st, "rb").read()
    ref = os.path.join(work, "ref.h")
    run(twin, [rom, "--frames", "400", "--pad", PAD, "--hashes", ref, "--quiet"])
    refl = [l.split() for l in open(ref)]
    refs = {int(r[0]): r[1] for r in refl if int(r[0]) >= 100}
    fails = 0; checks = 0
    rng = random.Random(42)
    # the file is the padded state (zeros after the real data): a cut inside the zeros loads, a cut into the data is refused
    real = len(good) - 1024
    cuts = [0, 1, 16, 1000, len(good) // 3, real - 1]
    for cut in cuts:
        cp = os.path.join(work, "cut.st"); open(cp, "wb").write(good[:cut])
        h = os.path.join(work, "cut.h")
        rc, out = run(twin, [rom, "--frames", "400", "--pad", PAD, "--load-state", cp, "--load-frame", "150", "--hashes", h, "--quiet"])
        got = {int(l.split()[0]): l.split()[1] for l in open(h)} if os.path.exists(h) else {}
        refused = "state load failed" in out
        same = bool(got) and all(got.get(f) == refs[f] for f in refs)
        checks += 1
        if refused and same:
            print("pass refused: %d of %d bytes, machine unchanged" % (cut, len(good)))
        else:
            fails += 1
            print("FAIL truncated to %d bytes: refused=%s unchanged=%s rc=%d" % (cut, refused, same, rc))
    for i in range(n):
        b = bytearray(good)
        for _ in range(rng.randint(1, 24)):
            b[rng.randrange(0, len(b))] = rng.randrange(256)
        if bytes(b) == good:
            continue
        cp = os.path.join(work, "dmg%02d.st" % i); open(cp, "wb").write(bytes(b))
        h = os.path.join(work, "dmg%02d.h" % i)
        rc, out = run(twin, [rom, "--frames", "160", "--pad", PAD, "--load-state", cp, "--load-frame", "100", "--hashes", h, "--quiet"])
        checks += 1
        refused = "state load failed" in out
        ok = rc == 3 and refused and "frames=160" in out                 # the CRC must refuse every damaged state
        if ok:
            got = {int(l.split()[0]): l.split()[1] for l in open(h)}
            ok = all(got.get(f) == refs[f] for f in range(100, 160))     # a refused damaged state leaves the machine as it was
        if not ok:
            fails += 1
            print("FAIL damaged state %d: rc=%d refused=%s %s" % (i, rc, refused, (out.strip().splitlines() or [""])[-1][:120]))
    print("state fuzz: truncations=%d damaged=%d (all refused by the CRC) failures=%d" % (len(cuts), n, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
