#!/usr/bin/env python3
"""test_apng.py -- the APNG writer (gpsp/vita/apng_write.c), checked with a
real decoder.

    python3 -I gpsp/vita/tests/apng/test_apng.py

Builds test_apng.c, runs it (it checks the chunk structure itself and writes
animated PNGs), then opens every file with PIL: the file is an APNG, has as
many frames as were given, each frame's pixels are exactly the picture given
(PIL composes the sub-rectangles, so this also proves the changed-rectangle
frames), and each frame lasts the delay given.  Pillow needs APNG support
(7.1 or newer)."""
import os
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
VITA = os.path.normpath(os.path.join(HERE, "..", ".."))


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_apng")
        cc = ["gcc", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
              "-O1", "-I", VITA, os.path.join(HERE, "test_apng.c"),
              os.path.join(VITA, "apng_write.c"), "-o", exe]
        r = subprocess.run(cc, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr)
            return 1
        r = subprocess.run([exe, tmp], capture_output=True, text=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode:
            return 1
        for name in ("walk", "still", "all", "gba", "one", "wide"):
            with open(os.path.join(tmp, name + ".txt")) as f:
                lines = f.read().split()
            w, h, n = map(int, lines[:3])
            delays = list(map(int, lines[3:]))
            raw = open(os.path.join(tmp, name + ".raw"), "rb").read()
            im = Image.open(os.path.join(tmp, name + ".png"))
            ok = (im.format == "PNG" and im.size == (w, h) and
                  getattr(im, "n_frames", 1) == n)
            if n > 1:
                ok = ok and im.is_animated and im.info.get("loop", 1) == 0
            for i in range(n):
                im.seek(i)
                got = im.convert("RGB").tobytes()
                want = raw[i * w * h * 3:(i + 1) * w * h * 3]
                if got != want or im.info.get("duration") != delays[i]:
                    print("FAIL %s frame %d: pixels %s, duration %s want %d" % (
                        name, i, "equal" if got == want else "DIFFER",
                        im.info.get("duration"), delays[i]))
                    ok = False
                    break
            print("%s %s: PIL reads %d frame(s) of %dx%d" % (
                "ok  " if ok else "FAIL", name, getattr(im, "n_frames", 1), w, h))
            fails += not ok
    print("test_apng.py: %s" % ("FAILED" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
