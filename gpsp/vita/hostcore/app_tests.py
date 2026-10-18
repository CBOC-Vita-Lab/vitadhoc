#!/usr/bin/env python3
"""app_tests.py -- the Vita APP's tests, on a host twin.

    ./app_tests.py x86                 the x86-64 twin (the x86 dynarec)
    ./app_tests.py armhf               the armhf twin under qemu-arm (the ARM
                                       dynarec, the one the Vita runs)
    ./app_tests.py armhf-vitasim       ...built VITASIM=1 (the Vita's own
                                       translation-cache path)
    ./app_tests.py x86-san             the x86 twin built SAN=1 (the app's
                                       objects under ASan + UBSan)
    options:  --only NAME[,NAME]   run some of the tests
              --update-refs        (x86) rewrite ref/*.png from this run
              --keep               keep each test's data root

What run_tests.sh is to the cores, this is to main_vita.c: the whole app --
the PSP UI's game list, the menu, save states, fast forward, the return to
the game list -- driven by plat_host.c's input script on simulated time and
checked three ways: the event log (the app says what it did), the files it
left (saves, states and their previews, screenshots), and the composed
960x544 picture at chosen frames.

PICTURES.  ref/*.png are the x86 twin's, committed.  An x86 run must
reproduce them exactly; an ARM run within a few levels a channel (the two
dynarecs differ in the lowest green bit of a GBA palette entry, see
run_tests.sh).  Every picture a run takes is left in build/apptest/<arch>/
for looking at; the test fails on a difference, never on a missing
reference.

The data root of every test is a copy of the fixture ($TESTROOT, default
/path/to/testroot: gba-tests, dmg-acid2, cgb-acid2) plus the port's
own cartridges from testrom/ (make -C testrom; `make -C testrom gb` for the
GB one).  Without the fixture the GB and GBC legs are skipped.
"""
import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.environ.get("TESTROOT", "/path/to/testroot")
QEMU = os.environ.get("QEMU", "qemu-arm -L /usr/arm-linux-gnueabihf").split()
OWN_GBA = os.path.join(HERE, "testrom", "gbadhoc-test.gba")
OWN_GB = os.path.join(HERE, "testrom", "gbsave.gb")
# the cheats workstream's cartridge: a "game" with lives and gold at known
# addresses, drawn as coloured blocks (make -C ../tests/cheats/rom)
CHEAT_GBA = os.path.join(HERE, "..", "tests", "cheats", "rom", "cheatrom.gba")
REF = os.path.join(HERE, "ref")

# Lines that must not be in the log of a run that went well.
BAD = re.compile(r"boot_failed|boot_refused|FAILED|state_load .*rc=-|"
                 r"sram_sync_timeout|evt_gap|quit_game|notice title|"
                 r"config_save_refused|screenshot failed|inproc_refused")


class Result:
    def __init__(self, rc, err, root, out, wall):
        self.rc, self.err, self.root, self.out, self.wall = rc, err, root, out, wall
        try:
            self.log = open(os.path.join(root, "log", "gbadhoc.log"),
                            errors="replace").read().splitlines()
        except OSError:
            self.log = []
        m = re.search(r"host: exit (\d+) after (\d+) refreshes \((\d+) "
                      r"presented, (\d+) emulated frames\).*audio "
                      r"frames=(\d+) crc=([0-9a-f]+) peak=(\d+)", err)
        self.host = dict(zip(("exit", "refreshes", "presented", "emulated",
                              "audio_frames", "audio_crc", "peak"),
                             m.groups())) if m else {}

    def evt(self, name):
        """Every `EVT name k=v ...` line as a dict (plus '_' = the line)."""
        out = []
        for line in self.log:
            if line.startswith("EVT " + name + " ") or line == "EVT " + name:
                d = dict(re.findall(r"(\w+)=(\"[^\"]*\"|\S*)", line))
                d["_"] = line
                out.append(d)
        return out

    def has(self, pattern):
        rx = re.compile(pattern)
        return any(rx.search(line) for line in self.log)

    def index(self, pattern, start=0):
        rx = re.compile(pattern)
        for i in range(start, len(self.log)):
            if rx.search(self.log[i]):
                return i
        return -1

    def shot(self, name):
        return Image.open(os.path.join(self.out, name)).convert("RGB")

    def shot_evt(self, name):
        for d in self.evt("host_shot"):
            if d.get("file") == name:
                return d
        return None

    def bad_lines(self):
        return [line for line in self.log if BAD.search(line)]


class Suite:
    def __init__(self, arch, update_refs, keep):
        self.arch, self.update, self.keep = arch, update_refs, keep
        self.exe = os.path.join(HERE, "build", arch, "gbadhoc_host")
        self.prefix = QEMU if arch.startswith("armhf") else []
        self.outdir = os.path.join(HERE, "build", "apptest", arch)
        self.fails, self.checks, self.notes = [], 0, []
        self.summary = {}
        self.test = "?"
        self.have_fixture = os.path.isdir(os.path.join(FIXTURE, "roms"))

    # ---- the data root ----------------------------------------------------
    def make_root(self, root, config, autoboot, vita_ini, extra):
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(os.path.join(root, "roms"))
        if self.have_fixture:
            for sub in ("roms", "boxart", "hero"):
                src = os.path.join(FIXTURE, sub)
                if os.path.isdir(src):
                    shutil.copytree(src, os.path.join(root, sub),
                                    dirs_exist_ok=True)
            # only cartridges: no saves or states from an earlier use
            for f in glob.glob(os.path.join(root, "roms", "*")):
                if not f.lower().endswith((".gba", ".gb", ".gbc")):
                    os.remove(f)
        shutil.copy(OWN_GBA, os.path.join(root, "roms"))
        if os.path.exists(OWN_GB):
            shutil.copy(OWN_GB, os.path.join(root, "roms"))
        # GBA_BIOS=/path/to/gba_bios.bin: every test with the real BIOS in
        # the data root (the file is its owner's and is never in the tree)
        if os.path.isfile(os.environ.get("GBA_BIOS", "")):
            shutil.copy(os.environ["GBA_BIOS"],
                        os.path.join(root, "gba_bios.bin"))
        for name, data in (extra or {}).items():
            os.makedirs(os.path.dirname(os.path.join(root, name)),
                        exist_ok=True)
            with open(os.path.join(root, name), "wb") as f:
                f.write(data if isinstance(data, bytes) else data.encode())
        # hints = 0: the first games' two lines of help are not in every
        # test's pictures (rewind_tests.py has the test that wants them)
        # wide_gba = 0: the tests' pictures are the console's 240 columns
        # with bars beside them (the wide view has its own test, which
        # hands None to see the key's default)
        # ui_home = 0: the list tests walk the shelf with UP / DOWN (the
        # home screen has its own test, t_home)
        cfg = {"last_rom": "gbadhoc-test.gba", "hints": 0, "wide_gba": 0,
               "ui_home": 0}
        cfg.update(config or {})
        with open(os.path.join(root, "config.ini"), "w") as f:
            for k, v in cfg.items():
                if v is not None:
                    f.write("%s = %s\n" % (k, v))
        if autoboot:
            with open(os.path.join(root, "autoboot.txt"), "w") as f:
                f.write(autoboot + "\n")
        if vita_ini:
            with open(os.path.join(root, "vita.ini"), "w") as f:
                for k, v in vita_ini.items():
                    f.write("%s = %s\n" % (k, v))

    def roms(self, root, ext):
        """The game list's order for one console: by name, case-blind."""
        names = [os.path.basename(f) for f in
                 glob.glob(os.path.join(root, "roms", "*"))
                 if f.lower().endswith(ext)]
        return sorted(names, key=str.lower)

    def run(self, name, script, config=None, autoboot=None, vita_ini=None,
            args=(), extra=None, prepared=None, cwd=None):
        out = os.path.join(self.outdir, name)
        root = os.path.join(out, "root")
        if prepared is None:
            shutil.rmtree(out, ignore_errors=True)
            os.makedirs(out)
            self.make_root(root, config, autoboot, vita_ini, extra)
        with open(os.path.join(out, "script.txt"), "w") as f:
            f.write(script)
        cmd = self.prefix + [self.exe, "--root", root, "--shot-dir", out,
                             "--wav", os.path.join(out, "audio.wav"),
                             "--input-file", os.path.join(out, "script.txt")]
        cmd += list(args)
        t0 = time.time()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=1500, env=dict(os.environ, TZ="UTC"),
                               cwd=cwd)
            rc, err = p.returncode, p.stderr
        except subprocess.TimeoutExpired:
            rc, err = -99, "TIMEOUT"
        with open(os.path.join(out, "stderr.txt"), "w") as f:
            f.write(err)
        return Result(rc, err, root, out, time.time() - t0)

    # ---- checks -----------------------------------------------------------
    def check(self, cond, what):
        self.checks += 1
        if not cond:
            self.fails.append("%s: %s" % (self.test, what))
            print("    FAIL  " + what)
        return bool(cond)

    def note(self, what):
        self.notes.append("%s: %s" % (self.test, what))
        print("    note  " + what)

    def clean(self, r, allow=None):
        """Exit code 0, the app's own goodbye, nothing alarming in the log."""
        self.check(r.rc == 0, "exit code 0 (got %s) %s" %
                   (r.rc, r.err.strip()[-200:] if r.rc else ""))
        self.check(r.has(r"^EVT exit ok"), "the log ends with 'exit ok'")
        bad = [b for b in r.bad_lines() if not (allow and re.search(allow, b))]
        self.check(not bad, "a clean log (%s)" % "; ".join(bad[:3]))

    def picture(self, r, name, ref=None, box=None, blank_ok=False):
        """A screenshot exists, is not one flat colour, and matches ref/."""
        try:
            im = r.shot(name)
        except OSError:
            self.check(False, "screenshot %s was taken" % name)
            return None
        if not blank_ok:
            self.check(len(im.getcolors(1 << 20) or [0, 0]) > 1,
                       "%s is not a blank picture" % name)
        if ref:
            self.compare_ref(im, ref, box)
        return im

    def compare_ref(self, im, ref, box=None):
        path = os.path.join(REF, ref)
        if self.update and self.arch == "x86":
            os.makedirs(REF, exist_ok=True)
            im.save(path, optimize=True)
        if not os.path.exists(path):
            self.note("no reference %s yet (--update-refs on x86)" % ref)
            return
        want = Image.open(path).convert("RGB")
        a, b = (im.crop(box), want.crop(box)) if box else (im, want)
        diff = ImageChops.difference(a, b)
        worst = max(hi for _, hi in diff.getextrema())
        # exact on the build the references come from; a level or two for
        # another x86 build (the text rasteriser's float rounding); the
        # palette's low green bit for the ARM dynarec
        limit = 0 if self.arch == "x86" else \
            2 if self.arch.startswith("x86") else 8
        self.check(worst <= limit, "%s matches ref/%s (worst channel "
                   "difference %d, allowed %d)" % (ref, ref, worst, limit))

    def finish(self):
        os.makedirs(self.outdir, exist_ok=True)
        with open(os.path.join(self.outdir, "summary.json"), "w") as f:
            json.dump(self.summary, f, indent=1, sort_keys=True)
        # the two dynarecs, where both have run: the sound must be the same
        other = "armhf" if self.arch.startswith("x86") else "x86"
        try:
            theirs = json.load(open(os.path.join(HERE, "build", "apptest",
                                                 other, "summary.json")))
        except (OSError, ValueError):
            theirs = None
        if theirs:
            self.test = "x86-vs-arm"
            for k in sorted(self.summary):
                if k in theirs and k.endswith(".audio_crc"):
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


# ---- script helpers --------------------------------------------------------
def menu_pick(row, wait=40):
    """Open the in-game menu (START+SELECT held) and choose its row:
    0 Resume, 1 Save states, 2 Play together, 3 Cheats, 4 Settings,
    5 Quit to game list, 6 Exit VitAdhoc (the last two ask first, and this
    answers yes).  The menu is the Vita's control centre, its cards in a
    row; DOWN steps along them as RIGHT does."""
    s = "+%d:start+select:20\n" % wait
    gap = 14
    for _ in range(row):
        s += "+%d:down\n" % gap
        gap = 8
    s += "+%d:cross\n" % gap
    if row >= 5:
        s += "+8:cross\n"          # leaving asks first: X again answers it
    return s


QUIT_TO_LIST = menu_pick(5)
EXIT_APP = menu_pick(6)


def decode_shot(path):
    """<state>.shot -> (w, h, RGB image), or None if it is not one."""
    data = open(path, "rb").read()
    if len(data) < 16 or data[:4] != b"STH1":
        return None
    w, h, stride = struct.unpack("<HHH", data[4:10])
    if stride != 256 or not 0 < w <= 240 or not 0 < h <= 160:
        return None
    if len(data) != 16 + h * 256 * 2:
        return None
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        row = struct.unpack_from("<%dH" % w, data, 16 + y * 512)
        for x in range(w):
            v = row[x]           # PSP order: R in bits 0-4, B in 11-15
            r, g, b = v & 31, (v >> 5) & 63, v >> 11
            px[x, y] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4),
                        (b << 3) | (b >> 2))
    return w, h, im


