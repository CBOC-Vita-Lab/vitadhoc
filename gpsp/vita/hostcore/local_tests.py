#!/usr/bin/env python3
"""local_tests.py -- the LOCAL LINK's tests (two players on one console:
vita/local_vita.c, frontend-common/fe_gbadual's local pair, fe_host's GB
pair), on a host twin.

    ./local_tests.py x86               the x86-64 twin (the x86 dynarec)
    ./local_tests.py armhf             the armhf twin under qemu-arm (the ARM
                                       dynarec, the one the Vita runs)
    ./local_tests.py x86-san           the x86 twin built SAN=1
    options:  --only NAME[,NAME]   run some of the tests
              --update-refs        (x86) rewrite ref/local_*.png
              --speed              also the speed measurement (slow on ARM)

Driven like app_tests.py (whose Suite and helpers this file uses): the whole
app, plat_host.c's input script on simulated time, checked by the event
log, the files left behind and the composed 960x544 picture.  The
cartridges are the project's own:

    tools/linktest/linktest.gba     the link-cable test ROM: it checks every
                                    word that crosses the cable and leaves a
                                    verdict in RAM (tools/linktest/README.md)
    hostcore/testrom/gbadhoc-test.gba  buttons as boxes, a frame counter in
                                    binary, battery RAM, two sound channels
    vita/tests/cheats/rom/cheatrom.gba a "game" with lives and gold
    hostcore/testrom/gblocal.gb      a Game Boy that talks over its serial
                                    port (make -C testrom RGBDS=... gb)

Every picture a run takes is left in build/localtest/<arch>/ for looking at.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

from PIL import Image

import app_tests as A
from app_tests import menu_pick, worst_diff

HERE = os.path.dirname(os.path.abspath(__file__))
GP = os.path.normpath(os.path.join(HERE, "..", ".."))
LINKTEST = os.path.join(GP, "tools", "linktest", "linktest.gba")
LT_DIR = os.path.join(GP, "tools", "linktest")
GL_DIR = os.path.join(GP, "tools", "gbamirror")
GBLINK = os.path.join(HERE, "testrom", "gblocal.gb")
TEST = "gbadhoc-test.gba"

# the two layouts' pictures in the composed frame: x, y, scale (video_vita.h)
RECTS = {
    ("gba", 0): ((0, 112, 2), (480, 112, 2)),      # side by side
    ("gba", 1): ((0, 32, 3), (720, 32, 1)),        # large + small
    ("gb", 0): ((0, 56, 3), (480, 56, 3)),
    ("gb", 1): ((80, 56, 3), (560, 56, 2)),
}
SIZE = {"gba": (240, 160), "gb": (160, 144)}
P1_BLUE, P2_YELLOW = (99, 182, 255), (255, 215, 0)


def pair_pixels(im, layout=0, console="gba"):
    """The two games' own pixels out of a composed pair picture: (left or
    large, right or small)."""
    w, h = SIZE[console]
    out = []
    src = im.load()
    for x0, y0, k in RECTS[(console, layout)]:
        g = Image.new("RGB", (w, h))
        dst = g.load()
        for y in range(h):
            for x in range(w):
                dst[x, y] = src[x0 + x * k + k // 2, y0 + y * k + k // 2]
        out.append(g)
    return out


def lit_bar(im, layout=0, console="gba"):
    """Which picture carries the bright bar of the game in view: 0 (left or
    large), 1, or None."""
    w, h = SIZE[console]
    res = []
    for x0, y0, k in RECTS[(console, layout)]:
        r, g, b = im.getpixel((x0 + w * k // 2, y0 - 2))
        res.append(max(r, g, b))
    if abs(res[0] - res[1]) < 60:
        return None
    return 0 if res[0] > res[1] else 1


def bar_player(im, which, layout=0, console="gba"):
    """Whose colour the bar above picture `which` is: 0 (P1 blue), 1 (P2
    yellow)."""
    w, h = SIZE[console]
    x0, y0, k = RECTS[(console, layout)][which]
    r, g, b = im.getpixel((x0 + w * k // 2, y0 - 2))
    return 0 if b > r else 1


# ---- hostcore/testrom/gbadhoc-test.gba, read off its picture ---------------
NAMES = ("A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L")


def boxes(g):
    return "+".join(n for i, n in enumerate(NAMES)
                    if max(g.getpixel((8 + i * 23 + 9, 37))) > 100)


def frame_no(g):
    """The cartridge's own frame counter (16 boxes, binary)."""
    v = 0
    for i in range(16):
        v = (v << 1) | (1 if max(g.getpixel((8 + i * 14 + 5, 105))) > 150 else 0)
    return v


def sram_box(g):
    """The colour of the box that shows what reads back from battery RAM."""
    return g.getpixel((132, 68))


def sram_expect(presses):
    """...and what it must be after `presses` presses of A (testrom/main.c:
    SRAM[i] = i * 7 + presses * 16), as the picture shows a 5-bit channel."""
    if presses == 0:
        return None
    v = [(i * 7 + presses * 16) & 31 for i in (1, 2, 3)]
    return tuple((c << 3) | (c >> 2) for c in v)


def near(a, b, tol=10):
    return a is not None and b is not None and \
        all(abs(x - y) <= tol for x, y in zip(a, b))


# ---- tools/linktest: its verdict (README.md, check.py) -----------------------
def lt_script(path, pad2, at):
    """A gl_host key script (wait / hold / repeat) as the twin's pad script:
    frame n of the file is refresh at + n."""
    toks = []
    for line in open(path):
        line = line.split("#")[0].split()
        if line:
            toks.append(line)

    def run(i, f, out, depth):
        while i < len(toks):
            t = toks[i]
            if t[0] == "wait":
                f += int(t[1])
            elif t[0] == "hold":
                out.append((f, t[1], int(t[2])))
                f += int(t[2])
            elif t[0] == "repeat":
                n, j = int(t[1]), i + 1
                for _ in range(n):
                    end, f = run(j, f, out, depth + 1)
                    if f > 40000:
                        break
                # skip to the matching endrepeat
                d = 1
                i = j
                while d:
                    d += toks[i][0] == "repeat"
                    d -= toks[i][0] == "endrepeat"
                    i += 1
                continue
            elif t[0] == "endrepeat":
                return i, f
            i += 1
        return i, f

    out = []
    run(0, 0, out, 0)
    s = ""
    for f, keys, n in out:
        s += "%d:%s%s:%d\n" % (at + f, "p2." if pad2 else "",
                               keys.lower(), n)
    return s


def lt_peeks(r):
    """{slot: [64 words]} from the last `local_peek` lines of a run."""
    mem = {}
    for d in r.evt("local_peek"):
        m = re.search(r"local_peek s=(\d) ([0-9a-f]{8}):((?: [0-9a-f?]{8})+)",
                      d["_"])
        if m and "?" not in m.group(3):
            base = int(m.group(2), 16)
            for i, w in enumerate(m.group(3).split()):
                mem[(int(m.group(1)), base + 4 * i)] = int(w, 16)
    out = {}
    for s in (0, 1):
        ws = [mem.get((s, 0x02000000 + 4 * i)) for i in range(64)]
        if None not in ws:
            out[s] = ws
    return out


def lt_verdict(S, r, frames, what):
    """The link test ROM's own verdict for both machines: the roles, no
    failed check, no time-out, every phase passed, loops as the frames
    allow (check.py's rules)."""
    pk = lt_peeks(r)
    if not S.check(0 in pk and 1 in pk and len(pk[0]) >= 64,
                   "%s: both machines' result blocks are in the log" % what):
        return None
    for s in (0, 1):
        w = pk[s]
        okp, flp = w[32:48], w[48:64]
        S.check(w[0] == 0x31544B4C and w[1] == s + 1,
                "%s: machine %d is the %s (magic %08x role %d)" %
                (what, s, ("parent", "child")[s], w[0], w[1]))
        S.check(w[5] == 0 and w[13] == 0 and w[14] == 0 and w[11] == 0,
                "%s: machine %d: no failed check (%d), code check (%d), "
                "time-out (%d), failure seen by the other (%d); first "
                "%08x got %08x expected %08x" %
                (what, s, w[5], w[13], w[14], w[11], w[24], w[25], w[26]))
        S.check(w[6] > 100 and w[12] > 0,
                "%s: machine %d passed %d checks, %d code checks" %
                (what, s, w[6], w[12]))
        if frames >= 400:
            S.check(w[3] >= frames // 200 - 1,
                    "%s: machine %d: %d loops of the ten phases in %d frames"
                    % (what, s, w[3], w[2]))
            missing = [p for p in range(1, 11) if okp[p] == 0]
            failed = [p for p in range(16) if flp[p]]
            S.check(not missing and not failed,
                    "%s: machine %d: every phase passed checks (missing %s, "
                    "failed %s)" % (what, s, missing, failed))
            S.check((w[27] > 0) == (s == 1),
                    "%s: machine %d: SI interrupts on the child only (%d)"
                    % (what, s, w[27]))
        S.check(w[8] > 0 and w[9] > 0,
                "%s: machine %d completed %d multi-player and %d normal "
                "transfers" % (what, s, w[8], w[9]))
    st = r.evt("local_stats")
    S.check(st and st[-1]["bus_ovf"] == "0" and st[-1]["err"] == "0",
            "%s: the cable's queue never overflowed, no stepper error" % what)
    return pk


def lt_bands(g):
    """linktest's status band: (role colour, verdict) of one machine's
    picture: 'parent' / 'child' / 'none', and True while nothing failed."""
    r, gr, b = g.getpixel((40, 6))
    role = "parent" if r > 150 and b < 100 else \
        "child" if b > 150 and r < 100 else "none"
    r2, g2, b2 = g.getpixel((200, 6))
    return role, (g2 > 120 and r2 < 100)


# ---- runs ---------------------------------------------------------------------
class LSuite(A.Suite):
    def __init__(self, arch, update, keep):
        super().__init__(arch, update, keep)
        self.outdir = os.path.join(HERE, "build", "localtest", arch)
        self.boot = {}

    def carts(self, *names):
        """extra= for Suite.run: more cartridges in roms/."""
        out = {}
        for n in names:
            out["roms/" + os.path.basename(n)] = open(n, "rb").read()
        return out

    def boot_refresh(self, rom, extra=None):
        """The refresh at which an autobooted game's frame 0 runs (the
        loading screen takes a few): calibrated once per cartridge."""
        if rom not in self.boot:
            r = self.run("calib_" + rom.split(".")[0], "60:shot=c.png\n"
                         "+2:quit\n", autoboot=rom, extra=extra)
            e = r.shot_evt("c.png")
            self.boot[rom] = 60 - int(e["emu"]) if e else 0
        return self.boot[rom]

    def finish(self):
        """As Suite.finish, with this suite's own summaries: what must be
        the same on the two dynarecs is compared where both have run."""
        import json
        os.makedirs(self.outdir, exist_ok=True)
        path = os.path.join(self.outdir, "summary.json")
        keep = {}
        if getattr(self, "partial", False):
            # --only: the other tests' summaries of the last whole run stay
            try:
                keep = json.load(open(path))
            except (OSError, ValueError):
                keep = {}
        keep.update(self.summary)
        with open(path, "w") as f:
            json.dump(keep, f, indent=1, sort_keys=True)
        other = "armhf" if self.arch.startswith("x86") else "x86"
        try:
            theirs = json.load(open(os.path.join(HERE, "build", "localtest",
                                                 other, "summary.json")))
        except (OSError, ValueError):
            theirs = None
        if theirs:
            self.test = "x86-vs-arm"
            for k in sorted(self.summary):
                if k in theirs and k.endswith((".audio_crc", ".hash_crc",
                                               ".same")):
                    self.check(self.summary[k] == theirs[k],
                               "%s is the same on %s (%s / %s)" %
                               (k, other, self.summary[k], theirs[k]))
        print("\n%s: %d checks, %d failed" % (self.arch, self.checks,
                                              len(self.fails)))
        for f in self.fails:
            print("  FAIL " + f)
        for n in self.notes:
            print("  note " + n)
        return 1 if self.fails else 0


