#!/usr/bin/env python3
"""rewind_tests.py -- rewind (vita/rewind_vita.c) in the app, on a host twin.

    ./rewind_tests.py x86            the x86-64 twin
    ./rewind_tests.py armhf          the armhf twin under qemu-arm (the ARM
                                     dynarec, the one the Vita runs)
    ./rewind_tests.py x86-san        the x86 twin built SAN=1 (ASan + UBSan)
    options:  --only NAME[,NAME]   some of the tests
              --quick              the long test plays 3 minutes a console,
                                   not 20

app_tests.py's machinery (the twin's runner, its script, its screenshots)
with rewind's own questions:

  gba, gb, gbc   going back shows EARLIER FRAMES, bit for bit the pictures a
                 run that never went back showed at those frames; letting go
                 continues exactly as that run continued
  chord          the mapped shortcut (SELECT + the fast-forward button) does
                 the same, without fast-forwarding and without its SELECT
                 reaching the game
  battery        going back across an in-game save takes the cartridge's
                 battery back too, and the .sav file follows (GBA, where the
                 state does not hold it; GB and GBC, where it does) -- with
                 and without the worker thread
  guards         off in Settings, cleared by a state load, refused in the
                 bench pair; a small heap; the first games' hints
  biglist        (not rewind: the same pass) 500 games, crossed by letter
  long           20 minutes of play a console in a 4 MiB budget: the budget
                 holds, the ring wraps, and what is left is still right
  cost           what a state costs the frame thread, measured (--realtime)

HOW A PICTURE IS MATCHED.  The app logs `rewind end tl=K frame=F`: K is the
game's own frame number of the picture on screen when the control was let
go, F what fe_host had counted by then (it also counts the one frame each
step back runs).  A later screenshot at count E therefore shows frame
K + (E - F) -- and a second run of the same game that never rewinds, with a
screenshot at that frame, must show the same pixels.  Pictures taken WHILE
going back are matched through the cartridge's own frame counter, which the
test cartridges draw in binary.

Cartridges: hostcore/testrom (make -C testrom) and tests/rewind/rom
(make -C ../tests/rewind/rom).
"""
import argparse
import os
import sys
import time
import zlib

from PIL import ImageChops

import app_tests as A
from app_tests import Suite, menu_pick, game_pixels, EXIT_APP

HERE = os.path.dirname(os.path.abspath(__file__))
ROMS = os.path.join(HERE, "..", "tests", "rewind", "rom")
QUICK = False

# the consoles' pictures in the composed frame (3x, centred)
GBA = dict(w=240, h=160, scale=3)
GB = dict(w=160, h=144, scale=3)


def rom(name):
    return open(os.path.join(ROMS, name), "rb").read()


def extra(*names):
    return {"roms/" + n: rom(n) for n in names}


def pix(r, name, dims):
    return game_pixels(r.shot(name), **dims)


def same(a, b, mask_top=0):
    """The worst channel difference of two game pictures (0 = identical)."""
    if mask_top:
        a, b = a.copy(), b.copy()
        for im in (a, b):
            im.paste((0, 0, 0), (0, 0, im.size[0], mask_top))
    return max(hi for _, hi in ImageChops.difference(a, b).getextrema())


def counter_gba(im):
    """hostcore/testrom: sixteen boxes at y 100, the frame counter."""
    v = 0
    for i in range(16):
        v = (v << 1) | (1 if im.getpixel((8 + i * 14 + 5, 105))[0] > 128 else 0)
    return v


def bits_gb(im, row, n):
    """tests/rewind/rom/rwtest: n tiles of background row `row` from column
    2, a solid (dark) tile = 1."""
    v = 0
    for i in range(n):
        v = (v << 1) | (1 if im.getpixel(((2 + i) * 8 + 4, row * 8 + 4))[0] < 128 else 0)
    return v


def counter_gb(im):
    return bits_gb(im, 2, 16)


def shot_emu(r, name):
    d = r.shot_evt(name)
    return (int(d["frame"]), int(d["emu"])) if d else (None, None)


def rewind_ends(r):
    return [dict(tl=int(d["tl"]), frame=int(d["frame"]),
                 back=int(d["back_frames"]), oldest=int(d["oldest"]),
                 left=int(d["left"]), why=d["why"])
            for d in r.evt("rewind") if d["_"].startswith("EVT rewind end")]


def rewind_begins(r):
    return [dict(tl=int(d["tl"]), entries=int(d["entries"]),
                 frames=int(d["frames"]), interval=int(d["interval"]))
            for d in r.evt("rewind") if d["_"].startswith("EVT rewind begin")]