def game_pixels(im, w=240, h=160, scale=3):
    """The game's own pixels out of a composed 960x544 picture drawn at an
    integer scale, centred."""
    x0, y0 = (960 - w * scale) // 2, (544 - h * scale) // 2
    out = Image.new("RGB", (w, h))
    src, dst = im.load(), out.load()
    for y in range(h):
        for x in range(w):
            dst[x, y] = src[x0 + x * scale + scale // 2,
                            y0 + y * scale + scale // 2]
    return out


def worst_diff(a, b, box=None):
    if box:
        a, b = a.crop(box), b.crop(box)
    return max(hi for _, hi in ImageChops.difference(a.convert("RGB"),
                                                     b.convert("RGB")).getextrema())


def wav_stats(path):
    data = open(path, "rb").read()[44:]
    n = len(data) // 2
    if not n:
        return 0, 0, 0
    s = struct.unpack("<%dh" % n, data[:n * 2])
    peak = max(abs(v) for v in s)
    loud = sum(1 for v in s if abs(v) > 256)
    return n // 2, peak, loud / n


# ---- the tests ---------------------------------------------------------------

def t_list(S):
    """The game list comes up with the data root's cartridges."""
    r = S.run("list", "90:shot=list.png\n+30:triangle\n+60:shot=list_gb.png\n"
                      "+10:quit\n")
    S.clean(r)
    gba = S.roms(r.root, ".gba")
    b = r.evt("ui_browser")
    S.check(b and int(b[0]["roms"]) == len(gba) and b[0]["console"] == "0",
            "the list holds the %d GBA cartridges of roms/ (log: %s)" %
            (len(gba), b[0]["_"] if b else "no ui_browser line"))
    S.check(r.host.get("emulated") == "0", "nothing was emulated")
    S.picture(r, "list.png", "list.png" if S.have_fixture else None)
    gb = S.roms(r.root, ".gb")
    sw = r.evt("ui_browser_console")
    S.check(sw and sw[0]["console"] == "1" and int(sw[0]["roms"]) == len(gb),
            "TRIANGLE turns to the GB list with its %d cartridges" % len(gb))
    S.picture(r, "list_gb.png")


def t_session(S):
    """One long sitting: play, the menu, states, shortcuts, presets, the
    pause screen, back to the list, a GB and a GBC game, a state from the
    shelf, exit from the menu."""
    fix = S.have_fixture
    script = """
# ---- game 1: the port's own GBA cartridge (preselected in the list) ----
60:cross
+3:shot=loading.png
+150:shot=game.png
# the menu, over the game: START+SELECT held; it opens on the 15th refresh
# of the hold, so the picture presented on the 14th (13 after the first) is
# the last frame of the game
+6:start+select:13
+0:shot=presave.png
+0:start+select:7
+30:shot=menu.png
# Save states > slot 2 > save (square)
+2:down
+8:cross
+20:down
+10:square
+30:shot=states.png
# back to the menu's top page, and out
+2:circle
+10:circle
+40:shot=p1.png
# change something: A (cross) lights a box and writes the battery RAM
+2:a:40
+0:shot=changed.png
+20:shot=p2.png
# Save states > slot 2 > load (cross): the menu closes itself
+2:start+select:20
+30:down
+8:cross
+20:down
+10:cross
+40:shot=p3.png
# quick save, quick load (slot 1), screenshot
+2:select+l:3
+20:shot=quicksave.png
+2:select+r:3
+20:l+r+select:3
+20:shot=aftershot.png
# the video presets: TRIANGLE five times comes back to 3x
+2:triangle
+20:shot=fit.png
+2:triangle
+20:shot=fit_sharp.png
+2:triangle
+20:shot=stretch.png
+2:triangle
+20:triangle
+20:shot=3x_again.png
# the pause screen (SELECT+TRIANGLE), continue
+60:select+triangle:3
+20:shot=pause.png
+2:cross
+30:shot=resumed.png
""" + QUIT_TO_LIST + """
+4:shot=returning.png
+70:shot=list_after.png
"""
    if fix:
        script += """
# ---- game 2: GB (TRIANGLE turns the list to the next console) ----
+2:triangle
+40:cross
+150:shot=gb.png
""" + QUIT_TO_LIST + """
# ---- game 3: GBC ----
+70:triangle
+40:cross
+150:shot=gbc.png
""" + QUIT_TO_LIST + """
+70:triangle
+40:shot=list_gba.png
"""
    # ---- last game: GBA again, from slot 2 on the list's state shelf ----
    out = os.path.join(S.outdir, "session")
    root = os.path.join(out, "root")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    S.make_root(root, None, None, None, None)
    gba = S.roms(root, ".gba")
    script += "\n"
    if fix:
        # the list opens on its first row here (the last game was not GBA)
        for _ in range(gba.index("gbadhoc-test.gba")):
            script += "+8:down\n"
    script += """
+10:left
+30:shot=shelf.png
+2:down
+10:cross
+60:shot=from_shelf.png
""" + EXIT_APP
    r = S.run("session", script, prepared=True)
    S.clean(r)
    games = r.evt("session")
    S.check(len(games) == (4 if fix else 2),
            "%d games ran in one process (log: %d)" %
            (4 if fix else 2, len(games)))

    # -- game 1 --
    S.check(r.has(r"ui_browser_pick rom=gbadhoc-test.gba"), "the list's pick")
    S.check(r.has(r"rom_loaded code=AGDE"), "the GBA cartridge loaded")
    S.picture(r, "loading.png")
    S.picture(r, "game.png", "game.png")
    S.picture(r, "menu.png", "menu.png")
    # the Save states page stamps the slot with the wall clock: compare the
    # preview panel only
    S.picture(r, "states.png", "states.png", box=(470, 120, 960, 455))

    # states: the files, as the PSP writes them
    stem = os.path.join(r.root, "roms", "gbadhoc-test")
    S.check(r.has(r"state_save slot=2 thumb=0 shot=0"), "slot 2 was saved")
    for slot in (1, 0):
        st = "%s.st%d" % (stem, slot)
        S.check(os.path.exists(st) and os.path.getsize(st) > 100000,
                "%s exists" % os.path.basename(st))
        th = st + ".thumb"
        data = open(th, "rb").read() if os.path.exists(th) else b""
        S.check(len(data) == 16 + 64 * 42 * 2 and data[:4] == b"STH1" and
                data[4:10] == bytes([64, 0, 42, 0, 64, 0]) and
                len(set(data[16:])) > 4,
                "%s is a PSP thumbnail (STH1, 64x42, %d bytes)" %
                (os.path.basename(th), len(data)))
        sh = decode_shot(st + ".shot") if os.path.exists(st + ".shot") else None
        S.check(sh and sh[0] == 240 and sh[1] == 160,
                "%s.shot is a PSP full-size preview (STH1, 240x160 at a "
                "256 stride)" % os.path.basename(st))
        if slot == 1 and sh:
            # ...and it is the picture the player was looking at
            pre = game_pixels(r.shot("presave.png"))
            S.check(worst_diff(pre, sh[2]) == 0,
                    "slot 2's .shot is the frame on screen when the menu "
                    "opened (difference %d)" % worst_diff(pre, sh[2]))
    S.check(not os.path.exists(stem + ".st2"), "no other slot was written")

    # the picture returns: p1 and p3 are the same distance (in emulated
    # frames) from the saved frame, with something else in between
    opens = [int(d["frame"]) for d in r.evt("menu_open")]
    p1, p2, p3 = (r.shot_evt(n) for n in ("p1.png", "p2.png", "p3.png"))
    S.check(r.has(r"state_load slot=2 via=menu rc=0"), "slot 2 was loaded")
    if S.check(p1 and p3 and len(opens) >= 2, "p1 and p3 were taken"):
        d1, d3 = int(p1["emu"]) - opens[0], int(p3["emu"]) - opens[1]
        S.check(d1 == d3, "p1 and p3 are equally far from the saved frame "
                "(%d and %d emulated frames)" % (d1, d3))
        # The game's own pixels, less two places: the top rows, where p3
        # still carries the "State loaded" toast, and the test cartridge's
        # battery-RAM box -- a state does not hold the battery RAM (it is
        # the .sav's), so the pattern A wrote in between stays.
        def state_part(name):
            im = game_pixels(r.shot(name))
            im.paste((0, 0, 0), (0, 0, 240, 16))
            im.paste((0, 0, 0), (112, 58, 152, 78))
            return im
        a, b, c = (state_part(n) for n in ("p1.png", "p2.png", "p3.png"))
        S.check(worst_diff(a, c) == 0,
                "after the load the picture is the one after the save "
                "(difference %d)" % worst_diff(a, c))
        S.check(worst_diff(a, b) > 0 and
                worst_diff(game_pixels(r.shot("changed.png")),
                           game_pixels(r.shot("p1.png"))) > 0,
                "...and in between it was a different one")
        box = (112, 58, 152, 78)
        S.check(worst_diff(game_pixels(r.shot("p3.png")).crop(box),
                           game_pixels(r.shot("p2.png")).crop(box)) == 0 and
                worst_diff(game_pixels(r.shot("p3.png")).crop(box),
                           game_pixels(r.shot("p1.png")).crop(box)) > 0,
                "the battery RAM is not rolled back by a state")
    S.check(r.has(r"state_save slot=1 ") and
            r.has(r"state_load slot=1 via=chord rc=0"),
            "quick save and quick load (slot 1)")
    S.picture(r, "quicksave.png")

    # battery save, screenshot
    sav = stem + ".sav"
    S.check(os.path.exists(sav) and os.path.getsize(sav) == 131072 and
            len(set(open(sav, "rb").read())) > 1,
            "the battery save is beside the ROM, 128 KiB, written")
    shots = glob.glob(os.path.join(r.root, "screenshots", "gbadhoc-test-*.png"))
    S.check(len(shots) == 1 and Image.open(shots[0]).size == (240, 160),
            "one 240x160 screenshot in screenshots/")
    S.check(len(r.evt("state_save")) == 4,
            "the screenshot chord did not also quick-save (state_save "
            "lines: %d)" % len(r.evt("state_save")))

    # presets
    modes = [(d["scale"], d["_"].split("filter=")[1]) for d in r.evt("video_mode")]
    S.check(modes == [("fit", "nearest"), ("fit", "sharp bilinear"),
                      ("stretch", "nearest"), ("stretch", "sharp bilinear"),
                      ("3x", "nearest")],
            "the preset cycle: fit, fit sharp, stretch, stretch sharp, 3x "
            "(%s)" % modes)
    S.picture(r, "fit.png", "fit.png")
    S.picture(r, "fit_sharp.png")
    S.picture(r, "stretch.png")
    S.check(worst_diff(game_pixels(r.shot("3x_again.png")),
                       game_pixels(r.shot("3x_again.png"))) == 0 and
            r.shot("3x_again.png").getpixel((60, 272)) == (0, 0, 0),
            "back at 3x the bars beside the picture are black")

    # pause
    S.picture(r, "pause.png", "pause.png")
    S.check(r.has(r"wake_menu choice=continue"), "the pause screen, Continue")
    S.check(len(modes) == 5, "the pause chord did not change the preset")
    S.picture(r, "resumed.png")

    # back to the list
    S.check(r.has(r"game_end reason=game_list") and
            r.has(r"ui_return fade_done"),
            "Quit to game list: unloaded and faded into the list")
    S.picture(r, "returning.png")
    S.picture(r, "list_after.png")

    if fix:
        gb = r.evt("gb_rom_loaded")
        S.check(len(gb) == 2 and gb[0]["console"] == "GB" and
                gb[1]["console"] == "GBC", "a GB and then a GBC game loaded")
        S.picture(r, "gb.png", "gb.png")
        S.picture(r, "gbc.png", "gbc.png")
        S.picture(r, "list_gba.png")

    S.picture(r, "shelf.png")
    S.check(r.has(r"ui_browser_pick rom=gbadhoc-test.gba state_slot=2") and
            r.has(r"state_load slot=2 via=shelf rc=0"),
            "the last game started from slot 2 on the list's shelf")
    S.picture(r, "from_shelf.png")
    S.check(r.has(r"game_end reason=menu_exit"), "Exit from the menu")

    # sound
    frames, peak, loud = wav_stats(os.path.join(r.out, "audio.wav"))
    S.check(frames > 48000 and peak > 4000 and loud > 0.05,
            "the WAV is not silent (%d frames, peak %d, %.0f%% above the "
            "noise floor)" % (frames, peak, loud * 100))
    S.summary["session.audio_crc"] = r.host.get("audio_crc")
    S.summary["session.emulated"] = r.host.get("emulated")


def t_ff(S):
    """The three fast-forward modes advance the right number of emulated
    frames per refresh and draw the right number of pictures."""
    hold = "120:square:120\n+0:shot=ff.png\n+60:quit\n"
    cases = (
        ("3x", {}, hold, 3.0, 3.0, 1.0, 1.0),
        # the twin gives a frame 3 ms: unlimited is 16.67 / 3 = 5.56 x
        ("unlimited", {"ff_mult_x10": 0, "ff_smooth": 0}, hold,
         5.3, 5.8, 0.45, 0.56),
        ("smooth", {"ff_mult_x10": 0, "ff_smooth": 1}, hold,
         5.3, 5.8, 0.9, 1.05),
        # toggle mode: one press on, one press off
        ("3x_toggle", {"ff_hold": 0}, "120:square\n+118:square\n+60:quit\n",
         3.0, 3.0, 1.0, 1.0),
    )
    for name, cfg, script, lo, hi, dlo, dhi in cases:
        r = S.run("ff_" + name, script, config=cfg,
                  autoboot="gbadhoc-test.gba")
        S.clean(r)
        st = r.evt("ff_stats")
        if not S.check(len(st) == 1, "%s: fast forward went on and off once "
                       "(ff_stats lines: %d)" % (name, len(st))):
            continue
        fr, vc, dr = (int(st[0][k]) for k in ("frames", "refreshes", "drawn"))
        S.check(st[0]["mode"] == name.split("_")[0], "%s: mode in the log (%s)"
                % (name, st[0]["mode"]))
        S.check(118 <= vc <= 122, "%s: held for 120 refreshes (%d)" % (name, vc))
        S.check(lo * vc <= fr <= hi * vc + 3,
                "%s: %d emulated frames in %d refreshes = %.2f per refresh "
                "(%.1f .. %.1f)" % (name, fr, vc, fr / vc, lo, hi))
        S.check(dlo * vc <= dr <= dhi * vc + 2,
                "%s: %d pictures drawn = %.2f per refresh (%.2f .. %.2f)"
                % (name, dr, dr / vc, dlo, dhi))
        # before and after: one frame per refresh
        total = int(r.host.get("emulated", 0))
        outside = int(r.host.get("refreshes", 0)) - vc
        S.check(abs(total - fr - outside) <= 8,
                "%s: outside fast forward one frame per refresh (%d frames "
                "in %d refreshes)" % (name, total - fr, outside))
        if "toggle" not in name:
            S.picture(r, "ff.png", "ff_3x.png" if name == "3x" else None)
        S.summary["ff_%s.audio_crc" % name] = r.host.get("audio_crc")


def t_suspend(S):
    """Suspend and resume: in a game (the save goes to the card, the wake
    overlay comes up, Continue), and in the game list."""
    script = """
100:a:10
+60:suspend
+30:shot=wake.png
+2:cross
+60:shot=after.png
""" + QUIT_TO_LIST + """
+70:suspend
+30:shot=list.png
+10:quit
"""
    r = S.run("suspend", script, autoboot="gbadhoc-test.gba")
    S.clean(r)
    i = r.index(r"power suspend game=1")
    j = r.index(r"sram_flush .*wrote=", i) if i >= 0 else -1
    k = r.index(r"power resume game=1", i) if i >= 0 else -1
    S.check(0 <= i < j < k, "in the game: suspend, the save flushed, resume")
    S.picture(r, "wake.png", "wake.png")
    S.check(r.has(r"wake_menu choice=continue"), "the wake overlay, Continue")
    a = r.shot_evt("after.png")
    w = r.shot_evt("wake.png")
    S.check(a and w and int(a["emu"]) > int(w["emu"]) + 30,
            "the game runs on afterwards")
    S.picture(r, "after.png")
    S.check(r.has(r"power suspend game=0") and r.has(r"power resume game=0"),
            "in the game list: suspend and resume")
    S.picture(r, "list.png")
    S.summary["suspend.audio_crc"] = r.host.get("audio_crc")


def t_bad(S):
    """A file that is not a cartridge: a notice, then the game list."""
    extra = {"roms/zero.gb": b"", "roms/tiny.gba": b"garbage"}
    for name in ("zero.gb", "tiny.gba"):
        r = S.run("bad_" + name.split(".")[1],
                  "40:shot=notice.png\n+20:cross\n+60:shot=list.png\n+10:quit\n",
                  autoboot=name, extra=extra)
        S.clean(r, allow=r"boot_failed|boot_refused|notice title|"
                         r"gbcore_create FAILED")
        S.check(r.has(r"notice title=\"This game could not be started\""),
                "%s: the notice" % name)
        S.check(r.has(r"^EVT ui_browser "), "%s: then the game list" % name)
        S.picture(r, "notice.png", "notice.png" if name == "zero.gb" else None)
        S.picture(r, "list.png")


def t_worker(S):
    """The I/O worker thread (the log and the GBA save off the frame's
    thread, as on the console) leaves the same save as the synchronous path."""
    script = "100:a:10\n+200:a:10\n" + menu_pick(6, wait=200)
    saves = []
    for name, args in (("worker_off", ()), ("worker_on", ("--worker",))):
        r = S.run(name, script, autoboot="gbadhoc-test.gba", args=args)
        S.clean(r)
        S.check(r.has(r"io_worker on=%d" % (1 if args else 0)),
                "%s: io_worker in the log" % name)
        p = os.path.join(r.root, "roms", "gbadhoc-test.sav")
        saves.append(open(p, "rb").read() if os.path.exists(p) else None)
    S.check(saves[0] and saves[0] == saves[1],
            "the same battery save with and without the worker thread")
    if os.path.exists(OWN_GB):
        r = S.run("worker_gb", "100:a:10\n+200:a:10\n" +
                  menu_pick(6, wait=200), autoboot="gbsave.gb",
                  args=("--worker",))
        S.clean(r)
        p = os.path.join(r.root, "roms", "gbsave.gb.sav")
        S.check(os.path.exists(p) and os.path.getsize(p) > 0,
                "a GB battery save, named Game.gb.sav, beside the ROM")


def t_cpufix(S):
    """The emulator fixes from gpSP's September 2026 commits, on this
    build's dynarec (testrom/cpufix.c): code run from VRAM and rewritten
    there, I/O registers reading back as the hardware's do, and the guest's
    flags kept across the ARM load stub's I/O paths.  The cartridge writes
    one result byte per test to its battery RAM."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-cpufix.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-cpufix.gba (make -C testrom cpufix): skipped")
        return
    names = ["VRAM code (Thumb), rewritten", "VRAM code (ARM), rewritten",
             "BG0CNT read mask", "WININ read mask",
             "invalid I/O 0x136 reads 0", "DMA3CNT_H read mask",
             "SOUNDCNT_X master bit", "Z set across I/O reads",
             "Z clear across I/O reads"]
    extra = {"roms/gbadhoc-cpufix.gba": open(rom, "rb").read()}
    r = S.run("cpufix", "120:shot=cpufix.png\n" + menu_pick(6, wait=20),
              autoboot="gbadhoc-cpufix.gba", extra=extra)
    S.clean(r)
    p = os.path.join(r.root, "roms", "gbadhoc-cpufix.sav")
    sav = open(p, "rb").read() if os.path.exists(p) else b""
    S.check(sav[:6] == b"CPUFIX", "the cartridge wrote its results")
    if sav[:6] != b"CPUFIX":
        return
    for n, name in enumerate(names):
        seen = int.from_bytes(sav[0x20 + 4 * n:0x24 + 4 * n], "little")
        S.check(sav[0x08 + n] == 1, "%s (saw 0x%x, result %d)" %
                (name, seen, sav[0x08 + n]))


def t_realtime(S):
    """--realtime: the wall clock, a real audio thread, a real worker."""
    r = S.run("realtime", "60:a:10\n", autoboot="gbadhoc-test.gba",
              args=("--realtime", "--worker", "--frames", "300"))
    S.clean(r)
    frames = int(r.host.get("emulated", 0))
    S.check(4.5 <= r.wall <= 12, "300 refreshes took about five seconds of "
            "the wall (%.1f s)" % r.wall)
    if S.arch == "x86":         # (a sanitizer build has no time to spare)
        S.check(270 <= frames <= 302, "about one emulated frame per refresh "
                "(%d in 300)" % frames)
        S.check(int(r.host.get("peak", 0)) > 4000 and
                int(r.host.get("audio_frames", 0)) > 150000,
                "the audio thread played (%s frames, peak %s)" %
                (r.host.get("audio_frames"), r.host.get("peak")))
    else:
        S.note("this twin is not fast enough to hold real time: %d frames "
               "in 300 refreshes" % frames)


def key_boxes(im):
    """Which of the test cartridge's ten button boxes are lit, as a string
    of GBA KEYINPUT names (the boxes are drawn in KEYINPUT bit order)."""
    names = ("A", "B", "Select", "Start", "Right", "Left", "Up", "Down",
             "R", "L")
    g = game_pixels(im)
    return "+".join(n for i, n in enumerate(names)
                    if max(g.getpixel((8 + i * 23 + 9, 37))) > 100)


def t_controls(S):
    """The control map, the left stick, the FPS chip."""
    script = """
100:shot=fps.png
+10:cross:10
+0:shot=cross.png
+0:cross:10
+10:circle+l:10
+0:shot=circle_l.png
+0:circle+l:10
+10:lstick-right+lstick-up:10
+0:shot=stick.png
+0:lstick-right+lstick-up:10
+10:quit
"""
    r = S.run("controls", script, config={"show_fps": 1},
              autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.check(key_boxes(r.shot("cross.png")) == "A",
            "CROSS is the GBA's A (lit: %s)" % key_boxes(r.shot("cross.png")))
    S.check(key_boxes(r.shot("circle_l.png")) == "B+L",
            "CIRCLE + L are B + L (lit: %s)" % key_boxes(r.shot("circle_l.png")))
    S.check(key_boxes(r.shot("stick.png")) == "Right+Up",
            "the left stick is the d-pad (lit: %s)" %
            key_boxes(r.shot("stick.png")))
    S.picture(r, "fps.png", "fps.png")

    # the PSP's layout by hand: A on CIRCLE, B on SQUARE, fast forward on X
    r = S.run("controls_remap", "100:circle:10\n+0:shot=circle.png\n"
              "+0:circle:10\n+10:square:10\n+0:shot=square.png\n"
              "+0:square:10\n+10:quit\n",
              config={"bind_a": "CIRCLE", "bind_b": "SQUARE",
                      "bind_ff": "CROSS"},
              autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.check(key_boxes(r.shot("circle.png")) == "A" and
            key_boxes(r.shot("square.png")) == "B",
            "config.ini bind_a = CIRCLE, bind_b = SQUARE (lit: %s, %s)" %
            (key_boxes(r.shot("circle.png")), key_boxes(r.shot("square.png"))))
    S.check(not r.evt("ff_stats"), "...and SQUARE no longer fast-forwards")


def runs_of(seq):
    """[(value, length), ...] of consecutive equal values."""
    out = []
    for v in seq:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return [tuple(x) for x in out]


def t_turbo(S):
    """Turbo buttons (GBAdhoc 4.0): Hold at 10 a second, Toggle at 30."""
    # Turbo A on SQUARE (fast forward moved off it), Hold, 10 a second: 3
    # frames pressed, 3 released.  Turbo B on TRIANGLE (the video preset
    # moved off it), Toggle, 30 a second: 1 and 1.
    conf = {"bind_ff": "NONE", "bind_video": "NONE",
            "bind_turbo_a": "SQUARE", "turbo_a_rate": "10",
            "turbo_a_mode": "hold",
            "bind_turbo_b": "TRIANGLE", "turbo_b_rate": "30",
            "turbo_b_mode": "toggle"}
    hold = ["h%02d.png" % i for i in range(14)]
    tog = ["t%02d.png" % i for i in range(10)]
    off = ["o%02d.png" % i for i in range(6)]
    script = "100:square:40\n"
    script += "".join("%d:shot=%s\n" % (112 + i, n) for i, n in enumerate(hold))
    script += "150:shot=released.png\n"
    script += "170:triangle:2\n"
    script += "".join("%d:shot=%s\n" % (190 + i, n) for i, n in enumerate(tog))
    script += "220:triangle:2\n"
    script += "".join("%d:shot=%s\n" % (236 + i, n) for i, n in enumerate(off))
    script += "250:quit\n"
    r = S.run("turbo", script, config=conf, autoboot="gbadhoc-test.gba")
    S.clean(r)
    ok = all(r.shot(n) is not None for n in hold + tog + off)
    S.check(ok, "every screenshot was taken")
    if not ok:
        return
    h = [key_boxes(r.shot(n)) for n in hold]
    hr = runs_of(h)
    S.check(set(h) == {"A", ""},
            "holding SQUARE presses A on and off (seen: %s)" % "/".join(
                x or "-" for x in h))
    S.check(len(hr) >= 4 and all(n == 3 for _, n in hr[1:-1]),
            "...three frames each way: 10 presses a second (runs: %s)" % hr)
    S.check(key_boxes(r.shot("released.png")) == "",
            "...and nothing once SQUARE is let go (Hold)")
    t = [key_boxes(r.shot(n)) for n in tog]
    tr = runs_of(t)
    S.check(set(t) == {"B", ""} and all(n == 1 for _, n in tr[1:-1]),
            "a press of TRIANGLE starts B every other frame, nothing held "
            "(seen: %s)" % "/".join(x or "-" for x in t))
    o = [key_boxes(r.shot(n)) for n in off]
    S.check(all(x == "" for x in o),
            "a second press stops it (Toggle) (seen: %s)" % "/".join(
                x or "-" for x in o))
    S.check(not r.evt("ff_stats"), "SQUARE no longer fast-forwards")
    S.check(not r.evt("config_bind_conflict"), "no binding was refused")


def t_switches(S):
    """vita.ini: the timer pacer (the GBA's exact 59.7275 Hz), the
    interpreter, no worker thread."""
    r = S.run("pace_timer", "700:quit\n", autoboot="gbadhoc-test.gba",
              vita_ini={"pace_mode": 1})
    S.clean(r)
    win = r.evt("vita_win")
    fps = int(win[0]["fps_x1000"]) if win else 0
    S.check(r.has(r"switches pace_mode=1") and 59700 <= fps <= 59755,
            "pace_mode = 1: the game runs at the GBA's 59.7275 Hz on a 60 Hz "
            "display (%d.%03d)" % (fps // 1000, fps % 1000))
    w = r.evt("audio")
    S.check(w and w[0]["speed_x1e6"] == "1000000",
            "...and the sound is resampled at the exact rate")

    r = S.run("interp", "150:shot=game.png\n+10:quit\n",
              autoboot="gbadhoc-test.gba",
              vita_ini={"drc": 0, "io_thread": 0}, args=("--worker",))
    S.clean(r)
    S.check(r.has(r"switches pace_mode=0 drc=0") and
            r.has(r"io_worker on=0"),
            "drc = 0 and io_thread = 0 are read")
    im = S.picture(r, "game.png")
    S.check(im and key_boxes(im) == "", "the interpreter runs the game")


def cheat_blocks(im):
    """The cheat cartridge's five blocks (lives, gold, and three results),
    as the colours it drew them in."""
    g = game_pixels(im)
    return [g.getpixel((n * 48 + 4, 60)) for n in range(5)]


BLACK, BLUE, OLIVE = (0, 0, 0), (0, 0, 255), (132, 130, 0)


def near(a, b, tol=8):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


CHT = """cheats = 3

cheat0_desc = "Infinite Lives"
cheat0_code = "82000000+0063"
cheat0_enable = false

cheat1_desc = "Gold"
cheat1_code = "82000004+0002"
cheat1_enable = false

cheat2_desc = "Press The Button For Gold"
cheat2_code = "82100004 000000AB"
cheat2_enable = true
"""


def t_cheats(S):
    """Cheats: the bundled database is found, the page opens in a running
    game, a code switched on there acts on the game, the cheat button."""
    if not os.path.exists(CHEAT_GBA):
        S.note("no %s (make -C ../tests/cheats/rom cheatrom.gba): skipped"
               % os.path.relpath(CHEAT_GBA, HERE))
        return
    rom = open(CHEAT_GBA, "rb").read()
    script = """
# the game alone: its five lives are gone after 40 frames, it has no gold
100:shot=before.png
# the cheat cartridge's button is the right stick pulled down: a code that
# acts only while it is held gives gold
+2:rstick-down:20
+0:shot=button.png
+0:rstick-down:10
""" + menu_pick(3) + """
+20:shot=cheats.png
# the code that is on leads the list; the next row is Infinite Lives: X
+2:down
+8:cross
+20:shot=cheats_on.png
+2:circle
+10:circle
+60:shot=after.png
""" + EXIT_APP
    r = S.run("cheats", script, autoboot="cheatrom.gba",
              extra={"roms/cheatrom.gba": rom, "cheats/cheatrom.cht": CHT})
    S.clean(r)
    db = r.evt("cheats_db")
    S.check(db and int(db[0]["games"]) > 1000,
            "the bundled database opens (%s games)" %
            (db[0]["games"] if db else "?"))
    ld = r.evt("cheats_load")
    S.check(ld and int(ld[0]["n"]) >= 3 and ld[0]["enabled"] == "1",
            "the game's .cht is loaded: %s" % (ld[0]["_"] if ld else "-"))
    b0 = cheat_blocks(r.shot("before.png"))
    S.check(b0[0] == BLACK and b0[1] == BLACK,
            "before: no lives left, no gold (blocks %s %s)" % (b0[0], b0[1]))
    b1 = cheat_blocks(r.shot("button.png"))
    S.check(near(b1[1], OLIVE) and b1[0] == BLACK,
            "the right stick held down is the cheat button: gold = 0xAB "
            "(block %s)" % (b1[1],))
    S.check(r.has(r"ui_screen name=cheats"), "the Cheats page opens")
    S.picture(r, "cheats.png", "cheats.png")
    S.picture(r, "cheats_on.png")
    S.check(r.has(r"cheat_toggle i=0 "), "X switches Infinite Lives on")
    b2 = cheat_blocks(r.shot("after.png"))
    S.check(near(b2[0], BLUE),
            "after: Infinite Lives acts on the game, lives = 0x63 (block %s)"
            % (b2[0],))
    cht = open(os.path.join(r.root, "cheats", "cheatrom.cht")).read()
    S.check(re.search(r'cheat0_enable\s*=\s*true', cht) and
            re.search(r'cheat1_enable\s*=\s*false', cht),
            "the choice is in the .cht when the menu has closed")

    # The database by FILE NAME: the port's own test cartridge (its header
    # is in no database) under a name the database has.
    name = "Kirby - Nightmare in Dream Land (USA).gba"
    r = S.run("cheats_name", "100:shot=game.png\n" + menu_pick(3) +
              "+20:shot=cheats.png\n+10:quit\n", autoboot=name,
              extra={"roms/" + name: open(OWN_GBA, "rb").read()})
    S.clean(r)
    ld = r.evt("cheats_load")
    S.check(ld and int(ld[0]["n"]) > 5 and ld[0]["enabled"] == "0" and
            ld[0]["match"] == "3" and "Kirby" in ld[0]["_"],
            "found in the database by its file name, every code off: %s" %
            (ld[0]["_"] if ld else "-"))
    S.picture(r, "cheats.png", "cheats_db.png")
    # ...and the same cartridge under its own name has none
    r = S.run("cheats_none", "100:shot=game.png\n" + menu_pick(3) +
              "+20:shot=cheats.png\n+10:quit\n",
              autoboot="gbadhoc-test.gba")
    S.clean(r)
    ld = r.evt("cheats_load")
    S.check(ld and ld[0]["n"] == "0" and ld[0]["match"] == "0",
            "a game the database does not know has no cheats")
    S.picture(r, "cheats.png")


def t_play(S):
    """Play together on a console with no network: every page opens, says
    what is missing, and gives the game back.  (Both layers are real here:
    link_tests.py is where two twins link over the network, local_tests.py
    where two games run on this one -- and where each is refused while the
    other is up.)"""
    script = menu_pick(2, wait=100) + """
+20:shot=play.png
# Host a game, Join a game: no Wi-Fi (the UI's own default answer)
+2:cross
+20:shot=host.png
+2:down
+8:cross
+20:shot=join.png
# Two players on this Vita: its page, and back
+2:down
+8:cross
+20:shot=local.png
+2:circle
# Connection, Mystery Gift (Start listening: no Wi-Fi), How to play together
+20:down
+8:cross
+20:shot=connection.png
+2:circle
+20:down
+8:cross
+20:shot=mgift.png
+2:cross
+20:shot=mgift_nowifi.png
+2:circle
+20:down
+8:cross
+20:shot=help.png
+2:circle
+20:circle
+20:circle
+60:shot=game.png
+10:quit
"""
    r = S.run("play", script, autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.check(r.has(r"ui_screen name=wireless"), "the Play together page opens")
    S.picture(r, "play.png", "play.png")
    S.picture(r, "host.png", "play_no_wifi.png")
    S.check(worst_diff(r.shot("host.png"), r.shot("join.png"),
                       (0, 496, 960, 544)) == 0,
            "Host and Join: the same plate (no Wi-Fi)")
    S.picture(r, "local.png", "play_local.png")
    S.check(r.has(r"ui_screen name=local"),
            "Two players on this Vita: its own page")
    for n in ("connection.png", "help.png"):
        S.picture(r, n)
    S.picture(r, "mgift.png", "play_mgift.png")
    S.check(r.has(r"ui_screen name=mystery_gift") and
            r.has(r'mg_start_refused why="Connect to Wi-Fi first"'),
            "Mystery Gift: its page, and Start listening without Wi-Fi says "
            "to connect first")
    S.check(r.has(r"ui_screen name=help"),
            "How to play together: the row below Mystery Gift")
    S.check(not r.has(r"play_host |play_join |play_local "),
            "nothing was started")
    a, b = r.shot_evt("play.png"), r.shot_evt("game.png")
    S.check(r.has(r"menu_closed") and a and b and
            int(b["emu"]) > int(a["emu"]) + 30,
            "nothing hangs: the menu closes and the game runs on")
    S.check(key_boxes(r.shot("game.png")) == "",
            "the buttons that left the menu did not reach the game")

    # Past the UI's own check (Connection: Ad hoc does not need Wi-Fi) the
    # actions reach main_vita.c and the link layer (link_vita.c).  This twin
    # has no radio cell (no nethost.ini: link_tests.py is where two twins
    # link), so the system's connection screen fails, and each page says so.
    script = menu_pick(2, wait=100) + """
# Host a game > the ad hoc page > Continue: the waiting page, the reason
+20:cross
+20:shot=adhoc.png
+2:cross
+3:shot=opening.png
+60:shot=host.png
# Join a game > Continue: the menu closes, the LINKING screen, its reason
+2:down
+8:cross
+20:cross
+60:shot=join.png
+2:circle
+60:shot=game.png
+10:quit
"""
    r = S.run("play_adhoc", script, autoboot="gbadhoc-test.gba",
              config={"net_conn": 2})
    S.clean(r)
    S.check(r.has(r"ui_screen name=adhoc") and r.has(r"play_host conn=2"),
            "ad hoc: Host a game is asked for")
    for n in ("adhoc.png", "opening.png"):
        S.picture(r, n)
    S.picture(r, "host.png", "play_adhoc_host.png")
    S.check(worst_diff(r.shot("host.png"), r.shot("adhoc.png"),
                       (0, 496, 960, 544)) > 100 and
            r.has(r"link_failed code=11 host=1"),
            "Host: the connection screen failed, the reason is on the plate")
    S.check(r.has(r"play_join adhoc") and len(r.evt("menu_closed")) == 1,
            "Join a game is asked for and the menu closes")
    S.picture(r, "join.png", "play_adhoc_join.png", box=(240, 0, 720, 64))
    S.check(r.has(r"link_failed code=1 host=0") and
            r.has(r"link_phase 6 -> 0"),
            "Join: the LINKING screen says nobody answered, until O")
    S.check(len(r.evt("link_net_down")) == 0,
            "nothing is left up: the radio never opened")
    a, b = r.shot_evt("join.png"), r.shot_evt("game.png")
    S.check(a and b and int(b["emu"]) - int(a["emu"]) >= 40,
            "and the game runs on")


def t_touch(S):
    """The touch screen: a tap on the game's picture opens the menu, a tap
    on a card chooses it; nothing else on the glass does anything."""
    script = """
100:touch=480/272
+20:shot=menu.png
# tap "Resume" (the control centre's first card)
+2:touch=96/420
+20:shot=resumed.png
# none of these is a tap on the picture with free hands: a tap while a
# button is held, a tap beside the picture, a long press, a swipe
160:circle:40
170:touch=480/272
210:touch=30/272
230:touch=480/272:40
290:drag=300/200/600/300:10
330:shot=still_game.png
# the Cheats card by touch, and back with the footer's hint
+2:touch=480/272
+20:touch=480/420
+20:shot=cheats.png
+10:quit
"""
    r = S.run("touch", script, autoboot="gbadhoc-test.gba")
    S.clean(r)
    opens = [d["via"] for d in r.evt("menu_open")]
    S.check(opens == ["touch", "touch"],
            "the menu opened by a tap, twice (menu_open: %s)" % opens)
    S.picture(r, "menu.png", "menu_touch.png")
    S.check(len(r.evt("menu_closed")) == 1, "a tap on Resume closes it")
    a, b = r.shot_evt("resumed.png"), r.shot_evt("still_game.png")
    S.check(a and b and int(b["emu"]) - int(a["emu"]) > 150,
            "a tap with a button held, a tap beside the picture, a long "
            "press and a swipe do not open it")
    S.check(r.has(r"ui_screen name=cheats"), "a tap on the Cheats row opens it")
    S.picture(r, "cheats.png")

    for name, kw in (("touch_off", {"config": {"touch": 0}}),
                     ("touch_menu_off", {"vita_ini": {"touch_menu": 0}})):
        r = S.run(name, "100:touch=480/272\n+40:shot=game.png\n+10:quit\n",
                  autoboot="gbadhoc-test.gba", **kw)
        S.clean(r)
        S.check(not r.evt("menu_open"),
                "%s: a tap does not open the menu" % name)


# The Add a cheat page's keypad, by the centre of each key (native pixels).
KEYPAD = {}
for _r, _row in enumerate(("123AB", "456CD", "789EF")):
    for _c, _k in enumerate(_row):
        KEYPAD[_k] = (79 + 84 * _c, 169 + 72 * _r)
KEYPAD.update({"0": (79, 385), " ": (205, 385), "save": (415, 385)})


def taps(keys, gap=4):
    return "".join("+%d:touch=%d/%d\n" % ((gap,) + KEYPAD[k]) for k in keys)


def t_typing(S):
    """A cheat of the player's own: the code typed on the page's keypad by
    touch, its name on the system keyboard, saved, and acting on the game."""
    if not os.path.exists(CHEAT_GBA):
        S.note("no cheat cartridge: skipped")
        return
    script = "100:shot=before.png\n" + menu_pick(3) + """
+20:triangle
+20:shot=add.png
# SELECT: the system keyboard asks for the name
+2:select
+4:type=Stay Alive
+20:shot=named.png
""" + taps("82000000 0063") + """
+10:shot=typed.png
""" + taps(["save"]) + """
+20:shot=added.png
+2:circle
+10:circle
+60:shot=after.png
+10:quit
"""
    r = S.run("typing", script, autoboot="cheatrom.gba",
              extra={"roms/cheatrom.gba": open(CHEAT_GBA, "rb").read()})
    S.clean(r)
    S.check(r.has(r"ui_screen name=cheat_add"), "the Add a cheat page opens")
    S.check(r.has(r'host_ime title="Cheat name" text="Stay Alive"'),
            "the keyboard was asked for the name")
    for n in ("add.png", "named.png", "added.png"):
        S.picture(r, n)
    S.picture(r, "typed.png", "cheat_typed.png")
    add = r.evt("cheat_add")
    S.check(add and int(add[0]["rc"]) >= 0 and add[0]["len"] == "13",
            "thirteen taps on the keypad, and Save: %s" %
            (add[0]["_"] if add else "-"))
    S.check(cheat_blocks(r.shot("before.png"))[0] == BLACK and
            near(cheat_blocks(r.shot("after.png"))[0], BLUE),
            "the new code acts on the game (lives: none, then 0x63)")
    chts = glob.glob(os.path.join(r.root, "cheats", "*.cht"))
    cht = open(chts[0]).read() if chts else ""
    S.check('"Stay Alive"' in cht and re.search(r'82000000[ +]0063', cht) and
            re.search(r'_enable\s*=\s*true', cht),
            "and is in the game's .cht, by name, switched on")


def gba_screen(rgb, dark):
    """What video_vita.c's colour table makes of one pixel (cc_build: gpSP's
    channel mix in linear light), in double precision."""
    r, g, b = rgb[0] >> 3, rgb[1] >> 2, rgb[2] >> 3
    gamma = 2.2 + (1.0 if dark else 0.0)
    lin = ((r / 31.0) ** gamma, (g / 63.0) ** gamma, (b / 31.0) ** gamma)
    mix = ((0.82, 0.24, -0.06), (0.125, 0.665, 0.21), (0.195, 0.075, 0.73))
    out = []
    for i, m in enumerate(mix):
        x = 0.94 * (m[0] * lin[0] + m[1] * lin[1] + m[2] * lin[2])
        x = min(1.0, max(0.0, x)) ** (1 / 2.2)
        if i == 1:
            v = int(x * 63 + 0.5)
            out.append((v << 2) | (v >> 4))
        else:
            v = int(x * 31 + 0.5)
            out.append((v << 3) | (v >> 2))
    return tuple(out)


def t_look(S):
    """Colours and motion blend: a GBA's screen colours by the table, the
    blend of two frames, the Settings rows, a Game Boy Color's own."""
    base = {"scale_gba": 4, "filter_gba": 0, "scale_gbc": 4, "filter_gbc": 0}
    shots = {}
    for name, color in (("raw", 0), ("gba", 1), ("dark", 2)):
        cfg = dict(base, color_gba=color)
        r = S.run("look_" + name, "150:shot=game.png\n+10:quit\n", config=cfg,
                  autoboot="gbadhoc-test.gba")
        S.clean(r)
        look = r.evt("display_look")
        S.check(look and look[-1]["color"] == str(color) and
                look[-1]["console"] == "GBA",
                "color_gba = %d reaches the renderer" % color)
        im = S.picture(r, "game.png", "look_%s.png" % name if color else None)
        shots[name] = game_pixels(im) if im else None
    if all(shots.values()):
        raw = shots["raw"]
        cols = sorted(raw.getcolors(1 << 20))
        S.check(len(cols) >= 4, "the test picture has %d colours" % len(cols))
        for name, dark in (("gba", False), ("dark", True)):
            got, bad = shots[name], 0
            for y in range(0, 160, 2):
                for x in range(0, 240, 2):
                    want = gba_screen(raw.getpixel((x, y)), dark)
                    if not near(got.getpixel((x, y)), want, 8):
                        bad += 1
            S.check(bad == 0, "%s: every pixel is the table's colour for the "
                    "raw one (%d of 9600 are not)" % (name, bad))
            S.check(got.tobytes() != raw.tobytes(),
                    "%s: the picture is not the raw one" % name)
        lum = {k: sum(n * sum(c) for n, c in v.getcolors(1 << 20))
               for k, v in shots.items()}
        S.check(lum["dark"] < lum["gba"],
                "the dark screen is darker (%d < %d)" % (lum["dark"],
                                                         lum["gba"]))

    # ---- the rows: Settings wraps upwards to Button mapping, Keep full
    # speed, FPS, Rewind, FF button, Fast-forward, Game previews, Game list,
    # Theme, (Wide view: menus -- not a row while the wide view is off, as
    # it is in these tests), Wide view, Motion blend, Colours
    script = "100:shot=before.png\n" + menu_pick(4, wait=10) + "+20:up\n" + \
        "+8:up\n" * 11 + "+10:shot=row.png\n+4:right\n+10:shot=row_gba.png\n" \
        "+4:down\n+8:right\n+10:shot=row_blend.png\n+4:circle\n+10:circle\n" \
        "+40:shot=after.png\n" + EXIT_APP
    r = S.run("look_rows", script, config=base, autoboot="gbadhoc-test.gba")
    S.clean(r)
    look = r.evt("display_look")
    S.check(len(look) >= 3 and
            [(e["color"], e["blend"]) for e in look[-3:]] ==
            [("0", "0"), ("1", "0"), ("1", "1")],
            "Settings > Colours then Motion blend: raw, GBA screen, and "
            "blended (%s)" % [(e["color"], e["blend"]) for e in look])
    S.picture(r, "row_gba.png", "look_row.png")
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check(re.search(r"^color_gba = 1$", ini, re.M) and
            re.search(r"^blend_gba = 1$", ini, re.M),
            "config.ini keeps color_gba = 1 and blend_gba = 1")
    a, b = S.picture(r, "before.png"), S.picture(r, "after.png")
    if a and b and shots.get("gba"):
        # the cartridge's picture moves; its background does not
        bg = [game_pixels(im).getpixel((2, 2)) for im in (a, b)]
        S.check(bg[0] == shots["raw"].getpixel((2, 2)) and
                near(bg[1], shots["gba"].getpixel((2, 2)), 8) and
                bg[0] != bg[1],
                "the game is in the new colours when the menu closes "
                "(background %s -> %s)" % (bg[0], bg[1]))

    # ---- the blend: a block that moves a column a frame leaves half of
    # itself behind
    rw = os.path.join(HERE, "..", "tests", "rewind", "rom", "rwtest.gbc")
    if os.path.exists(rw):
        got = {}
        for blend in (0, 1):
            r = S.run("look_blend%d" % blend, "200:shot=game.png\n+10:quit\n",
                      config=dict(base, blend_gbc=blend), autoboot="rwtest.gbc",
                      extra={"roms/rwtest.gbc": open(rw, "rb").read()})
            S.clean(r)
            im = S.picture(r, "game.png")
            got[blend] = game_pixels(im, 160, 144) if im else None
        if got[0] and got[1]:
            n0, n1 = (len(got[k].getcolors(1 << 20)) for k in (0, 1))
            S.check(n1 > n0, "with the blend the moving block has in-between "
                    "shades (%d colours, %d without)" % (n1, n0))
    else:
        S.note("no tests/rewind/rom/rwtest.gbc: the blend leg is skipped")

    # ---- a Game Boy Color's colours are its core's
    if S.have_fixture:
        got = {}
        for color in (1, 0):
            r = S.run("look_gbc%d" % color, "200:shot=game.png\n+10:quit\n",
                      config=dict(base, color_gbc=color),
                      autoboot="cgb-acid2.gbc")
            S.clean(r)
            look = r.evt("display_look")
            S.check(look and look[-1]["console"] == "GBC" and
                    look[-1]["color"] == str(color),
                    "color_gbc = %d is the GBC profile's" % color)
            got[color] = S.picture(r, "game.png")
        if got[0] and got[1]:
            S.check(worst_diff(got[0], got[1]) > 16,
                    "GBC screen and vivid are different pictures")


def t_wide(S):
    """The wide picture: 21 more columns on each side of a GBA's 240.  A
    "Mode 7" cartridge (testrom/wide7.c) is shown wide, and beside it the
    same frames as a console shows them with everything moved 21 pixels
    right, then left (the cartridge does that while L or R is held): the
    margins must be those columns, pixel for pixel.  Then the renderer's own
    check, the scales that have no room, and the Settings row."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-wide.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-wide.gba (make -C testrom wide): skipped")
        return
    extra = {"roms/gbadhoc-wide.gba": open(rom, "rb").read()}
    base = {"scale_gba": 4, "filter_gba": 0}
    shots = "200:shot=a.png\n260:shot=b.png\n331:shot=c.png\n"
    M = 21

    def run(name, wide, keys="", frames=360, cfg=None):
        r = S.run("wide_" + name, keys + shots + "%d:quit\n" % frames,
                  config=dict(base, **(cfg or {})), autoboot="gbadhoc-wide.gba",
                  vita_ini=None if wide is None else {"wide": wide},
                  extra=extra)
        S.clean(r)
        return r

    def pictures(r, w):
        out = []
        for n in "abc":
            im = S.picture(r, n + ".png")
            out.append(game_pixels(im, w=w) if im else None)
        return out

    r_wide = run("all", 10, frames=1300)          # everywhere + the self check
    r_test = run("rows", 12)                      # the rows-from-memory profile
    r_l = run("shift_right", 0, "20:l:340\n")
    r_r = run("shift_left", 0, "20:r:340\n")
    r_0 = run("plain", 0)
    wide, test = pictures(r_wide, 240 + 2 * M), pictures(r_test, 240 + 2 * M)
    left, right, plain = pictures(r_l, 240), pictures(r_r, 240), pictures(r_0, 240)
    def differ(a, ax, b, bx, n):
        return sum(1 for y in range(160) for x in range(n)
                   if a.getpixel((ax + x, y)) != b.getpixel((bx + x, y)))
    if all(wide + test + left + right + plain):
        for i, n in enumerate("abc"):
            S.check(differ(wide[i], M, plain[i], 0, 240) == 0,
                    "%s: the 240 columns in the middle are the console's "
                    "picture" % n)
            S.check(differ(wide[i], 0, left[i], 0, M) == 0,
                    "%s: the left margin is what a console shows 21 pixels "
                    "further left (%d of %d pixels differ)" %
                    (n, differ(wide[i], 0, left[i], 0, M), M * 160))
            S.check(differ(wide[i], 240 + M, right[i], 240 - M, M) == 0,
                    "%s: the right margin is what a console shows 21 pixels "
                    "further right (%d of %d pixels differ)" %
                    (n, differ(wide[i], 240 + M, right[i], 240 - M, M), M * 160))
            # (the profile that hands over rows from memory is a test's, and
            # is not in a build made as the Vita's: -DVITA, "-vitasim")
            if not S.arch.endswith("-vitasim"):
                S.check(test[i].tobytes() == wide[i].tobytes(),
                        "%s: map rows handed over by a profile draw the same "
                        "picture" % n)
        m = wide[1].crop((0, 0, M, 160))
        S.check(len(m.getcolors(1 << 16) or []) >= 6,
                "the margin has the sky, the floor and a sprite in it "
                "(%d colours)" % len(m.getcolors(1 << 16) or []))
        S.check(left[1].tobytes() != plain[1].tobytes() and
                right[1].tobytes() != plain[1].tobytes(),
                "the cartridge's L and R do move its picture")
    def selfcheck(r, what):
        st = [e for e in r.evt("vita_wide")
              if e.get("selftest", "0/0").split("/")[1] != "0"]
        S.check(st and all(e["selftest"].startswith("0/") for e in st) and
                st[-1]["w"] == "282" and st[-1]["margin"] == "21",
                "%s: the renderer's own check -- the picture's first and "
                "last 21 columns drawn as a margin are the picture (%s)" %
                (what, st[-1]["selftest"] if st else "no vita_wide line"))
    selfcheck(r_wide, "plain")

    # ---- mosaic: blocks are counted from the screen's left edge.  3x3
    # blocks fall the same way in a picture moved by 21; 4x4 do not, and a
    # strip has to begin three pixels early
    m_w = run("mosaic3", 10, "20:a:1270\n", frames=1300)
    m_l = run("mosaic3_right", 0, "20:a+l:340\n")
    m_r = run("mosaic3_left", 0, "20:a+r:340\n")
    mw, ml, mr = pictures(m_w, 240 + 2 * M), pictures(m_l, 240), pictures(m_r, 240)
    if all(mw + ml + mr) and all(wide):
        for i, n in enumerate("abc"):
            bl = differ(mw[i], 0, ml[i], 0, M)
            br = differ(mw[i], 240 + M, mr[i], 240 - M, M)
            S.check(bl == 0 and br == 0,
                    "%s, mosaic 3x3: both margins are the moved console's "
                    "columns (%d and %d pixels differ)" % (n, bl, br))
        S.check(mw[1].tobytes() != wide[1].tobytes(),
                "the cartridge's A does switch its mosaic on")
    selfcheck(m_w, "mosaic 3x3")
    m4 = run("mosaic4", 10, "20:b:1270\n", frames=1300)
    selfcheck(m4, "mosaic 4x4")
    p4 = pictures(m4, 240 + 2 * M)
    if all(p4) and all(mw):
        S.check(p4[1].tobytes() != mw[1].tobytes(),
                "the cartridge's B is another mosaic")

    # ---- the rows' defaults (wide 1, fill 1): a game without a profile
    # keeps its picture, and the margins carry its edges on softly
    r = run("default", None, cfg={"wide_gba": None})
    w = r.evt("wide")
    S.check(w and w[-1]["mode"] == "1" and w[-1]["fill"] == "1" and
            w[-1]["margin"] == "21" and w[-1]["profile"] == "0",
            "the defaults: wide, no profile, soft edges (%s)" %
            (w[-1]["_"] if w else "no line"))
    d = pictures(r, 240 + 2 * M)
    if all(d) and all(plain) and all(wide):
        S.check(all(differ(d[i], M, plain[i], 0, 240) == 0 for i in range(3)),
                "...the 240 columns in the middle are the console's picture")
        # beside the seam: the edge's colour, a little darker; never the
        # game's own margin, never black where the edge is not
        def lum(p): return p[0] + p[1] + p[2]
        seam = [(d[1].getpixel((M - 1, y)), plain[1].getpixel((0, y)))
                for y in range(0, 160, 4)]
        S.check(all(lum(a) <= lum(b) + 12 for a, b in seam) and
                sum(lum(a) for a, _ in seam) > sum(lum(b) for _, b in seam) // 2,
                "...the margin beside the seam is the edge, dimmed")
        S.check(differ(d[1], 0, wide[1], 0, M) > 500,
                "...and is not the game's own margin")
    for fill, name in ((0, "bars"), (2, "stretch")):
        r = S.run("wide_fill_" + name, shots + "360:quit\n", config=base,
                  autoboot="gbadhoc-wide.gba",
                  vita_ini={"wide": 1, "wide_fill": fill}, extra=extra)
        S.clean(r)
        f = pictures(r, 240 + 2 * M)
        if not (all(f) and all(plain)):
            continue
        if fill == 0:
            S.check(all(f[1].getpixel((x, y)) == (0, 0, 0)
                        for x in list(range(M)) + list(range(240 + M, 240 + 2 * M))
                        for y in range(0, 160, 3)) and
                    differ(f[1], M, plain[1], 0, 240) == 0,
                    "wide_fill = 0: black bars, the picture between them")
        else:
            ends = sum(1 for y in range(160)
                       if f[1].getpixel((0, y)) == plain[1].getpixel((0, y)) and
                       f[1].getpixel((239 + 2 * M, y)) == plain[1].getpixel((239, y)))
            S.check(ends == 160 and differ(f[1], M, plain[1], 0, 240) > 1000,
                    "wide_fill = 2: the picture's first and last columns are "
                    "the screen's, the rest is stretched between (%d of 160 "
                    "lines)" % ends)

    # ---- no room: stretch and 4x already take the width
    for scale, name in ((2, "stretch"), (3, "4x")):
        r = S.run("wide_" + name, "200:shot=a.png\n1300:quit\n",
                  config={"scale_gba": scale, "filter_gba": 0},
                  autoboot="gbadhoc-wide.gba", vita_ini={"wide": 2},
                  extra=extra)
        S.clean(r)
        e = [x for x in r.evt("vita_wide")]
        S.check(e and e[-1]["margin"] == "21" and e[-1]["w"] == "240",
                "%s: the margins have no room, the picture is drawn as "
                "before (%s)" % (name, e[-1]["_"][:60] if e else "no line"))
    r_fit = S.run("wide_fit", "200:shot=a.png\n1300:quit\n",
                  config={"scale_gba": 1, "filter_gba": 0},
                  autoboot="gbadhoc-wide.gba", vita_ini={"wide": 2}, extra=extra)
    S.clean(r_fit)
    im = S.picture(r_fit, "a.png")
    if im:
        col = [im.getpixel((x, 300)) for x in (4, 955)]
        S.check(all(sum(c) > 60 for c in col),
                "fit: the picture reaches both edges of the screen (%s)" % col)

    # ---- the rows: Settings wraps upwards to Button mapping, Keep full
    # speed, FPS, Rewind, FF button, Fast-forward, Game previews, Game list,
    # Theme, Wide view: menus, Wide view
    script = menu_pick(4, wait=100) + "+20:up\n" + "+8:up\n" * 9 + \
        "+10:right\n+10:up\n" \
        "+10:shot=row.png\n+4:right\n+10:shot=row_on.png\n+4:circle\n" \
        "+10:circle\n+60:shot=after.png\n" + EXIT_APP
    r = S.run("wide_row", script, config=dict(base, wide_gba=None),
              autoboot="gbadhoc-wide.gba", extra=extra)
    S.clean(r)
    look = r.evt("display_look")
    S.check(look and look[-1].get("wide") == "2" and
            look[-1].get("wide_fill") == "2",
            "Settings > Wide view: menus, one step right is stretch; Wide "
            "view, one step right is 2 (%s)" %
            [(e.get("wide"), e.get("wide_fill")) for e in look])
    w = r.evt("wide")
    S.check(w and w[-1]["mode"] == "2" and w[-1]["fill"] == "2" and
            w[-1]["margin"] == "21",
            "...and the core is told (%s)" % (w[-1]["_"] if w else "no line"))
    S.picture(r, "row_on.png", "wide_row.png")
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check(re.search(r"^wide_gba = 2$", ini, re.M) and
            re.search(r"^wide_fill_gba = 2$", ini, re.M),
            "config.ini keeps wide_gba = 2 and wide_fill_gba = 2")
    im = S.picture(r, "after.png")
    if im:
        got = game_pixels(im, w=240 + 2 * M)
        S.check(len(got.crop((0, 0, M, 160)).getcolors(1 << 16) or []) >= 4,
                "the game is wide when the menu closes")


def t_wide_auto(S):
    """The wide picture in a game the app knows nothing about (wide_gba = 3):
    a side-scroller (testrom/scroll.c: a level streamed into a 256 pixel
    ring, a sky that repeats, a status bar) is walked right, back left,
    thrown to another part of its level and walked on.  Beside it the same
    frames as a console shows them with its camera 21 pixels further left,
    then right (the cartridge does that while L or R is held).  What the
    automatic mode puts in the margins -- from what the game has shown, and
    from its second machine running ahead -- must be those columns."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-scroll.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-scroll.gba (make -C testrom scroll): skipped")
        return
    extra = {"roms/gbadhoc-scroll.gba": open(rom, "rb").read()}
    base = {"scale_gba": 4, "filter_gba": 0}
    walk = "100:right:120\n150:shot=a.png\n215:shot=b.png\n260:left:60\n" \
           "300:shot=c.png\n360:shot=d.png\n400:a:4\n420:right:80\n" \
           "470:shot=e.png\n520:shot=f.png\n1300:quit\n"
    M = 21

    def run(name, wide, keys=""):
        r = S.run("auto_" + name, keys + walk, config=base,
                  autoboot="gbadhoc-scroll.gba", vita_ini={"wide": wide},
                  extra=extra)
        S.clean(r)
        return r

    def pictures(r, w):
        out = []
        for n in "abcdef":
            im = S.picture(r, n + ".png")
            out.append(game_pixels(im, w=w) if im else None)
        return out

    r_auto = run("wide", 3 + 16)                 # + a line for every look ahead
    r_l = run("camera_left", 0, "20:l:1270\n")
    r_r = run("camera_right", 0, "20:r:1270\n")
    r_0 = run("plain", 0)
    auto, left, right, plain = (pictures(r_auto, 240 + 2 * M), pictures(r_l, 240),
                                pictures(r_r, 240), pictures(r_0, 240))
    if all(auto + left + right + plain):
        # (below the status bar, which a wider screen would not have there)
        def differ(a, ax, b, bx, n, y0=8):
            return sum(1 for y in range(y0, 160) for x in range(n)
                       if a.getpixel((ax + x, y)) != b.getpixel((bx + x, y)))
        when = {"a": "walking right, 50 frames in", "b": "115 frames in",
                "c": "walking back left", "d": "standing",
                "e": "50 frames after the jump to another place",
                "f": "standing there"}
        for i, n in enumerate("abcdef"):
            S.check(differ(auto[i], M, plain[i], 0, 240, 0) == 0,
                    "%s: the 240 columns in the middle are the console's "
                    "picture" % n)
            bl, br = differ(auto[i], 0, left[i], 0, M), \
                differ(auto[i], 240 + M, right[i], 240 - M, M)
            S.check(bl == 0 and br == 0,
                    "%s (%s): both margins are what lies beside the screen "
                    "(%d and %d of %d pixels differ)" %
                    (n, when[n], bl, br, M * 152))
        S.check(len(auto[3].crop((0, 8, M, 160)).getcolors(1 << 16) or []) >= 4,
                "the margin has the level in it")
    ends = r_auto.evt("scout_end")
    S.check(len(ends) >= 2 and sum(int(e["learned"]) for e in ends) > 500,
            "the second machine ran ahead and added to what is known (%d "
            "looks, %d map entries)" %
            (len(ends), sum(int(e["learned"]) for e in ends)))
    S.check(any(e["why"] == "scene" for e in ends),
            "a look ahead under way when the scene changed was dropped (%s)" %
            [e["why"] for e in ends])
    w = [e for e in r_auto.evt("vita_wide") if e.get("profile") == "3"]
    S.check(w and w[-1]["w"] == "282",
            "the log names the automatic profile (%s)" %
            (w[-1]["_"][:50] if w else "no vita_wide line"))