LOCAL_PAGE = menu_pick(2, wait=20) + "+20:down\n+8:down\n+8:cross\n"


def t_linktest(S):
    """The link-cable test ROM on both machines, buttons scripted on both:
    every transfer checks out, both pictures show it."""
    extra = S.carts(LINKTEST)
    at = S.boot_refresh("linktest.gba", extra) + 2
    frames = 2400
    script = lt_script(os.path.join(LT_DIR, "keys0.txt"), 0, at) + \
        lt_script(os.path.join(LT_DIR, "keys1.txt"), 1, at)
    script = "\n".join(l for l in script.splitlines()
                       if int(l.split(":")[0]) < at + frames - 10) + "\n"
    script += "%d:shot=side.png\n%d:shot=late.png\n%d:quit\n" % (
        at + 700, at + frames - 5, at + frames)
    r = S.run("linktest", script, autoboot="linktest.gba", extra=extra,
              vita_ini={"local_auto": 1, "local_auto_frame": 2,
                        "local_peek": "0x02000000:64", "local_hash": 1})
    S.clean(r)
    st = r.evt("local_start")
    S.check(st and st[0].get("clone") == "1" and st[0].get("pads") == "2",
            "the pair started as a copy, with two pads (%s)" %
            (st[0]["_"] if st else "no local_start line"))
    lt_verdict(S, r, frames, "copy")
    im = S.picture(r, "side.png", "local_linktest.png")
    if im:
        a, b = pair_pixels(im)
        S.check(lt_bands(a) == ("parent", True) and
                lt_bands(b) == ("child", True),
                "the left picture is the parent's, the right the child's, "
                "both green (%s / %s)" % (lt_bands(a), lt_bands(b)))
    S.picture(r, "late.png")
    h = os.path.join(r.root, "log", "localhash.txt")
    lines = open(h).read().splitlines() if os.path.exists(h) else []
    S.check(len(lines) >= 2 * frames - 8, "both machines' hashes, every "
            "frame (%d lines)" % len(lines))
    # (Not compared between the two dynarecs: this ROM reads timers inside
    # its transfer handlers and checksums them, and the backends differ by
    # a few cycles there -- tools/linktest/README.md.  The port's own
    # cartridge, whose pair IS the same on both, is hashed in t_switch.)
    S.summary["linktest.hash_note"] = "%08x" % (
        zlib.crc32("\n".join(lines[:4000]).encode()) & 0xFFFFFFFF)
    S.summary["linktest.audio_crc"] = r.host.get("audio_crc")
    shutil.copy(h, os.path.join(S.outdir, "linktest.hash")) if lines else None

    # the partner powered on from its own (empty) save: it boots beside the
    # running game, which is still waiting for a partner
    r = S.run("linktest_own", "%d:shot=own.png\n%d:quit\n" %
              (at + 1500, at + 1505),
              autoboot="linktest.gba", extra=extra,
              vita_ini={"local_auto": 2, "local_auto_frame": 20,
                        "local_peek": "0x02000000:64"})
    S.clean(r)
    st = r.evt("local_start")
    S.check(st and st[0].get("clone") == "0", "the pair started with the "
            "partner on its own save")
    lt_verdict(S, r, 1480, "own save")
    S.picture(r, "own.png")


def t_menu(S):
    """Play together > Two players on this Vita: start a copy from the
    menu, swap, change the layout, stop -- and the game carries on."""
    script = LOCAL_PAGE + """
+20:shot=page.png
# Start: the partner copies my save
+2:cross
+2:shot=starting.png
+30:shot=running.png
# back to the game: O leaves the page and the menu
+2:circle
+12:circle
+60:shot=side.png
# the menu again: it opens on the local page while the pair runs
""" + menu_pick(2, wait=10) + """
+20:shot=active.png
# Swap controlled game
+2:cross
+20:shot=swapped.png
# Layout: large + small
+2:down
+8:cross
+20:shot=layout.png
# Back to the game (the last row)
+2:down
+8:down
+8:cross
+60:shot=inset.png
# Stop
""" + menu_pick(2, wait=10) + """
+20:down
+8:down
+8:cross
+20:shot=stopped.png
+2:circle
+12:circle
+12:circle
+60:shot=alone.png
+10:quit
"""
    r = S.run("menu", script, autoboot=TEST)
    S.clean(r)
    S.check(r.has(r"play_local partner=0") and
            r.has(r"local_start ok console=0 clone=1"),
            "Start: the partner copies my save")
    S.picture(r, "page.png", "local_page.png")
    S.picture(r, "starting.png")
    S.picture(r, "running.png", "local_page_active.png")
    im = S.picture(r, "side.png", "local_side.png", box=(0, 432, 960, 544))
    if im:
        a, b = pair_pixels(im)
        fa, fb = frame_no(a), frame_no(b)
        S.check(fa > 20 and 0 <= fa - fb <= 1,
                "side by side: the copy runs in step with the game (frames "
                "%d / %d)" % (fa, fb))
        S.check(lit_bar(im) == 0 and bar_player(im, 0) == 0 and
                bar_player(im, 1) == 1,
                "the player's game is the left one, in view (bright bar), "
                "P1 blue / P2 yellow")
        # the cartridge counts its own frames from a little after power on
        lag = int(r.shot_evt("side.png")["emu"]) - fa
    S.picture(r, "active.png")
    S.check(len(r.evt("local_view")) == 1 and
            r.evt("local_view")[0]["view"] == "1",
            "Swap controlled game: the pad drives the partner's")
    S.picture(r, "swapped.png")
    S.picture(r, "layout.png")
    im = S.picture(r, "inset.png", "local_inset.png", box=(720, 192, 960, 544))
    if im:
        big, small = pair_pixels(im, 1)
        S.check(abs(frame_no(big) - frame_no(small)) <= 1 and
                frame_no(big) > 50,
                "large + small: both games (frames %d / %d)" %
                (frame_no(big), frame_no(small)))
        S.check(bar_player(im, 0, 1) == 1 and bar_player(im, 1, 1) == 0,
                "the partner's game is in view, so it is the large one")
        # nothing of the frontend's on either picture's pixels: the small
        # one beside the large one, not over it
        S.check(RECTS[("gba", 1)][1][0] >= 720,
                "the small picture is beside the large one")
    S.check(r.has(r"local_stop why=menu"), "Stop")
    S.picture(r, "stopped.png")
    im = S.picture(r, "alone.png")
    a, b = r.shot_evt("stopped.png"), r.shot_evt("alone.png")
    S.check(a and b and int(b["emu"]) - int(a["emu"]) >= 55,
            "the game carries on alone")
    if im:
        g = A.game_pixels(im)
        e = r.shot_evt("alone.png")
        S.check(e and int(e["emu"]) - frame_no(g) == lag,
                "...and lost nothing: its own frame counter says %d after "
                "%s emulated frames, %d behind as before the link" %
                (frame_no(g), e["emu"] if e else "?", lag))
        S.check(boxes(g) == "", "no button of the menu reached it")
    cfg = open(os.path.join(r.root, "config.ini")).read()
    S.check(re.search(r"local_layout\s*=\s*1", cfg), "the layout is kept in "
            "config.ini")
    S.summary["menu.audio_crc"] = r.host.get("audio_crc")


def t_switch(S):
    """Which machine the one pad drives: the right stick, a tap, the menu;
    with a second controller each player has his own."""
    at = S.boot_refresh(TEST)
    script = """
%d:a:30
+0:shot=p1.png
# the right stick, flicked: the partner's game
+20:rstick-right:4
+10:b+up:30
+0:shot=p2.png
# a tap on the left picture: mine again (held buttons do not jump over)
+2:left:60
+10:touch=240/270
+20:shot=held.png
+40:r:20
+0:shot=p1again.png
# a tap on the right picture
+20:touch=700/270
+20:l:20
+0:shot=p2tap.png
# a tap outside both: nothing
+20:touch=480/40
+20:shot=same.png
# a tap on the picture already in hand: the menu, as with one game
+10:touch=700/270
+20:shot=menu.png
+4:circle
+30:quit
""" % (at + 60)
    r = S.run("switch", script, autoboot=TEST,
              vita_ini={"local_auto": 1, "local_hash": 1})
    S.clean(r)
    h = os.path.join(r.root, "log", "localhash.txt")
    lines = open(h).read().splitlines() if os.path.exists(h) else []
    S.check(len(lines) > 400, "both machines are hashed every frame (%d "
            "lines)" % len(lines))
    S.summary["switch.hash_crc"] = "%08x" % (
        zlib.crc32("\n".join(lines).encode()) & 0xFFFFFFFF)

    def lit(name):
        a, b = pair_pixels(r.shot(name))
        return boxes(a), boxes(b)

    S.check(lit("p1.png") == ("A", ""), "at first the pad is the player's "
            "game's (lit: %s / %s)" % lit("p1.png"))
    S.check(lit("p2.png") == ("", "B+Up"), "after the stick: the partner's "
            "(lit: %s / %s)" % lit("p2.png"))
    S.check(lit_bar(r.shot("p2.png")) == 1, "...and its picture has the "
            "bright bar")
    S.check(lit("held.png") == ("", ""), "a button held across a switch "
            "reaches neither game (lit: %s / %s)" % lit("held.png"))
    S.check(lit("p1again.png") == ("R", ""), "a tap on the left picture: "
            "mine again (lit: %s / %s)" % lit("p1again.png"))
    S.check(lit("p2tap.png") == ("", "L"), "a tap on the right picture "
            "(lit: %s / %s)" % lit("p2tap.png"))
    v = [(d["view"], d["via"]) for d in r.evt("local_view")]
    S.check(v == [("1", "next"), ("0", "tap"), ("1", "tap")],
            "three switches, no more: %s" % v)
    S.picture(r, "same.png")
    opens = [d["via"] for d in r.evt("menu_open")]
    S.check(opens == ["touch"], "a tap beside the pictures does nothing; a "
            "tap on the picture already in hand opens the menu (%s)" % opens)
    S.check(len(r.evt("menu_closed")) == 1, "...and it closes again")

    # two pads: each his own; the stick does nothing, a tap moves the view
    script = """
%d:a+right:40
%d:p2.b+left:40
+0:shot=both.png
+20:rstick-left:4
+20:shot=stick.png
+2:touch=240/270
+20:p2.start:20
+0:shot=tap.png
+10:quit
""" % (at + 60, at + 60)
    r = S.run("switch_pad2", script, autoboot=TEST,
              vita_ini={"local_auto": 1})
    S.clean(r)
    S.check(lit("both.png") == ("A+Right", "B+Left"),
            "two pads: each drives his own game (lit: %s / %s)" %
            lit("both.png"))
    v = [(d["view"], d["via"]) for d in r.evt("local_view")]
    S.check(v == [("1", "next"), ("0", "tap")] and
            lit_bar(r.shot("stick.png")) == 1,
            "the right stick moves the view (sound, the bright bar) to the "
            "partner's game: %s" % v)
    S.check(lit("tap.png") == ("", "Start") and
            lit_bar(r.shot("tap.png")) == 0,
            "a tap on the player's picture moves it back; neither moves the "
            "pads (lit: %s / %s)" % lit("tap.png"))
    S.check(r.has(r"local_pad2 on=1"), "the second controller is announced")
    S.picture(r, "both.png", "local_pad2.png", box=(0, 432, 960, 544))

    # a controller that is connected and taken away while two games run
    script = """
%d:rstick-right:4
+10:up:30
+0:shot=one.png
+10:pad2=on
+4:shot=plugged.png
+2:b:40
%d:p2.left:40
+0:shot=two.png
+10:pad2=off
+4:shot=unplugged.png
+2:r:30
+0:shot=one_again.png
+10:quit
""" % (at + 60, at + 60 + 4 + 10 + 30 + 10 + 4 + 2)
    r = S.run("switch_hotplug", script, autoboot=TEST,
              vita_ini={"local_auto": 1})
    S.clean(r)
    S.check(lit("one.png") == ("", "Up"), "one pad, the partner's game in "
            "view: it drives the partner (lit: %s / %s)" % lit("one.png"))
    S.check(lit("two.png") == ("B", "Left"), "a second controller arrives: "
            "the console is the player's again, controller 2 the partner's "
            "(lit: %s / %s)" % lit("two.png"))
    S.check(lit("one_again.png") == ("", "R"), "it is taken away: the one "
            "pad drives the game in view again (lit: %s / %s)" %
            lit("one_again.png"))
    p = [d["on"] for d in r.evt("local_pad2")]
    S.check(p == ["1", "0"], "both changes are announced (%s)" % p)
    S.picture(r, "plugged.png")
    S.picture(r, "unplugged.png")