def stats(r):
    """The last `rewind_stats` line, numbers as ints ("a/b" as a tuple)."""
    out = {}
    for d in r.evt("rewind_stats"):
        out = {}
        for k, v in d.items():
            if k == "_":
                continue
            if "/" in v:
                out[k] = tuple(int(x) for x in v.split("/"))
            else:
                try:
                    out[k] = int(v)
                except ValueError:
                    out[k] = v
    return out


# ---- going back and carrying on ---------------------------------------------

def back_and_on(S, name, cart, dims, counter, control, extra_files=None,
                config=None, args=()):
    """Play, hold `control` for 96 refreshes, let go, play on; then a run
    that never went back, photographed at the frames the first one showed."""
    T0, H = 500, 96
    during = (T0 + 40, T0 + 70, T0 + 95)
    after = (T0 + H, T0 + H + 1, T0 + H + 2, T0 + H + 50, T0 + H + 300)
    script = "%d:shot=pre.png\n%d:%s:%d\n" % (T0 - 10, T0, control, H)
    for i, f in enumerate(during):
        script += "%d:shot=in%d.png\n" % (f, i)
    for i, f in enumerate(after):
        script += "%d:shot=post%d.png\n" % (f, i)
    script += "%d:quit\n" % (after[-1] + 5)
    r = S.run(name, script, autoboot=cart, extra=extra_files, config=config,
              args=args)
    S.clean(r)
    ends = rewind_ends(r)
    if not S.check(len(ends) == 1 and ends[0]["why"] == "released",
                   "one rewind, ended by letting go (%s)" % ends):
        return None
    K, F = ends[0]["tl"], ends[0]["frame"]
    pre_ref, pre_emu = shot_emu(r, "pre.png")
    c = pre_ref - pre_emu                  # refresh number - game frame
    began = rewind_begins(r)[0]
    S.check(40 <= ends[0]["back"] <= 4 * H and K == began["tl"] -
            ends[0]["back"] + 1,
            "it went back %d frames, to frame %d (from %d)" %
            (ends[0]["back"], K, began["tl"]))
    S.check(not ends[0]["oldest"], "without running out of history")

    # which game frame each picture should be
    want = {}
    g0 = counter(pix(r, "pre.png", dims)) if counter else None
    seen = []
    for i in range(len(during)):
        if not counter:
            break
        g = counter(pix(r, "in%d.png" % i, dims))
        seen.append(g)
        want["in%d.png" % i] = g - g0 + pre_emu
    if counter:
        S.check(seen == sorted(seen, reverse=True) and seen[0] < g0 and
                len(set(seen)) == len(seen),
                "the pictures while going back are earlier and earlier "
                "frames (the cartridge's own counter: %d before, then %s)" %
                (g0, seen))
    for i in range(len(after)):
        _, e = shot_emu(r, "post%d.png" % i)
        want["post%d.png" % i] = K + (e - F)
    S.check(want["post0.png"] == K + 1, "the frame after the one on screen "
            "is the next one played")

    # the run that never went back
    script = "".join("%d:shot=%s\n" % (t + c, n) for n, t in want.items())
    script += "%d:quit\n" % (max(want.values()) + c + 5)
    s = S.run(name + "_straight", script, autoboot=cart, extra=extra_files,
              config=config, args=args)
    S.clean(s)
    S.check(not rewind_ends(s), "the straight run did not rewind")
    for n, t in sorted(want.items()):
        _, e = shot_emu(s, n)
        if not S.check(e == t, "straight run: %s is frame %d (%s)" % (n, t, e)):
            continue
        mask = 8 if n.startswith("in") else 0      # the "<< REWIND" chip
        d = same(pix(r, n, dims), pix(s, n, dims), mask)
        S.check(d == 0, "%s is frame %d of the straight run, bit for bit "
                "(worst difference %d)" % (n, t, d))
    # the chip, and that it goes away
    chip = r.shot("in1.png").crop((700, 8, 950, 56))
    S.check(len(chip.getcolors(4096) or []) > 4, "the rewind chip is on "
            "screen while going back")
    S.check(r.shot("post3.png").crop((700, 8, 950, 30)).getcolors(4096) ==
            s.shot("post3.png").crop((700, 8, 950, 30)).getcolors(4096),
            "and gone afterwards")
    S.summary[name + ".back_frames"] = ends[0]["back"]
    return r