def t_wide_top(S):
    """The automatic mode in a world seen from above (testrom/topdown.c: two
    layers of one map streamed into 256 pixel rings, a text panel that does
    not scroll with them): walked down into new ground, back up, right; the
    panel slid away; thrown elsewhere and walked up.  A picture that moves
    up or down shows the game nothing new beside the screen -- only the
    second machine, stepping aside and going ahead, can.  A margin's line is
    either drawn, and then it is exactly what a console shows with its
    camera 21 pixels to that side, or it is left out (black here: wide_fill
    = bars); standing, and on ground walked before, none is left out."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-top.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-top.gba (make -C testrom top): skipped")
        return
    extra = {"roms/gbadhoc-top.gba": open(rom, "rb").read()}
    base = {"scale_gba": 4, "filter_gba": 0, "wide_fill_gba": 0}
    walk = "100:down:120\n160:shot=a.png\n215:shot=b.png\n300:shot=c.png\n" \
           "320:up:100\n380:shot=d.png\n440:right:80\n500:shot=e.png\n" \
           "560:start:4\n600:shot=f.png\n640:a:4\n660:up:150\n" \
           "720:shot=g.png\n780:shot=h.png\n860:shot=i.png\n1000:quit\n"
    names = "abcdefghi"
    M = 21

    def run(name, wide, keys=""):
        r = S.run("top_" + name, keys + walk, config=base,
                  autoboot="gbadhoc-top.gba", vita_ini={"wide": wide},
                  extra=extra)
        S.clean(r)
        return r

    def pictures(r, w):
        out = []
        for n in names:
            im = S.picture(r, n + ".png")
            out.append(game_pixels(im, w=w) if im else None)
        return out

    r_auto = run("wide", 3 + 16)
    r_l = run("camera_left", 0, "20:l:1000\n")
    r_r = run("camera_right", 0, "20:r:1000\n")
    r_0 = run("plain", 0)
    auto, left, right, plain = (pictures(r_auto, 240 + 2 * M), pictures(r_l, 240),
                                pictures(r_r, 240), pictures(r_0, 240))
    when = {"a": ("walking down into new ground, 60 frames in", 100, 0),
            "b": ("115 frames in", 120, 120),
            "c": ("standing", 160, 160),
            "d": ("walking back up", 160, 160),
            "e": ("walking right", 160, 150),
            "f": ("standing, the panel slid away", 160, 160),
            "g": ("walking up, 60 frames after the jump elsewhere", 130, 130),
            "h": ("120 frames after it", 130, 130),
            "i": ("standing there", 160, 160)}
    if all(auto + left + right + plain):
        black = bytes(3 * M)
        for i, n in enumerate(names):
            S.check(auto[i].crop((M, 0, M + 240, 160)).tobytes() ==
                    plain[i].tobytes(),
                    "%s: the 240 columns in the middle are the console's "
                    "picture" % n)
            drawn, wrong = [0, 0], [0, 0]
            for side, (ax, ref, rx) in enumerate(((0, left[i], 0),
                                                  (240 + M, right[i], 240 - M))):
                for y in range(160):
                    got = auto[i].crop((ax, y, ax + M, y + 1)).convert("RGB").tobytes()
                    if got == black:
                        continue
                    drawn[side] += 1
                    if got != ref.crop((rx, y, rx + M, y + 1)).convert("RGB").tobytes():
                        wrong[side] += 1
            S.check(wrong == [0, 0],
                    "%s (%s): every margin line that is drawn is what lies "
                    "beside the screen (%d and %d lines are not)" %
                    (n, when[n][0], wrong[0], wrong[1]))
            S.check(drawn[0] >= when[n][1] and drawn[1] >= when[n][2],
                    "%s: %d and %d of 160 lines of the margins are drawn "
                    "(at least %d and %d)" %
                    (n, drawn[0], drawn[1], when[n][1], when[n][2]))
    ends = r_auto.evt("scout_end")
    S.check(sum(1 for e in ends if e.get("vdir") in ("1", "-1")) >= 4,
            "the second machine stepped aside and went ahead of a picture "
            "moving up or down (%d such looks of %d)" %
            (sum(1 for e in ends if e.get("vdir") in ("1", "-1")), len(ends)))


def t_home(S):
    """The home screen (ui_home = 1, the Vita's default): the row of tiles
    and LEFT / RIGHT along it, a game started from it, the picture it leaves
    behind (scenes/), a state saved in the game and loaded from the home
    screen's cards."""
    script = """
60:shot=home.png
+2:right
+20:shot=home_next.png
+2:left
+20:cross
+150:shot=game.png
""" + menu_pick(1) + """
+20:down
+10:square
+30:circle
+10:circle
""" + QUIT_TO_LIST + """
+90:shot=home_back.png
+2:down
+30:shot=home_states.png
+10:cross
+90:shot=from_card.png
""" + EXIT_APP
    r = S.run("home", script, config={"ui_home": 1})
    S.clean(r)
    picks = r.evt("ui_browser_pick")
    S.check(len(picks) >= 2 and picks[0].get("rom") == "gbadhoc-test.gba",
            "X on the focused tile starts the game the list remembered "
            "(log: %s)" % [p["_"] for p in picks])
    nxt = [p for p in r.evt("ui_browser_pick") if p.get("state_slot")]
    S.check(nxt and nxt[0].get("state_slot") == "2" and
            nxt[0].get("rom") == "gbadhoc-test.gba",
            "DOWN shows the states as cards on the first one held; X loads "
            "slot 2 "
            "(log: %s)" % [p["_"] for p in picks])
    sv = r.evt("scene_save")
    S.check(sv and sv[0].get("ok") == "1" and
            os.path.exists(os.path.join(r.root, "scenes", "gbadhoc-test.shot")),
            "leaving the game leaves its picture in scenes/ (log: %s)" %
            (sv[0]["_"] if sv else "no scene_save line"))
    sc = decode_shot(os.path.join(r.root, "scenes", "gbadhoc-test.shot")) \
        if sv else None
    S.check(sc is not None and sc[0] == 240 and sc[1] == 160,
            "the picture is a 240 x 160 .shot")
    S.picture(r, "home.png")
    S.picture(r, "home_next.png")
    S.picture(r, "home_back.png")
    S.picture(r, "home_states.png")
    a, b = r.shot("home.png"), r.shot("home_next.png")
    S.check(a is not None and b is not None and worst_diff(a, b) > 0,
            "RIGHT moves the focus along the row")


def read_clip(path):
    """clips/<game>.clip (clip_vita.h) -> (w, h, us_per_tick, [frame
    bytes]), or None when it is not one."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    if len(d) < 32 or d[:4] != b"VCL1":
        return None
    w, h, n, us, ln = struct.unpack_from("<HHIII", d, 4)
    base = 32 + 4 * (n + 1)
    if not n or len(d) != base + ln:
        return None
    offs = struct.unpack_from("<%dI" % (n + 1), d, 32)
    return w, h, us, [d[base + offs[i]:base + offs[i + 1]]
                      for i in range(n)]


def decode_clip(w, h, frames):
    """Every frame back into pixels (libretro RGB565 lists), with each
    frame's (kind, ticks, dx, dy, {op: pixels}).  Raises on a bad one."""
    out, info, prev = [], [], None
    n = w * h
    for fr in frames:
        kind, ticks = chr(fr[0]), fr[1]
        dx, dy = struct.unpack_from("bb", fr, 2)
        cur, ops = [0] * n, {0: 0, 1: 0, 2: 0, 3: 0}
        i, s = 0, 4
        while i < n:
            t = fr[s]
            s += 1
            m = t & 63
            if m == 63:
                v = sh = 0
                while True:
                    b = fr[s]
                    s += 1
                    v |= (b & 127) << sh
                    sh += 7
                    if not b & 128:
                        break
                m += v
            c, op = m + 1, t >> 6
            ops[op] += c
            if op == 0:
                cur[i:i + c] = prev[i:i + c]
            elif op == 1:
                k = 0
                while k < c:
                    q = i + k
                    y, x = divmod(q, w)
                    run = min(c - k, w - x)
                    src = (y + dy) * w + x + dx
                    cur[q:q + run] = prev[src:src + run]
                    k += run
            elif op == 2:
                cur[i:i + c] = struct.unpack_from("<%dH" % c, fr, s)
                s += 2 * c
            else:
                cur[i:i + c] = [struct.unpack_from("<H", fr, s)[0]] * c
                s += 2
            i += c
        if s != len(fr) or (kind == "D" and prev is None):
            raise ValueError("bad frame %d" % len(out))
        out.append(cur)
        info.append((kind, ticks, dx, dy, ops))
        prev = cur
    return out, info


def t_preview(S):
    """The home screen's previews (clip_vita.h): a side-scroller walked and
    left; the last seconds of it are in clips/, squeezed with the scroll
    followed, end on the still the home screen shows, and play behind the
    row when it rests on the game.  With Settings > Game previews off,
    nothing is recorded or played."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-scroll.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-scroll.gba (make -C testrom scroll): skipped")
        return
    extra = {"roms/gbadhoc-scroll.gba": open(rom, "rb").read()}
    # walk right, back left, stand five seconds: the clip ends a second
    # after the camera stopped, not on five seconds of nothing
    script = "100:right:180\n+20:left:120\n+300:shot=game.png\n" + \
        QUIT_TO_LIST + "+30:shot=still.png\n+170:shot=clip_a.png\n" \
        "+60:shot=clip_b.png\n+10:quit\n"
    cfg = {"ui_home": 1, "last_rom": "gbadhoc-scroll.gba"}
    r = S.run("preview", script, config=cfg, autoboot="gbadhoc-scroll.gba",
              extra=extra)
    S.clean(r)
    sv = r.evt("clip_save")
    S.check(sv and sv[-1].get("rc") == "0",
            "leaving the game saves its clip (log: %s)" %
            (sv[-1]["_"] if sv else "no clip_save line"))
    S.check(sv and 180 <= int(sv[-1].get("calm_cut", 0)) <= 300,
            "the five still seconds at the end are left out but one (%s "
            "ticks cut)" % (sv[-1].get("calm_cut") if sv else "?"))
    path = os.path.join(r.root, "clips", "gbadhoc-scroll.clip")
    c = read_clip(path)
    S.check(c is not None, "clips/gbadhoc-scroll.clip is a clip")
    if c:
        w, h, us, frames = c
        S.check((w, h) == (240, 160) and us == 16743,
                "240 x 160, a core frame a tick (%dx%d, %d us)" % (w, h, us))
        try:
            pix, info = decode_clip(w, h, frames)
        except (ValueError, IndexError, struct.error) as e:
            pix, info = None, None
            S.check(False, "every frame decodes (%s)" % e)
        if pix:
            ticks = sum(i[1] for i in info[1:])
            S.check(info[0][0] == "K" and 180 <= ticks <= 600,
                    "it starts whole and lasts 3 to 10 seconds (%s, %d ticks, "
                    "%d frames)" % (info[0][0], ticks, len(info)))
            # the level scrolls 4 px a kept frame, its sky 2 (behind it,
            # at half speed): one motion a frame follows the level
            moved = [k for k, i in enumerate(info) if i[2] or i[3]]
            S.check(len(moved) > 20 and
                    sum(info[k][4][1] for k in moved) > len(moved) * w * h // 3
                    and sum(len(frames[k]) for k in moved) * 10 <
                    len(moved) * w * h * 2,
                    "the scroll is followed: %d frames move a third of their "
                    "pixels or more, and take under a tenth of a raw frame"
                    % len(moved))
            raw = len(frames) * w * h * 2
            size = sum(len(f) for f in frames)
            S.check(size * 5 < raw, "squeezed below a fifth of the raw "
                    "frames (%d of %d bytes)" % (size, raw))
            sc = decode_shot(os.path.join(r.root, "scenes",
                                          "gbadhoc-scroll.shot"))
            if sc:
                last = pix[-1]
                scp = sc[2].load()
                same = all(
                    ((last[y * w + x] >> 11) << 3 | (last[y * w + x] >> 13),
                     ((last[y * w + x] >> 5) & 63) << 2 |
                     ((last[y * w + x] >> 9) & 3),
                     (last[y * w + x] & 31) << 3 | ((last[y * w + x] >> 2) & 7))
                    == scp[x, y] for y in range(0, h, 4) for x in range(0, w, 4))
                S.check(same, "the clip ends on the very picture the home "
                        "screen's still is")
            first, mid = pix[0], pix[len(pix) // 3]
            S.check(first != mid, "the clip moves")
    op = r.evt("home_clip")
    S.check(op and op[0].get("frames") and
            "home_clip open" in op[0]["_"],
            "resting on the game opens its clip (log: %s)" %
            [o["_"] for o in op])
    a, b = r.shot("clip_a.png"), r.shot("clip_b.png")
    S.check(a is not None and b is not None and
            worst_diff(a, b, (480, 100, 960, 300)) > 40,
            "and it plays behind the row: two moments of it differ")
    S.picture(r, "still.png")
    S.picture(r, "clip_a.png")

    # Settings > Game previews off
    r = S.run("preview_off", script, config=dict(cfg, previews=0),
              autoboot="gbadhoc-scroll.gba", extra=extra)
    S.clean(r)
    S.check(r.has(r"^EVT clip_begin on=0") and
            not os.path.exists(os.path.join(r.root, "clips",
                                            "gbadhoc-scroll.clip")) and
            not r.evt("home_clip"),
            "previews off: nothing recorded, nothing played")


def t_gameinfo(S):
    """What the home screen knows by itself (gamedb.h, artfetch.h): a
    cartridge named the No-Intro way shows Wikipedia's first lines about
    the game under its name, and its box art is fetched from the thumbnail
    server (here a local one, vita.ini art_url) into boxart/ and replaces
    the tile's card; cartridges the server has nothing for are written to
    boxart/.missing.txt and not asked for again."""
    import http.server
    import threading
    import functools
    import io
    name = "Pokemon - Emerald Version (USA, Europe)"
    serve = os.path.join(S.outdir, "gameinfo_server")
    shutil.rmtree(serve, ignore_errors=True)
    os.makedirs(os.path.join(serve, "Nintendo - Game Boy Advance",
                             "Named_Boxarts"))
    # a square "box": a gradient with a diagonal, nothing like the card
    im = Image.new("RGB", (256, 256))
    im.putdata([((x * 255) // 255, (y * 200) // 255, 160 if x > y else 40)
                for y in range(256) for x in range(256)])
    buf = io.BytesIO()
    im.save(buf, "PNG")
    png = buf.getvalue()
    with open(os.path.join(serve, "Nintendo - Game Boy Advance",
                           "Named_Boxarts", name + ".png"), "wb") as f:
        f.write(png)
    # a title screen for the test cartridge (which has no box art): the
    # home screen's picture for a game never played (artfetch_titles)
    os.makedirs(os.path.join(serve, "Nintendo - Game Boy Advance",
                             "Named_Titles"))
    tim = Image.new("RGB", (240, 160), (30, 140, 60))
    buf = io.BytesIO()
    tim.save(buf, "PNG")
    title_png = buf.getvalue()
    with open(os.path.join(serve, "Nintendo - Game Boy Advance",
                           "Named_Titles", "gbadhoc-test.png"), "wb") as f:
        f.write(title_png)
    asked = []

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, fmt, *args):
            asked.append(self.path)

    srv = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(Quiet, directory=serve))
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    port = srv.server_address[1]
    extra = {"roms/%s.gba" % name: open(OWN_GBA, "rb").read()}
    script = "940:shot=info_art.png\n+10:quit\n"
    cfg = {"ui_home": 1, "last_rom": name + ".gba"}
    r = S.run("gameinfo", script, config=cfg, extra=extra,
              vita_ini={"art_url": "http://127.0.0.1:%d" % port})
    srv.shutdown()
    S.clean(r)
    db = r.evt("gamedb")
    S.check(db and int(db[0].get("records", "0")) > 10000,
            "the bundled game list is read (log: %s)" %
            (db[0]["_"] if db else "no gamedb line"))
    got = os.path.join(r.root, "boxart", name + ".png")
    ok = r.evt("art_fetch")
    S.check(os.path.exists(got) and open(got, "rb").read() == png,
            "the box art was fetched into boxart/ whole (log: %s; asked: %s)" %
            ([e["_"] for e in ok], asked[:4]))
    S.check(any("/Named_Boxarts/Pokemon%20-%20Emerald%20Version%20%28USA"
                "%2C%20Europe%29.png" in a for a in asked),
            "asked for under its No-Intro name, the server's way "
            "(asked: %s)" % asked[:6])
    miss = os.path.join(r.root, "boxart", ".missing.txt")
    lines = open(miss).read().split("\n") if os.path.exists(miss) else []
    S.check("gbadhoc-test" in lines,
            "a cartridge the server has nothing for is remembered as such "
            "(.missing.txt: %s)" % lines[:6])
    # (when the picture arrives is the network's business: the first
    # screenshot may or may not have it; by the last it is drawn)
    S.picture(r, "info_art.png", ref="info_art.png")
    S.check(not os.path.exists(got + ".tmp"), "no half-written file left")
    tgot = os.path.join(r.root, "titles", "gbadhoc-test.png")
    S.check(os.path.exists(tgot) and open(tgot, "rb").read() == title_png,
            "the test cartridge's title screen was fetched into titles/ "
            "(asked: %s)" % [a for a in asked if "Named_Titles" in a][:4])
    tmiss = os.path.join(r.root, "titles", ".missing.txt")
    tl = open(tmiss).read().split("\n") if os.path.exists(tmiss) else []
    S.check(name in tl, "Emerald, which has no title screen here, is "
            "remembered in titles/.missing.txt (%s)" % tl[:4])


def t_title_bg(S):
    """A game never played, with a title screen fetched (titles/, from
    artfetch.h): its tile and the picture behind the home screen's row are
    that title screen, not a plain card and an empty backdrop."""
    import io
    tim = Image.new("RGB", (240, 160), (30, 160, 60))
    buf = io.BytesIO()
    tim.save(buf, "PNG")
    extra = {"titles/gbadhoc-test.png": buf.getvalue()}
    cfg = {"ui_home": 1, "last_rom": "gbadhoc-test.gba"}
    r = S.run("title_bg", "200:shot=title_bg.png\n+10:quit\n", config=cfg,
              extra=extra)
    S.clean(r)
    im = S.picture(r, "title_bg.png")
    if not im:
        return
    px = [im.getpixel((x, 250)) for x in range(600, 900, 30)]
    green = [p for p in px if p[1] > p[0] + 25 and p[1] > p[2] + 25]
    S.check(len(green) >= 5, "the backdrop is the green title screen "
            "(samples %s)" % px[:4])
    tile = im.getpixel((110, 150))
    S.check(tile[1] > tile[0] + 60 and tile[1] > tile[2] + 60,
            "the tile is the title screen too, not the console's card (%s)" %
            (tile,))


def t_fullspeed(S):
    """Keep full speed (main_vita.c, catching up): with the console's
    display queue modelled (--flip-queue 2), one slow frame (45 ms: the
    recompiler meeting new code) and then three in a row (25 ms each).
    Setting on: the frames owed are run without being drawn -- the game's
    time keeps the display's, the sound has no gap, the present never waits
    in the queue, and a frame starts only once the last picture is shown.
    Setting off: the game falls behind for good (or waits in the queue
    every frame from then on)."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-scroll.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-scroll.gba: skipped")
        return
    extra = {"roms/gbadhoc-scroll.gba": open(rom, "rb").read()}
    res = {}
    for on in (1, 0):
        for slow in ("700:1:45000", "700:3:25000"):
            name = "fullspeed_%s_%s" % ("on" if on else "off",
                                        slow.split(":")[1])
            r = S.run(name, "100:right:900\n", extra=extra,
                      config={"full_speed": on,
                              "last_rom": "gbadhoc-scroll.gba"},
                      autoboot="gbadhoc-scroll.gba",
                      args=("--frames", "1900", "--flip-queue", "2",
                            "--slow", slow))
            S.clean(r)
            wins = r.evt("vita_win")
            au = r.evt("vita_audio")
            res[(on, slow)] = dict(
                emulated=int(r.host.get("emulated", 0)),
                caught=sum(int(w.get("caught", 0)) for w in wins),
                forgiven=int(wins[-1].get("forgiven", 0)) if wins else -1,
                swap=max(int(w.get("swap_us", "0/0").split("/")[0])
                         for w in wins[1:]) if len(wins) > 1 else -1,
                under=int(au[-1].get("under", 0)) if au else -1)
    # ---- the row: Settings wraps upwards to Button mapping, Keep full
    # speed; CROSS turns it off, and config.ini keeps that
    script = menu_pick(4, wait=100) + "+20:up\n+8:up\n+10:shot=row.png\n" \
        "+4:cross\n+10:shot=row_off.png\n+4:circle\n+10:circle\n+30:quit\n"
    r = S.run("fullspeed_row", script, autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.picture(r, "row.png", "fullspeed_row.png")
    ev = r.evt("full_speed_setting")
    S.check(ev and ev[-1].get("on") == "0",
            "Settings > Keep full speed: CROSS turns it off (%s)" %
            (ev[-1]["_"] if ev else "no line"))
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check(re.search(r"^full_speed = 0$", ini, re.M),
            "config.ini keeps full_speed = 0")

    for slow in ("700:1:45000", "700:3:25000"):
        a, b = res[(1, slow)], res[(0, slow)]
        S.check(a["caught"] >= 1 and a["forgiven"] == 0,
                "on, %s: the frames owed are run, none forgiven (%s)" %
                (slow, a))
        S.check(a["under"] == 0, "on, %s: no gap in the sound (%s)" %
                (slow, a))
        S.check(a["swap"] < 2000, "on, %s: the present never waits in the "
                "display queue (%s)" % (slow, a))
        S.check(b["caught"] == 0 and (b["emulated"] < a["emulated"] or
                                      b["swap"] > 5000),
                "off, %s: the game is left behind, or waits in the queue "
                "from then on (on: %s, off: %s)" % (slow, a, b))


def t_upfade(S):
    """A picture that changes whole (a fade, a jump to another place) at xBRZ
    4x: scaled at 2x at once (upscale_vita.c UP_HOT_HOLD), drawn the same
    size and place from the top left of the same 4x textures, and 4x again
    once the pictures settle -- no textures made in between."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-top.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-top.gba: skipped")
        return
    extra = {"roms/gbadhoc-top.gba": open(rom, "rb").read()}
    # A jumps the camera far away: the next picture is all new
    script = "150:shot=before.png\n+2:a:2\n+6:shot=jump.png\n" \
             "+70:shot=after.png\n+10:quit\n"
    shots = {}
    for name, filt in (("xbrz", 4), ("near", 0)):
        r = S.run("upfade_" + name, script, extra=extra,
                  config={"scale_gba": 1, "filter_gba": filt,
                          "last_rom": "gbadhoc-top.gba"},
                  autoboot="gbadhoc-top.gba")
        S.clean(r)
        shots[name] = {k: S.picture(r, k + ".png")
                       for k in ("before", "jump", "after")}
        if name == "xbrz":
            fac = [int(e["f"]) for e in r.evt("up_factor")]
            tex = r.evt("up_tex")
            S.check(fac[:1] == [4] and 2 in fac and fac[-1] == 4,
                    "xBRZ at fit: 4x, 2x after the jump, 4x again (%s)" % fac)
            S.check(len(tex) == 1,
                    "the 4x textures were made once (%d up_tex lines)" %
                    len(tex))

    def blocks(im):
        # the game's rect at fit (3.4x: 816 x 544, centred), 17 x 17 blocks
        g = im.crop((72, 0, 888, 544)).resize((48, 32), Image.BOX)
        return [g.getpixel((x, y)) for y in range(32) for x in range(48)]

    for k in ("before", "jump", "after"):
        a, b = shots["xbrz"].get(k), shots["near"].get(k)
        if not a or not b:
            continue
        ba, bb = blocks(a), blocks(b)
        worst = max(max(abs(p - q) for p, q in zip(u, v))
                    for u, v in zip(ba, bb))
        S.check(worst <= 40, "%s: the smoothed picture is the game's, in "
                "the same place and size (block colours within %d of the "
                "sharp one's)" % (k, worst))


def t_gpu_xbrz(S):
    """xBRZ drawn by the GPU (video_vita.c "xBRZ on the GPU"; the shaders
    in shaders/ run per pixel by the twin with --gpu-progs, as the console's
    GPU runs them), asked for with vita.ini gpu_xbrz = 1: the CPU upscalers
    stay idle, every frame is smoothed at the final size, and the picture
    is xBRZ's -- next to the CPU's 4x one (same place and size, edges
    smoothed where the CPU smooths them), not the sharp one.  By default
    (the console's GPU is too slow for it) and without --gpu-progs (a
    console without the shader compiler) xBRZ is the CPU's as before."""
    if S.arch != "x86":
        # (the programs are plain C++ per pixel: nothing of the ARM
        # recompiler in them, and very slow under qemu)
        S.note("gpu_xbrz: x86 only")
        return
    rom = os.path.join(HERE, "testrom", "gbadhoc-top.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-top.gba: skipped")
        return
    extra = {"roms/gbadhoc-top.gba": open(rom, "rb").read()}
    # (the GPU run goes on past the log's first ten-second window)
    script = "150:shot=a.png\n+2:right:40\n+4:shot=b.png\n"
    conf = {"scale_gba": 1, "filter_gba": 4, "last_rom": "gbadhoc-top.gba"}
    # the CPU's pictures all at 4x (fade_drop = 0): the one to be near
    runs = (("gpu", conf, ("--gpu-progs",), {"gpu_xbrz": 1}, "620:quit\n"),
            ("cpu", conf, ("--gpu-progs",), {"fade_drop": 0}, "+10:quit\n"),
            ("off", conf, (), {"gpu_xbrz": 1}, "+10:quit\n"),
            ("near", dict(conf, filter_gba=0), (), None, "+10:quit\n"))
    shots, logs = {}, {}
    for name, cfg, args, ini, end in runs:
        r = S.run("gpux_" + name, script + end, extra=extra, config=cfg,
                  autoboot="gbadhoc-top.gba", args=args, vita_ini=ini)
        S.clean(r)
        shots[name] = [S.picture(r, n) for n in ("a.png", "b.png")]
        logs[name] = r
    g = logs["gpu"]
    on = g.evt("gpu_xbrz")
    S.check(on and on[0].get("on") == "1",
            "--gpu-progs: the GPU draws xBRZ (%s)" % (on[0]["_"] if on else
                                                      "no gpu_xbrz line"))
    pics = sum(int(e.get("pictures", 0)) for e in g.evt("vita_gpu"))
    S.check(pics >= 150, "every frame smoothed on the GPU (%d pictures)" % pics)
    S.check(not g.evt("up_tex") and
            not any(int(e.get("jobs", 0)) for e in g.evt("vita_upscale")),
            "the CPU upscalers did nothing (%d up_tex, jobs %s)" %
            (len(g.evt("up_tex")),
             [e.get("jobs") for e in g.evt("vita_upscale")]))
    for name in ("cpu", "off"):
        r = logs[name]
        S.check(len(r.evt("up_tex")) >= 1 and
                not any(int(e.get("pictures", 0)) for e in r.evt("vita_gpu")),
                "%s: xBRZ on the CPU as before" % name)

    def stats(a, b):
        # the game's rect at fit (3.4x: 816 x 544, centred)
        a = a.convert("RGB").crop((72, 0, 888, 544))
        b = b.convert("RGB").crop((72, 0, 888, 544))
        pa, pb = list(a.tobytes()), list(b.tobytes())
        pa = list(zip(pa[0::3], pa[1::3], pa[2::3]))
        pb = list(zip(pb[0::3], pb[1::3], pb[2::3]))
        diff = [max(abs(x - y) for x, y in zip(p, q)) for p, q in zip(pa, pb)]
        ga = a.resize((48, 32), Image.BOX)
        gb = b.resize((48, 32), Image.BOX)
        blk = max(max(abs(x - y) for x, y in zip(ga.getpixel((i, j)),
                                                  gb.getpixel((i, j))))
                  for j in range(32) for i in range(48))
        return sum(diff) / float(len(diff)), \
            sum(1 for d in diff if d > 48) / float(len(diff)), blk

    for i, n in enumerate(("a.png", "b.png")):
        gp, cp, nr = shots["gpu"][i], shots["cpu"][i], shots["near"][i]
        if not (gp and cp and nr):
            S.check(False, "%s: the three pictures were taken" % n)
            continue
        mean_c, far_c, blk_c = stats(gp, cp)
        mean_n, far_n, blk_n = stats(gp, nr)
        mean_cn, far_cn, _ = stats(cp, nr)
        S.check(blk_c <= 24 and blk_n <= 40,
                "%s: the GPU's picture is the game's, in the same place and "
                "size (blocks within %d of the CPU's, %d of the sharp one)"
                % (n, blk_c, blk_n))
        S.check(mean_c < mean_cn and far_c < 0.02,
                "%s: closer to the CPU's xBRZ than the sharp picture is "
                "(mean %.2f vs %.2f; %.2f%% of pixels far off)"
                % (n, mean_c, mean_cn, 100 * far_c))
        S.check(mean_n > 0.3,
                "%s: smoothed, not the sharp picture (mean %.2f from it)"
                % (n, mean_n))
        S.summary["gpu_xbrz.%s" % n] = "cpu %.2f/%.3f near %.2f" % (
            mean_c, far_c, mean_n)
    if shots["gpu"][0]:
        S.picture(logs["gpu"], "a.png", "gpu_xbrz_a.png")
    t = [e for e in g.evt("gpu_xbrz_time")]
    S.check(len(t) >= 1 and not g.evt("gpu_xbrz off"),
            "the guard timed the GPU and left it on (%s)" %
            [e["_"] for e in t][:2])

    # the guard: a GPU that takes 15 ms a frame gives xBRZ back to the CPU
    r = S.run("gpux_slow", "100:right:200\n+10:shot=s.png\n+10:quit\n",
              extra=extra, config=conf, autoboot="gbadhoc-top.gba",
              args=("--gpu-progs", "--gpu-us", "15000"),
              vita_ini={"gpu_xbrz": 1})
    S.clean(r)
    off = [e for e in r.evt("gpu_xbrz") if e["_"].startswith("EVT gpu_xbrz off")]
    S.check(len(off) == 1 and off[0].get("reason") == "slow" and
            len(r.evt("gpu_xbrz_time")) == 2,
            "a slow GPU: timed twice, then off (%s)" %
            [e["_"] for e in r.evt("gpu_xbrz")])
    S.check(len(r.evt("up_tex")) >= 1,
            "...and xBRZ is the CPU's from then on (%d up_tex)" %
            len(r.evt("up_tex")))
    S.picture(r, "s.png")

    # the wide view: the margins and the middle, smoothed together
    wide = dict(conf, wide_gba=3)
    pics = {}
    for name, args in (("gpu", ("--gpu-progs",)), ("cpu", ())):
        r = S.run("gpux_wide_" + name, "150:shot=w.png\n+10:quit\n",
                  extra=extra, config=wide, autoboot="gbadhoc-top.gba",
                  args=args, vita_ini={"fade_drop": 0, "gpu_xbrz": 1})
        S.clean(r)
        pics[name] = S.picture(r, "w.png")
        w = r.evt("wide")
        S.check(w and int(w[-1].get("margin", "0")) > 0,
                "wide %s: the wide view is on" % name)
    if pics["gpu"] and pics["cpu"]:
        a = pics["gpu"].convert("RGB")
        b = pics["cpu"].convert("RGB")
        ga, gb = a.resize((60, 34), Image.BOX), b.resize((60, 34), Image.BOX)
        blk = max(max(abs(x - y) for x, y in zip(ga.getpixel((i, j)),
                                                  gb.getpixel((i, j))))
                  for j in range(34) for i in range(60))
        S.check(blk <= 24, "wide: the GPU's picture is the CPU's, edge to "
                "edge (blocks within %d)" % blk)


def t_render_thread(S):
    """The GBA's picture drawn on another core (render_thread.h): checked
    against the core's own picture on every frame of the four test
    cartridges (render_thread = 2), the same screenshots with it on (the
    default) and off, and off by itself with the wide view."""
    roms = {
        "gbadhoc-test.gba": "100:a:5\n+50:b:5\n+50:right:200\n+50:up:100\n",
        "gbadhoc-scroll.gba": "100:right:600\n+10:a:3\n+20:left:200\n",
        "gbadhoc-top.gba": "100:right:200\n+10:down:200\n+10:a:3\n"
                           "+20:left:300\n+10:a:3\n",
        "gbadhoc-wide.gba": "100:right:400\n+10:left:400\n",
    }
    for rom, keys in roms.items():
        path = os.path.join(HERE, "testrom", rom)
        if not os.path.exists(path):
            S.note("no testrom/%s: skipped" % rom)
            continue
        extra = {"roms/" + rom: open(path, "rb").read()}
        tag = rom.split(".")[0].split("-")[1]
        r = S.run("rt_check_" + tag, keys, extra=extra,
                  config={"last_rom": rom}, autoboot=rom,
                  vita_ini={"render_thread": 2}, args=("--frames", "1300"))
        S.clean(r)
        st = r.evt("vita_render")
        checked = sum(int(e["checked"]) for e in st)
        bad = sum(int(e["bad_frames"]) for e in st)
        S.check(checked >= 1100 and bad == 0,
                "%s: the thread's picture is the core's on every frame "
                "(%d checked, %d differed: %s)" %
                (rom, checked, bad, [e["_"] for e in r.evt("render_check")][:2]))
        shots = {}
        for mode in (1, 0):
            r = S.run("rt_%s_%d" % (tag, mode),
                      keys +
                      "+5:shot=a.png\n+40:shot=b.png\n+10:quit\n",
                      extra=extra, config={"last_rom": rom}, autoboot=rom,
                      vita_ini={"render_thread": mode})
            S.clean(r)
            shots[mode] = [S.picture(r, n) for n in ("a.png", "b.png")]
            if mode:
                S.check(sum(int(e["frames"]) for e in r.evt("vita_render")) > 0
                        or r.has(r"render_thread=1"),
                        "%s: the thread draws (render_thread = 1)" % rom)
        for i, n in enumerate(("a.png", "b.png")):
            a, b = shots[1][i], shots[0][i]
            if a and b:
                S.check(a.tobytes() == b.tobytes(),
                        "%s: %s is the same with the thread and without" %
                        (rom, n))

    # the wide view on: the margins are still drawn as the lines go by, the
    # middle by the thread, and the lines put together at the frame's end --
    # the same pictures as without the thread (fit and the test cartridge's
    # own profile; a Mode 7 floor, the side-scroller and the map from above)
    for rom, keys in (("gbadhoc-wide.gba", "100:right:400\n+10:left:200\n"),
                      ("gbadhoc-scroll.gba", "100:right:500\n"),
                      ("gbadhoc-top.gba", "100:right:200\n+10:down:150\n")):
        path = os.path.join(HERE, "testrom", rom)
        if not os.path.exists(path):
            continue
        tag = rom.split(".")[0].split("-")[1]
        shots = {}
        for mode in (1, 0):
            r = S.run("rt_wide_%s_%d" % (tag, mode),
                      keys + "+5:shot=a.png\n+40:shot=b.png\n+10:quit\n",
                      config={"last_rom": rom, "wide_gba": 3,
                              "scale_gba": 1},
                      autoboot=rom, extra={"roms/" + rom: open(path, "rb").read()},
                      vita_ini={"render_thread": mode})
            S.clean(r)
            shots[mode] = [S.picture(r, n) for n in ("a.png", "b.png")]
            if mode:
                w = r.evt("wide")
                S.check(w and int(w[-1].get("margin", "0")) > 0,
                        "%s: the wide view is on (%s)" %
                        (rom, w[-1]["_"] if w else "no wide line"))
        for i, n in enumerate(("a.png", "b.png")):
            a, b = shots[1][i], shots[0][i]
            if a and b:
                S.check(a.tobytes() == b.tobytes(),
                        "%s, wide view: %s is the same with the thread and "
                        "without" % (rom, n))


def t_render_skips(S):
    """The thread's picture across frames that are never drawn: the frames
    caught up after slow ones (Keep full speed), fast forward (3x: two of
    three not drawn; unlimited: the core's own frame skip) and rewind (a
    state loaded into the core every few frames, VRAM written without the
    recompiler's page marks).  The VRAM pages handed over must still be
    every page written since the last picture.  Checked frame by frame
    (render_thread = 2) and by the pictures with the thread and without."""
    walk = {"gbadhoc-top.gba": "100:right:200\n+10:down:200\n+10:left:400\n",
            "gbadhoc-scroll.gba": "100:right:500\n+10:left:300\n"}
    cases = (
        # name, config, extra keys (absolute frames), host args, what to see
        ("catch", {"full_speed": 1}, "",
         ("--slow", "300:4:30000"), "caught"),
        ("ff3", {}, "200:square:200\n", (), "ff"),
        ("ffu", {"ff_mult_x10": 0, "ff_smooth": 0}, "200:square:200\n", (),
         "ff"),
        ("rewind", {}, "500:select+square:60\n", (), "rewind"),
    )
    for rom, keys in walk.items():
        path = os.path.join(HERE, "testrom", rom)
        if not os.path.exists(path):
            S.note("no testrom/%s: skipped" % rom)
            continue
        tag = rom.split(".")[0].split("-")[1]
        extra = {"roms/" + rom: open(path, "rb").read()}
        for name, cfg, more, args, see in cases:
            conf = dict({"last_rom": rom}, **cfg)
            script = keys + more
            r = S.run("rts_%s_%s" % (tag, name), script, extra=extra,
                      config=conf, autoboot=rom,
                      vita_ini={"render_thread": 2},
                      args=("--frames", "1300") + args)
            S.clean(r)
            st = r.evt("vita_render")
            checked = sum(int(e["checked"]) for e in st)
            bad = sum(int(e["bad_frames"]) for e in st)
            if see == "caught":
                happened = sum(int(w.get("caught", 0))
                               for w in r.evt("vita_win"))
            elif see == "ff":
                happened = sum(int(e.get("frames", 0))
                               for e in r.evt("ff_stats"))
            else:
                happened = len([d for d in r.evt("rewind")
                                if d["_"].startswith("EVT rewind end")])
            S.check(happened > 0, "%s %s: the frames were skipped (%s %d)"
                    % (rom, name, see, happened))
            S.check(checked >= 500 and bad == 0,
                    "%s %s: the thread's picture is the core's on every drawn "
                    "frame (%d checked, %d differed: %s)" %
                    (rom, name, checked, bad,
                     [e["_"] for e in r.evt("render_check")][:2]))
            shots = {}
            for mode in (1, 0):
                r = S.run("rts_%s_%s_%d" % (tag, name, mode),
                          script + "900:shot=a.png\n+30:shot=b.png\n+10:quit\n",
                          extra=extra, config=conf, autoboot=rom,
                          vita_ini={"render_thread": mode}, args=args)
                S.clean(r)
                shots[mode] = [S.picture(r, n) for n in ("a.png", "b.png")]
            for i, n in enumerate(("a.png", "b.png")):
                a, b = shots[1][i], shots[0][i]
                S.check(a is not None and b is not None and
                        a.tobytes() == b.tobytes(),
                        "%s %s: %s is the same with the thread and without"
                        % (rom, name, n))


# ---- games from other folders (vita.ini extra_roms, ux0:ROMS/GBA ...) ---------

def list_oracle(primary, extras, ext):
    """What the game list must hold for one console, in its order, from the
    files themselves: [(file name, the name the list stores)].  The rules of
    rom_scan: roms/ and two levels beneath it, the console's extension, a
    name that begins with '.' is not a game (nor a folder to enter), the
    games of roms/ named relative to it and those of an extra folder by
    their whole path, sorted by file name without regard to case."""
    found = []

    def walk(root, rel, depth, whole):
        here = os.path.join(root, rel) if rel else root
        try:
            names = os.listdir(here)
        except OSError:
            return
        for n in names:
            if os.path.isdir(os.path.join(here, n)):
                if depth > 1 and not n.startswith("."):
                    walk(root, rel + "/" + n if rel else n, depth - 1, whole)
            elif not n.startswith(".") and n.lower().endswith(ext):
                r = rel + "/" + n if rel else n
                found.append((n, root + "/" + r if whole else r))

    walk(primary, "", 3, False)
    for e in extras:
        walk(e, "", 3, True)
    return sorted(found, key=lambda t: t[0].lower())


def list_crc(entries):
    """rom_scan's EVT rom_scan_done crc: the names in list order, a newline
    after each."""
    return "%08x" % (zlib.crc32("".join(n + "\n" for _, n in entries)
                                .encode()) & 0xFFFFFFFF)


def many_games(count, seed):
    """`count` distinct, realistic names: mixed case, brackets, a version, an
    index that keeps every name different even without regard to case."""
    import random
    rnd = random.Random(seed)
    words = ["Advance", "Wars", "Pokemon", "emerald", "Mario", "kart", "Zelda",
             "minish", "Cap", "Golden", "Sun", "metroid", "Fusion", "Sonic",
             "Battle", "Kirby", "Nightmare", "Wario", "Ware", "Fire", "Emblem",
             "Final", "Fantasy", "Tactics", "Castlevania", "Harmony", "Dawn"]
    tags = ["(U)", "(E)", "(J)", "(U) (V1.1)", "(USA, Europe)", "(Rev 1)"]
    out = []
    for i in range(count):
        w = [rnd.choice(words) for _ in range(rnd.randint(1, 3))]
        w[0] = w[0].lower() if rnd.random() < 0.3 else w[0]
        out.append("%s %05d %s [!].gba" % (" ".join(w), i, rnd.choice(tags)))
    return out


def t_extra_roms(S):
    """Games from folders besides roms/: vita.ini extra_roms and the built-in
    places; listed by their whole path, `._` files left out, started, their
    save and state written beside them, 1,500 games complete and in order."""
    fix = S.have_fixture
    have_gb = os.path.exists(OWN_GB)
    own = open(OWN_GBA, "rb").read()
    junk = b"\x00\x05\x16\x07" + bytes(4092)          # an AppleDouble file
    game = "Extra Adventure (U) [!].gba"

    # ---- the folder, in a data root of its own ----
    out = os.path.join(S.outdir, "extra_roms")
    root = os.path.join(out, "root")
    X = os.path.join(root, "extra")                    # an absolute host path
    extra = {
        "extra/" + game: own,
        "extra/._" + game: junk,                       # Mac junk beside it
        "extra/.Hidden Dot Game.gba": own,
        "extra/.hidden dir/Hidden Dir Game.gba": own,
        "extra/Sub Folder/Deep Game (E).gba": own,    # a game in a sub-folder
        "extra/A/B/Level Two.gba": own,                # as deep as roms/ goes
        "extra/A/B/C/Level Three.gba": own,            # one level too deep
        "extra/notes.txt": "not a game",
        "extra/gbadhoc-test.gba": own,                 # same name as roms/'s
        # the link mode of the game, set under its file name
        "roms/linkmode.txt": "%s = cable\n" % game,
        "roms/._arm.gba": junk,                        # junk in roms/ too
    }
    if have_gb:
        extra["extra/Extra Mono.gb"] = open(OWN_GB, "rb").read()
        extra["extra/._Extra Mono.gb"] = junk
    ini = {"extra_roms": X}
    # the last game played is the extra one: its name is a whole path
    cfg = {"last_rom": X + "/" + game}
    S.make_root(root, cfg, None, ini, extra)
    twin = X + "/gbadhoc-test.gba"
    want = list_oracle(os.path.join(root, "roms"), [X], ".gba")
    order = [n for _, n in want]
    downs = order.index(twin) - order.index(X + "/" + game)
    script = ("90:shot=list.png\n"
              "+2:cross\n"                             # start the extra game
              "+3:shot=loading.png\n"
              "+150:shot=game.png\n"
              "+2:a:40\n"                              # writes the battery RAM
              "+20:shot=changed.png\n"
              + menu_pick(1) +                         # Save states
              "+20:down\n+10:square\n+30:circle\n+10:circle\n"
              + QUIT_TO_LIST +
              "+4:shot=returning.png\n"
              "+70:shot=list_after.png\n"
              # the extra copy of gbadhoc-test.gba is a few rows down; star it
              + "+2:down\n" * downs +
              "+10:square\n"
              "+30:shot=starred.png\n"
              "+10:quit\n")
    r = S.run("extra_roms", script, prepared=True)
    S.clean(r)

    prim = list_oracle(os.path.join(root, "roms"), [], ".gba")
    S.check(len(want) - len(prim) == 4,
            "the fixture holds four games besides roms/'s (the oracle: %d)" %
            (len(want) - len(prim)))
    rr = r.evt("rom_roots")
    S.check(len(rr) == 1 and rr[0]["extra"] == "1" and
            rr[0]["source"] == "ini" and rr[0]["list"] == '"%s"' % X,
            "the log names the one extra folder (%s)" %
            (rr[0]["_"] if rr else "no rom_roots line"))
    b = r.evt("ui_browser")
    S.check(b and int(b[0]["roms"]) == len(want) and
            int(b[0]["found"]) == len(want),
            "the list holds roms/'s %d games and the folder's 4 (log: %s)" %
            (len(prim), b[0]["_"] if b else "no ui_browser line"))
    sc = [d for d in r.evt("rom_scan_root") if d["path"] == '"%s"' % X]
    S.check(sc and sc[0]["found"] == "4" and sc[0]["listed"] == "4",
            "the folder: four games found, `._` files, a dot-folder and "
            "the level too deep left out (%s)" %
            (sc[0]["_"] if sc else "no rom_scan_root line"))
    done = r.evt("rom_scan_done")
    S.check(done and done[0]["crc"] == list_crc(want) and
            int(done[0]["n"]) == len(want),
            "the list is complete and in order: crc %s (want %s)" %
            (done[0]["crc"] if done else "-", list_crc(want)))
    S.check(not r.has(r"rom_scan_skip"), "no name was skipped")
    S.picture(r, "list.png", "extra_list.png" if fix else None)

    # -- the extra game starts, from its own folder --
    S.check(r.has(r'^EVT ui_browser_pick rom=%s$' % re.escape(X + "/" + game)),
            "the pick is the game's whole path")
    S.check(r.has(r"rom_loaded code=AGDE"), "the cartridge loaded")
    sav = os.path.join(X, "Extra Adventure (U) [!].sav")
    S.check(r.has(r"^EVT game rom=%s size=\d+ console=GBA code=AGDE save=%s$" %
                  (re.escape(X + "/" + game), re.escape(sav))),
            "the game's save is named beside the ROM")
    S.check(os.path.exists(sav) and os.path.getsize(sav) == 131072 and
            len(set(open(sav, "rb").read())) > 1,
            "the battery save was written beside the ROM, in its own folder")
    st = os.path.join(X, "Extra Adventure (U) [!].st1")
    S.check(os.path.exists(st) and os.path.getsize(st) > 100000 and
            os.path.exists(st + ".thumb"),
            "slot 2's state and its preview were written beside the ROM")
    S.check(r.has(r"state_save slot=2 "), "the state was saved from the menu")
    S.check(not glob.glob(os.path.join(root, "roms", "Extra Adventure*")),
            "nothing of the game was written to roms/")
    S.check(os.path.exists(os.path.join(root, "scenes", "Extra Adventure (U) "
                                        "[!].shot")),
            "the home screen's picture is kept under the file's name")
    S.check(r.has(r'^EVT link_game code=AGDE key="%s" .*override=1' %
                  re.escape(game)),
            "the game's link mode is read under its file name (linkmode.txt)")
    S.picture(r, "game.png")
    S.check(r.has(r"game_end reason=game_list"), "Quit to game list")

    # -- back in the list: the save shows, and the game is still the last --
    cfgtxt = open(os.path.join(root, "config.ini")).read()
    S.check(("last_rom = %s/%s" % (X, game)) in cfgtxt,
            "the last game played is remembered by its whole path")
    before, after = r.shot("list.png"), r.shot("list_after.png")
    S.check(worst_diff(before, after, (0, 380, 960, 544)) > 0,
            "the list says `save present` for it now (the picture changed)")
    S.picture(r, "list_after.png", "extra_list_after.png" if fix else None)

    # -- a star on the extra copy of a name roms/ has too --
    fav = os.path.join(root, "roms", "favourites.txt")
    S.check(r.has(r"ui_browser_fav op=add rom=%s " % re.escape(twin)) and
            os.path.exists(fav) and open(fav).read().split("\n")[:1] == [twin],
            "the star is kept under the whole path in favourites.txt")

    # -- a second process: the star survived, and it is the extra copy's --
    shutil.rmtree(os.path.join(root, "log"), ignore_errors=True)
    os.makedirs(os.path.join(root, "log"))
    r2 = S.run("extra_roms", "60:select\n+40:shot=favs.png\n+2:cross\n"
               "+150:shot=game.png\n" + EXIT_APP, prepared=True)
    S.clean(r2)
    S.check(r2.has(r"favs_load n=1 "), "the star was read back")
    S.check(r2.has(r"^EVT ui_browser_pick rom=%s$" % re.escape(twin)),
            "the favourites view holds the extra copy, not roms/'s game of "
            "that name")
    S.check(r2.has(r"^EVT game rom=%s .*save=%s$" % (
        re.escape(twin), re.escape(os.path.join(X, "gbadhoc-test.sav")))),
        "and it starts from its folder")
    S.picture(r2, "favs.png")

    # ---- the GB leg: a Game Boy game of the folder, its .gb.sav beside it ----
    if have_gb:
        shutil.rmtree(os.path.join(root, "log"), ignore_errors=True)
        os.makedirs(os.path.join(root, "log"))
        gbn = list_oracle(os.path.join(root, "roms"), [X], ".gb")
        mono = X + "/Extra Mono.gb"
        r3 = S.run("extra_roms", "60:triangle\n+60:shot=list_gb.png\n"
                   + "+10:down\n" * [n for _, n in gbn].index(mono) +
                   "+10:cross\n+150:a:10\n+200:a:10\n" + EXIT_APP,
                   prepared=True)
        S.clean(r3)
        sw = r3.evt("ui_browser_console")
        S.check(sw and sw[0]["console"] == "1" and
                int(sw[0]["roms"]) == len(gbn),
                "TRIANGLE: the GB list holds %d games, the folder's too "
                "(log: %s)" % (len(gbn), sw[0]["_"] if sw else "-"))
        d3 = r3.evt("rom_scan_done")
        S.check(d3 and d3[-1]["crc"] == list_crc(gbn),
                "the GB list is complete and in order")
        S.check(r3.has(r"^EVT ui_browser_pick rom=%s$" % re.escape(mono)),
                "the folder's GB game was picked")
        gsav = mono + ".sav"
        S.check(os.path.exists(gsav) and os.path.getsize(gsav) > 0,
                "its battery save is Game.gb.sav beside it")
        S.check(not glob.glob(os.path.join(root, "roms", "Extra Mono*")),
                "and nothing of it is in roms/")

    # ---- many games: more than roms/ could ever list ----
    many_p, many_x = 900, 600
    names = many_games(many_p + many_x, 7)
    out = os.path.join(S.outdir, "extra_roms_many")
    root = os.path.join(out, "root")
    X = os.path.join(root, "library")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    S.make_root(root, None, None, {"extra_roms": X}, None)
    os.makedirs(X)
    for i, n in enumerate(names):
        if i < many_p:                    # roms/, some in sub-folders
            d = os.path.join(root, "roms",
                             "" if i % 10 else "Folder %d" % (i // 10 % 7))
        else:                             # the library, some in sub-folders,
            d = os.path.join(X, "" if i % 10 else "set%d" % (i // 10 % 5))
            junkp = os.path.join(d, "._" + n)         # beside each, a junk
            os.makedirs(d, exist_ok=True)
            with open(junkp, "wb") as f:
                f.write(junk)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, n), "wb") as f:
            f.write(b"\x00" * 16)
    want = list_oracle(os.path.join(root, "roms"), [X], ".gba")
    S.check(len(want) > 1024, "the fixture is larger than the old limit (%d)"
            % len(want))
    r = S.run("extra_roms_many", "90:shot=many.png\n+10:r\n+30:r\n"
              "+30:shot=many_r.png\n+10:quit\n", prepared=True)
    S.clean(r)
    b = r.evt("ui_browser")
    S.check(b and int(b[0]["roms"]) == len(want) and
            int(b[0]["found"]) == len(want),
            "%d games listed, none cut (log: %s)" %
            (len(want), b[0]["_"] if b else "no ui_browser line"))
    done = r.evt("rom_scan_done")
    S.check(done and done[0]["crc"] == list_crc(want),
            "all %d, each once, sorted without regard to case (crc %s, "
            "want %s)" % (len(want), done[0]["crc"] if done else "-",
                          list_crc(want)))
    S.check(not r.has(r"rom_scan_skip"), "no name was skipped")
    S.picture(r, "many.png")
    S.picture(r, "many_r.png")
    S.check(worst_diff(r.shot("many.png"), r.shot("many_r.png")) > 0,
            "R jumped along the list")

    # ---- more than the list holds: the cut is said, and it is the folder's ----
    over_p, over_x = 1500, 700
    names = many_games(over_p + over_x, 11)
    out = os.path.join(S.outdir, "extra_roms_full")
    root = os.path.join(out, "root")
    X = os.path.join(root, "library")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    S.make_root(root, None, None, {"extra_roms": X}, None)
    os.makedirs(X)
    for i, n in enumerate(names):
        d = os.path.join(root, "roms") if i < over_p else X
        with open(os.path.join(d, n), "wb") as f:
            f.write(b"\x00" * 16)
    prim = list_oracle(os.path.join(root, "roms"), [], ".gba")
    total = len(list_oracle(os.path.join(root, "roms"), [X], ".gba"))
    r = S.run("extra_roms_full", "40:shot=notice.png\n+200:shot=list.png\n"
              "+10:quit\n", prepared=True)
    S.clean(r)
    b = r.evt("ui_browser")
    S.check(b and int(b[0]["roms"]) == 2048 and int(b[0]["found"]) == total,
            "the list holds 2048 of %d (log: %s)" %
            (total, b[0]["_"] if b else "no ui_browser line"))
    sc = [d for d in r.evt("rom_scan_root") if d["path"] == '"%s"' % X]
    S.check(sc and int(sc[0]["listed"]) == 2048 - len(prim),
            "roms/'s %d games are all there; it is the folder that is cut "
            "(%s)" % (len(prim), sc[0]["_"] if sc else "no line"))
    S.picture(r, "notice.png", "extra_notice.png" if fix else None)
    S.picture(r, "list.png")

    # ---- which folders: vita.ini's list ----
    # (folders named "ux0:aN" under a working directory of their own: a line
    # of vita.ini is read 511 bytes at a time, and a host path is long)
    cwd = os.path.join(S.outdir, "extra_roms_cwd")
    shutil.rmtree(cwd, ignore_errors=True)
    for i in range(10):
        os.makedirs(os.path.join(cwd, "ux0:a%d" % i))
        with open(os.path.join(cwd, "ux0:a%d" % i, "g%d.gba" % i), "wb") as f:
            f.write(b"\x00" * 16)
    out = os.path.join(S.outdir, "extra_roms_ini")
    root = os.path.join(out, "root")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    d = ["ux0:a%d" % i for i in range(10)]
    ini = ("%s/roms ; %s ; %s/ ; ; relative/path ; /no/such/folder ;  %s\\ ; "
           "UX0:A0" % (root, d[0], d[0], d[1]))
    S.make_root(root, None, None, {"extra_roms": ini}, None)
    r = S.run("extra_roms_ini", "40:quit\n", prepared=True, cwd=cwd)
    S.clean(r)
    rr = r.evt("rom_roots")
    S.check(rr and rr[0]["extra"] == "2" and rr[0]["skipped"] == "5" and
            rr[0]["list"] == '"%s;%s"' % (d[0], d[1]),
            "a list with blanks, a doubled folder, an empty item, roms/ "
            "itself, a path with no drive, a missing folder and one spelt "
            "in capitals gives the two that count (%s)" %
            (rr[0]["_"] if rr else "no rom_roots line"))
    why = sorted(x["reason"] for x in r.evt("rom_roots_skip"))
    S.check(why == ["duplicate", "duplicate", "missing", "no_device",
                    "same_as_roms"],
            "and says why each of the others was left out (%s)" % why)
    b = r.evt("ui_browser")
    S.check(b and int(b[0]["found"]) == 2 + len(S.roms(root, ".gba")),
            "the two folders' games are in the list")
    # at most eight
    out2 = os.path.join(S.outdir, "extra_roms_ten")
    root2 = os.path.join(out2, "root")
    shutil.rmtree(out2, ignore_errors=True)
    os.makedirs(out2)
    S.make_root(root2, None, None, {"extra_roms": ";".join(d)}, None)
    r = S.run("extra_roms_ten", "40:quit\n", prepared=True, cwd=cwd)
    S.clean(r)
    rr = r.evt("rom_roots")
    S.check(rr and rr[0]["extra"] == "8" and rr[0]["skipped"] == "2" and
            rr[0]["list"] == '"%s"' % ";".join(d[:8]),
            "ten folders: the first eight are used (%s)" %
            (rr[0]["_"] if rr else "no rom_roots line"))
    # extra_roms = 0, and extra_roms = (nothing), with the places below there
    cwd = os.path.join(S.outdir, "extra_roms_cwd2")
    shutil.rmtree(cwd, ignore_errors=True)
    for sub in ("ux0:ROMS/GBA", "ux0:ROMS/GB", "ux0:roms/GBA",
                "ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Advance"):
        os.makedirs(os.path.join(cwd, sub))
    for sub, n in (("ux0:ROMS/GBA", "Built In One.gba"),
                   ("ux0:roms/GBA", "Same Folder On A Card.gba"),
                   ("ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Advance",
                    "Retro Flow Game.gba"),
                   ("ux0:ROMS/GB", "Built In Mono.gb")):
        with open(os.path.join(cwd, sub, n), "wb") as f:
            f.write(b"\x00" * 16)
    plain = None
    for tag, ini in (("built_in", None), ("off", {"extra_roms": "0"}),
                     ("empty", {"extra_roms": ""})):
        r = S.run("extra_roms_" + tag, "40:triangle\n+60:quit\n",
                  vita_ini=ini, cwd=cwd)
        S.clean(r)
        rr = r.evt("rom_roots")
        if tag == "built_in":
            fold = "ux0:ROMS/GBA;ux0:ROMS/GB;ux0:data/RetroFlow/ROMS/" \
                   "Nintendo - Game Boy Advance"
            S.check(rr and rr[0]["extra"] == "3" and
                    rr[0]["source"] == "builtin" and
                    rr[0]["list"] == '"%s"' % fold,
                    "no key: the built-in places that exist, in order, with "
                    "the same folder under another spelling once (%s)" %
                    (rr[0]["_"] if rr else "no rom_roots line"))
            b = r.evt("ui_browser")
            S.check(b and int(b[0]["found"]) ==
                    2 + len(S.roms(r.root, ".gba")),
                    "their games are listed (%s)" %
                    (b[0]["_"] if b else "no ui_browser line"))
            names = [d["path"] for d in r.evt("rom_scan_root")]
            S.check('"ux0:ROMS/GBA"' in names,
                    "the games are named by a device path: %s" % names[:3])
            sw = r.evt("ui_browser_console")
            S.check(sw and int(sw[0]["roms"]) == 1 + len(S.roms(r.root, ".gb")),
                    "the GB list has the built-in GB folder's game")
        else:
            S.check(rr and rr[0]["extra"] == "0" and rr[0]["source"] == "off",
                    "extra_roms = %r: no extra folder at all (%s)" %
                    (ini["extra_roms"], rr[0]["_"] if rr else "none"))
            S.check(not r.evt("rom_scan_root") and not r.evt("rom_scan_done"),
                    "and nothing more in the log than before")
    # this machine: none of the places, no key -- what a card without them
    # does (and what every other test runs)
    r = S.run("extra_roms_none", "40:quit\n")
    S.clean(r)
    rr = r.evt("rom_roots")
    S.check(rr and rr[0]["extra"] == "0" and rr[0]["source"] == "builtin" and
            not r.evt("rom_scan_root"),
            "no key and none of the places: nothing extra, nothing logged "
            "per scan")

    # ---- no game anywhere: the empty page says where else it looked ----
    cwd = os.path.join(S.outdir, "extra_roms_cwd3")
    shutil.rmtree(cwd, ignore_errors=True)
    os.makedirs(os.path.join(cwd, "ux0:e0"))
    for home in (0, 1):
        pics = {}
        for tag, ini in (("none", {"extra_roms": "0"}),
                         ("one", {"extra_roms": "ux0:e0"})):
            name = "extra_roms_empty_%s%d" % (tag, home)
            out = os.path.join(S.outdir, name)
            root = os.path.join(out, "root")
            S.make_root(root, {"ui_home": home}, None, ini, None)
            for f in glob.glob(os.path.join(root, "roms", "*")):
                os.remove(f)
            r = S.run(name, "60:shot=empty.png\n+10:quit\n", prepared=True,
                      cwd=cwd)
            S.clean(r)
            S.check(r.has(r"ui_browser console=0 roms=0 "),
                    "%s, %s: an empty list" % (tag, "home" if home else "list"))
            pics[tag] = S.picture(r, "empty.png")
        if pics["none"] and pics["one"]:
            # the line "Also looked in ux0:e0", under the page's last one
            box = (250, 478, 710, 508) if home else (250, 452, 710, 484)
            S.check(worst_diff(pics["none"], pics["one"], box) > 60,
                    "%s: with an extra folder the page says it looked there "
                    "too" % ("the home screen" if home else "the list"))
            S.check(worst_diff(pics["none"], pics["one"], (0, 90, 960, 440))
                    <= (8 if home else 0),
                    "and the rest of the page is as it was")


TESTS = [("list", t_list), ("extra_roms", t_extra_roms),
         ("home", t_home), ("preview", t_preview),
         ("gameinfo", t_gameinfo), ("title_bg", t_title_bg), ("fullspeed", t_fullspeed),
         ("upfade", t_upfade), ("render_thread", t_render_thread),
         ("render_skips", t_render_skips), ("gpu_xbrz", t_gpu_xbrz),
         ("session", t_session),
         ("ff", t_ff),
         ("suspend", t_suspend), ("controls", t_controls), ("turbo", t_turbo),
         ("cheats", t_cheats), ("play", t_play), ("touch", t_touch),
         ("typing", t_typing), ("look", t_look), ("wide", t_wide),
         ("wide_auto", t_wide_auto), ("wide_top", t_wide_top),
         ("switches", t_switches), ("bad", t_bad), ("worker", t_worker), ("cpufix", t_cpufix),
         ("realtime", t_realtime)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arch", nargs="?", default="x86")
    ap.add_argument("--only")
    ap.add_argument("--update-refs", action="store_true")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    S = Suite(a.arch, a.update_refs, a.keep)
    if not os.path.exists(S.exe):
        sys.exit("no %s: make ARCH=%s first" % (S.exe, a.arch.split("-")[0]))
    if not os.path.exists(OWN_GBA):
        sys.exit("no %s: make -C testrom first" % OWN_GBA)
    if not S.have_fixture:
        print("no fixture at %s: the GB and GBC legs are skipped" % FIXTURE)
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
        except Exception as e:                     # a test's own bug is a failure
            S.check(False, "the test itself raised %r" % (e,))
        print("    %s  (%.0f s)" % ("ok" if len(S.fails) == before else
                                    "FAILED", time.time() - t0))
    sys.exit(S.finish())


if __name__ == "__main__":
    main()