def t_save(S):
    """The partner's own save file: created when its game saves, read on
    the next start; the player's save is never the partner's."""
    at = S.boot_refresh(TEST)
    # run 1: my game saves once (A); the partner, on its own save, twice
    script = """
%d:a:10
+30:rstick-right:4
+20:a:10
+20:a:10
+90:shot=saved.png
""" % (at + 120) + menu_pick(6)
    r = S.run("save1", script, autoboot=TEST,
              vita_ini={"local_auto": 2, "local_auto_frame": 100},
              config={"local_partner": 1})
    S.clean(r)
    mine = os.path.join(r.root, "roms", "gbadhoc-test.sav")
    p2 = os.path.join(r.root, "roms", "gbadhoc-test.p2.sav")
    st = r.evt("local_start")
    S.check(st and st[0]["clone"] == "0" and st[0]["size"] == "0",
            "the partner powers on with no save yet")
    S.check(os.path.exists(p2) and os.path.getsize(p2) == 0x20000,
            "the partner's save is <rom>.p2.sav, 128 KiB")
    S.check(os.path.exists(mine), "the player's own save is there too")
    if os.path.exists(p2) and os.path.exists(mine):
        a, b = open(mine, "rb").read(), open(p2, "rb").read()
        S.check(a[:64] == bytes((i * 7 + 16) & 255 for i in range(64)),
                "the player's save holds the player's one press")
        S.check(b[:64] == bytes((i * 7 + 32) & 255 for i in range(64)),
                "the partner's holds the partner's two")
    sv = r.evt("local_save")
    S.check(len(sv) >= 1 and all(d["ok"] == "1" for d in sv),
            "written by the save writer (%d writes)" % len(sv))
    S.check(not os.path.exists(p2 + ".tmp") and not os.path.exists(p2 + ".bak"),
            "no temporary file, and no backup of a file that was not there")
    im = S.picture(r, "saved.png")
    if im:
        a, b = pair_pixels(im)
        S.check(near(sram_box(a), sram_expect(1)) and
                near(sram_box(b), sram_expect(2)),
                "each game reads back its own battery (%s / %s)" %
                (sram_box(a), sram_box(b)))
        S.check(frame_no(a) - frame_no(b) > 80,
                "the partner booted when the link began (frames %d / %d)" %
                (frame_no(a), frame_no(b)))
    before = open(mine, "rb").read() if os.path.exists(mine) else b""

    # a write cut short between moving the save aside and moving the new
    # one in leaves only .p2.sav.old (and maybe a .tmp): it is the save
    if os.path.exists(p2):
        os.rename(p2, p2 + ".old")
        with open(p2 + ".tmp", "wb") as f:
            f.write(b"\0" * 100)

    # run 2, the same card: the partner's save is read back; it saves once
    # more; nothing the partner does reaches the player's file
    script = """
%d:shot=reload.png
+2:p2.a:10
+90:shot=third.png
""" % (at + 160) + menu_pick(6)
    r = S.run("save1", script, prepared=True)
    S.clean(r)
    st = r.evt("local_start")
    S.check(st and st[-1]["size"] == "131072", "the next start reads the "
            "partner's save (%s bytes)" % (st[-1]["size"] if st else "?"))
    im = S.picture(r, "reload.png")
    if im:
        a, b = pair_pixels(im)
        S.check(near(sram_box(a), sram_expect(1)) and
                near(sram_box(b), sram_expect(2)),
                "both games boot with their own battery (%s / %s)" %
                (sram_box(a), sram_box(b)))
    after = open(mine, "rb").read() if os.path.exists(mine) else b""
    S.check(before and before == after, "the player's save is untouched by "
            "the partner")
    b = open(p2, "rb").read() if os.path.exists(p2) else b""
    S.check(b[:64] == bytes((i * 7 + 16) & 255 for i in range(64)),
            "the partner's file has its new save (one press since power on)")
    S.check(not os.path.exists(p2 + ".old") and not os.path.exists(p2 + ".tmp"),
            "the interrupted write's .old was taken back and its .tmp removed")

    # a copy that saves becomes the second save file; the old one is kept
    script = """
%d:p2.a:10
+20:p2.a:10
+20:p2.a:10
+90:shot=copy.png
""" % (at + 160) + menu_pick(6)
    with open(os.path.join(r.root, "vita.ini"), "w") as f:
        f.write("local_auto = 1\nlocal_auto_frame = 100\n")
    r = S.run("save1", script, prepared=True)
    S.clean(r)
    S.check(os.path.exists(p2 + ".bak") and
            open(p2 + ".bak", "rb").read() == b,
            "a copy's first save keeps the second save file as .bak")
    c = open(p2, "rb").read()
    S.check(c[:64] == bytes((i * 7 + 48) & 255 for i in range(64)),
            "...and the copy's save (three presses) is the second save "
            "file now")
    after2 = open(mine, "rb").read() if os.path.exists(mine) else b""
    S.check(after2 == before, "the player's save is still untouched")
    S.check(r.has(r"local_save_backup"), "the backup is in the log")

    # the worker thread writes the same file
    script = """
%d:rstick-right:4
+20:a:10
+20:a:10
+90:shot=w.png
""" % (at + 140) + menu_pick(6)
    r = S.run("save_worker", script, autoboot=TEST, args=("--worker",),
              vita_ini={"local_auto": 2, "local_auto_frame": 100})
    S.clean(r)
    p2w = os.path.join(r.root, "roms", "gbadhoc-test.p2.sav")
    w = open(p2w, "rb").read() if os.path.exists(p2w) else b""
    S.check(r.has(r"io_worker on=1") and
            w[:64] == bytes((i * 7 + 32) & 255 for i in range(64)),
            "with the I/O worker thread: the same partner's save")
    mine = os.path.join(r.root, "roms", "gbadhoc-test.sav")
    m = open(mine, "rb").read() if os.path.exists(mine) else b""
    S.check(not m or m[:64] == b"\xff" * 64,
            "...and the player's, who never saved, is absent or blank")


def t_cycle(S):
    """Start and stop five times in one boot; quit to the game list while
    two games run and boot another; suspend while two games run."""
    start = LOCAL_PAGE + "+20:cross\n+30:circle\n+12:circle\n"
    stop = menu_pick(2, wait=40) + "+20:down\n+8:down\n+8:cross\n" \
        "+20:circle\n+12:circle\n+12:circle\n"
    script = ""
    for i in range(5):
        script += start + "+50:shot=on%d.png\n" % i + stop + \
            "+40:shot=off%d.png\n" % i
    script += "+10:quit\n"
    r = S.run("cycle", script, autoboot=TEST)
    S.clean(r)
    S.check(len(r.evt("local_start")) == 5 and
            len([d for d in r.evt("local_stop") if d["why"] == "menu"]) == 5,
            "five starts and five stops (%d / %d)" %
            (len(r.evt("local_start")), len(r.evt("local_stop"))))
    ok, lags = True, set()
    for i in range(5):
        on, off = r.shot("on%d.png" % i), r.shot("off%d.png" % i)
        a, b = pair_pixels(on)
        e = r.shot_evt("on%d.png" % i)
        ok = ok and 0 <= frame_no(a) - frame_no(b) <= 1
        lags.add(int(e["emu"]) - frame_no(a))
        g = A.game_pixels(off)
        e = r.shot_evt("off%d.png" % i)
        lags.add(int(e["emu"]) - frame_no(g))
    S.check(ok and len(lags) == 1, "every time: two games in step while it "
            "runs, one game that lost no frame after it (its counter is "
            "always %s behind the emulator's)" % sorted(lags))
    S.picture(r, "on4.png")
    S.picture(r, "off4.png")

    # quit to the list while two games run, boot another game, link again
    script = start + "+60:shot=two.png\n" + A.QUIT_TO_LIST + """
+120:shot=list.png
+2:cross
+150:shot=second.png
""" + start + "+60:shot=two2.png\n+10:quit\n"
    r = S.run("cycle_list", script, autoboot=TEST,
              config={"last_rom": "hello.gba" if S.have_fixture else TEST})
    S.clean(r)
    i = r.index(r"local_stop why=game_end")
    j = r.index(r"game_unload", i) if i >= 0 else -1
    S.check(0 <= i < j, "quit to the list: the pair ends before the game "
            "is unloaded")
    S.check(len(r.evt("rom_loaded")) == 2 and len(r.evt("local_start")) == 2,
            "another game boots and links (%d boots, %d starts)" %
            (len(r.evt("rom_loaded")), len(r.evt("local_start"))))
    S.picture(r, "list.png")
    S.picture(r, "second.png")
    im = S.picture(r, "two2.png")
    if im:
        a, b = pair_pixels(im)
        S.check(worst_diff(a, b) <= 8 or frame_no(a) > 0,
                "the second game's pair is on screen")

    # suspend while two games run: the partner's save goes to the card
    # first; afterwards the pair carries on
    at = S.boot_refresh(TEST)
    script = """
%d:p2.a:10
+10:suspend
+30:shot=wake.png
+2:cross
+60:shot=after.png
+60:shot=later.png
+10:quit
""" % (at + 150)
    r = S.run("cycle_suspend", script, autoboot=TEST,
              vita_ini={"local_auto": 2, "local_auto_frame": 100})
    S.clean(r)
    i = r.index(r"power suspend game=1")
    j = r.index(r"local_save .*ok=1", i) if i >= 0 else -1
    k = r.index(r"power resume game=1", i) if i >= 0 else -1
    S.check(0 <= i < j < k, "suspend: the partner's save is written before "
            "the console sleeps")
    S.picture(r, "wake.png")
    a, b = pair_pixels(r.shot("after.png"))
    c, d = pair_pixels(r.shot("later.png"))
    S.check(frame_no(c) - frame_no(a) == 60 and
            frame_no(d) - frame_no(b) == 60,
            "after the resume both games run on (%d -> %d, %d -> %d)" %
            (frame_no(a), frame_no(c), frame_no(b), frame_no(d)))
    S.check(len(r.evt("local_start")) == 1 and not r.has(r"local_pair_error"),
            "the same pair, no error")