def t_gba(S):
    """GBA: back in time, bit for bit, and on from there (the right stick)."""
    r = back_and_on(S, "rw_gba", "gbadhoc-test.gba", GBA, counter_gba,
                    "rstick-left")
    if r:
        st = stats(r)
        S.check(st.get("landings", 0) > 20 and st.get("skipped") == 0,
                "states were restored (%s landings)" % st.get("landings"))
    # ...and a game that rewrites kilobytes a frame
    back_and_on(S, "rw_busy", "rwbusy.gba", GBA, None, "rstick-left",
                extra("rwbusy.gba"))


def t_gb(S):
    """GB: the same."""
    back_and_on(S, "rw_gb", "rwtest.gb", GB, counter_gb, "rstick-left",
                extra("rwtest.gb"))


def t_gbc(S):
    """GBC: the same."""
    back_and_on(S, "rw_gbc", "rwtest.gbc", GB, counter_gb, "rstick-left",
                extra("rwtest.gbc"))


def t_async(S):
    """With the worker thread squeezing the states, as on the console."""
    r = back_and_on(S, "rw_async", "gbadhoc-test.gba", GBA, counter_gba,
                    "rstick-left", args=("--worker",))
    if r:
        S.check(r.has(r"rewind on .*async=1"), "the worker has the squeeze")
    back_and_on(S, "rw_async_gbc", "rwtest.gbc", GB, counter_gb,
                "rstick-left", extra("rwtest.gbc"), args=("--worker",))


