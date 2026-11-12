#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Long scripted runs of the twin with in-run save-state round trips and render-off stretches.

For every ROM: FRAMES frames (default 4000) with a scripted pad: the selftest's opening presses (start, a) and then a seeded
random sequence of pad changes; two in-run rewinds (--rt): the state is saved at AT, the run goes on to AT+BACK, the state is
loaded again and the frames AT..AT+BACK-1 are replayed and compared with the first pass (picture and audio); a stretch with
drawing switched off (--render-range) in the middle. The check: exit status 0, rewinds done with zero mismatches, and no
sanitizer report written (ASAN_OPTIONS / UBSAN_OPTIONS log_path is set by the caller: any file in SAN_LOG_DIR is a finding).

  soak.py [--twin BIN] [--work DIR] [--frames N] [--roms DIR] [--public DIR] [--private FILE]

--roms: the synthetic ROMs (make_roms.py output; built here when missing). --public: public test ROMs, if any (*.ws/*.wsc/*.pc2 are
run). --private: the owner's acceptance game, used only as an input file: its name and contents are never printed.
"""
import glob, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_roms as mr

KEYS = ["x1", "x2", "x3", "x4", "y1", "y2", "y3", "y4", "a", "b", "start", "none", "x1+a", "y2+b", "x3+y4", "none"]


def pad_script(seed, frames):
    """The selftest's opening (start at 120, a at 200 for the private game) and then random changes every 2..30 frames."""
    rng = random.Random(seed)
    ev = [(120, "start"), (140, "none"), (200, "a"), (220, "none")]
    f = 300
    while f < frames:
        ev.append((f, rng.choice(KEYS)))
        f += rng.randint(2, 30)
    ev.sort()
    return ",".join("%d:%s" % e for e in ev)


def san_logs(d):
    return sorted(glob.glob(os.path.join(d, "*"))) if d and os.path.isdir(d) else []


def main():
    twin = os.environ.get("WSWAN_TWIN", os.path.join(HERE, "..", "build", "wswan_twin"))
    work = None; frames = 4000; roms_dir = None; public = None; private = os.environ.get("WSWAN_PRIVATE_ROM", "")
    a = sys.argv[1:]
    while a:
        k = a.pop(0)
        if k == "--twin": twin = a.pop(0)
        elif k == "--work": work = a.pop(0)
        elif k == "--frames": frames = int(a.pop(0))
        elif k == "--roms": roms_dir = a.pop(0)
        elif k == "--public": public = a.pop(0)
        elif k == "--private": private = a.pop(0)
        else: print(__doc__); return 2
    work = work or tempfile.mkdtemp(prefix="soak_")
    os.makedirs(work, exist_ok=True)
    san_dir = os.environ.get("SAN_LOG_DIR", "")
    roms = []
    if roms_dir is None:
        roms_dir = os.path.join(work, "roms"); os.makedirs(roms_dir, exist_ok=True)
        for n, kw in mr.ROMS.items():
            with open(os.path.join(roms_dir, n), "wb") as f: f.write(mr.build(n, **kw))
        with open(os.path.join(roms_dir, "orient_table.ws"), "wb") as f:
            rng = random.Random(7)
            f.write(mr.build_orient_table([(rng.choice([0, 1, 2, 3]), rng.choice([0, 2, 4, 6])) for _ in range(frames + 1)]))
    for p in sorted(glob.glob(os.path.join(roms_dir, "*.ws"))):
        roms.append(("synthetic/" + os.path.basename(p), p))
    if public and os.path.isdir(public):
        for root, _, files in os.walk(public):
            for f in sorted(files):
                if f.lower().endswith((".ws", ".wsc", ".pc2")):
                    roms.append(("public/" + f, os.path.join(root, f)))
    if private and os.path.isfile(private):
        roms.append(("private-acceptance", private))
    with open(os.path.join(work, "roms.txt"), "w") as lf:     # the inputs, for the API comparison (arm_compare.sh)
        for label, path in roms: lf.write(path + "\n")
    fails = 0
    for i, (label, path) in enumerate(roms):
        d = os.path.join(work, "run%02d" % i); os.makedirs(d, exist_ok=True)
        pad = os.path.join(d, "pad.txt")
        with open(pad, "w") as f: f.write(pad_script(100 + i, frames))
        at1 = frames // 4; at2 = frames // 2
        args = [twin, path, "--frames", str(frames), "--pad-file", pad, "--wav", os.path.join(d, "a.wav"),
                "--hashes", os.path.join(d, "h.txt"), "--rt", "%d:300" % at1, "--rt", "%d:500" % at2,
                "--render-range", "%d:%d" % (frames // 3, frames // 3 + 200), "--quiet"]
        if label.startswith("synthetic/") and "nosave" not in path and "dconan" not in path and "orient_table" not in path:
            args += ["--srm", os.path.join(d, "g.srm"), "--backup-dir", os.path.join(d, "bk")]
        before = set(san_logs(san_dir))
        p = subprocess.run(args, capture_output=True, text=True, errors="replace", timeout=3600)
        out = p.stdout.strip().splitlines()
        rew = [l for l in out if l.startswith("rewind:")]
        last = out[-1] if out else ""
        ok = p.returncode == 0 and rew
        if rew:
            r = dict(x.split("=") for x in rew[0].split()[1:])
            ok = ok and r["mismatch_video"] == "0" and r["mismatch_audio"] == "0" and r["pending"] == "0" \
                and r["rewound"] == "2" and int(r["compared_audio"]) > 0 and int(r["compared_video"]) > 0
            detail = "rewinds %s, replayed frames compared: picture %s, audio %s; mismatches %s/%s" % (
                r["rewound"], r["compared_video"], r["compared_audio"], r["mismatch_video"], r["mismatch_audio"])
        else:
            detail = "no rewind line"
        new_san = sorted(set(san_logs(san_dir)) - before)
        if new_san:
            ok = False
            detail += "; SANITIZER REPORT FILES: %d" % len(new_san)
        if "peak" in last:
            peak = last.split("audio_peak=")[1].split()[0]
            clip = last.split("audio_clipped=")[1].split()[0]
            detail += "; audio peak %s clipped %s" % (peak, clip)
        print("%-4s %-28s rc=%d frames=%d %s" % ("ok" if ok else "FAIL", label, p.returncode, frames, detail))
        if not ok:
            fails += 1
            print("     stderr tail: " + (p.stderr.strip().splitlines() or [""])[-1][:200])
    print("soak: runs=%d failures=%d (frames %d, work %s)" % (len(roms), fails, frames, work))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