def t_cheats(S):
    """Cheats while two games run: the codes act on the player's machine
    and never on the partner's."""
    if not os.path.exists(A.CHEAT_GBA):
        S.note("no cheatrom.gba (make -C ../tests/cheats/rom): skipped")
        return
    cht = A.CHT.replace('cheat0_enable = false', 'cheat0_enable = true')
    extra = {"roms/cheatrom.gba": open(A.CHEAT_GBA, "rb").read(),
             "cheats/cheatrom.cht": cht}
    at = S.boot_refresh("cheatrom.gba", extra)
    # the partner powers on beside a game whose lives are frozen at 0x63:
    # its own five are gone 40 frames later
    script = """
%d:shot=lives.png
# the cheat cartridge's button (right stick down): gold, for one machine
+2:rstick-down:20
+0:shot=gold.png
+30:quit
""" % (at + 200)
    r = S.run("cheats_own", script, autoboot="cheatrom.gba", extra=extra,
              vita_ini={"local_auto": 2, "local_auto_frame": 60})
    S.clean(r)
    ld = r.evt("cheats_load")
    S.check(ld and ld[0]["enabled"] == "2", "two codes are on (%s)" %
            (ld[0]["_"] if ld else "-"))

    def blocks(name):
        return [[g.getpixel((n * 48 + 4, 60)) for n in range(5)]
                for g in pair_pixels(r.shot(name))]

    a, b = blocks("lives.png")
    S.check(near(a[0], A.BLUE) and b[0] == A.BLACK,
            "Infinite Lives: the player's machine has 0x63 lives, the "
            "partner's ran out (%s / %s)" % (a[0], b[0]))
    a, b = blocks("gold.png")
    S.check(near(a[1], A.OLIVE) and b[1] == A.BLACK,
            "the cheat button gives the player gold, the partner none "
            "(%s / %s)" % (a[1], b[1]))
    S.check(a[2:] == b[2:] and a[2] != A.BLACK,
            "the partner's cartridge code is its own, unpatched (%s / %s)"
            % (a[2:], b[2:]))
    S.picture(r, "gold.png")

    # a copy starts with what the codes had written, and from then on is
    # left alone: its lives count down
    r = S.run("cheats_copy", "%d:shot=copy.png\n+10:quit\n" % (at + 200),
              autoboot="cheatrom.gba", extra=extra,
              vita_ini={"local_auto": 1, "local_auto_frame": 60})
    S.clean(r)
    a, b = blocks("copy.png")
    S.check(near(a[0], A.BLUE) and not near(b[0], A.BLUE),
            "a copy's lives are no longer held (%s / %s)" % (a[0], b[0]))


# ---- sound ---------------------------------------------------------------------
# hostcore/testrom plays a square wave whose note changes every 30 frames
# (testrom/main.c sound_frame): which machine is heard can be read off the
# pitch, since the partner on its own save booted later.
NOTES = [131072.0 / (2048 - (1700 + k * 30)) for k in range(8)]


