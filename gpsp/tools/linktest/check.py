#!/usr/bin/env python3
"""check.py -- the linktest ROM's verdict from a gl_host run.

    gl_host --rom linktest.gba ... --peek 0x02000000:28 --peek 0x02000080:16 \
            --peek 0x020000c0:16 | python3 check.py [--frames N] [--solo]

Reads gl_host's output on stdin, checks what the run must show (README.md),
prints one line per check and exits 0 only if every one holds.

Pair run: both machines ran N frames; slot 0 is the parent and slot 1 the
child; no failed check, no time-out, no failed self-modifying-code check on
either; every phase 1..10 passed checks on both; both ports completed
multi-player and normal-mode transfers; the cable bus never overflowed.
--solo: one machine, no partner (the yield test): role 3, no failures.
"""
import re
import sys

NPH = 10


def main():
    args = sys.argv[1:]
    solo = "--solo" in args
    frames = 0
    if "--frames" in args:
        frames = int(args[args.index("--frames") + 1])
    text = sys.stdin.read()
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print("%-4s %s%s" % ("ok" if cond else "FAIL", name,
                             (": " + detail) if detail else ""))
        if not cond:
            ok = False

    peek = {}
    for m in re.finditer(r"^peek s=(\d) ([0-9a-f]{8}):((?: [0-9a-f?]{8})+)$",
                         text, re.M):
        try:
            peek[(int(m.group(1)), int(m.group(2), 16))] = [
                int(w, 16) for w in m.group(3).split()]
        except ValueError:
            check("peek s=%s readable" % m.group(1), False, m.group(0))
    slots = [0] if solo else [0, 1]
    for s in slots:
        r = peek.get((s, 0x02000000))
        okp = peek.get((s, 0x02000080))
        flp = peek.get((s, 0x020000C0))
        if not r or len(r) < 28 or not okp or not flp:
            check("slot %d result block present" % s, False)
            continue
        check("slot %d magic" % s, r[0] == 0x31544B4C, "%08x" % r[0])
        want_role = 3 if solo else s + 1
        check("slot %d role" % s, r[1] == want_role, "%d" % r[1])
        check("slot %d no failed check" % s, r[5] == 0,
              "fail=%d ok=%d first=%08x got=%08x exp=%08x" %
              (r[5], r[6], r[24], r[25], r[26]))
        check("slot %d no failed code check" % s, r[13] == 0 and r[12] > 0,
              "smc ok=%d fail=%d" % (r[12], r[13]))
        check("slot %d no time-out" % s, r[14] == 0, "%d" % r[14])
        if solo:
            check("slot %d solo checks ran" % s, okp[11] > 0, "%d" % okp[11])
            continue
        check("slot %d no failure reported by the other side" % s, r[11] == 0)
        if frames:
            check("slot %d loops" % s, r[3] >= frames // 200,
                  "%d in %d frames" % (r[3], r[2]))
        missing = [p for p in range(1, NPH + 1) if okp[p] == 0]
        failed = [p for p in range(16) if flp[p]]
        check("slot %d every phase passed checks" % s,
              not missing and not failed,
              "ok=%s missing=%s failed=%s" % (okp[1:NPH + 1], missing, failed))
        check("slot %d SI interrupts (child only)" % s,
              (r[27] > 0) == (s == 1), "%d" % r[27])
    if not solo:
        st = re.findall(r"^slot (\d) \(\w+\): frames=(\d+) .* mp=(\d+) n=(\d+)$",
                        text, re.M)
        check("both slots reported", len(st) == 2)
        for s, f, mp, n in st:
            check("slot %s ran and transferred" % s,
                  int(mp) > 0 and int(n) > 0 and
                  (not frames or int(f) >= frames - 1),
                  "frames=%s mp_xfers=%s n_xfers=%s" % (f, mp, n))
        m = re.search(r"^bus: .* overflow=(\d+)", text, re.M)
        check("bus overflow 0", bool(m) and m.group(1) == "0",
              m.group(0) if m else "no bus line")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
