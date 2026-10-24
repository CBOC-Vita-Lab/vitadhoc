#!/usr/bin/env python3
"""gen_msc.py -- Mario Kart: Super Circuit's fixture scripts: the menus from
scripts/msc.spec (pair_script.py) plus a CLOSED-LOOP driver for the race,
written in plain autopilot steps (no new engine op).

A blind input script cannot drive a lap, and a blind script tuned on the
desktop's x86 dynarec would not survive the PSP's MIPS dynarec anyway (the
CPU karts' RNG differs between engines).  So each machine steers its own
kart from RAM, every few frames:

  RAM (pret-style map found with gl_host --ram-dump; kart struct 0x1A0 B):
    kart k heading  u16 0x03003BEE + k*0x1A0  (0 = north, +RIGHT, 65536 = 360)
    kart k X, Y     u16 0x03003B9A + k*0x1A0, +4 (track pixels)
    kart k progress u8  0x03003BE0 + k*0x1A0  (0 -> 4 after lap 1 -> 12 after
                    lap 2 -> 0x34/0x3C finished: bit 0x20 = finished)
    k = 0 the parent's (host's) kart, 1 the child's (join's), 2-7 CPU.

  the line: scripts/msc_line.json -- one clean lap of a CPU kart (kart 3,
  Peach Circuit, 100 cc), recorded on the desktop mirror.

  the controller: the map is cut into 128 px cells; every cell the line
  passes through gets a target heading -- pure pursuit: from the cell's
  centre toward the line point LOOKAHEAD px past the line's nearest point.
  The loop body tests the kart's cell with `ifram` on X and Y (mask 0xFF80),
  then its heading sector (8 sectors, mask 0xE000; MSC_SECTORS=16 is finer
  but does not fit the engine's 1024 steps) against the target:
  turn RIGHT or LEFT for 3 frames when it is off by a sector or more, and
  always `hold A 2` (accelerate) at the end of the body.  `ifram` is
  equality-only, so a circular range of sectors is written as aligned
  power-of-two blocks (one mask each).  Every step is RAM-gated, so the
  same script drives on either engine.

  python gen_msc.py      (writes scripts/msc_host.txt, scripts/msc_join.txt)
"""
import json
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CELL = 128
LOOKAHEAD = int(os.environ.get("MSC_LOOKAHEAD", "250"))
ITER = int(os.environ.get("MSC_ITER", "4000"))   # loop iterations (1-6 frames each)
GOAL = int(os.environ.get("MSC_GOAL", "4"), 0)   # 4 = lap 1, 0x20 = the finish
KART = 0x1A0
H0, X0, ST0, SPD0 = 0x03003BEE, 0x03003B9A, 0x03003BE0, 0x03003C56


NSEC = int(os.environ.get("MSC_SECTORS", "16"))     # 8 or 16
SBITS = {8: 3, 16: 4}[NSEC]
SSHIFT = 16 - SBITS


def blocks(start, n):
    """circular sector range [start, start+n) of NSEC -> aligned (mask, val)"""
    out = []
    s = start % NSEC
    full = (0xFFFF << SSHIFT) & 0xFFFF
    while n > 0:
        size = 1
        while size * 2 <= n and s % (size * 2) == 0 and s + size * 2 <= NSEC:
            size *= 2
        mask = full & ~((size - 1) << SSHIFT) & 0xFFFF
        out.append((mask, (s << SSHIFT) & 0xFFFF))
        s = (s + size) % NSEC
        n -= size
    return out