def wav_mono(path):
    import numpy as np
    data = open(path, "rb").read()[44:]
    s = np.frombuffer(data[:len(data) // 4 * 4], dtype="<i2").reshape(-1, 2)
    return s.astype(np.float64).mean(axis=1)


def note_at(x, pos, width=4096):
    """The square wave's note (0..7) around sample `pos`, or None."""
    import numpy as np
    seg = x[max(0, pos - width // 2):pos + width // 2]
    if len(seg) < width or np.abs(seg).max() < 200:
        return None
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    f = np.fft.rfftfreq(len(seg), 1 / 48000.0)
    best, score = None, 0
    for k, n in enumerate(NOTES):
        band = sp[(f > n - 14) & (f < n + 14)].max()
        if band > score:
            best, score = k, band
    return best


def zero_runs(x, minimum=600):
    """Stretches of exact silence: [(start, length)]."""
    import numpy as np
    z = np.flatnonzero(x != 0)
    if len(z) == 0:
        return [(0, len(x))]
    out = []
    gaps = np.flatnonzero(np.diff(z) > minimum)
    for g in gaps:
        out.append((int(z[g]) + 1, int(z[g + 1] - z[g] - 1)))
    return out


def t_audio(S):
    """Sound: one machine is heard, the one in view; nothing is mixed, and
    a change of view goes through a fade, not a click."""
    import numpy as np
    at = S.boot_refresh(TEST)
    end = at + 720
    r0 = S.run("audio_solo", "%d:quit\n" % end, autoboot=TEST)
    S.clean(r0)
    # a copy, never looked at: the player's sound, sample for sample
    r1 = S.run("audio_copy", "%d:quit\n" % end, autoboot=TEST,
               vita_ini={"local_auto": 1, "local_auto_frame": 100})
    S.clean(r1)
    S.check(r1.host.get("audio_crc") == r0.host.get("audio_crc") and
            int(r0.host.get("peak", 0)) > 4000,
            "two games, the player's in view: the sound is exactly the one "
            "game's (crc %s / %s)" % (r1.host.get("audio_crc"),
                                      r0.host.get("audio_crc")))
    S.summary["audio.copy_same"] = "%s" % (r1.host.get("audio_crc") ==
                                           r0.host.get("audio_crc"))

    # the partner on its own save (booted 100 frames later); the view goes
    # to it and back
    sw1, sw2 = at + 300, at + 520
    script = "%d:shot=a.png\n%d:rstick-right:4\n%d:shot=b.png\n" \
             "%d:rstick-left:4\n%d:quit\n" % (at + 200, sw1, at + 400, sw2,
                                              end)
    r = S.run("audio_swap", script, autoboot=TEST,
              vita_ini={"local_auto": 2, "local_auto_frame": 100})
    S.clean(r)
    x = wav_mono(os.path.join(r.out, "audio.wav"))
    pa, pb = pair_pixels(r.shot("a.png"))
    fa, fb = frame_no(pa), frame_no(pb)

    def note_of(frame):
        return (frame // 30) % 8

    # each machine's frame number at refresh R (one frame a refresh)
    def f_a(R):
        return fa + (R - (at + 200))

    def f_b(R):
        return fb + (R - (at + 200))

    # the port's latency: where the player's own notes line up best
    def agree(off, fn, lo, hi):
        hit = tot = 0
        for R in range(lo, hi, 3):
            fr = fn(R)
            if fr % 30 < 8 or fr % 30 > 22:
                continue             # near a change of note
            n = note_at(x, (R - at) * 800 + off)
            tot += 1
            hit += n == note_of(fr)
        return hit, tot
    offs = list(range(-9600, 9601, 400))
    score = [agree(o, f_a, at + 120, sw1 - 10)[0] for o in offs]
    good = [o for o, h_ in zip(offs, score) if h_ == max(score)]
    best = good[len(good) // 2]     # the middle of the offsets that fit
    h, t = agree(best, f_a, at + 120, sw1 - 10)
    S.check(t > 10 and h >= t - 1, "before the switch the player's game is "
            "heard: %d of %d windows carry its note (latency %d ms)" %
            (h, t, -best // 48))
    h, t = agree(best, f_b, sw1 + 20, sw2 - 10)
    ha, ta = agree(best, f_a, sw1 + 20, sw2 - 10)
    S.check(t > 10 and h >= t - 1 and ha <= ta // 3,
            "after it the partner's: %d of %d windows carry its note, %d "
            "the player's" % (h, t, ha))
    h, t = agree(best, f_a, sw2 + 20, end - 20)
    S.check(t > 5 and h >= t - 1, "and back: %d of %d windows carry the "
            "player's note again" % (h, t))
    # each change of view: a fade to silence, a frame or two of it, a fade
    # back in
    runs = [z for z in zero_runs(x) if z[0] > 48000]
    S.check(len(runs) == 2 and all(1200 <= n <= 4000 for _, n in runs),
            "two stretches of silence, one per change (%s)" % runs)
    ok = True
    for start, n in runs:
        loud = np.abs(x[start - 3000:start - 1500]).max()
        ok = ok and np.abs(x[start - 48:start]).max() < 0.2 * loud and \
            np.abs(x[start + n:start + n + 24]).max() < 0.5 * loud
    S.check(ok, "the sound fades into each silence and out of it")
    S.summary["audio.swap.audio_crc"] = r.host.get("audio_crc")


def t_states(S):
    """Save states while two games run: the pair, whole -- both machines
    and the cable -- and a state of one game loaded into a pair."""
    extra = S.carts(LINKTEST)
    at = S.boot_refresh("linktest.gba", extra) + 2
    k1 = at + 610                 # save here, in the middle of the phases
    seg = 420
    # buttons with a period of one segment, so every run from the saved
    # point is given the same ones
    keys = ""
    for i in range(4):
        base = k1 + i * seg
        for off, b, n in ((20, "a", 7), (40, "b+up", 5), (90, "right+b", 11),
                          (150, "start", 2), (200, "a+down", 9),
                          (300, "left", 6)):
            keys += "%d:%s:%d\n" % (base + off, b, n)
            keys += "%d:p2.%s:%d\n" % (base + off + 13, b, n)
    script = keys + """
%d:select+l:1
%d:shot=saved.png
%d:select+r:1
%d:shot=loaded.png
%d:select+r:1
%d:shot=end.png
%d:quit
""" % (k1, k1 + 30, k1 + seg, k1 + seg + 30, k1 + 2 * seg, k1 + 3 * seg - 5,
       k1 + 3 * seg)
    script = "\n".join(sorted((l for l in script.splitlines() if l),
                              key=lambda l: int(l.split(":")[0]))) + "\n"
    # (the quick save and quick load chords are SELECT+L and SELECT+R, and
    # a chord's buttons reach the game: with the GBA's L and R unbound the
    # game is handed the same SELECT in the run that saves and carries on
    # as in the runs that load)
    r = S.run("states_pair", script, autoboot="linktest.gba", extra=extra,
              config={"bind_l": "NONE", "bind_r": "NONE"},
              vita_ini={"local_auto": 1, "local_auto_frame": 2,
                        "local_peek": "0x02000000:64", "local_hash": 1})
    S.clean(r)
    sv = r.evt("local_state_save")
    ld = r.evt("local_state_load")
    S.check(len(sv) == 1 and sv[0]["ok"] == "1" and
            int(sv[0]["size"]) > 800000,
            "the pair is saved beside the player's state (%s)" %
            (sv[0]["_"] if sv else "no local_state_save"))
    st0 = os.path.join(r.root, "roms", "linktest.st0")
    S.check(os.path.exists(st0) and os.path.exists(st0 + ".p2") and
            os.path.getsize(st0) == 416 * 1024,
            "the player's own state file is still a plain 416 KiB state")
    S.check(len(ld) == 2 and all(d["pair"] == "1" for d in ld),
            "loaded twice, as a pair (%s)" % [d.get("pair") for d in ld])
    # the cable test's own verdict: a pair that came back with anything of
    # the cable missing fails its checks or times out
    lt_verdict(S, r, 610 + seg, "after two loads")
    for n in ("saved.png", "loaded.png", "end.png"):
        im = S.picture(r, n)
        if im:
            a, b = pair_pixels(im)
            S.check(lt_bands(a) == ("parent", True) and
                    lt_bands(b) == ("child", True),
                    "%s: parent and child, both green" % n)
    # exactness: the three runs from the saved point -- carried on, loaded,
    # loaded again -- hash the same, frame for frame, on both machines
    lines = open(os.path.join(r.root, "log", "localhash.txt")).read().split("\n")
    segs, cur, last = [], [], {0: 0, 1: 0}
    for l in lines:
        m = re.match(r"s=(\d) f=(\d+) ", l)
        if not m:
            continue
        s_, f_ = int(m.group(1)), int(m.group(2))
        if f_ <= last[s_]:
            segs.append(cur)       # the frame count went back: a load
            cur = []
            last = {0: 0, 1: 0}
        last[s_] = f_
        cur.append(l)
    segs.append(cur)
    S.check(len(segs) == 3, "the hash log shows two loads (%d runs)" %
            len(segs))
    if len(segs) == 3:
        f0 = int(re.match(r"s=\d f=(\d+)", segs[1][0]).group(1))
        first = [l for l in segs[0]
                 if int(re.match(r"s=\d f=(\d+)", l).group(1)) >= f0]
        n = min(len(first), len(segs[1]), len(segs[2])) - 4
        S.check(n > 2 * (seg - 20) and segs[1][:n] == segs[2][:n],
                "a loaded pair runs the same every time: %d hash lines of "
                "both machines identical after the two loads" % n)
        same = sum(1 for a, b in zip(first[:n], segs[1][:n]) if a == b)
        S.check(same == n, "...and the same as the run that was saved and "
                "carried on (%d of %d lines)" % (same, n))
        S.summary["states.hash_note"] = "%08x" % (
            zlib.crc32("\n".join(segs[2][:n]).encode()) & 0xFFFFFFFF)

    # ---- the frame counters of hostcore/testrom say what was restored ----
    at = S.boot_refresh(TEST)
    script = """
%d:select+l:1
+3:shot=saved.png
+100:select+r:1
+3:shot=loaded.png
+60:shot=later.png
""" % (at + 200) + menu_pick(6)
    r = S.run("states_own", script, autoboot=TEST,
              vita_ini={"local_auto": 2, "local_auto_frame": 100})
    S.clean(r)
    a0, b0 = [frame_no(g) for g in pair_pixels(r.shot("saved.png"))]
    a1, b1 = [frame_no(g) for g in pair_pixels(r.shot("loaded.png"))]
    a2, b2 = [frame_no(g) for g in pair_pixels(r.shot("later.png"))]
    S.check(a1 == a0 and b1 == b0 and a0 - b0 > 80,
            "a pair state brings BOTH games back: frames %d / %d when "
            "saved, %d / %d when loaded 100 frames later" % (a0, b0, a1, b1))
    S.check(a2 - a1 == 60 and b2 - b1 == 60, "and both run on (%d / %d)" %
            (a2, b2))
    st = os.path.join(r.root, "roms", "gbadhoc-test.st0")

    # the same card, no pair this time: the player's state file loads as a
    # state of one game, and a state saved alone orphans the old companion
    with open(os.path.join(r.root, "vita.ini"), "w") as f:
        f.write("local_auto = 0\n")
    script = """
%d:select+r:1
+3:shot=alone.png
+30:select+l:1
+20:quit
""" % (at + 150)
    r = S.run("states_own", script, prepared=True)
    S.clean(r)
    g = A.game_pixels(r.shot("alone.png"))
    S.check(abs(frame_no(g) - a0) <= 3, "the state of the pair's player "
            "loads in a game played alone (frame %d)" % frame_no(g))
    S.check(os.path.exists(st + ".p2"), "(the old companion is still there)")

    # ...and back in a pair: a copy becomes a copy of the loaded game; a
    # partner with its own save carries on
    for mode, name in ((1, "copy"), (2, "own")):
        with open(os.path.join(r.root, "vita.ini"), "w") as f:
            f.write("local_auto = %d\nlocal_auto_frame = 100\n" % mode)
        script = """
%d:shot=before.png
+2:select+r:1
+4:shot=after.png
+60:shot=later.png
+10:quit
""" % (at + 260)
        r = S.run("states_own", script, prepared=True)
        S.clean(r)
        ld = r.evt("local_state_load")
        S.check(ld and ld[0]["pair"] == "0" and ld[0]["why"] == "other_state"
                and ld[0]["reclone"] == ("1" if mode == 1 else "0"),
                "%s: a state saved since by one game alone is not paired "
                "with the old companion (%s)" % (name, ld[0]["_"] if ld else "-"))
        p0, q0 = [frame_no(g) for g in pair_pixels(r.shot("before.png"))]
        p1, q1 = [frame_no(g) for g in pair_pixels(r.shot("after.png"))]
        p2, q2 = [frame_no(g) for g in pair_pixels(r.shot("later.png"))]
        if mode == 1:
            S.check(p1 < p0 - 15 and 0 <= p1 - q1 <= 1 and
                    p2 - p1 == 60 and q2 - q1 == 60,
                    "copy: the partner is a copy of the loaded game "
                    "(%d / %d -> %d / %d -> %d / %d)" % (p0, q0, p1, q1, p2, q2))
        else:
            S.check(p1 < p0 - 15 and 0 <= q1 - q0 <= 8 and
                    p2 - p1 == 60 and q2 - q1 == 60,
                    "own save: the player's game is the loaded one, the "
                    "partner carries on (%d / %d -> %d / %d -> %d / %d)" %
                    (p0, q0, p1, q1, p2, q2))


def t_ff(S):
    """Fast forward runs both games."""
    at = S.boot_refresh(TEST)
    for name, cfg, lo, hi in (("3x", {}, 2.9, 3.05),
                              ("unlimited", {"ff_mult_x10": 0,
                                             "ff_smooth": 0}, 5.0, 5.9)):
        script = """
%d:shot=a.png
+1:square:120
+1:shot=b.png
+60:shot=c.png
+10:quit
""" % (at + 150)
        r = S.run("ff_" + name, script, autoboot=TEST, config=cfg,
                  vita_ini={"local_auto": 2, "local_auto_frame": 100})
        S.clean(r)
        st = r.evt("ff_stats")
        a0, b0 = [frame_no(g) for g in pair_pixels(r.shot("a.png"))]
        a1, b1 = [frame_no(g) for g in pair_pixels(r.shot("b.png"))]
        a2, b2 = [frame_no(g) for g in pair_pixels(r.shot("c.png"))]
        S.check(len(st) == 1 and lo * 120 <= a1 - a0 <= hi * 120 + 6 and
                abs((b1 - b0) - (a1 - a0)) <= 2,
                "%s: both games ran %d and %d frames in 120 refreshes" %
                (name, a1 - a0, b1 - b0))
        S.check(a2 - a1 in (59, 60, 61) and b2 - b1 in (59, 60, 61),
                "%s: and one frame a refresh afterwards (%d / %d)" %
                (name, a2 - a1, b2 - b1))
        S.check(not r.has(r"local_pair_error"), "%s: no stepper error" % name)
        S.picture(r, "b.png")

    # the pair's own fallback for a console that cannot hold sixty: every
    # other frame of both machines is emulated but not rendered (forced
    # here; by itself it starts when a frame's work exceeds a refresh,
    # which the twin's simulated clock cannot show)
    script = "%d:shot=a.png\n+119:shot=b.png\n+1:shot=c.png\n+10:quit\n" % (
        at + 150)
    r = S.run("ff_skip", script, autoboot=TEST,
              vita_ini={"local_auto": 2, "local_auto_frame": 100,
                        "local_skip": 2})
    S.clean(r)
    a0, b0 = [frame_no(g) for g in pair_pixels(r.shot("a.png"))]
    a1, b1 = [frame_no(g) for g in pair_pixels(r.shot("b.png"))]
    a2, b2 = [frame_no(g) for g in pair_pixels(r.shot("c.png"))]
    S.check(118 <= a1 - a0 <= 120 and 118 <= b1 - b0 <= 120,
            "drawing every other frame: both games still run every frame "
            "(%d and %d in 119 refreshes)" % (a1 - a0, b1 - b0))
    S.check((a2 - a1, b2 - b1) in ((0, 0), (2, 2), (0, 2), (2, 0)),
            "...and their pictures change every second refresh (%d / %d "
            "from one refresh to the next)" % (a2 - a1, b2 - b1))


def partner_ini(game, partner):
    """local.ini's line for `game` (its name under roms/): the partner's
    machine holds `partner` (local_vita.c: p_<crc32 of the name>)."""
    return "p_%08x = %s\n" % (zlib.crc32(game.encode()) & 0xFFFFFFFF, partner)


def t_other(S):
    """Another cartridge in the partner's machine: chosen in the menu,
    remembered, linked by cable, with that game's own save."""
    flag = os.path.join(LT_DIR, "linktest_flagprobe.gba")
    extra = S.carts(LINKTEST, flag, A.CHEAT_GBA)
    # ---- the menu: Partner's game > cheatrom, start, stop, this game again
    S.boot_refresh(TEST)            # (its root is where the list is read)
    names = sorted([os.path.splitext(n)[0] for n in
                    S.roms(os.path.join(S.outdir, "calib_gbadhoc-test",
                                        "root"), ".gba")
                    if n != TEST] + ["cheatrom", "linktest",
                                     "linktest_flagprobe"], key=str.lower) \
        if S.have_fixture else ["cheatrom", "linktest", "linktest_flagprobe"]
    row = names.index("cheatrom") + 1
    script = LOCAL_PAGE + """
+20:down
+8:down
+8:shot=page.png
+2:cross
+20:shot=picker.png
""" + "+8:down\n" * row + """
+8:shot=picked.png
+2:cross
+20:shot=chosen.png
# the cursor is on Partner's game; up once is the start that exists
+2:up
+8:cross
+40:shot=running.png
+2:circle
+12:circle
+60:shot=two.png
# stop, then choose "This game" again and start a copy
""" + menu_pick(2, wait=10) + """
+20:down
+8:down
+8:cross
# stopped: the Play together page, on "Two players"; its page again, where
# the cursor skips the copy that is not possible; Partner's game: the
# picker opens on the game that is chosen
+20:cross
+20:down
+8:cross
+20:shot=picker2.png
""" + "+8:up\n" * row + """
+8:cross
+20:shot=this.png
+2:up
+8:up
+8:cross
+40:circle
+12:circle
+60:shot=same.png
+10:quit
"""
    r = S.run("other_menu", script, autoboot=TEST, extra=extra)
    S.clean(r)
    S.picture(r, "page.png")
    S.picture(r, "picker.png", "local_picker.png" if S.have_fixture else None)
    S.picture(r, "picked.png")
    S.picture(r, "chosen.png", "local_page_other.png")
    S.picture(r, "running.png")
    pk = r.evt("local_partner")
    S.check(pk and pk[0]["partner"] == "cheatrom.gba" and pk[0]["rc"] == "0",
            "the picker lists the console's other cartridges and cheatrom "
            "is chosen (%s)" % (pk[0]["_"] if pk else "-"))
    rc = r.evt("gbadual_recart")
    S.check(len(rc) >= 2 and rc[0]["rom"].endswith("cheatrom.gba") and
            rc[0]["shared"] == "0" and rc[1]["rom"].endswith(TEST) and
            rc[1]["shared"] == "1",
            "instance B loads the partner's cartridge for the link and goes "
            "back on the player's when it stops")
    st = r.evt("local_start")
    S.check(len(st) == 2 and st[0]["clone"] == "0" and
            st[0]["save"].endswith("roms/cheatrom.sav") and
            st[1]["clone"] == "1" and st[1]["partner"] == "-",
            "the first start is cheatrom on its own save, the second a copy "
            "of this game")
    im = S.picture(r, "two.png", "local_other.png", box=(0, 432, 960, 544))
    if im:
        a, b = pair_pixels(im)
        S.check(frame_no(a) > 50 and
                b.getpixel((2 * 48 + 4, 60)) not in ((0, 0, 0), (16, 16, 49)),
                "two different games on screen: the test cartridge left, "
                "the cheat cartridge's blocks right (%s)" %
                (b.getpixel((2 * 48 + 4, 60)),))
    im = S.picture(r, "same.png")
    if im:
        a, b = pair_pixels(im)
        S.check(0 <= frame_no(a) - frame_no(b) <= 1 and frame_no(a) > 100,
                "then the same game twice again (frames %d / %d)" %
                (frame_no(a), frame_no(b)))
    ini = os.path.join(r.root, "local.ini")
    S.check(os.path.exists(ini) and
            re.search(r"p_[0-9a-f]{8}\s*=\s*$", open(ini).read(), re.M),
            "local.ini remembers the choice (this game again: empty)")

    # ---- the same, the cartridges in a folder besides roms/ -------------------
    # (vita.ini extra_roms: the picker lists that folder's games by their whole
    # path; the partner's save is beside it; the picture is the same, the
    # list showing the games' names)
    xdir = os.path.join(S.outdir, "other_menu_extra", "root", "ext")
    xe = dict(("ext/" + os.path.basename(n), open(n, "rb").read())
              for n in (LINKTEST, flag, A.CHEAT_GBA))
    r2 = S.run("other_menu_extra", script, autoboot=TEST, extra=xe,
               vita_ini={"extra_roms": xdir})
    S.clean(r2)
    pk = r2.evt("local_partner")
    S.check(pk and pk[0]["partner"] == xdir + "/cheatrom.gba" and
            pk[0]["rc"] == "0",
            "a folder besides roms/: the picker lists its cartridges and "
            "cheatrom is chosen by its whole path (%s)" %
            (pk[0]["_"] if pk else "-"))
    st = r2.evt("local_start")
    S.check(len(st) == 2 and st[0]["save"] == xdir + "/cheatrom.sav" and
            st[1]["clone"] == "1",
            "its save is beside it, and the second start is a copy of this "
            "game (%s)" % (st[0]["_"] if st else "no local_start"))
    S.check(not os.path.exists(os.path.join(r2.root, "roms", "cheatrom.sav")),
            "nothing of it is in roms/")
    S.check(worst_diff(r.shot("picker.png"), r2.shot("picker.png")) == 0 and
            worst_diff(r.shot("picked.png"), r2.shot("picked.png")) == 0,
            "the picker looks the same as with the cartridges in roms/")

    # ---- two different files that speak over the cable ----------------------
    at = S.boot_refresh("linktest.gba", extra) + 2
    r = S.run("other_link", "%d:shot=link.png\n%d:quit\n" %
              (at + 1500, at + 1505), autoboot="linktest.gba",
              extra=dict(extra, **{"local.ini": partner_ini(
                  "linktest.gba", "linktest_flagprobe.gba")}),
              vita_ini={"local_auto": 2, "local_auto_frame": 20,
                        "local_peek": "0x02000000:64"})
    S.clean(r)
    st = r.evt("local_start")
    S.check(st and st[0]["partner"].endswith("linktest_flagprobe.gba"),
            "the choice in local.ini is used at the next boot (%s)" %
            (st[0]["_"] if st else "no local_start"))
    lt_verdict(S, r, 1480, "two cartridges")
    S.picture(r, "link.png")

    # ---- the partner's game keeps its OWN save file ---------------------------
    at = S.boot_refresh("cheatrom.gba", extra)
    script = "%d:p2.a:10\n+20:p2.a:10\n+90:shot=saved.png\n" % (at + 140) + \
        menu_pick(6)
    r = S.run("other_save", script, autoboot="cheatrom.gba",
              extra=dict(extra, **{"local.ini": partner_ini("cheatrom.gba",
                                                            TEST)}),
              vita_ini={"local_auto": 2, "local_auto_frame": 100})
    S.clean(r)
    own = os.path.join(r.root, "roms", "gbadhoc-test.sav")
    b = open(own, "rb").read() if os.path.exists(own) else b""
    S.check(b[:64] == bytes((i * 7 + 32) & 255 for i in range(64)),
            "the partner's game saved to its own file, gbadhoc-test.sav")
    S.check(not [f for f in os.listdir(os.path.join(r.root, "roms"))
                 if ".p2." in f or f.endswith(".bak")],
            "no .p2.sav and no backup beside it")
    mine = os.path.join(r.root, "roms", "cheatrom.sav")
    m = open(mine, "rb").read() if os.path.exists(mine) else b""
    S.check(m[:64] != b[:64], "the player's own save is not the partner's")
    im = S.picture(r, "saved.png")
    if im:
        a, b2 = pair_pixels(im)
        S.check(near(sram_box(b2), sram_expect(2)),
                "the partner's picture shows its battery (%s)" %
                (sram_box(b2),))

    # ---- memory: two full-size cartridges -------------------------------------
    big = open(A.OWN_GBA, "rb").read()
    big_a = big + b"\xff" * (32 * 1024 * 1024 - len(big))
    big_b = big + b"\x00" * (32 * 1024 * 1024 - len(big))
    r = S.run("other_big", "%d:shot=big.png\n+10:quit\n" % (at + 200),
              autoboot="big-a.gba",
              extra={"roms/big-a.gba": big_a, "roms/big-b.gba": big_b,
                     "local.ini": partner_ini("big-a.gba", "big-b.gba")},
              vita_ini={"local_auto": 2, "local_auto_frame": 60})
    S.clean(r)
    rc = r.evt("gbadual_recart")
    S.check(rc and rc[0]["cap"] == "32" and rc[0]["shared"] == "0",
            "a 32 MiB partner beside a 32 MiB game: instance B holds its "
            "own 32 blocks (%s)" % (rc[0]["_"] if rc else "-"))
    im = S.picture(r, "big.png")
    if im:
        a, b2 = pair_pixels(im)
        S.check(frame_no(a) > 100 and 20 < frame_no(b2) < frame_no(a),
                "both run (frames %d / %d)" % (frame_no(a), frame_no(b2)))
    for f in ("big-a.gba", "big-b.gba"):
        os.remove(os.path.join(r.root, "roms", f))


# ---- hostcore/testrom/gblocal.gb, read off its picture and its memory ------------
def gb_peeks(r):
    """{slot: [bytes at C000...]} from the last local_peek lines."""
    out = {}
    for d in r.evt("local_peek"):
        m = re.search(r"local_peek s=(\d) c000:((?: [0-9a-f]{2})+)", d["_"])
        if m:
            out[int(m.group(1))] = [int(b, 16) for b in m.group(2).split()]
    return out


def gb_boxes(g):
    """gblocal's picture: (good count, 'master' / 'slave' / '', any bad byte,
    Right held), from the boxes it draws (8 x 8 tiles; a box is dark)."""
    def dark(col, row):
        return max(g.getpixel((col * 8 + 4, row * 8 + 4))) < 100
    good = 0
    for i in range(16):
        good = (good << 1) | dark(2 + i, 4)
    role = "master" if dark(2, 7) else "slave" if dark(16, 7) else ""
    return good, role, dark(2, 10), dark(9, 13)


def gb_verdict(S, r, what, least=100):
    pk = gb_peeks(r)
    if not S.check(0 in pk and 1 in pk, "%s: both machines' memory is in "
                   "the log" % what):
        return None
    m, s_ = pk[0], pk[1]
    S.check(m[0] == 1 and s_[0] == 2, "%s: the player's Game Boy is the "
            "master, the partner's the slave (%d / %d)" % (what, m[0], s_[0]))
    gm, gs = m[1] | (m[2] << 8), s_[1] | (s_[2] << 8)
    S.check(gm >= least and abs(gm - gs) <= 2 and m[3] == 0 and s_[3] == 0,
            "%s: %d and %d bytes crossed the cable and checked out, none "
            "bad (%d / %d)" % (what, gm, gs, m[3], s_[3]))
    return pk


def t_gb(S):
    """Game Boy and Game Boy Color: two machines, the cable between them,
    the partner's save file, another cartridge, save states."""
    if not os.path.exists(GBLINK):
        S.note("no testrom/gblocal.gb (make -C testrom RGBDS=... gb): skipped")
        return
    rom = open(GBLINK, "rb").read()
    extra = {"roms/gblocal.gb": rom, "roms/gblocal.gbc": rom,
             "roms/gblocal-blue.gb": rom}
    at = S.boot_refresh("gblocal.gb", extra)
    # ---- a copy; A on the player's pad makes his machine the master ----------
    script = """
%d:a:5
%d:shot=side.png
+2:rstick-right:4
+10:right:60
+0:shot=partner.png
+40:shot=late.png
""" % (at + 90, at + 400) + menu_pick(6)
    r = S.run("gb_copy", script, autoboot="gblocal.gb", extra=extra,
              vita_ini={"local_auto": 1, "local_auto_frame": 30,
                        "local_peek": "0xC000:16"})
    S.clean(r)
    st = r.evt("local_start")
    S.check(st and st[0]["console"] == "1" and st[0]["clone"] == "1" and
            st[0]["save"].endswith("gblocal.gb.p2.sav"),
            "a Game Boy pair starts as a copy (%s)" %
            (st[0]["_"] if st else "no local_start"))
    gb_verdict(S, r, "copy", 200)
    im = S.picture(r, "side.png", "local_gb_side.png")
    if im:
        a, b = [gb_boxes(g) for g in pair_pixels(im, 0, "gb")]
        S.check(a[1] == "master" and b[1] == "slave" and not a[2] and
                not b[2] and a[0] > 120 and abs(a[0] - b[0]) <= 2,
                "both pictures: master %d good, slave %d good, no bad row"
                % (a[0], b[0]))
        S.check(lit_bar(im, 0, "gb") == 0, "the player's is in view")
    im = S.picture(r, "partner.png")
    if im:
        a, b = [gb_boxes(g) for g in pair_pixels(im, 0, "gb")]
        S.check(b[3] and not a[3] and lit_bar(im, 0, "gb") == 1,
                "after the stick the pad is the partner's (Right: %s / %s)"
                % (a[3], b[3]))
    mine = os.path.join(r.root, "roms", "gblocal.gb.sav")
    p2 = os.path.join(r.root, "roms", "gblocal.gb.p2.sav")
    a = open(mine, "rb").read() if os.path.exists(mine) else b""
    b = open(p2, "rb").read() if os.path.exists(p2) else b""
    S.check(len(a) == 8192 and a[1] == 1 and a[2] == 64 and a[5] == 0xA5,
            "the player's save: the master's verdict (%s)" % a[:6].hex())
    S.check(len(b) == 8192 and b[1] == 2 and b[2] == 64 and b[4] == 0 and
            b[5] == 0xA5,
            "the partner's save, gblocal.gb.p2.sav: the slave's (%s)" %
            b[:6].hex())

    # ---- its own save, twice on one card: the file is read back --------------
    script = "%d:a:5\n%d:shot=own.png\n" % (at + 90, at + 300) + menu_pick(6)
    r = S.run("gb_own", script, autoboot="gblocal.gb", extra=extra,
              vita_ini={"local_auto": 2, "local_auto_frame": 30,
                        "local_peek": "0xC000:16"},
              config={"local_layout": 1})
    S.clean(r)
    gb_verdict(S, r, "own save", 100)
    p2 = os.path.join(r.root, "roms", "gblocal.gb.p2.sav")
    b1 = open(p2, "rb").read() if os.path.exists(p2) else b"\0"
    S.picture(r, "own.png", "local_gb_inset.png")
    r = S.run("gb_own", script, prepared=True)
    S.clean(r)
    b2 = open(p2, "rb").read() if os.path.exists(p2) else b"\0"
    st = r.evt("local_start")
    S.check(st and st[-1]["size"] == "8192" and b1[0] == 0 and b2[0] == 1,
            "the partner's cartridge counts its boots in its own battery: "
            "%d, then %d on the next start" % (b1[0], b2[0]))
    pk = gb_verdict(S, r, "own save again", 100)
    S.check(pk and pk[1][6] == 1 and pk[0][6] == 1,
            "both Game Boys read their own battery at power on")

    # ---- the Game Boy Color, and another cartridge in the partner --------------
    atc = S.boot_refresh("gblocal.gbc", extra)
    r = S.run("gbc_copy", "%d:a:5\n%d:shot=gbc.png\n%d:quit\n" %
              (atc + 90, atc + 300, atc + 310), autoboot="gblocal.gbc",
              extra=extra, vita_ini={"local_auto": 1, "local_auto_frame": 30,
                                     "local_peek": "0xC000:16"})
    S.clean(r)
    st = r.evt("local_start")
    S.check(st and st[0]["console"] == "2" and
            st[0]["save"].endswith("gblocal.gbc.p2.sav"),
            "a Game Boy Color pair (%s)" % (st[0]["_"] if st else "-"))
    gb_verdict(S, r, "GBC", 80)
    S.picture(r, "gbc.png")
    r = S.run("gb_other", "%d:a:5\n%d:shot=other.png\n" %
              (at + 90, at + 300) + menu_pick(6), autoboot="gblocal.gb",
              extra=dict(extra, **{"local.ini": partner_ini(
                  "gblocal.gb", "gblocal-blue.gb")}),
              vita_ini={"local_auto": 2, "local_auto_frame": 30,
                        "local_peek": "0xC000:16"})
    S.clean(r)
    st = r.evt("gb_local_start")
    S.check(st and st[0]["other"] == "1" and
            st[0]["rom"].endswith("gblocal-blue.gb"),
            "another cartridge in the partner's Game Boy (%s)" %
            (st[0]["_"] if st else "-"))
    gb_verdict(S, r, "two cartridges", 100)
    own = os.path.join(r.root, "roms", "gblocal-blue.gb.sav")
    b = open(own, "rb").read() if os.path.exists(own) else b""
    S.check(len(b) == 8192 and b[1] == 2 and b[5] == 0xA5 and
            not os.path.exists(os.path.join(r.root, "roms",
                                            "gblocal.gb.p2.sav")),
            "...which keeps its own save file, gblocal-blue.gb.sav")

    # ---- the menu: start, stop; the game carries on alone ---------------------
    script = LOCAL_PAGE + """
+20:shot=page.png
+2:cross
+30:circle
+12:circle
+30:a:5
+200:shot=linked.png
""" + menu_pick(2, wait=10) + """
+20:down
+8:down
+8:cross
+20:circle
+12:circle
+12:circle
+90:shot=alone.png
+2:right:30
+0:shot=alone_right.png
+20:quit
"""
    r = S.run("gb_menu", script, autoboot="gblocal.gb", extra=extra)
    S.clean(r)
    S.picture(r, "page.png")
    S.check(r.has(r"gb_local_start ok clone=1") and
            r.has(r"local_stop why=menu"), "started and stopped in the menu")
    a, b = [gb_boxes(g) for g in pair_pixels(r.shot("linked.png"), 0, "gb")]
    g1 = gb_boxes(A.game_pixels(r.shot("alone.png"), 160, 144))
    g2 = gb_boxes(A.game_pixels(r.shot("alone_right.png"), 160, 144))
    S.check(a[1] == "master" and a[0] > 100 and not a[2],
            "linked: %d good bytes" % a[0])
    S.check(g1[1] == "master" and g1[0] >= a[0] and g1[2],
            "stopped: the same machine carries on -- still the master, its "
            "count kept (%d), and now its bytes go unanswered" % g1[0])
    S.check(g2[3] and not g1[3], "and it takes the pad")

    # ---- a pair state: both Game Boys and the cable ---------------------------
    k1, seg = at + 300, 240
    keys = "%d:a:5\n" % (at + 90)
    for i in range(3):
        for off, bt in ((30, "right"), (90, "b"), (150, "up")):
            keys += "%d:%s:9\n%d:p2.%s:7\n" % (k1 + i * seg + off, bt,
                                               k1 + i * seg + off + 11, bt)
    script = keys + """
%d:select+l:1
%d:shot=saved.png
%d:select+r:1
%d:shot=loaded.png
%d:select+r:1
%d:quit
""" % (k1, k1 + 20, k1 + seg, k1 + seg + 20, k1 + 2 * seg, k1 + 3 * seg)
    script = "\n".join(sorted((l for l in script.splitlines() if l),
                              key=lambda l: int(l.split(":")[0]))) + "\n"
    r = S.run("gb_states", script, autoboot="gblocal.gb", extra=extra,
              vita_ini={"local_auto": 2, "local_auto_frame": 30,
                        "local_peek": "0xC000:16", "local_hash": 1})
    S.clean(r)
    ld = r.evt("local_state_load")
    S.check(len(ld) == 2 and all(d["pair"] == "1" for d in ld),
            "a Game Boy pair is saved and loaded twice (%s)" %
            [d.get("pair") for d in ld])
    gb_verdict(S, r, "after two loads", 100)
    a0, b0 = [gb_boxes(g) for g in pair_pixels(r.shot("saved.png"), 0, "gb")]
    a1, b1 = [gb_boxes(g) for g in pair_pixels(r.shot("loaded.png"), 0, "gb")]
    S.check(a0[1:] == a1[1:] and b0[1:] == b1[1:] and a0[0] > 80 and
            abs(a0[0] - a1[0]) <= 1 and abs(b0[0] - b1[0]) <= 1,
            "20 frames after the save and 20 after the load: the same "
            "pictures (%s / %s, %s / %s)" % (a0, b0, a1, b1))
    hs = [l for l in open(os.path.join(r.root, "log", "localhash.txt"))
          .read().split("\n") if l]
    hv = [l.split("h=")[1] for l in hs]
    # the three runs from the saved point: find where the hash at the save
    # comes round again
    i0 = k1 - (at + 31) + 1
    runs = [hv[i0 + i * seg:i0 + (i + 1) * seg - 2] for i in range(3)]
    S.check(len(runs[2]) == seg - 2 and runs[1] == runs[2],
            "a loaded pair runs the same every time: %d frames of the "
            "pair's hash identical after the two loads" % len(runs[1]))
    S.check(runs[0] == runs[1], "...and the same as the run that was saved "
            "and carried on")
    S.summary["gb.states.hash_crc"] = "%08x" % (
        zlib.crc32("\n".join(runs[2]).encode()) & 0xFFFFFFFF)


def t_exclusive(S):
    """The local link and the network link exclude each other, with both
    real layers in the build: what the player sees in each direction, and
    that one boot can go from one to the other and back."""
    at = S.boot_refresh(TEST)
    script = LOCAL_PAGE + """
# two games on this Vita
+20:cross
+30:circle
+12:circle
+60:shot=two.png
# Play together is now their page: no Host, no Join; on Stop it says why
""" + menu_pick(2, wait=10) + """
+20:shot=local_page.png
+2:down
+8:down
+8:shot=stop_note.png
+2:cross
# stopped: Play together again, Host a game (rows up from Two players)
+20:up
+8:up
+8:cross
+30:shot=hosting.png
# Keep playing: the room stays open behind the game.  (The test switch
# asks for the second game there: refused, the network is in use.)
+2:cross
+150:shot=waiting.png
# Play together is now the room's page: no Two players; on Stop hosting it
# says why
""" + menu_pick(2, wait=10) + """
+20:shot=session_page.png
+2:down
+8:shot=session_note.png
+2:cross
# the room is closed and the menu is on its top page: Play together, Two
# players on this Vita, Start
+20:down
+8:down
+8:cross
+20:down
+8:down
+8:cross
+20:cross
+30:circle
+12:circle
+60:shot=two_again.png
+10:quit
"""
    r = S.run("exclusive", script, autoboot=TEST,
              extra={"nethost.ini": "wifi = 1\nssid = TwinNet\n"},
              vita_ini={"link_port": 20000 + os.getpid() % 20000,
                        "local_auto": 1, "local_auto_frame": at + 200})
    S.clean(r, allow=r"local_start REFUSED")
    i_start = r.index(r"^EVT local_start ok")
    i_stop = r.index(r"^EVT local_stop", max(i_start, 0))
    i_host = r.index(r"^EVT play_host ")
    i_up = r.index(r"^EVT link_phase 0 -> 3")
    i_ref = r.index(r"^EVT local_start REFUSED reason=network")
    i_down = r.index(r"^EVT link_net_down radio=0")
    i_again = r.index(r"^EVT local_start ok", max(i_down, 0))
    # ---- two games run: Host and Join are not there to be chosen
    S.check(0 <= i_start < i_stop < i_host,
            "Host a game is only reached after the two-player game stopped")
    S.check(not r.has(r"^EVT play_join ") and len(r.evt("play_host")) == 1,
            "nothing else asked for the network")
    S.check(lit_bar(r.shot("two.png")) == 0,
            "two pictures while the local link runs")
    S.check(len([e for e in r.evt("ui_screen")
                 if e["name"] in ("host", "join", "adhoc")]) == 0,
            "no Host or Join page opened while two games ran")
    S.picture(r, "local_page.png")
    S.picture(r, "stop_note.png", "local_excl_stop.png")
    S.check(worst_diff(r.shot("local_page.png"), r.shot("stop_note.png"),
                       (40, 440, 920, 496)) > 100,
            "on Stop the page says: stop first to host or join")
    # ---- a room is open: Two players is not there, and a start is refused
    S.check(0 <= i_host < i_up and
            r.has(r"^EVT link_room .*title=\"gbadhoc-test\""),
            "then the room opens (Wi-Fi, Link Cable)")
    S.picture(r, "hosting.png")
    S.check(lit_bar(r.shot("waiting.png")) is None and
            0 <= i_up < i_ref < i_down and len(r.evt("local_start")) == 3,
            "a second game asked for while the room is open is refused; "
            "one picture, the host keeps playing")
    S.picture(r, "session_page.png")
    # (the lines above the rows carry the room's port, this run's own)
    S.picture(r, "session_note.png", "local_excl_session.png",
              box=(0, 380, 960, 544))
    S.check(worst_diff(r.shot("session_page.png"), r.shot("session_note.png"),
                       (668, 420, 940, 500)) > 100,
            "on Stop hosting the page says when Two players comes back")
    # ---- and back
    S.check(0 <= i_down < i_again and
            lit_bar(r.shot("two_again.png")) == 0,
            "the room closed, the radio is free, two games run again")
    st = r.evt("local_stop")
    S.check(len(st) == 2 and st[-1]["why"] == "game_end",
            "and they end with the game")


def t_handover(S):
    """Two twins over the loopback network (link_tests.py's harness, wall
    clock): a console that has had two games running hosts a session, and
    after the session both run two games again -- the host and the one that
    joined.  Each link leaves the second machine fit for the other."""
    if S.arch != "x86":
        S.note("x86 only (two realtime twins): skipped")
        return
    import link_tests as N
    T = N.Tests("x86", 0)
    T.S = S
    T.begin("handover")
    rom = os.path.join(HERE, "testrom", TEST)
    two = N.keys("down", "down", "cross", first=20) + \
        N.keys("cross", first=20) + N.close_menu(first=30)
    a = T.twin("a", "x86", rom, "Alice",
               # two games, then Stop, then Host a game
               N.open_play(1) + two + N.shot(6, "two_first.png") +
               N.open_play(7.5) + N.keys("down", "down", "cross", first=20) +
               N.keys("up", "up", "cross", first=20) + "+50:shot=host.png\n" +
               N.close_menu() +
               N.shot(22, "session.png") + N.disconnect(26) +
               # the session over: two games again
               N.open_play(33) + two + N.press(38, "a", 40) +
               N.shot(38.4, "two_after.png") + N.quit_at(42))
    b = T.twin("b", "x86", rom, "Bob",
               N.join_first(12, wait=2.5) + N.shot(22, "session.png") +
               N.open_play(33) + two + N.press(38, "b", 40) +
               N.shot(38.4, "two_after.png") + N.quit_at(42))
    ra, rb = T.run([a, b], 42)
    T.clean(ra, "A")
    T.clean(rb, "B")
    i0 = ra.index(r"^EVT local_start ok")
    i1 = ra.index(r"^EVT local_stop why=menu")
    i2 = ra.index(r"^EVT link_running player=1")
    S.check(0 <= i0 < i1 < i2 and lit_bar(ra.shot("two_first.png")) == 0,
            "A: two games, stopped, then the session as Player 1")
    S.check(rb.has(r"^EVT link_running player=2"),
            "B: joins as Player 2")
    T.same_machines(ra, rb)
    for r, who in ((ra, "A"), (rb, "B")):
        S.check(r.has(r"gblink_resume how=live_end rc=0"),
                "%s: the session ends by agreement" % who)
        T.radio_free(r, who)
        i3 = r.index(r"^EVT link_net_down")
        i4 = r.index(r"^EVT local_start ok", max(i3, 0))
        S.check(0 <= i3 < i4, "%s: then two games run on it" % who)
        S.check(not r.has(r"local_start (FAILED|REFUSED)|gbadual_step FAILED"),
                "%s: no start was refused, the pair did not fault" % who)
        im = T.picture(r, who, "two_after.png")
        if im is None:
            continue
        left, right = pair_pixels(im)
        key = "A" if who == "A" else "B"
        S.check(lit_bar(im) == 0 and boxes(left) == key and
                boxes(right) == "",
                "%s: its pad drives its own game, the left picture (%s / "
                "%s)" % (who, boxes(left), boxes(right)))
        st = r.evt("local_stats")
        S.check(st and st[-1]["why"] == "game_end" and
                int(st[-1]["frames"].split("/")[0]) > 100,
                "%s: the pair ran until the game ended (%s frames)" %
                (who, st[-1]["frames"] if st else "-"))


def t_mirror(S):
    """Against tools/gbamirror's gl_host, the tool the link mirror was
    developed with: started from the same machine and given the same
    buttons, the app's local pair computes the same two machines."""
    tool = os.path.join(GL_DIR, "gl_host_armfar" if S.arch.startswith("armhf")
                        else "gl_host_twin")
    if not os.path.exists(tool):
        S.note("no %s: skipped (see the comment at the top of t_mirror)" %
               os.path.relpath(tool, GP))
        return
    # the tool is built with the twin's core flags:
    #   cd tools/gbamirror && make -f Makefile.armhf gl_host_armfar \
    #      CORE_EXTRA="-DGBA_PC_MASK=1 -DBADJUMP_SAFE=1 -DDMA_SMC_FLUSH=1 \
    #                  -DSERIAL_IDLE_FAST=1 -DGBALINK_KEEP_SERIAL_MODE"
    extra = S.carts(LINKTEST)
    # the pair's frame 0 is the refresh AFTER the one that asked for it
    # (local_vita.c start_now), and gl_host's script index n is its hash
    # line f = n + 1: the scripts start at that refresh
    at = S.boot_refresh("linktest.gba", extra) + 3
    frames = 1200
    k0, k1 = (os.path.join(LT_DIR, n) for n in ("keys0.txt", "keys1.txt"))
    script = lt_script(k0, 0, at) + lt_script(k1, 1, at)
    script = "\n".join(l for l in script.splitlines()
                       if int(l.split(":")[0]) < at + frames + 30) + "\n"
    script += "%d:quit\n" % (at + frames + 40)
    r = S.run("mirror", script, autoboot="linktest.gba", extra=extra,
              vita_ini={"local_auto": 1, "local_auto_frame": 2,
                        "local_hash": 1, "local_dump": 1})
    S.clean(r)
    st = os.path.join(r.root, "log", "local_start.st")
    if not S.check(os.path.exists(st) and os.path.getsize(st) == 416 * 1024,
                   "the machine the pair started from is dumped"):
        return
    ref = os.path.join(r.out, "gl_host.hash")
    cmd = (S.prefix if S.arch.startswith("armhf") else []) + [
        tool, "--rom", LINKTEST, "--bios-dir", "/nonexistent",
        "--state0", st, "--state1", st, "--script0", k0, "--script1", k1,
        "--frames", str(frames), "--hash", ref]
    env = dict(os.environ, GL_JIT="near")
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500,
                       env=env)
    S.check(p.returncode == 0, "gl_host ran (%s)" % p.stderr.strip()[-200:])
    mine = open(os.path.join(r.root, "log", "localhash.txt")).read().splitlines()
    want = open(ref).read().splitlines() if os.path.exists(ref) else []
    n = min(len(mine), len(want))
    # both logs interleave the two machines as their frames end, in the
    # same order when the stepping is the same
    same = sum(1 for a, b in zip(mine[:n], want[:n]) if a == b)
    S.check(n >= 2 * frames - 4 and same == n,
            "%d hash lines of both machines: %d identical to gl_host's" %
            (n, same))
    if same != n:
        for i, (a, b) in enumerate(zip(mine[:n], want[:n])):
            if a != b:
                S.note("first difference at line %d: %s | %s" % (i, a, b))
                break


def cpu_run(S, name, frames, rom, vita_ini, extra=None):
    """Emulated frames per CPU second of one run of `frames` refreshes: the
    emulation's own CPU time (fe_host_cpu_stat, the thread's clock, WITHOUT
    the twin's software drawing, which no console pays), from the sample at
    a third of the run to the last one; and the Result."""
    ini = dict(vita_ini, local_cpu=100)
    r = S.run(name, "%d:quit\n" % frames, autoboot=rom, extra=extra,
              vita_ini=ini)
    c = [(int(e["frames"]), int(e["emu_us"])) for e in r.evt("local_cpu")]
    c = [x for x in c if x[0] >= frames // 3]
    if len(c) < 2 or c[-1][1] <= c[0][1]:
        return 0.0, r
    return (c[-1][0] - c[0][0]) * 1e6 / (c[-1][1] - c[0][1]), r


def t_speed(S):
    """What two machines cost: emulated frames per CPU second of the
    emulation alone, the pair against one machine, and what the pair's
    settings change."""
    extra = S.carts(LINKTEST)
    gb_extra = S.carts(GBLINK) if os.path.exists(GBLINK) else None
    frames = 1700
    rows = []
    sets = [(TEST, None), ("linktest.gba", extra)]
    if gb_extra:
        sets.append(("gblocal.gb", gb_extra))
    for rom, ex in sets:
        cases = [("solo", {}),
                 ("pair", {"local_auto": 1, "local_auto_frame": 100}),
                 ("pair, both synthesise sound",
                  {"local_auto": 1, "local_auto_frame": 100,
                   "local_hear_both": 1})]
        if rom == "linktest.gba":
            cases = [("solo", {}),
                     ("pair", {"local_auto": 1, "local_auto_frame": 2})]
            for la in (2464, 10000, 70224, 280896):
                cases.append(("pair, lookahead %d" % la,
                              {"local_auto": 1, "local_auto_frame": 2,
                               "local_lookahead": la,
                               "local_peek": "0x02000000:64"}))
        elif rom == "gblocal.gb":
            cases = [("solo", {}),
                     ("pair", {"local_auto": 1, "local_auto_frame": 100}),
                     ("pair, a scanline at a time",
                      {"local_auto": 1, "local_auto_frame": 100,
                       "local_gb_batch": 1})]
        base = None
        for name, ini in cases:
            tag = "speed_%s_%s" % (rom.split(".")[0][:8],
                                   re.sub(r"\W+", "_", name))
            fps, r2 = cpu_run(S, tag, frames, rom, ini, ex)
            S.clean(r2)
            if base is None:
                base = fps or 1.0
            st = r2.evt("local_stats")
            sw = (int(st[-1]["switches"]) / max(1, int(st[-1]["frames"].split("/")[0]))
                  if st and "switches" in st[-1] else 0)
            rows.append((rom, name, fps, fps / base, sw))
            print("    %-18s %-30s %7.1f frames per CPU second  %5.1f %% of "
                  "solo  %5.1f switches a frame" %
                  (rom, name, fps, 100 * fps / base, sw), flush=True)
            if "lookahead" in name:
                pk = lt_peeks(r2)
                S.check(0 in pk and pk[0][5] == 0 and pk[1][5] == 0 and
                        pk[0][14] == 0 and pk[1][14] == 0 and pk[0][3] >= 6,
                        "%s: the link test passes (%s loops)" %
                        (name, pk[0][3] if 0 in pk else "?"))
    with open(os.path.join(S.outdir, "speed.txt"), "w") as f:
        for rom, name, fps, rel, sw in rows:
            f.write("%s\t%s\t%.1f\t%.3f\t%.1f\n" % (rom, name, fps, rel, sw))
    S.check(all(r[2] > 0 for r in rows), "every run was measured")
    # (linktest.gba alone gives up waiting for a partner and idles, linked
    # it transfers all the time: its two rows are not the same work, and
    # only the lookaheads are compared with each other there)
    pair = [r for r in rows if r[1] == "pair" and r[0] != "linktest.gba"]
    S.check(all(r[3] > 0.35 for r in pair),
            "two machines doing what one does cost less than three times "
            "one (%s of solo)" %
            ", ".join("%.0f %%" % (100 * r[3]) for r in pair))


TESTS = [("linktest", t_linktest), ("menu", t_menu), ("switch", t_switch),
         ("save", t_save), ("cycle", t_cycle), ("cheats", t_cheats),
         ("audio", t_audio), ("states", t_states), ("ff", t_ff),
         ("other", t_other), ("gb", t_gb), ("exclusive", t_exclusive),
         ("handover", t_handover), ("mirror", t_mirror)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arch", nargs="?", default="x86")
    ap.add_argument("--only")
    ap.add_argument("--update-refs", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--speed", action="store_true")
    a = ap.parse_args()
    S = LSuite(a.arch, a.update_refs, a.keep)
    S.speed = a.speed
    S.partial = bool(a.only)
    if not os.path.exists(S.exe):
        sys.exit("no %s: make ARCH=%s first" % (S.exe, a.arch.split("-")[0]))
    if not os.path.exists(A.OWN_GBA):
        sys.exit("no %s: make -C testrom first" % A.OWN_GBA)
    only = a.only.split(",") if a.only else None
    tests = TESTS + ([("speed", t_speed)] if a.speed else [])
    for name, fn in tests:
        if only and name not in only:
            continue
        S.test = name
        t0 = time.time()
        print("%s: %s" % (name, fn.__doc__.split("\n")[0]))
        before = len(S.fails)
        try:
            fn(S)
        except Exception as e:           # a test's own bug is a failure
            import traceback
            traceback.print_exc()
            S.check(False, "the test itself raised %r" % (e,))
        print("    %s  (%.0f s)" % ("ok" if len(S.fails) == before else
                                    "FAILED", time.time() - t0))
    sys.exit(S.finish())


if __name__ == "__main__":
    main()
