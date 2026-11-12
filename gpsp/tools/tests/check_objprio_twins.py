#!/usr/bin/env python3
"""check_objprio_twins.py -- the objprio.gba cartridge on both host twins.

Why this exists.  run_tests.sh once reported a DIFFERENT picture of
testrom/gbadhoc-objprio.gba on the x86 twin and the armhf twin.  There was no
undefined behaviour behind it: the armhf binary it ran predated video.cc's
OBJ_EXACT (obj_exact_build, the hardware's per-pixel OBJ priority), so it drew
the cartridge the old way (cases 1 and 3 behind the BG) while the freshly built
x86 twin drew it the new way.  Two such pictures only "agree" when both
binaries are stale, so comparing them is not enough.  This check

  1. looks for obj_exact_build in the binaries' objects (nm; skipped with a
     note when the build directory has none), so a stale twin is named as such;
  2. runs the cartridge on each twin and checks the colours the cartridge's
     header comment promises at the sample points (the same list as
     app_tests.py t_objprio), not only that the two pictures are alike;
  3. compares the two pictures, with the lowest green bit masked as run_tests.sh
     does (the x86 stub copies bit 15 of a palette entry into green's sixth bit
     and the ARM stub clears it).

  check_objprio_twins.py [--x86 build/x86] [--arm build/armhf] [--frames 130]

Exit 0: all three hold.  The armhf twin runs under qemu-arm (QEMU env var).
"""
import argparse, os, subprocess, sys, tempfile, shutil
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.normpath(os.path.join(HERE, "..", "..", "vita", "hostcore"))
QEMU = os.environ.get("QEMU", "qemu-arm -L /usr/arm-linux-gnueabihf").split()

# (x, class, what): the cartridge's header comment, rows 20..27; the GBA
# picture is drawn 3.4x (default config) with its left edge at 72.
WANT = [
    (22, "B", "case 1: A alone, behind the BG"),
    (26, "R", "case 1: A where B is transparent, drawn at B's priority"),
    (30, "G", "case 1: B's own pixel"),
    (62, "R", "case 2: C in front of the BG"),
    (66, "R", "case 2: C over D"),
    (70, "B", "case 2: D behind the BG"),
    (102, "R", "case 3: E under the affine F's transparent texture"),
    (110, "B", "case 3: nothing there"),
    (152, "B", "case 4: the prio-2 sprite behind the BG"),
    (172, "B", "case 4: the prio-0 sprite's transparent half"),
    (176, "G", "case 4: the prio-0 sprite's own half"),
]

def run(binary, qemu, rom, frames, shot_at, png):
    root = tempfile.mkdtemp(prefix="objprio-")
    try:
        os.makedirs(os.path.join(root, "roms"))
        shutil.copy(rom, os.path.join(root, "roms"))
        open(os.path.join(root, "autoboot.txt"), "w").write(os.path.basename(rom) + "\n")
        cmd = (QEMU if qemu else []) + [binary, "--root", root, "--frames", str(frames),
                                        "--shot", "%d:%s" % (shot_at, png)]
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return r.returncode
    finally:
        shutil.rmtree(root, ignore_errors=True)

def colour(im, x, y):
    px = im.getpixel((int(72 + 3.4 * (x + 0.5)), int(3.4 * (y + 0.5))))
    return "K" if max(px) < 100 else "RGB"[px.index(max(px))]

def stale(build):
    """True/False: does this build's renderer have OBJ_EXACT?  None: unknown."""
    objs = [os.path.join(build, "core", "video.o"),
            os.path.join(build, "render", "video_r.o")]
    nm = "arm-linux-gnueabihf-nm" if "armhf" in build else "nm"
    seen = False
    for o in objs:
        if not os.path.exists(o):
            continue
        seen = True
        out = subprocess.run([nm, "-C", o], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True).stdout
        if "obj_exact_build" in out or "render_scanline_objs_exact" in out:
            return False
    return True if seen else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--x86", default=os.path.join(HOST, "build", "x86"))
    ap.add_argument("--arm", default=os.path.join(HOST, "build", "armhf"))
    ap.add_argument("--frames", type=int, default=130)
    a = ap.parse_args()
    rom = os.path.join(HOST, "testrom", "gbadhoc-objprio.gba")
    if not os.path.exists(rom):
        print("no testrom/gbadhoc-objprio.gba (make -C vita/hostcore/testrom): skipped")
        return 0
    bad = 0
    pics = {}
    tmp = tempfile.mkdtemp(prefix="objprio-png-")
    for name, build, q in (("x86", a.x86, False), ("armhf", a.arm, True)):
        binary = os.path.join(build, "gbadhoc_host")
        if not os.path.exists(binary):
            print("%s: no %s" % (name, binary)); bad += 1; continue
        s = stale(build)
        if s:
            print("%s: STALE build -- %s has no obj_exact_build; rebuild it" % (name, build))
            bad += 1
        png = os.path.join(tmp, name + ".png")
        rc = run(binary, q, rom, a.frames, a.frames - 10, png)
        if rc != 0 or not os.path.exists(png):
            print("%s: run failed (rc %d)" % (name, rc)); bad += 1; continue
        im = Image.open(png).convert("RGB")
        pics[name] = im
        for y in (20, 24, 27):
            for x, c, what in WANT:
                got = colour(im, x, y)
                if got != c:
                    print("%s: row %d x %d: %s: got %s, want %s" % (name, y, x, what, got, c))
                    bad += 1
    if len(pics) == 2:
        m = lambda p: (p[0], p[1] & 0xFB, p[2])
        d = sum(1 for p, q in zip(pics["x86"].getdata(), pics["armhf"].getdata()) if m(p) != m(q))
        print("x86 vs armhf: %d pixels differ (lowest green bit masked)" % d)
        bad += d != 0
    shutil.rmtree(tmp, ignore_errors=True)
    print("OK" if not bad else "FAILED (%d)" % bad)
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
