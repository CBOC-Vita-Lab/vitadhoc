#!/usr/bin/env python3
"""Frame-hash identity of the GBA renderer between two host builds.

  video_identity.py --a BUILD_A/gbadhoc_host --b BUILD_B/gbadhoc_host
                    --rom-dir DIR [--frames 3000] [--checkpoints 15]
                    [--qemu-b] [--mask-green] [--games N,N,...]

Runs every *.gba in DIR on both binaries (hostcore/Makefile's gbadhoc_host,
x86 or armhf under qemu-arm) with the same scripted pad input and compares

  - the composed picture at CHECKPOINTS evenly spaced frames (hash of the
    decoded pixels),
  - everything the audio port was given (hash of the WAV's samples),
  - the save files (battery save and the quick-save state) at the end.

The ROMs are INPUT ONLY: they are copied into a scratch root outside the
repository, and what is printed names them g1..gN in directory order, not by
title, so the output can go into repository notes.  Typical uses:

  * the fast kernels against the scalar reference build, both x86:
      make -C vita/hostcore ARCH=x86                        (A)
      same tree built with -DVIDEO_KERNELS_SCALAR           (B)
  * the x86 dynarec against the ARM one (--qemu-b, with --mask-green: the
    ARM palette stub clears bit 15's copy in green, run_tests.sh).

Exit status 0 only when every game is identical on every checkpoint, the
audio and the saves.
"""
import warnings
warnings.simplefilter('ignore')
import argparse, glob, hashlib, os, shlex, shutil, subprocess, sys, tempfile, time
from PIL import Image


def script(frames, ncp, rom_name):
    """The pad script (a fixed pseudo-random walk over a few buttons) and the
    checkpoint frames."""
    seed = 0x2545F491
    presses = []
    f = 90
    choices = ["A", "START", "A+START", "RIGHT", "DOWN", "LEFT", "UP", "B",
               "A", "A", "UP+RIGHT", "DOWN+LEFT", "START", "A+B"]
    while f < frames - 20:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        btn = choices[(seed >> 8) % len(choices)]
        hold = 2 + (seed >> 20) % 8
        presses.append((f, "%s:%d" % (btn, hold)))
        f += 14 + (seed >> 4) % 40
    save_f, load_f = frames * 6 // 10, frames * 8 // 10
    presses.append((save_f, "SELECT+L:3"))
    presses.append((load_f, "SELECT+R:3"))
    cps = [frames * (i + 1) // ncp - 2 for i in range(ncp)]
    ents = [(fr, "%s" % b) for fr, b in presses]
    ents += [(fr, "shot=cp%02d.png" % i) for i, fr in enumerate(cps)]
    ents.sort(key=lambda e: e[0])
    return ",".join("%d:%s" % e for e in ents), cps


def masked(im):
    return bytes(b & m for px in im.getdata() for b, m in zip(px, (0xFF, 0xFB, 0xFF)))


def run(binary, qemu, rom, frames, ncp, mask, keep=None):
    root = tempfile.mkdtemp(prefix="vid-id-")
    try:
        os.makedirs(root + "/roms")
        name = os.path.basename(rom)
        shutil.copy(rom, root + "/roms/" + name)
        open(root + "/autoboot.txt", "w").write(name + "\n")
        inp, cps = script(frames, ncp, name)
        cmd = (shlex.split(qemu) if qemu else []) + [
            binary, "--root", root, "--frames", str(frames), "--input", inp,
            "--shot-dir", root + "/shots", "--wav", root + "/audio.wav"]
        os.makedirs(root + "/shots")
        t0 = time.time()
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        secs = time.time() - t0
        shots = []
        for i in range(ncp):
            try:
                im = Image.open("%s/shots/cp%02d.png" % (root, i)).convert("RGB")
                data = masked(im) if mask else im.tobytes()
                shots.append((hashlib.sha256(data).hexdigest()[:12], len(set(im.getdata())) > 1))
            except Exception:
                shots.append(("missing", False))
        try:
            wav = open(root + "/audio.wav", "rb").read()
            audio = hashlib.sha256(wav[44:]).hexdigest()[:12] + "/%d" % len(wav)
        except OSError:
            audio = "missing"
        sv = b"".join(open(f, "rb").read() for f in sorted(
            glob.glob(root + "/roms/*.sav") + glob.glob(root + "/roms/*.srm") +
            glob.glob(root + "/roms/*.st[0-4]")))
        save = hashlib.sha256(sv).hexdigest()[:12] + "/%d" % len(sv)
        return dict(rc=p.returncode, shots=shots, audio=audio, save=save, secs=secs)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--rom-dir", required=True)
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--checkpoints", type=int, default=15)
    ap.add_argument("--qemu-b", action="store_true")
    ap.add_argument("--qemu", default="qemu-arm -L /usr/arm-linux-gnueabihf")
    ap.add_argument("--mask-green", action="store_true")
    ap.add_argument("--games", default="", help="1-based, comma separated (default all)")
    ap.add_argument("--only-a", action="store_true", help="run A only (timing)")
    a = ap.parse_args()
    roms = sorted(glob.glob(a.rom_dir + "/*.gba"))
    want = [int(x) for x in a.games.split(",") if x] or list(range(1, len(roms) + 1))
    bad = 0
    print("%-4s %-7s %-8s %-8s %-6s %-6s %-14s %-7s %-7s" %
          ("game", "frames", "distinct", "picture", "audio", "save", "A save", "A secs", "B secs"))
    for n in want:
        rom = roms[n - 1]
        ra = run(a.a, None, rom, a.frames, a.checkpoints, a.mask_green)
        if a.only_a:
            print("g%d   A secs %.0f rc=%d" % (n, ra["secs"], ra["rc"]))
            continue
        rb = run(a.b, a.qemu if a.qemu_b else None, rom, a.frames, a.checkpoints, a.mask_green)
        same = sum(1 for x, y in zip(ra["shots"], rb["shots"]) if x == y and x[0] != "missing")
        live = len(set(x[0] for x in ra["shots"]))     # distinct pictures
        pic = "%d/%d" % (same, a.checkpoints)
        aud = "same" if ra["audio"] == rb["audio"] and ra["audio"] != "missing" else "DIFFER"
        sav = "same" if ra["save"] == rb["save"] else "DIFFER"
        ok = (same == a.checkpoints and aud == "same" and sav == "same"
              and ra["rc"] == 0 and rb["rc"] == 0)
        bad += not ok
        print("g%-3d %-7d %-8d %-8s %-6s %-6s %-14s %-7.0f %-7.0f %s" %
              (n, a.frames, live, pic, aud, sav, ra["save"], ra["secs"], rb["secs"],
               "" if ok else "<-- rc=%d/%d" % (ra["rc"], rb["rc"])), flush=True)
        if not ok:
            for i, (x, y) in enumerate(zip(ra["shots"], rb["shots"])):
                if x != y:
                    print("      checkpoint %d: %s vs %s" % (i, x[0], y[0]))
    sys.exit(1 if bad else 0)


main()