def line():
    pts = json.load(open(os.path.join(HERE, "scripts", "msc_line.json")))
    # resample every ~8 px
    out = [pts[0]]
    for p in pts[1:] + pts[:1]:
        q = out[-1]
        d = math.hypot(p[0] - q[0], p[1] - q[1])
        steps = int(d // 8)
        for i in range(1, steps + 1):
            out.append((q[0] + (p[0] - q[0]) * i / max(steps, 1),
                        q[1] + (p[1] - q[1]) * i / max(steps, 1)))
    return out


BAND = int(os.environ.get("MSC_BAND", "60"))
# the defaults are the tuned fixture (desktop sweep: both karts finish)
STRAIGHT = int(os.environ.get("MSC_STRAIGHT", "2"))
SHARP = float(os.environ.get("MSC_SHARP", "999"))
TURN_SHARP = os.environ.get("MSC_TURN_SHARP", "%s 3")   # coast through hairpins


def brake_cell(gx, gy):
    """MSC_BRAKE="x0,y0,x1,y1;..." (cells, inclusive): turn with the brake
    in those rectangles whatever the bend says -- Peach Circuit's hairpins,
    which a kart reaches at full speed"""
    for r in os.environ.get("MSC_BRAKE", "5,0,7,3;10,7,13,11").split(";"):
        if not r:
            continue
        x0, y0, x1, y1 = (int(v) for v in r.split(","))
        if x0 <= gx <= x1 and y0 <= gy <= y1:
            return True
    return False


def forced(gx, gy, T):
    """MSC_FORCE="x0,y0,x1,y1=T;...": a fixed target sector in those cells
    -- the first hairpin's approach, where pure pursuit from a cell left of
    the line says "north" until it is too late to turn"""
    for r in os.environ.get("MSC_FORCE", "").split(";"):
        if not r:
            continue
        box, t = r.split("=")
        x0, y0, x1, y1 = (int(v) for v in box.split(","))
        if x0 <= gx <= x1 and y0 <= gy <= y1:
            return int(t)
    return T


def cells(pts):
    """every cell within BAND px of the line (a kart a little off the line
    still steers back), each with its pure-pursuit target sector"""
    L = len(pts)
    seg = 8.0
    ahead = int(LOOKAHEAD / seg)
    tab = {}
    reach = (BAND + CELL * 0.71) ** 2
    for gx in range(0, 2048 // CELL):
        for gy in range(0, 2048 // CELL):
            cx, cy = gx * CELL + CELL / 2, gy * CELL + CELL / 2
            if min((p[0] - cx) ** 2 + (p[1] - cy) ** 2 for p in pts) > reach:
                continue
            c = (gx, gy)
            # nearest line point to the cell centre, then look ahead from it
            j = min(range(L), key=lambda k: (pts[k][0] - cx) ** 2 + (pts[k][1] - cy) ** 2)
            tx, ty = pts[(j + ahead) % L]
            ang = math.atan2(tx - cx, -(ty - cy))      # 0 = north, clockwise
            # floor, not round: "straight" is the whole sector [T, T+1) the
            # target angle lies in -- round() would centre it 1/2 sector
            # clockwise of the target and the kart drifts right into walls
            T = int((ang % (2 * math.pi)) / (2 * math.pi) * NSEC) % NSEC
            # sharp corner ahead (the line turns more than SHARP degrees
            # within the lookahead): turn with the brake held
            a0 = math.atan2(pts[(j + 2) % L][0] - pts[j][0], -(pts[(j + 2) % L][1] - pts[j][1]))
            a1 = math.atan2(pts[(j + ahead + 2) % L][0] - pts[(j + ahead) % L][0],
                            -(pts[(j + ahead + 2) % L][1] - pts[(j + ahead) % L][1]))
            bend = abs((math.degrees(a1 - a0) + 180) % 360 - 180)
            T = forced(gx, gy, T)
            tab[c] = (T, bend > SHARP or brake_cell(gx, gy))
    return tab


DEBUG = os.environ.get("MSC_DEBUG") == "1"
TURN = os.environ.get("MSC_TURN", "A+%s 3")      # % LEFT / RIGHT


def act(cond, hold, tag):
    if DEBUG:
        return [cond[:-1] + "2", hold, "evt " + tag]
    return [cond, hold]


def steer(cell, haddr):
    T, sharp = cell
    turn = TURN_SHARP if sharp else TURN
    """steps for one cell: right = the half circle behind T (T-N/2..T-1),
    left = T+1..T+N/2-1; T itself = straight"""
    st = []
    h = NSEC // 2
    for mask, val in blocks(T - h, h):
        st += act("ifram 2 0x%08X 0x%04X 0x%04X 1" % (haddr, mask, val), "hold " + turn % "RIGHT", "R%d" % T)
    for mask, val in blocks(T + 1, h - 1):
        st += act("ifram 2 0x%08X 0x%04X 0x%04X 1" % (haddr, mask, val), "hold " + turn % "LEFT", "L%d" % T)
    return st


def race(k, tab):
    h, x = H0 + k * KART, X0 + k * KART
    y = x + 4
    body = []
    cols = {}
    for (cx, cy), T in sorted(tab.items()):
        cols.setdefault(cx, []).append((cy, T))
    for cx, rows in sorted(cols.items()):
        inner = []
        rows = dict(rows)
        done = set()
        for cy in sorted(rows):
            if cy in done:
                continue
            T = rows[cy]
            # an aligned pair of rows with the same target: one test
            if cy % 2 == 0 and rows.get(cy + 1) == T:
                ymask, done = 0xFF00, done | {cy, cy + 1}
            else:
                ymask, done = 0xFF80, done | {cy}
            st = steer(T, h)
            inner.append("ifram 2 0x%08X 0x%04X 0x%04X %d" % (y, ymask, cy * CELL, len(st)))
            inner += st
        body.append("ifram 2 0x%08X 0xFF80 0x%04X %d" % (x, cx * CELL, len(inner)))
        body += inner
    body.append("hold A %d" % STRAIGHT)
    # stuck (speed below 64: against a wall, or the start): back off
    rec = os.environ.get("MSC_RECOVER", "")
    if rec:
        body += ["ifram 2 0x%08X 0xFFC0 0x0000 1" % (SPD0 + k * KART), "hold " + rec]
    # drive only until this kart reaches GOAL (its progress byte runs
    # 0 -> 4 after lap 1 -> 12 after lap 2 -> 0x34/0x3C at the finish; the
    # default goal is bit 0x04, lap 1); after it, press nothing: the kart
    # coasts, and a finished race's results are never clicked through into
    # the cup's next race
    st0 = ST0 + k * KART
    g = GOAL
    body = (["ifram 1 0x%08X 0x%02X 0x00 %d" % (st0, g, len(body))] + body +
            ["ifram 1 0x%08X 0x%02X 0x%02X 1" % (st0, g, g), "wait 1"])
    # after the loop: both karts' lap 1, on THIS console's copy of the race
    # (RAM-gated: whichever kart is slower, both consoles wait for it; a
    # kart that never makes it is an ap_fail, i.e. a failed fixture)
    marks = []
    for who, kk in (("host", 0), ("join", 1)):
        a = ST0 + kk * KART
        marks += ["waitram 1 0x%08X 0x04 0x04 12000" % a, "evt %s_lap1" % who,
                  "logram %s_progress 1 0x%08X" % (who, a)]
    pre = int(os.environ.get("MSC_PRE", "0"))   # sweep perturbation only
    return (["# ---- the race: closed-loop driver (gen_msc.py), %d cells ----" % len(tab)] +
            (["wait %d" % pre] if pre else []) + [
             "evt race_drive",
             "repeat %d" % ITER] + body + ["endrepeat"] + marks + [
             "logram race_x 2 0x%08X" % x,
             "logram race_y 2 0x%08X" % y,
             "dump",
             "evt race_drive_done"])


def main():
    sc = os.path.join(HERE, "scripts")
    od = os.environ.get("MSC_OUT", sc)     # sweeps write elsewhere
    subprocess.check_call([sys.executable, os.path.join(HERE, "pair_script.py"),
                           os.path.join(sc, "msc.spec"),
                           os.path.join(od, "msc_host.txt"),
                           os.path.join(od, "msc_join.txt")])
    tab = cells(line())
    for name, k in (("msc_host.txt", 0), ("msc_join.txt", 1)):
        p = os.path.join(od, name)
        s = open(p).read().rstrip("\n").split("\n")
        s += race(k, tab)
        s[0] = "# generated by gen_msc.py (menus: msc.spec via pair_script.py) -- edit those, not this"
        open(p, "w", newline="\n").write("\n".join(s) + "\n")
        print(p, len(s), "lines,", sum(1 for l in s if l.strip() and not l.startswith("#")), "steps")


if __name__ == "__main__":
    main()