def t_chord(S):
    """The mapped shortcut: SELECT + the fast-forward button."""
    r = back_and_on(S, "rw_chord", "gbadhoc-test.gba", GBA, counter_gba,
                    "select+square")
    if r:
        S.check(not r.has(r"^EVT ff on"), "fast forward did not engage")
    # SELECT stays down after SQUARE is let go: the game must not see it
    # until it has been released; a fresh press it does see.
    script = ("400:select+square:60\n460:select:40\n480:shot=held.png\n"
              "520:select:20\n530:shot=fresh.png\n540:quit\n")
    r = S.run("rw_chord_keys", script, autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.check(A.key_boxes(r.shot("held.png")) == "",
            "SELECT still held after the rewind is not the game's (%s)" %
            A.key_boxes(r.shot("held.png")))
    S.check(A.key_boxes(r.shot("fresh.png")) == "Select",
            "a fresh SELECT is (%s)" %
            A.key_boxes(r.shot("fresh.png")))
    # the same stick the other way: fast forward, while it is held
    r = S.run("rw_ffstick", "300:rstick-right:60\n380:shot=after.png\n"
              "420:quit\n", autoboot="gbadhoc-test.gba")
    S.clean(r)
    on, off = r.index(r"^EVT ff on"), r.index(r"^EVT ff off")
    S.check(0 <= on < off and not rewind_ends(r) and
            int(r.host.get("emulated", 0)) > 420 + 80,
            "the right stick held right fast-forwards while it is held "
            "(%s frames in 420 refreshes)" % r.host.get("emulated"))
    # a rebound shortcut: L+R, written as the Controls page writes it
    r = S.run("rw_rebound", "400:l+r:60\n500:select+square:30\n560:quit\n",
              autoboot="gbadhoc-test.gba", config={"bind_rewind": "L+R"})
    S.clean(r)
    S.check(len(rewind_ends(r)) == 1, "bind_rewind = L+R rewinds with L+R, "
            "and SELECT+SQUARE no longer does (%d rewinds)" %
            len(rewind_ends(r)))


# ---- the battery --------------------------------------------------------------

def sav_crc(first64):
    return zlib.crc32(bytes(first64) + b"\xff" * (0x20000 - 64)) & 0xffffffff


def t_battery(S):
    """GBA: back across an in-game save -- the cartridge and the .sav too."""
    p1 = [(i * 7 + 16) & 255 for i in range(64)]
    p2 = [(i * 7 + 32) & 255 for i in range(64)]
    # A at 200 (save 1) and at 260 (save 2); by 900 the second save is in
    # the file; then back to between the two.
    T0, H = 900, 6 + 189
    for name, args in (("rw_battery", ()), ("rw_battery_worker", ("--worker",))):
        script = ("200:a:5\n260:a:5\n%d:shot=before.png\n"
                  "%d:rstick-left:%d\n%d:shot=after.png\n%d:shot=later.png\n"
                  % (T0 - 5, T0, H, T0 + H + 1, T0 + H + 30))
        script += "%d:start+select:20\n" % (T0 + H + 400)
        script += "".join("+8:down\n" for _ in range(6)) + "+8:cross\n+8:cross\n"
        r = S.run(name, script, autoboot="gbadhoc-test.gba", args=args)
        S.clean(r)
        ends = rewind_ends(r)
        if not S.check(len(ends) == 1, "one rewind (%s)" % ends):
            continue
        K, F = ends[0]["tl"], ends[0]["frame"]
        pre_ref, pre_emu = shot_emu(r, "before.png")
        c = pre_ref - pre_emu
        S.check(205 - c < K < 255 - c, "it went back to between the two "
                "saves (game frame %d; they were at %d and %d)" %
                (K, 200 - c, 260 - c))
        begin = r.index(r"^EVT rewind begin")
        end = r.index(r"^EVT rewind end")
        flushed2 = r.index(r"sram_flush .*crc=%08x" % sav_crc(p2))
        S.check(0 <= flushed2 < begin, "the second save was in the .sav "
                "file before the rewind")
        flushed1 = r.index(r"sram_flush .*crc=%08x" % sav_crc(p1), end)
        S.check(flushed1 > end, "afterwards the file was written again, "
                "with the first save")
        sav = open(os.path.join(r.root, "roms", "gbadhoc-test.sav"), "rb").read()
        S.check(len(sav) == 0x20000 and list(sav[:64]) == p1,
                "the .sav holds the FIRST save (starts %s)" %
                sav[:4].hex())
        S.check(stats(r).get("battery_steps", 0) >= 2,
                "two steps of the history carried the battery (%s)" %
                stats(r).get("battery_steps"))

        # the run that saved once and never went back
        want = {}
        for n in ("after.png", "later.png"):
            _, e = shot_emu(r, n)
            want[n] = K + (e - F)
        script = "200:a:5\n"
        script += "".join("%d:shot=%s\n" % (t + c, n) for n, t in want.items())
        script += "%d:start+select:20\n" % (max(want.values()) + c + 400)
        script += "".join("+8:down\n" for _ in range(6)) + "+8:cross\n+8:cross\n"
        s = S.run(name + "_straight", script, autoboot="gbadhoc-test.gba",
                  args=args)
        S.clean(s)
        for n in want:
            d = same(pix(r, n, GBA), pix(s, n, GBA))
            S.check(d == 0, "%s: the game, its battery box included, is the "
                    "run that saved once (difference %d)" % (n, d))
        sav2 = open(os.path.join(s.root, "roms", "gbadhoc-test.sav"), "rb").read()
        S.check(sav == sav2, "and so is the .sav file, byte for byte")
        # the picture before: the second save's colours, not the first's
        S.check(same(pix(r, "before.png", GBA).crop((112, 58, 152, 78)),
                     pix(r, "after.png", GBA).crop((112, 58, 152, 78))) > 0,
                "(the battery box did change between the two)")


def t_battery_gb(S):
    """GB and GBC: the same, where the state carries the cartridge RAM."""
    T0, H = 900, 6 + 189
    for cart in ("rwtest.gb", "rwtest.gbc"):
        name = "rw_bat_" + cart.split(".")[1]
        script = ("200:a:5\n260:a:5\n%d:shot=before.png\n"
                  "%d:rstick-left:%d\n%d:shot=after.png\n" %
                  (T0 - 5, T0, H, T0 + H + 1))
        script += "%d:start+select:20\n" % (T0 + H + 400)
        script += "".join("+8:down\n" for _ in range(6)) + "+8:cross\n+8:cross\n"
        r = S.run(name, script, autoboot=cart, extra=extra(cart))
        S.clean(r)
        ends = rewind_ends(r)
        if not S.check(len(ends) == 1, "%s: one rewind" % cart):
            continue
        b, a = pix(r, "before.png", GB), pix(r, "after.png", GB)
        S.check((bits_gb(b, 4, 8), bits_gb(b, 6, 8)) == (0x20, 2),
                "%s before: two saves made (RAM %02x, count %d)" %
                (cart, bits_gb(b, 4, 8), bits_gb(b, 6, 8)))
        S.check((bits_gb(a, 4, 8), bits_gb(a, 6, 8)) == (0x10, 1),
                "%s after: back at the first (RAM %02x, count %d)" %
                (cart, bits_gb(a, 4, 8), bits_gb(a, 6, 8)))
        sav = open(os.path.join(r.root, "roms", cart + ".sav"), "rb").read()
        S.check(list(sav[:64]) == list(range(0x10, 0x50)),
                "%s: the .sav holds the first save (starts %s)" %
                (cart, sav[:4].hex()))


# ---- the guards ----------------------------------------------------------------

def t_guards(S):
    """Off in Settings; cleared by a state load; refused in the bench pair."""
    r = S.run("rw_off", "300:rstick-left:60\n400:shot=on.png\n420:quit\n",
              autoboot="gbadhoc-test.gba", config={"rewind": 0})
    S.clean(r)
    S.check(not r.has(r"^EVT rewind on") and r.has(r"rewind refused "
            r"reason=off") and not rewind_ends(r),
            "rewind = 0: no memory taken, the control refused")
    _, e = shot_emu(r, "on.png")
    f, _ = shot_emu(r, "on.png")
    S.check(f - e < 12, "and the game played on meanwhile (%d frames in %d "
            "refreshes)" % (e, f))

    # quick save at 200, quick load at 500: at 520 there are 20 frames of
    # history, not 520
    r = S.run("rw_stateload", "200:select+l:3\n500:select+r:3\n"
              "520:rstick-left:200\n700:shot=oldest.png\n740:quit\n",
              autoboot="gbadhoc-test.gba")
    S.clean(r)
    b, e = rewind_begins(r), rewind_ends(r)
    S.check(r.has(r"state_load slot=1 via=chord rc=0"), "the state loaded")
    S.check(b and b[0]["frames"] < 40 and e and e[0]["oldest"] == 1 and
            e[0]["back"] < 40,
            "after a state load the history starts over (%s frames held, "
            "went back %s)" % (b[0]["frames"] if b else "?",
                               e[0]["back"] if e else "?"))
    S.check(len((r.shot("oldest.png").crop((560, 8, 950, 56))
                 .getcolors(4096) or [])) > 4,
            "the chip says so at the oldest point")

    # the menu's Settings row: GAMEPLAY > Rewind (wrapping upwards: Button
    # mapping, THIS GAME's Display settings, Keep full speed, FPS counter,
    # Rewind), stepped once to the
    # left of "1 min" is "30 s"; the game loop hears of it when the menu
    # closes
    r = S.run("rw_setting", menu_pick(4, wait=120) + "+20:up\n+8:up\n+8:up\n"
              "+8:up\n+8:up\n+10:shot=row.png\n+4:left\n+10:shot=row30.png\n"
              "+4:circle\n+10:circle\n+60:rstick-left:30\n+40:quit\n",
              autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.check(r.has(r"rewind_setting seconds=30"), "Settings > Rewind steps "
            "to 30 s")
    S.check(r.has(r"^EVT rewind on seconds=30"), "and the buffers were "
            "rebuilt for it")
    S.check(len(rewind_ends(r)) == 1, "and rewind works afterwards")
    cfg = open(os.path.join(r.root, "config.ini")).read()
    S.check("rewind = 30" in cfg and "rewind_mb" not in cfg,
            "config.ini has rewind = 30 (and no rewind_mb it was not given)")

    # a heap with little to spare: the rewind takes less, or nothing, and
    # 40 MiB stay for a link's second cartridge
    r = S.run("rw_heap50", "300:rstick-left:60\n420:quit\n",
              autoboot="gbadhoc-test.gba", args=("--heap-free-mb", "50"))
    S.clean(r)
    on = [d for d in r.evt("rewind") if d["_"].startswith("EVT rewind on")]
    S.check(on and int(on[0]["total"]) <= 10 << 20 and
            len(rewind_ends(r)) == 1,
            "50 MiB of heap left: rewind takes %s bytes (10 MiB at most) and "
            "works" % (on[0]["total"] if on else "?"))
    r = S.run("rw_heap42", "300:rstick-left:60\n420:quit\n",
              autoboot="gbadhoc-test.gba", args=("--heap-free-mb", "42"))
    S.clean(r)
    S.check(r.has(r"rewind off reason=heap") and
            r.has(r"rewind refused reason=memory") and not rewind_ends(r),
            "42 MiB left: no rewind, and the control says why")

    # a fresh install: the first three games begin with two lines of help
    r = S.run("rw_hints", "100:shot=hint1.png\n260:shot=hint2.png\n" +
              menu_pick(6, wait=200), autoboot="gbadhoc-test.gba",
              config={"hints": 3})
    S.clean(r)
    S.check(r.has(r"hints shown have=3") and r.has(r"hints counted left=2"),
            "the first game shows the hints, and opening the menu counts it")
    for n in ("hint1.png", "hint2.png"):
        S.check(len(r.shot(n).crop((200, 8, 760, 60)).getcolors(4096) or
                    []) > 8, "%s: a line of help is on screen" % n)
    S.check("hints = 2" in open(os.path.join(r.root, "config.ini")).read(),
            "and config.ini counts them down")
    # ...but only for a player who has found the menu
    r = S.run("rw_hints_unseen", "100:shot=hint1.png\n300:quit\n",
              autoboot="gbadhoc-test.gba", config={"hints": 3})
    S.clean(r)
    S.check(r.has(r"hints shown have=3") and not r.has(r"hints counted") and
            "hints = 2" not in open(os.path.join(r.root, "config.ini")).read(),
            "a game in which the menu was never opened does not use one up")

    r = S.run("rw_bench", "300:rstick-left:60\n420:quit\n",
              autoboot="gbadhoc-test.gba", vita_ini={"link_bench": 1})
    S.clean(r)
    S.check(r.has(r"rewind refused reason=locked") and not rewind_ends(r),
            "the bench pair (two machines) is not rewound")


# ---- the rest of the extras pass --------------------------------------------------

def t_biglist(S):
    """A game list of 500: L and R cross it by initial letter."""
    import random
    random.seed(11)
    names = set()
    while len(names) < 500:
        names.add("%s%s %s %d" % (random.choice("ABCDEFGHKLMNPRSTWZ"),
                                  random.choice("aeiou"),
                                  random.choice(("Quest", "Kart", "Saga",
                                                 "Land", "Wars")),
                                  random.randint(1, 999)))
    files = {"roms/" + n + ".gba": b"\0" * 0x400 for n in names}
    # the list opens on the test cartridge ("gbadhoc-test", a G); R, R, R
    # and X: the game three initials on; then the model below says which
    out = os.path.join(S.outdir, "rw_biglist")
    root = os.path.join(out, "root")
    import shutil
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    S.make_root(root, None, None, None, files)
    order = S.roms(root, ".gba")

    def initial(n):
        c = n[0].upper()
        return c if "A" <= c <= "Z" else "#"

    def page(cur, d):
        if d > 0:
            for i in range(cur + 1, len(order)):
                if initial(order[i]) != initial(order[cur]):
                    return i
            return 0
        i = cur
        while i > 0 and initial(order[i - 1]) == initial(order[cur]):
            i -= 1
        if i != cur:
            return i
        j = cur - 1 if cur > 0 else len(order) - 1
        while j > 0 and initial(order[j - 1]) == initial(order[j]):
            j -= 1
        return j

    cur = order.index("gbadhoc-test.gba")
    script = "60:shot=list.png\n"
    for key, d in (("r", 1), ("r", 1), ("r", 1), ("l", -1), ("down", 0),
                   ("l", -1), ("l", -1)):
        script += "+12:%s\n" % key
        cur = page(cur, d) if d else (cur + 1) % len(order)
    script += "+40:shot=jumped.png\n+2:cross\n+60:quit\n"
    r = S.run("rw_biglist", script, prepared=True)
    S.check(r.rc == 0, "exit code 0")
    b = r.evt("ui_browser")
    S.check(b and int(b[0]["roms"]) == len(order), "the list holds %d games"
            % len(order))
    S.check(r.has(r"ui_browser_pick rom=%s$" % order[cur].replace("(", r"\(")),
            "R R R L down L L lands on %s (log: %s)" %
            (order[cur], [d["_"] for d in r.evt("ui_browser_pick")]))
    # (the "games" are a kilobyte of zeros each: long enough to be taken
    # for a cartridge by size, and refused by their header)
    S.check(r.has(r"boot_refused reason=not_a_cartridge header=") and
            r.has(r'notice title="This game could not be started"'),
            "a file of zeros under a game's name gets the notice, not a run")
    S.check(int(r.host.get("refreshes", 0)) and r.wall < 60,
            "and the list of 500 is not slow (%.1f s for %s refreshes)" %
            (r.wall, r.host.get("refreshes")))


# ---- twenty minutes --------------------------------------------------------------

def t_long(S):
    """20 minutes of play a console in a 4 MiB budget."""
    minutes = 3 if QUICK else 20
    frames = minutes * 60 * 60
    # Unlimited fast forward (5.5 frames a refresh on the twin's clock, a
    # picture drawn for one in eleven): the play is the same, the test is
    # shorter.  SQUARE switches it on and off.
    ff_refreshes = frames * 3000 // 16667 + 50
    for cart, dims, counter, deep in (
            ("rwbusy.gba", GBA, None, 500),
            ("rwtest.gb", GB, counter_gb, 2700),
            ("rwtest.gbc", GB, counter_gb, 2700)):
        name = "rw_long_" + cart.split(".")[1]
        t_off = 30 + ff_refreshes
        t1 = t_off + 700                 # ordinary play, photographed
        H1 = 6 + 150                     # back about 480 frames
        script = "30:square:3\n%d:square:3\n" % t_off
        pre = [t1 - 600 + 60 * i for i in range(10)]
        for i, f in enumerate(pre):
            script += "%d:shot=pre%d.png\n" % (f, i)
        script += "%d:rstick-left:%d\n" % (t1, H1)
        post0 = t1 + H1
        for i in range(60):
            script += "%d:shot=post%d.png\n" % (post0 + i, i)
        t2 = post0 + 200
        script += "%d:rstick-left:%d\n%d:shot=deep.png\n%d:quit\n" % (
            t2, deep, t2 + deep - 3, t2 + deep + 30)
        t0 = time.time()
        r = S.run(name, script, autoboot=cart, extra=extra(cart),
                  config={"rewind": 300, "rewind_mb": 4, "ff_mult_x10": 0,
                          "ff_hold": 0, "ff_smooth": 0})
        S.clean(r)
        st = stats(r)
        emu = int(r.host.get("emulated", 0))
        S.check(emu >= frames, "%s: %d emulated frames = %.1f minutes of "
                "play (%.0f s of the wall)" % (cart, emu, emu / 3600.0,
                                               time.time() - t0))
        on = r.evt("rewind")
        on = [d for d in on if d["_"].startswith("EVT rewind on")]
        total = int(on[0]["total"]) if on else 1 << 30
        S.check(total <= 4 << 20, "%s: %d bytes allocated, within the "
                "4 MiB budget" % (cart, total))
        S.check(st and st["used"] <= st["ring"],
                "%s: %s of the ring's %s bytes in use" %
                (cart, st.get("used"), st.get("ring")))
        ends = rewind_ends(r)
        if not S.check(len(ends) == 2, "%s: two rewinds (%d)" %
                       (cart, len(ends))):
            continue
        # 1. a little way back: one of sixty consecutive frames after it is
        #    a frame photographed before
        K, F = ends[0]["tl"], ends[0]["frame"]
        was = {}
        for i in range(len(pre)):
            _, e = shot_emu(r, "pre%d.png" % i)
            was[e] = "pre%d.png" % i
        hit = 0
        for i in range(60):
            _, e = shot_emu(r, "post%d.png" % i)
            t = K + (e - F)
            if t in was:
                d = same(pix(r, "post%d.png" % i, dims), pix(r, was[t], dims))
                S.check(d == 0, "%s: frame %d played again is the frame "
                        "that was played the first time (difference %d)" %
                        (cart, t, d))
                hit += 1
        S.check(hit >= 1, "%s: a replayed frame was compared (%d)" %
                (cart, hit))
        # 2. all the way back
        b2 = rewind_begins(r)[1]
        S.check(ends[1]["oldest"] == 1 and ends[1]["left"] == 0,
                "%s: held to the oldest point: %d frames (%.1f min) back, "
                "%d steps at interval %d" %
                (cart, ends[1]["back"], ends[1]["back"] / 3600.0,
                 b2["entries"], b2["interval"]))
        S.check(ends[1]["back"] >= b2["frames"] and
                ends[1]["tl"] == b2["tl"] - ends[1]["back"] + 1,
                "%s: every frame of the history was gone back over" % cart)
        if counter:
            # the cartridge's own counter agrees with where the log says
            # the machine is (16 bits: it wraps every 18 minutes)
            g_pre = counter(pix(r, "pre0.png", dims))
            _, e_pre = shot_emu(r, "pre0.png")
            g = counter(pix(r, "deep.png", dims))
            S.check((g - g_pre) & 0xFFFF == (ends[1]["tl"] - e_pre) & 0xFFFF,
                    "%s: the picture at the oldest point is frame %d by "
                    "the cartridge's own counter" % (cart, ends[1]["tl"]))
        else:
            S.check(st.get("dropped", 0) > 0, "%s: the ring was full and "
                    "wrapped (%s steps dropped)" % (cart, st.get("dropped")))
        held_s = ends[1]["back"] / 59.7275
        S.summary[name + ".history_s"] = round(held_s, 1)
        S.summary[name + ".interval"] = b2["interval"]
        S.summary[name + ".ring_bytes"] = st.get("ring")
        S.summary[name + ".bytes_per_step"] = (
            st["packed"] // max(1, st["captures"] - 1))
        S.note("%s: %.0f s of history in a %.2f MiB ring (%.0f s/MiB), "
               "interval %d, %d bytes a step, ratio %.2f %%" %
               (cart, held_s, st["ring"] / 1048576.0,
                held_s / (st["ring"] / 1048576.0), b2["interval"],
                st["packed"] // max(1, st["captures"] - 1),
                100.0 * st["packed"] / max(1, st["raw"])))


# ---- what it costs -----------------------------------------------------------------

def t_cost(S):
    """The frame thread's cost of a state, on the wall clock (--realtime)."""
    for cart, files in (("gbadhoc-test.gba", None),
                        ("rwbusy.gba", extra("rwbusy.gba")),
                        ("rwtest.gbc", extra("rwtest.gbc"))):
        row = {}
        for mode, args in (("sync", ("--realtime",)),
                           ("worker", ("--realtime", "--worker")),
                           ("off", ("--realtime",))):
            r = S.run("rw_cost_%s_%s" % (cart.split(".")[0], mode),
                      "", autoboot=cart, extra=files,
                      config={"rewind": 0 if mode == "off" else 60},
                      args=args + ("--frames", "900"))
            S.clean(r)
            st = stats(r)
            win = r.evt("vita_win")
            work = [int(d["work_us"].split("/")[0]) for d in win]
            row[mode] = (st, work[-1] if work else 0)
            if mode == "sync":
                on = [d for d in r.evt("rewind") if "state" in d]
                state = int(on[0]["state"]) if on else 0
        st, work = row["sync"]
        stw, _ = row["worker"]
        n = max(1, st.get("captures", 1))
        cap = st["capture_us"][0] / n          # take + squeeze, one thread
        pack = st["pack_us"][0] / n
        capw = stw["capture_us"][0] / max(1, stw.get("captures", 1))
        # (a twin under qemu-arm does not hold real time: fewer frames, and
        # so fewer states, in the same fifteen seconds)
        S.check(st.get("captures", 0) > 20, "%s: states were taken (%s)" %
                (cart, st.get("captures")))
        S.note("%s: state %d bytes every %d frames: take %.0f us + squeeze "
               "%.0f us on one thread (worst %d); with the worker the frame "
               "thread pays %.0f us (worst %d), skipped %d.  A frame's work "
               "here: %d us with rewind, %d us without" %
               (cart, state, st["interval"], cap - pack, pack, st["capture_us"][1], capw,
                stw["capture_us"][1], stw.get("skipped", 0), work,
                row["off"][1]))
        key = "rw_cost_" + cart.split(".")[0]
        S.summary[key + ".take_us"] = round(cap - pack)
        S.summary[key + ".squeeze_us"] = round(pack)
        S.summary[key + ".frame_thread_us_with_worker"] = round(capw)


TESTS = [("gba", t_gba), ("gb", t_gb), ("gbc", t_gbc), ("async", t_async),
         ("chord", t_chord), ("battery", t_battery),
         ("battery_gb", t_battery_gb), ("guards", t_guards),
         ("biglist", t_biglist), ("cost", t_cost), ("long", t_long)]


def main():
    global QUICK
    ap = argparse.ArgumentParser()
    ap.add_argument("arch", nargs="?", default="x86")
    ap.add_argument("--only")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    QUICK = a.quick
    S = Suite(a.arch, False, a.keep)
    S.outdir = os.path.join(HERE, "build", "rewindtest", a.arch)
    if not os.path.exists(S.exe):
        sys.exit("no %s: make ARCH=%s first" % (S.exe, a.arch.split("-")[0]))
    for f in (A.OWN_GBA, os.path.join(ROMS, "rwtest.gb"),
              os.path.join(ROMS, "rwbusy.gba")):
        if not os.path.exists(f):
            sys.exit("no %s: make -C testrom and make -C ../tests/rewind/rom "
                     "first" % f)
    only = a.only.split(",") if a.only else None
    for name, fn in TESTS:
        if only and name not in only:
            continue
        S.test = name
        t0 = time.time()
        print("%s: %s" % (name, fn.__doc__.split("\n")[0]))
        before = len(S.fails)
        try:
            fn(S)
        except Exception as e:                 # a test's own bug is a failure
            import traceback
            traceback.print_exc()
            S.check(False, "the test itself raised %r" % (e,))
        print("    %s  (%.0f s)" % ("ok" if len(S.fails) == before else
                                    "FAILED", time.time() - t0))
    sys.exit(S.finish())


if __name__ == "__main__":
    main()
