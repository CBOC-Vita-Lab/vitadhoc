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
        # Optional monotonic metadata must not become part of a ROM filename
        # in the older exact-payload assertions. Keep the raw evidence too.
        self.raw_log = self.log
        self.log = [re.sub(r" ts_us=\d+$", "", line) for line in self.raw_log]
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
        # Existing pixel samplers and preset-cycle assertions use the legacy
        # 3x geometry. New full-height quality cases select Fit explicitly.
        # 1.3: the right stick rewinds and L+R+Select takes a screenshot only
        # when asked (Settings > Controls); the tests that use either say so
        # here, and t_controls_defaults hands None to see the real defaults.
        cfg = {"last_rom": "gbadhoc-test.gba", "hints": 0, "wide_gba": 0,
               "ui_home": 0, "scale_gba": 4, "scale_gb": 4, "scale_gbc": 4,
               "stick_rewind": 1, "bind_screenshot": "L+R+SELECT",
               "ambient_gba": 0, "ambient_gb": 0, "ambient_gbc": 0}
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
            args=(), extra=None, prepared=None, cwd=None, env=None):
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
                               timeout=1500,
                               env=dict(os.environ, TZ="UTC", **(env or {})),
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
def sidecar_hash(folder):
    """psp/sidecar.h sidecar_dir_hash: FNV-1a of the folder's path, 24 bits."""
    h = 2166136261
    for b in folder.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return (h ^ (h >> 24)) & 0xFFFFFF


def only_beside(folder, stem, allowed):
    """The names in `folder` that belong to the game `stem` (its file name
    without the extension, then a dot or end), minus the `allowed` ones."""
    return sorted(n for n in os.listdir(folder)
                  if (n == stem or n.startswith(stem + ".")) and n not in allowed)


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
    stem = os.path.join(r.root, "states", "gba", "gbadhoc-test")
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
    sav = os.path.join(r.root, "roms", "gbadhoc-test.sav")   # beside the ROM (1.3)
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


def t_save_fail_shown(S):
    """A battery save that cannot be written (here its temporary file's name
    is taken by a folder) is shown to the player, not only logged."""
    if not os.path.exists(OWN_GB):
        S.note("no testrom/gbsave.gb: skipped")
        return
    r = S.run("save_fail_shown", "100:a:10\n+100:shot=toast.png\n+150:quit\n",
              autoboot="gbsave.gb",
              extra={"roms/gbsave.gb.sav.tmp/keep": b"x"})
    S.check(r.rc == 0, "ran (rc %s)" % r.rc)
    S.check(r.has(r"gb_sram_flush size=\d+ FAILED"), "the write failed")
    S.check(r.has(r"^EVT save_failed_shown count=1"),
            "the player is told the save was not written")
    S.picture(r, "toast.png")


def t_ambient_bars(S):
    """Fit with each ambient mode: the bars beside the picture are drawn
    (the console's palette, or the game's own picture blurred) and the
    picture is whole -- the frame is not cleared first when they cover the
    screen anyway."""
    for mode in (0, 1, 2):
        r = S.run("ambient_%d" % mode, "200:shot=game.png\n+10:quit\n",
                  autoboot="gbadhoc-test.gba",
                  config={"scale_gba": 1, "ambient_gba": mode})
        S.clean(r)
        im = S.picture(r, "game.png")
        if im:
            bar = [im.getpixel((x, 272)) for x in (8, 40, 920, 950)]
            S.check((mode == 0) == all(p == (0, 0, 0) for p in bar),
                    "ambient %d: the bars are %s (%s)" %
                    (mode, "black" if mode == 0 else "painted", bar))


def sensor_bars(im):
    """The four bars sensors.c draws (tilt X/8, tilt Y/8, gyro/8, light):
    their lengths in game pixels, read from the picture at 3x."""
    g = game_pixels(im)
    out = []
    for y in (25, 45, 65, 85):
        n = 0
        while n < 240 and sum(g.getpixel((n, y))) > 120:
            n += 1
        out.append(n)
    return out


def t_sensors(S):
    """Sensor cartridges (cart_sensor.h, fe_sensor.h): a tilt, a gyro and a
    light-sensor game read the Vita's motion sensor, the right stick and the
    Sunlight setting, through the cartridge's own protocol (sensors.c reads
    them as GBATEK's routines do and draws the values)."""
    rom = lambda name: open(os.path.join(HERE, "testrom", name), "rb").read()
    cfg = {"scale_gba": 4, "quality_gba": 1}

    def run(name, game, script, extra=None, args=()):
        ex = {"roms/" + game: rom(game)}
        ex.update(extra or {})
        r = S.run("sensors_" + name, script, autoboot=game, config=cfg,
                  extra=ex, args=args)
        S.clean(r)
        return r

    near = lambda got, want, tol=1: abs(got - want) <= tol

    # TILT (Yoshi Topsy-Turvy's).  The first reading is the level; the right
    # side down half a g lowers X, the top edge down raises Y (GBATEK's
    # ranges, fe_sensor.h).
    r = run("tilt", "gbadhoc-tilt.gba",
            "1:motion=0.2/0.5/0\n200:shot=level.png\n"
            "210:motion=0.7/0.5/0\n320:shot=right.png\n"
            "330:motion=0.2/0.0/0\n440:shot=top.png\n450:quit\n")
    S.check(r.has(r"^EVT sensor_cart kinds=4"), "KYGE is a tilt cartridge")
    S.check(r.has(r"^EVT sensor_begin kinds=4 motion=1 mode=0"),
            "the Vita's motion sensor is read")
    b = [sensor_bars(r.shot(n)) for n in ("level.png", "right.png", "top.png")]
    S.check(near(b[0][0], 0x392 // 8) and near(b[0][1], 0x3A0 // 8),
            "held as at the start reads level (X %d, Y %d px)" % tuple(b[0][:2]))
    S.check(near(b[1][0], (0x392 - 112) // 8) and near(b[1][1], 0x3A0 // 8),
            "right side down half a g: X lower (%d px)" % b[1][0])
    S.check(near(b[2][1], (0x3A0 + 112) // 8) and near(b[2][0], 0x392 // 8),
            "top edge down half a g: Y higher (%d px)" % b[2][1])

    # Settings > THIS GAME, in a tilt game: wrapping upwards, Button
    # mapping, then Level -- X makes the way the Vita is held now level, and
    # the game's file keeps it.
    r = run("level", "gbadhoc-tilt.gba",
            "1:motion=0.0/0.0/0\n100:motion=0.3/0.6/0\n" +
            menu_pick(4, wait=60) + "+20:up\n+8:up\n+8:up\n+8:up\n+8:up\n+10:shot=level_row.png\n"
            "+4:cross\n+20:up\n+20:shot=rows.png\n+4:circle\n+10:circle\n"
            "+120:shot=level.png\n+10:quit\n")
    S.picture(r, "rows.png")
    S.check(r.has(r"^EVT sensor_level right=300 down=600"),
            "Level: the hold now (%s)" % [e["_"] for e in r.evt("sensor_level")])
    g = open(os.path.join(r.root, "games", "gbadhoc-tilt.gba.ini")).read()
    S.check(re.search(r"^level_set = 1$", g, re.M) and
            re.search(r"^level_right = 300$", g, re.M) and
            re.search(r"^level_down = 600$", g, re.M),
            "the game's file keeps the level")
    b = sensor_bars(r.shot("level.png"))
    S.check(near(b[0], 0x392 // 8) and near(b[1], 0x3A0 // 8),
            "held that way, the game reads level (%d, %d px)" % tuple(b[:2]))

    # GYRO (WarioWare: Twisted!'s): clockwise raises the reading, about 55
    # units per rad/s; still reads 0x6C0.
    r = run("gyro", "gbadhoc-gyro.gba",
            "1:motion=0/0/0\n150:shot=still.png\n160:motion=0/0/-3\n"
            "260:shot=cw.png\n270:motion=0/0/2\n370:shot=acw.png\n380:quit\n")
    S.check(r.has(r"^EVT sensor_cart kinds=2"), "RZWE is a gyro cartridge")
    b = [sensor_bars(r.shot(n)) for n in ("still.png", "cw.png", "acw.png")]
    S.check(near(b[0][2], 0x6C0 // 8), "still: 0x6C0 (%d px)" % b[0][2])
    S.check(near(b[1][2], (0x6C0 + 165) // 8), "clockwise 3 rad/s (%d px)" % b[1][2])
    S.check(near(b[2][2], (0x6C0 - 110) // 8), "anticlockwise 2 rad/s (%d px)" % b[2][2])

    # LIGHT (Boktai's): the Sunlight setting, 6 of 10 by default; a game's
    # own file can say full sun.  Darker counts further (sensors.c).
    r = run("solar", "gbadhoc-solar.gba", "150:shot=sun6.png\n160:quit\n")
    S.check(r.has(r"^EVT sensor_cart kinds=1") and
            r.has(r"^EVT sensor_begin kinds=1 motion=0 .* sun=6"),
            "U3IE is a light-sensor cartridge, no motion sampling")
    sun6 = sensor_bars(r.shot("sun6.png"))[3]
    S.check(near(sun6, 0xAA), "sunlight 6 counts to 0xAA (%d)" % sun6)
    r = run("solar_full", "gbadhoc-solar.gba", "150:shot=sun10.png\n160:quit\n",
            extra={"games/gbadhoc-solar.gba.ini": "sunlight=10\n"})
    sun10 = sensor_bars(r.shot("sun10.png"))[3]
    S.check(r.has(r"^EVT game_settings load=1 .* sun=10") and near(sun10, 0x31),
            "the game's own file: full sun counts to 0x31 (%d)" % sun10)

    # THE RIGHT STICK as the sensor: chosen in the game's settings, and the
    # stick is no longer fast forward (held right) while it is.
    r = run("stick", "gbadhoc-tilt.gba",
            "1:motion=0/0/0\n100:rstick-right:200\n250:shot=stick.png\n"
            "320:quit\n",
            extra={"games/gbadhoc-tilt.gba.ini": "sensor_mode=1\n"})
    x = sensor_bars(r.shot("stick.png"))[0]
    S.check(r.has(r"^EVT sensor_begin kinds=4 motion=1 mode=1") and
            near(x, (0x392 - 225) // 8), "stick right: the right side down (%d px)" % x)
    S.check(not r.has(r"^EVT ff on"),
            "the stick held right did not fast-forward")

    # A PS TV has no motion sensor: the stick, whatever the setting says.
    r = run("pstv", "gbadhoc-tilt.gba",
            "100:rstick-left:200\n250:shot=stick.png\n320:quit\n",
            args=("--no-motion",))
    x = sensor_bars(r.shot("stick.png"))[0]
    S.check(r.has(r"^EVT sensor_begin kinds=4 motion=-1") and
            near(x, (0x392 + 225) // 8),
            "no motion sensor: the stick tilts instead (%d px)" % x)

    # Reversed and stronger, from the game's file.
    r = run("reverse", "gbadhoc-gyro.gba",
            "1:motion=0/0/-2\n150:shot=rev.png\n160:quit\n",
            extra={"games/gbadhoc-gyro.gba.ini":
                   "sensor_reverse=1\nsensor_sens=2\n"})
    g = sensor_bars(r.shot("rev.png"))[2]
    S.check(near(g, (0x6C0 - round(2 * 1.6 * 55)) // 8),
            "reversed and high: a clockwise turn reads anticlockwise, 1.6x (%d px)" % g)

    # REWIND gives a frame back the reading it had: going back past the
    # moment the turn began shows the still readings again, though the Vita
    # is still turning; let go, the game goes on with the turn.
    r = S.run("sensors_rewind", "1:motion=0/0/0\n300:motion=0/0/-3\n"
              "400:shot=turning.png\n450:rstick-left:130\n"
              "570:shot=back.png\n760:shot=after.png\n780:quit\n",
              autoboot="gbadhoc-gyro.gba",
              config=dict(cfg, rewind=60),
              extra={"roms/gbadhoc-gyro.gba": rom("gbadhoc-gyro.gba")})
    S.clean(r)
    b = [sensor_bars(r.shot(n))[2] for n in ("turning.png", "back.png", "after.png")]
    S.check(r.has(r"^EVT rewind (start|begin|on)") or r.has(r"^EVT rewind end"),
            "the rewind ran")
    S.check(near(b[0], (0x6C0 + 165) // 8) and near(b[1], 0x6C0 // 8) and
            near(b[2], (0x6C0 + 165) // 8),
            "turning %d px, rewound to before the turn %d px, after %d px"
            % tuple(b))
    S.check(not r.has(r"rewind .*(anomal|distrust|mismatch)"),
            "the rewind found its re-run frames as they were")

    # An ordinary game has no sensor and samples nothing -- and there the
    # stick held right is fast forward, as before.
    r = S.run("sensors_none", "100:rstick-right:200\n320:quit\n",
              autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.check(not r.has(r"^EVT sensor_") and not r.has(r"^EVT motion "),
            "no sensor game, no sensor")
    S.check(r.has(r"^EVT ff on"), "the stick held right fast-forwards there")


def t_game_look(S):
    """Settings > THIS GAME > Display settings: a game can have a look of its
    own (games/<ROM>.ini, config_psp.h pcfg_game).  Turned on, it starts from
    what is on screen; changes then reach only that game's file -- the
    console's profile in config.ini stays -- and the game starts in its own
    look next time; turned off, the console's look comes back."""
    cfg = {"scale_gba": 1, "quality_gba": 1}
    gini = lambda r: open(os.path.join(r.root, "games",
                                       "gbadhoc-test.gba.ini")).read() \
        if os.path.exists(os.path.join(r.root, "games",
                                       "gbadhoc-test.gba.ini")) else ""
    # Settings wraps upwards to Button mapping, then THIS GAME's Display
    # settings; CROSS gives the game its own look.  Then TRIANGLE (the
    # size preset) makes it stretch: the game's file, not config.ini.
    r = S.run("game_look_on", menu_pick(4, wait=60) +
              "+20:up\n+8:up\n+8:up\n+8:up\n+8:up\n+10:shot=row.png\n+4:cross\n"
              "+10:shot=row_own.png\n+4:circle\n+10:circle\n+40:triangle\n"
              "+60:shot=stretch.png\n+10:quit\n",
              autoboot="gbadhoc-test.gba", config=cfg)
    S.clean(r)
    S.picture(r, "row_own.png")
    S.check(r.has(r"^EVT game_settings load=0 override=0"),
            "a game without a file starts with the console's look")
    S.check(r.has(r"^EVT game_look own=1 scale=fit quality=1"),
            "Display settings: just this game, from the look on screen")
    S.check(r.has(r"^EVT video_mode scale=stretch"), "the preset made it stretch")
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check(re.search(r"^scale_gba = 1$", ini, re.M) and
            re.search(r"^quality_gba = 1$", ini, re.M),
            "config.ini keeps the console's fit")
    g = gini(r)
    S.check(re.search(r"^override = 1$", g, re.M) and
            re.search(r"^scale = 2$", g, re.M) and
            re.search(r"^quality = 1$", g, re.M),
            "games/gbadhoc-test.gba.ini has the game's own stretch (%r)" % g[:200])

    # Next time it starts in its own look; turning it off brings the
    # console's back, and the next start is the console's.
    r = S.run("game_look_off", menu_pick(4, wait=60) +
              "+20:up\n+8:up\n+8:up\n+8:up\n+8:up\n+4:cross\n+4:circle\n+10:circle\n+30:quit\n",
              autoboot="gbadhoc-test.gba", config=cfg,
              extra={"games/gbadhoc-test.gba.ini":
                     "override = 1\nscale = 2\nquality = 1\nfilter = 4\n"})
    S.clean(r)
    S.check(r.has(r"^EVT game_settings load=1 override=1") and
            r.has(r"^EVT display_profile console=GBA scale=stretch"),
            "the game starts in its own stretch")
    S.check(r.has(r"^EVT game_look own=0 scale=fit"),
            "turned off: the console's fit is back")
    S.check(re.search(r"^override = 0$", gini(r), re.M),
            "the file says so")
    r = S.run("game_look_other", "100:quit\n", autoboot="gbadhoc-wide.gba",
              config=cfg, extra={
                  "roms/gbadhoc-wide.gba": open(os.path.join(HERE, "testrom",
                                                "gbadhoc-wide.gba"), "rb").read(),
                  "games/gbadhoc-test.gba.ini": "override = 1\nscale = 2\n"})
    S.clean(r)
    S.check(r.has(r"^EVT display_profile console=GBA scale=fit"),
            "another game keeps the console's look")


def t_recent(S):
    """Play time and Recent (play_log.h): a session is counted when a game
    starts and the time played (not the menus) is added; the game page says
    when and how long; SELECT reaches the Recent tab once something has been
    played, latest first; Settings > Game order sorts the list by last played
    or most played; L/R then page plainly."""
    home = {"ui_home": 1, "last_rom": "gbadhoc-test.gba"}
    # Ten seconds in the game, a menu that stays open for five, the list.
    r = S.run("recent_play", "60:cross\n+600:start+select:20\n+300:circle\n"
              "+60:start+select:20\n+14:down\n+8:down\n+8:down\n+8:down\n"
              "+8:down\n+8:cross\n+8:cross\n+150:shot=home.png\n"
              "+10:select\n+40:select\n+60:shot=recent.png\n+10:quit\n",
              config=home)
    S.clean(r)
    log = os.path.join(r.root, "playlog.txt")
    lines = open(log).read().splitlines() if os.path.exists(log) else []
    rec = [l.split("\t") for l in lines]
    S.check(len(rec) == 1 and rec[0][3] == "gbadhoc-test.gba" and
            rec[0][2] == "1" and 9 <= int(rec[0][1]) <= 12,
            "playlog.txt: one start and about ten seconds, not the menu's five (%r)"
            % lines)
    S.check(r.has(r"^EVT play_end secs=(9|10|11|12) count=1"), "the session's end is logged")
    S.check(r.has(r"^EVT ui_browser_view favs=1 shown=0$") and
            r.has(r"^EVT ui_browser_view favs=0 shown=1 recent=1"),
            "SELECT: Starred, then Recent with the game in it")
    S.picture(r, "home.png")
    S.picture(r, "recent.png")

    # Nothing played: SELECT goes Games -> Starred -> Games, as before.
    r = S.run("recent_none", "60:select\n+40:select\n+40:quit\n", config=home)
    S.clean(r)
    S.check(r.has(r"^EVT ui_browser_view favs=0 shown=1$") and
            not r.has(r"recent=1"), "no Recent tab before anything was played")

    # The orders.  Three copies of the test cartridge with a play log: aa
    # played long ago for an hour, mm yesterday for a minute, zz today for
    # ten minutes.
    rom = open(OWN_GBA, "rb").read()
    now = 1700000000
    log = ("%d\t3600\t5\taa.gba\n%d\t60\t1\tmm.gba\n%d\t600\t2\tzz.gba\n"
           % (now - 86400 * 40, now - 86400, now - 100))
    extra = {"roms/aa.gba": rom, "roms/mm.gba": rom, "roms/zz.gba": rom,
             "playlog.txt": log}
    base = {"ui_home": 1, "last_rom": None}
    picks = {}
    for name, cfg, script in (
            ("name", {}, "60:cross\n+30:quit\n"),
            ("last", {"lib_sort": 1}, "60:cross\n+30:quit\n"),
            ("most", {"lib_sort": 2}, "60:cross\n+30:quit\n"),
            ("recent_tab", {}, "60:select\n+40:select\n+40:right\n+20:cross\n+30:quit\n")):
        r = S.run("recent_order_" + name, script, config=dict(base, **cfg),
                  extra=extra)
        S.clean(r)
        p = r.evt("ui_browser_pick")
        picks[name] = p[0]["rom"] if p else None
    S.check(picks == {"name": "aa.gba", "last": "zz.gba", "most": "aa.gba",
                      "recent_tab": "mm.gba"},
            "first game: by name aa, last played zz, most played aa; Recent's "
            "second is mm (%s)" % picks)

    # Settings > LIBRARY > Game order, from the game list: the browser's
    # Settings, wrapping upwards from the top: Button mapping, then Game
    # order (LIBRARY, between GAMEPLAY and CONTROLS).
    r = S.run("recent_setting", "60:start\n+30:up\n+8:up\n+8:up\n+8:up\n+8:up\n+10:shot=row.png\n"
              "+4:right\n+10:shot=row_last.png\n+4:circle\n+60:left\n+30:cross\n"
              "+30:quit\n", config=base, extra=extra)
    S.clean(r)
    S.picture(r, "row_last.png")
    S.check(r.has(r"^EVT lib_sort order=1") and r.has(r"^EVT lib_order order=1 shown=4"),
            "Game order: last played first, the list sorted again")
    p = r.evt("ui_browser_pick")
    S.check(p and p[0]["rom"] == "mm.gba",
            "...aa keeps the focus, and the game before it is now mm (%s)"
            % (p[0]["rom"] if p else None))
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check(re.search(r"^lib_sort = 1$", ini, re.M), "config.ini keeps lib_sort = 1")


def t_resume(S):
    """Where you left off (main_vita.c resume_*, state_slots.h): leaving a
    game keeps its moment (Game.rsm); the Home page then offers Continue,
    which puts the game back there, and Start over (UP), which plays from the
    battery save.  An in-game save makes a new point half a second later, so
    a console closed from LiveArea (no notice) continues after it."""
    home = {"ui_home": 1, "last_rom": "gbadhoc-test.gba"}
    rsm = lambda r: os.path.join(r.root, "states", "gba", "gbadhoc-test.rsm")
    # Play, leave to the list: the point is written; the page says Continue.
    r = S.run("resume_leave", "60:cross\n+300:right:60\n+60:shot=left.png\n" +
              QUIT_TO_LIST + "+150:shot=home.png\n+10:quit\n", config=home)
    S.clean(r)
    S.check(r.has(r"^EVT resume_begin on=1") and
            r.has(r"^EVT resume_take why=leave size=\d+") and
            r.has(r"^EVT resume_write size=\d+ rc=0"),
            "leaving the game writes where it was left")
    S.check(os.path.exists(rsm(r)) and os.path.getsize(rsm(r)) > 100000 and
            not os.path.exists(rsm(r) + ".tmp") and
            not os.path.exists(rsm(r) + ".old"),
            "gbadhoc-test.rsm is there, alone")
    S.picture(r, "home.png")
    # the test cartridge draws its frame counter in binary (main.c)
    frame_of = lambda im: sum(1 << (15 - i) for i in range(16)
                              if sum(game_pixels(im).getpixel((13 + i * 14, 105))) > 500)
    f_left = frame_of(r.shot("left.png"))

    # Continue: the same moment comes back.
    root = r.root
    def again(name, script):
        out = os.path.join(S.outdir, name)
        shutil.rmtree(out, ignore_errors=True)
        shutil.copytree(root, os.path.join(out, "root"))
        for f in glob.glob(os.path.join(out, "root", "log", "*")):
            os.remove(f)
        return S.run(name, script, prepared=True)
    r = again("resume_continue", "60:cross\n+20:shot=back.png\n+10:quit\n")
    S.clean(r)
    S.check(r.has(r"^EVT ui_browser_pick rom=gbadhoc-test.gba continue=1") and
            r.has(r"^EVT state_load path=.*gbadhoc-test.rsm via=continue rc=0"),
            "CROSS continues: the point is loaded")
    f_back = frame_of(r.shot("back.png"))
    S.check(f_left <= f_back <= f_left + 150,
            "the game goes on from where it was left: its frame %d, %d when "
            "it was last seen" % (f_back, f_left))
    r = again("resume_fresh", "60:up\n+20:shot=fresh.png\n+10:quit\n")
    S.clean(r)
    f_fresh = frame_of(r.shot("fresh.png"))
    S.check(r.has(r"^EVT ui_browser_pick rom=gbadhoc-test.gba$") and
            not r.has(r"^EVT state_load") and f_fresh < 100,
            "UP starts over, from the battery save (frame %d)" % f_fresh)

    # An in-game save (A writes the battery), then the console closed with
    # no notice: the point taken after the save is on the card.
    r = S.run("resume_save", "60:cross\n+200:a:5\n+120:kill\n", config=home)
    S.check(r.has(r"^EVT resume_take why=save") and
            r.has(r"^EVT resume_write size=\d+ rc=0") and
            os.path.exists(rsm(r)),
            "an in-game save makes a new point at once, before any leaving")
    S.check(not r.has(r"^EVT resume_take why=leave"),
            "(the process ended without leaving)")


def ups_varint(v):
    out = bytearray()
    while True:
        x = v & 0x7F
        v >>= 7
        if not v:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        v -= 1


def t_patch(S):
    """ROM hacks and translations (rom_patch.h, main_vita.c patch_prepare):
    Game.ips / .bps / .ups beside the game is applied as it starts -- the
    cores load a patched copy in patched/, made once and reused; the game's
    own file is untouched, its saves keep its name; a patch for another
    version of the game is refused and the game starts unpatched."""
    import zlib
    rom = open(OWN_GBA, "rb").read()
    home = {"ui_home": 1, "last_rom": "gbadhoc-test.gba"}
    # IPS: the game code AGDE -> PTCH, and a run of 0xFFFF bytes of 0x77
    # at 0x10000, which makes the cartridge grow.
    ips = b"PATCH" + bytes([0, 0, 0xAC, 0, 4]) + b"PTCH" + \
        bytes([0x01, 0x00, 0x00, 0, 0, 0xFF, 0xFF, 0x77]) + b"EOF"
    want = bytearray(rom) + bytearray(max(0, 0x1FFFF - len(rom)))
    want[0xAC:0xB0] = b"PTCH"
    want[0x10000:0x1FFFF] = b"\x77" * 0xFFFF
    r = S.run("patch_ips", "60:cross\n+120:shot=game.png\n+10:quit\n" +
              "", config=home, extra={"roms/gbadhoc-test.ips": ips})
    S.clean(r)
    S.check(r.has(r"^EVT rom_patch file=.*gbadhoc-test.ips kind=1 rc=1 size=%d cached=0"
                  % len(want)), "the IPS patch is applied (%s)"
            % [e["_"] for e in r.evt("rom_patch")])
    S.check(r.has(r"^EVT rom_loaded code=PTCH size=%d" % len(want)),
            "the core runs the patched cartridge")
    pf = os.path.join(r.root, "patched", "gbadhoc-test.gba")
    S.check(os.path.exists(pf) and open(pf, "rb").read() == bytes(want),
            "patched/gbadhoc-test.gba is the result, byte for byte")
    S.check(open(os.path.join(r.root, "roms", "gbadhoc-test.gba"), "rb").read() == rom,
            "the game's own file is untouched")
    S.check(r.has(r"^EVT game rom=.*/roms/gbadhoc-test.gba .* save=.*/roms/gbadhoc-test.sav"),
            "the battery save keeps the game's name")
    # The page says so; the second start reuses the copy.
    root = r.root
    out = os.path.join(S.outdir, "patch_again")
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(root, os.path.join(out, "root"))
    for f in glob.glob(os.path.join(out, "root", "log", "*")):
        os.remove(f)
    r = S.run("patch_again", "100:shot=home.png\n+10:up\n+60:quit\n", prepared=True)
    S.clean(r)
    S.picture(r, "home.png")
    S.check(r.has(r"^EVT rom_patch .* rc=1 .* cached=1"), "made once, reused after")

    # BPS: checks the game it is applied to.
    def crc(b):
        return (zlib.crc32(b) & 0xFFFFFFFF).to_bytes(4, "little")
    target = bytearray(rom)
    target[0xAC:0xB0] = b"BPST"
    body = b"BPS1" + ups_varint(len(rom)) + ups_varint(len(target)) + ups_varint(0)
    body += ups_varint(((0xAC - 1) << 2) | 0)                   # SourceRead
    body += ups_varint(((4 - 1) << 2) | 1) + b"BPST"            # TargetRead
    # SourceCopy: its offset is relative to its own cursor, which SourceRead
    # does not move -- from 0 to 0xB0
    body += ups_varint(((len(rom) - 0xB0 - 1) << 2) | 2) + ups_varint(0xB0 << 1)
    body += crc(rom) + crc(bytes(target))
    bps = body + crc(body)
    r = S.run("patch_bps", "60:cross\n+60:quit\n", config=home,
              extra={"roms/gbadhoc-test.bps": bps})
    S.clean(r)
    S.check(r.has(r"^EVT rom_patch .*kind=2 rc=1") and
            r.has(r"^EVT rom_loaded code=BPST size=%d" % len(rom)),
            "the BPS patch is applied")
    other = bytearray(rom)
    other[0x200] ^= 1                    # another release of the game
    body2 = body[:-8] + crc(bytes(other)) + crc(bytes(target))
    r = S.run("patch_wrong", "60:cross\n+20:shot=game.png\n+60:quit\n", config=home,
              extra={"roms/gbadhoc-test.bps": body2 + crc(body2)})
    S.clean(r)
    S.check(r.has(r'^EVT rom_patch .*kind=2 rc=-1 .*why="made for another version of the game"') and
            r.has(r"^EVT rom_loaded code=AGDE size=%d" % len(rom)),
            "a patch for another version: refused, the game starts as it is")
    S.picture(r, "game.png")

    # UPS: XOR hunks.
    t2 = bytearray(rom)
    t2[0xAC:0xB0] = b"UPST"
    hunks = ups_varint(0xAC) + bytes(a ^ b for a, b in zip(rom[0xAC:0xB0], b"UPST")) + b"\0"
    body = b"UPS1" + ups_varint(len(rom)) + ups_varint(len(t2)) + hunks + \
        crc(rom) + crc(bytes(t2))
    r = S.run("patch_ups", "60:cross\n+60:quit\n", config=home,
              extra={"roms/gbadhoc-test.ups": body + crc(body)})
    S.clean(r)
    S.check(r.has(r"^EVT rom_patch .*kind=3 rc=1") and
            r.has(r"^EVT rom_loaded code=UPST"), "the UPS patch is applied")

    # Two games with one name (Game.gb beside Game.gba): Game.ips could be
    # either's, so it is not applied; Game.gba.ips names its game.
    gb = open(OWN_GB, "rb").read() if os.path.exists(OWN_GB) else b"\0" * 0x8000
    r = S.run("patch_shared", "60:cross\n+60:quit\n", config=home,
              extra={"roms/gbadhoc-test.ips": ips, "roms/gbadhoc-test.gb": gb})
    S.clean(r)
    S.check(r.has(r'^EVT rom_patch .*rc=-1 .*why="two games have this name') and
            r.has(r"^EVT rom_loaded code=AGDE"),
            "Game.ips beside Game.gb and Game.gba: not applied to either")
    r = S.run("patch_whole_name", "60:cross\n+60:quit\n", config=home,
              extra={"roms/gbadhoc-test.gba.ips": ips, "roms/gbadhoc-test.gb": gb})
    S.clean(r)
    S.check(r.has(r"^EVT rom_patch file=.*gbadhoc-test.gba.ips kind=1 rc=1") and
            r.has(r"^EVT rom_loaded code=PTCH"), "Game.gba.ips: applied")

    # Continue keeps which patch the moment was made with: a point made
    # patched is not loaded once the patch has gone.
    r = S.run("patch_leave", "60:cross\n+120:right:20\n" + QUIT_TO_LIST +
              "+100:quit\n", config=home, extra={"roms/gbadhoc-test.ips": ips})
    S.clean(r)
    out = os.path.join(S.outdir, "patch_removed")
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(r.root, os.path.join(out, "root"))
    for f in glob.glob(os.path.join(out, "root", "log", "*")):
        os.remove(f)
    os.remove(os.path.join(out, "root", "roms", "gbadhoc-test.ips"))
    pt = os.path.join(out, "root", "states", "gba", "gbadhoc-test.rsm.pt")
    S.check(os.path.exists(pt) and open(pt).read() != "-\n",
            "the Continue point records its patch")
    r = S.run("patch_removed", "60:cross\n+60:quit\n", prepared=True)
    S.clean(r)
    S.check(r.has(r"^EVT state_load via=continue refused=patch_changed") and
            not r.has(r"^EVT state_load path="),
            "the patch removed: Continue starts from the save instead")


def t_sgb(S):
    """The Super Game Boy's border (gbcore_sgb_border, main_vita.c
    sgb_compose): a game that sends one (CHR_TRN, PCT_TRN: testrom/
    sgbborder.py) is shown inside it, its tiles and colours exactly; the
    border is kept for the next start, so a game that has not sent it yet
    (or a state just loaded) has it from the first frame; Settings > SGB
    border off is the picture alone."""
    sys.path.insert(0, os.path.join(HERE, "testrom"))
    import sgbborder
    cfg = {"scale_gb": 0, "quality_gb": 1, "gb_palette_gb": 0, "gb_palette": 0}
    rom = sgbborder.rom()

    def at(im, x, y):
        # 256x224 at 2x, centred: (224, 48)
        return im.getpixel((224 + 2 * x + 1, 48 + 2 * y + 1))

    def want(x, y):
        v = sgbborder.border_pixel(x, y)
        return tuple(((c << 3) | (c >> 2)) for c in (v & 31, (v >> 5) & 31,
                                                     (v >> 10) & 31))

    def check_border(im, what):
        bad = n = 0
        for y in range(4, 224, 9):
            for x in range(3, 256, 7):
                if 48 <= x < 208 and 40 <= y < 184:
                    continue           # the game's own picture
                n += 1
                g, w = at(im, x, y), want(x, y)
                if max(abs(a - b) for a, b in zip(g, w)) > 10:
                    bad += 1
        S.check(n > 300 and bad == 0, "%s: %d of %d border pixels are the "
                "border's tiles in its colours (%d are not)" % (what, n - bad, n, bad))

    r = S.run("sgb_border", "400:shot=border.png\n+10:quit\n",
              autoboot="sgb.gb", config=cfg, extra={"roms/sgb.gb": rom})
    S.clean(r)
    S.check(r.has(r"^EVT sgb_border shown gen=") and
            r.has(r"^EVT sgb_border_save bytes=10372 ok=1"),
            "the border the game sent is shown, and kept")
    check_border(r.shot("border.png"), "sent")
    S.picture(r, "border.png")
    root = r.root

    # The same cartridge, silent: the kept border from the first frame.
    out = os.path.join(S.outdir, "sgb_kept")
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(root, os.path.join(out, "root"))
    for f in glob.glob(os.path.join(out, "root", "log", "*")):
        os.remove(f)
    open(os.path.join(out, "root", "roms", "sgb.gb"), "wb").write(sgbborder.rom(True))
    open(os.path.join(out, "root", "autoboot.txt"), "w").write("sgb.gb\n")
    r = S.run("sgb_kept", "30:shot=early.png\n+10:quit\n", prepared=True)
    S.clean(r)
    S.check(r.has(r"^EVT sgb_border_load bytes=10372 rc=0") and
            r.has(r"^EVT sgb_border shown"), "a later start shows it at once")
    check_border(r.shot("early.png"), "kept")

    # The upscalers and the other sizes take the 256x224 picture as any.
    for name, extra_cfg in (("sgb_fit_2x", {"scale_gb": 1, "quality_gb": 2}),
                            ("sgb_fit_3x", {"scale_gb": 1, "quality_gb": 3}),
                            ("sgb_stretch", {"scale_gb": 2, "quality_gb": 0})):
        r = S.run(name, "400:shot=game.png\n+10:quit\n", autoboot="sgb.gb",
                  config=dict(cfg, **extra_cfg), extra={"roms/sgb.gb": rom})
        S.clean(r)
        S.check(r.has(r"^EVT sgb_border shown"), "%s: the border is shown" % name)
        S.picture(r, "game.png")

    # Off: the picture alone, at its own 2x.
    r = S.run("sgb_off", "400:shot=off.png\n+10:quit\n", autoboot="sgb.gb",
              config=dict(cfg, sgb_border_gb=0), extra={"roms/sgb.gb": rom})
    S.clean(r)
    im = r.shot("off.png")
    S.check(not r.has(r"^EVT sgb_border shown") and im.getpixel((230, 60)) == (0, 0, 0),
            "SGB border off: no border, black where it would be")


def t_vita_ini_once(S):
    """vita.ini is read once at boot and its keys answered from memory, with
    the same values as before."""
    r = S.run("vita_ini_once", "60:quit\n", vita_ini={"fade_drop": 1, "up_slide": 1})
    S.clean(r)
    b = r.evt("vita_ini")
    S.check(b and b[0].get("bound") == "1" and int(b[0].get("hits", "0")) >= 30,
            "the boot switches and extra folders come from one read (%s)" %
            (b[0]["_"] if b else "no vita_ini line"))
    S.check(r.has(r"^EVT vita_ini link bound=1 hits=\d+"), "the link's settings too")
    S.check(r.has(r"^EVT switches2 .* up_slide=1 .* fade_drop=1 "), "values set in vita.ini still apply")


def t_gb_save_soon(S):
    """A Game Boy save is on the card within about two seconds of being made:
    the Vita gives no warning before the app is closed from LiveArea, so the
    process is killed 2.5 s after the save, with no exit path."""
    if not os.path.exists(OWN_GB):
        S.note("no testrom/gbsave.gb: skipped")
        return
    for name, args in (("gb_save_soon", ()), ("gb_save_soon_worker", ("--worker",))):
        r = S.run(name, "100:a:10\n+150:kill\n", autoboot="gbsave.gb", args=args)
        p = os.path.join(r.root, "roms", "gbsave.gb.sav")
        S.check(os.path.exists(p) and os.path.getsize(p) > 0 and
                any(open(p, "rb").read()),
                "%s: the save made 2.5 s before the app was killed is on the card" % name)


def t_cpufix(S):
    """The emulator fixes from gpSP's September 2026 commits, on this
    build's dynarec (testrom/cpufix.c): code run from VRAM and rewritten
    there, I/O registers reading back as the hardware's do, and the guest's
    flags kept across the ARM load stub's I/O paths.  And two for packed
    start-up code (an intro added to Pokemon Pinball: Ruby & Sapphire): an
    MSR that sets the T bit continues in Thumb, and code runs from palette
    RAM and OAM.  The cartridge writes one result byte per test to its
    battery RAM."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-cpufix.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-cpufix.gba (make -C testrom cpufix): skipped")
        return
    names = ["VRAM code (Thumb), rewritten", "VRAM code (ARM), rewritten",
             "BG0CNT read mask", "WININ read mask",
             "invalid I/O 0x136 reads 0", "DMA3CNT_H read mask",
             "SOUNDCNT_X master bit", "Z set across I/O reads",
             "Z clear across I/O reads", "MSR setting T goes to Thumb",
             "palette RAM code (Thumb), rewritten",
             "OAM code (ARM), rewritten",
             "timer reads its reload right after overflowing",
             "register shifts use Rs[7:0] (ROM)",
             "register shifts use Rs[7:0] (VRAM)",
             "Rm = PC in a register shift reads PC + 12",
             "BX to ARM with bit 1 set fetches the word below",
             "branch into a run of same-condition ARM code (ROM)",
             "branch into a run of same-condition ARM code (IWRAM)",
             "FIQ has its own r8-r12",
             "LDM {pc}^ loads the CPSR from the SPSR",
             "MSR CPSR_c keeps the live flags",
             "RLUnCompWram from a halfword-aligned source",
             "LZ77UnCompWram from a halfword-aligned source"]
    extra = {"roms/gbadhoc-cpufix.gba": open(rom, "rb").read()}
    r = S.run("cpufix", "120:shot=cpufix.png\n" + menu_pick(6, wait=20),
              autoboot="gbadhoc-cpufix.gba", extra=extra)
    S.clean(r)
    p = os.path.join(r.root, "roms", "gbadhoc-cpufix.sav")
    sav = open(p, "rb").read() if os.path.exists(p) else b""
    S.check(sav[:6] == b"CPUFIX", "the cartridge wrote its results")
    if sav[:6] != b"CPUFIX":
        return
    # The x86 twin's emitter is a stand-in for the ARM one the Vita runs and
    # is not fixed for these three (Thumb register shifts, LDM {pc}^, MSR
    # flags): they are only enforced on the ARM twins.
    arm_only = {13, 20, 21}
    for n, name in enumerate(names):
        seen = int.from_bytes(sav[0x20 + 4 * n:0x24 + 4 * n], "little")
        if n in arm_only and S.arch.startswith("x86"):
            S.note("%s: x86 dynarec not fixed (result %d); checked on armhf" %
                   (name, sav[0x08 + n]))
            continue
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

    # ---- the rows: Settings wraps upwards to Button mapping, THIS GAME's
    # Display settings, Clock (RTC), Save type, CPU speed, Keep full speed, FPS, Rewind, FF button,
    # Fast-forward, Screenshot folder, Game previews, Game list, Theme,
    # Screen overlay, Grid strength, (Wide view: menus -- not a row while the
    # wide view is off, as it is in these tests), Wide view, Motion blend,
    # Colours
    script = "100:shot=before.png\n" + menu_pick(4, wait=10) + "+20:up\n" + \
        "+8:up\n" * 21 + "+10:shot=row.png\n+4:right\n+10:shot=row_gba.png\n" \
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

    def run(name, wide, keys="", frames=360, cfg=None, env=None):
        r = S.run("wide_" + name, keys + shots + "%d:quit\n" % frames,
                  config=dict(base, **(cfg or {})), autoboot="gbadhoc-wide.gba",
                  vita_ini=None if wide is None else {"wide": wide},
                  extra=extra, env=env)
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

    # ---- the rows' defaults (wide 1, fill 0): a game without a profile is
    # the console's own 240 columns, no margins at all (1.3: it used to open
    # them and fill them with soft edges, which looked stretched)
    r = run("default", None, cfg={"wide_gba": None})
    w = r.evt("wide")
    S.check(w and w[-1]["mode"] == "1" and w[-1]["fill"] == "0" and
            w[-1]["margin"] == "0" and w[-1]["profile"] == "0",
            "the defaults: wide for known games only, no profile, no margins, "
            "bars (%s)" % (w[-1]["_"] if w else "no line"))
    d0 = pictures(r, 240)
    if all(d0) and all(plain):
        S.check(all(d0[i].tobytes() == plain[i].tobytes() for i in range(3)),
                "...and the picture is the console's, unchanged")
    # the margins of a game with no more to show, where one is opened (the
    # twin's GBADHOC_WIDE_MARGIN_ALWAYS stands in for a profiled game's):
    # soft edges when asked for
    twin = {"GBADHOC_WIDE_MARGIN_ALWAYS": "1"}
    r = run("soft", 1, cfg={"wide_fill_gba": 1}, env=twin)
    w = r.evt("wide")
    S.check(w and w[-1]["mode"] == "1" and w[-1]["fill"] == "1" and
            w[-1]["margin"] == "21" and w[-1]["profile"] == "0",
            "soft edges: wide, a margin, soft edges (%s)" %
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
                  vita_ini={"wide": 1, "wide_fill": fill}, extra=extra,
                  env=twin)
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

    # ---- the rows: Settings wraps upwards to Button mapping, THIS GAME's
    # Display settings, Clock (RTC), Save type, CPU speed, Keep full speed, FPS, Rewind, FF button,
    # Fast-forward, Screenshot folder, Game previews, Game list, Theme,
    # Screen overlay, Grid strength, Wide view: menus, Wide view
    script = menu_pick(4, wait=100) + "+20:up\n" + "+8:up\n" * 19 + \
        "+10:right\n+10:up\n" \
        "+10:shot=row.png\n+4:right\n+10:shot=row_on.png\n+4:circle\n" \
        "+10:circle\n+60:shot=after.png\n" + EXIT_APP
    r = S.run("wide_row", script, config=dict(base, wide_gba=None),
              autoboot="gbadhoc-wide.gba", extra=extra)
    S.clean(r)
    look = r.evt("display_look")
    S.check(look and look[-1].get("wide") == "2" and
            look[-1].get("wide_fill") == "1",
            "Settings > Wide view: menus, one step right from bars is soft "
            "edges; Wide "
            "view, one step right is 2 (%s)" %
            [(e.get("wide"), e.get("wide_fill")) for e in look])
    w = r.evt("wide")
    S.check(w and w[-1]["mode"] == "2" and w[-1]["fill"] == "1" and
            w[-1]["margin"] == "21",
            "...and the core is told (%s)" % (w[-1]["_"] if w else "no line"))
    S.picture(r, "row_on.png", "wide_row.png")
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check(re.search(r"^wide_gba = 2$", ini, re.M) and
            re.search(r"^wide_fill_gba = 1$", ini, re.M),
            "config.ini keeps wide_gba = 2 and wide_fill_gba = 1")
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
    with open(OWN_GBA, "rb") as cartridge:
        second_game = cartridge.read()
    r = S.run("home", script, config={"ui_home": 1},
              extra={"roms/zz-home-test.gba": second_game})
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


def t_find(S):
    """Find composes with Starred, resolves real ROMs, and owns IME dismissal input."""
    cartridge = open(OWN_GBA, "rb").read()
    first, target = "find-only-01.gba", "find-only-02-NeEdLe.gba"
    extra = {"roms/" + name: cartridge for name in (first, target)}
    config = {"ui_home": 1, "last_rom": "gbadhoc-test.gba", "previews": 0, "wide_gba": 1}

    def run(name, script, favs=None):
        files = dict(extra)
        if favs is not None:
            files["roms/favourites.txt"] = favs
        result = S.run("find_" + name, script, config=config, extra=files)
        S.clean(result)
        return result

    def picked(result, name):
        picks = result.evt("ui_browser_pick")
        return len(picks) == 1 and picks[0]["_"] == "EVT ui_browser_pick rom=" + name

    r = run("exact", "90:circle\n94:type=find-only-02\n150:shot=filtered.png\n"
            "190:right\n230:cross\n400:shot=game.png\n420:quit\n")
    S.check(r.has(r'^EVT host_ime .* text="find-only-02"$') and
            r.has(r"^EVT browser_find active=1 shown=1 ") and picked(r, target),
            "Circle Find accepts a query; RIGHT remains on its unique result and Play resolves the exact ROM")
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check("last_rom = " + target + "\n" in ini and
            not re.search(r"^(?:find|query|search)\w*\s*=", ini, re.M),
            "only the selected ROM identity persists, never the query or filtered index")
    S.picture(r, "filtered.png")
    S.picture(r, "game.png")

    # Nothing starred matches, but a game does: the empty page says so, and
    # SELECT (All games) shows it with the search kept.
    r = run("starred_elsewhere", "60:select\n120:circle\n124:type=needle\n"
            "180:shot=starred_none.png\n200:select\n280:shot=all.png\n"
            "300:cross\n440:shot=game.png\n460:quit\n",
            favs="gbadhoc-test.gba\n")
    S.check(r.has(r"^EVT browser_find active=1 shown=0 scope=1") and
            r.has(r"^EVT ui_browser_view favs=0 shown=1") and picked(r, target),
            "an empty Starred search offers All games, which keeps the search and finds the game")
    S.picture(r, "starred_none.png")

    r = run("cancel", "90:circle\n94:type=\n160:cross\n300:shot=game.png\n320:quit\n")
    S.check(r.has(r'^EVT host_ime .* text="<cancel>"$') and
            not r.evt("browser_find") and picked(r, "gbadhoc-test.gba"),
            "cancelling Find preserves the unfiltered view and selected game")

    r = run("empty", "90:circle\n94:type=no-such-find-only-title\n150:shot=none.png\n"
            "170:cross\n190:square\n210:circle\n250:circle\n254:type=\n"
            "300:shot=cleared.png\n320:quit\n")
    S.check(r.has(r"^EVT browser_find active=1 shown=0 ") and
            r.has(r"^EVT browser_find active=0 ") and
            len(r.evt("ui_text_open")) == 2 and not r.evt("ui_browser_pick") and
            not r.evt("ui_browser_fav") and len(r.evt("rom_scan_time")) == 1,
            "zero results cannot launch, star or rescan; Circle clears without exiting and Find opens again")
    S.picture(r, "none.png")
    S.picture(r, "cleared.png")

    r = run("dismiss", "90:circle\n94:type=find-only-02\n100:cross:50\n"
            "200:shot=after_ime.png\n230:cross\n400:shot=game.png\n420:quit\n")
    shot = r.shot_evt("after_ime.png")
    S.check(shot and shot.get("emu") == "0" and picked(r, target),
            "a held CROSS that accepts the IME cannot launch until released and deliberately pressed again")

    r = run("starred", "90:circle\n94:type=find-only\n150:select\n190:right\n"
            "230:cross\n400:shot=game.png\n420:quit\n", favs=target + "\n")
    S.check(r.has(r"^EVT ui_browser_view favs=1 shown=1$") and
            len(r.evt("ui_text_open")) == 1 and picked(r, target),
            "Starred intersects the active query and preserves full ROM identity")

    r = run("unstar", "90:circle\n94:type=find-only\n150:select\n190:square\n"
            "230:shot=unstarred.png\n260:select\n310:cross\n460:shot=game.png\n480:quit\n",
            favs=target + "\n")
    S.check(r.has(r"^EVT ui_browser_fav op=remove ") and
            r.has(r"^EVT ui_browser_view favs=0 shown=2$") and picked(r, first),
            "unstar removes the filtered Starred result safely; returning to Games keeps the query")

    r = run("refresh", "90:circle\n94:type=find-only\n150:start\n" +
            "+8:down\n" * 23 + "+20:cross\n+70:select\n+70:cross\n"
            "+140:shot=game.png\n+20:quit\n", favs=target + "\n")
    S.check(r.has(r"^EVT library_refresh_done console=0 ") and
            r.has(r"^EVT ui_browser_view favs=1 shown=1$") and
            len(r.evt("ui_text_open")) == 1 and picked(r, target),
            "Settings and explicit Refresh preserve Find and rebuild it before Starred selection")

    r = run("console", "90:circle\n94:type=find-only\n150:triangle\n215:triangle\n"
            "280:triangle\n330:triangle\n400:cross\n540:shot=game.png\n560:quit\n")
    S.check(r.has(r"^EVT browser_find active=0 ") and
            len(r.evt("ui_browser_console")) == 4 and picked(r, "gbadhoc-test.gba"),
            "a console change clears Find instead of hiding the remembered game on return")

    r = run("touch", "90:touch=715/37\n95:type=find-only-02\n"
            "170:touch=715/37\n175:type=find-only-01\n230:shot=edited.png\n"
            "250:touch=446/71\n300:cross\n440:shot=game.png\n460:quit\n")
    S.check(len(r.evt("ui_text_open")) == 2 and
            r.has(r'^EVT host_ime .* text="find-only-01"$') and
            r.has(r"^EVT browser_find active=0 ") and picked(r, first),
            "the touch magnifier edits an active query; the separate clear chip clears without launching")
    S.picture(r, "edited.png")


def t_library_cache(S):
    """Console tabs reuse complete scans; Refresh library rescans and keeps focus."""
    with open(OWN_GBA, "rb") as cartridge:
        second_game = cartridge.read()
    # four console tabs (GBA, GB, GBC, SNES) since 1.3: two full turns
    script = ("60:triangle\n+60:triangle\n+60:triangle\n+70:triangle\n"
              "+60:triangle\n+60:triangle\n+60:triangle\n+70:triangle\n"
              "+60:right\n+20:start\n" + "+8:down\n" * 10 +
              "+20:shot=quality_setting.png\n" + "+8:down\n" * 13 +
              "+20:shot=refresh_command.png\n+8:cross\n"
              "+60:shot=refreshed.png\n+8:cross\n+100:shot=game.png\n+20:quit\n")
    r = S.run("library_cache", script,
              config={"ui_home": 1, "wide_gba": 1, "scale_gba": 1, "quality_gba": 1},
              extra={"roms/zz-home-test.gba": second_game})
    S.clean(r)
    scans = r.evt("rom_scan_time")
    sides = r.evt("rom_scan_side")
    S.check(len(scans) == 2 and {d.get("console") for d in scans} == {"0"} and
            len(sides) == 6 and {d.get("console") for d in sides} == {"1", "2", "3"},
            "one walk lists all four consoles; only the explicit refresh walks again "
            "(scans %s, side lists %s)" % ([d["_"] for d in scans], [d["_"] for d in sides]))
    S.check(len(r.evt("rom_cache_hit")) >= 4, "revisited console tabs use cached lists")
    S.check(r.has(r"library_refresh_asked") and r.has(r"library_refresh_done console=0 roms=2"),
            "the Refresh library command returns to the complete GBA list")
    picks = r.evt("ui_browser_pick")
    S.check(picks and picks[0].get("rom") == "zz-home-test.gba",
            "refresh preserves the focused game rather than jumping to last played")
    for name in ["quality_setting.png", "refresh_command.png", "refreshed.png", "game.png"]:
        S.picture(r, name)


def t_settings_order(S):
    """One DOWN from Screen size adjusts quality, not the filter, in both Settings views."""
    config = {"ui_home": 1, "scale_gba": 1, "filter_gba": 4, "quality_gba": 1,
              "scale_gb": 0, "filter_gb": 5, "quality_gb": 3,
              "scale_gbc": 4, "filter_gbc": 2, "quality_gbc": 0}
    for context, down in (("browser", 9), ("game", 7)):
        enter = "60:start\n" if context == "browser" else \
            "100:shot=before.png\n" + menu_pick(4, wait=10)
        leave = "+30:cross\n+120:shot=game.png\n+10:quit\n" if context == "browser" else \
            "+12:circle\n+60:shot=game.png\n+10:quit\n"
        script = (enter + "+8:down\n" * down +
                  "+20:shot=screen_size.png\n+4:right\n+10:left\n" +
                  "+10:down\n+10:shot=image_quality.png\n+4:right\n+12:circle\n" + leave)
        r = S.run("settings_order_" + context, script, config=config,
                  autoboot="gbadhoc-test.gba" if context == "game" else None)
        S.clean(r)
        modes = r.evt("video_mode")
        S.check(len(modes) == 2 and
                r.has(r"^EVT video_mode scale=4x crop filter=xBRZ smooth profile=GBA$") and
                r.has(r"^EVT video_mode scale=fit filter=xBRZ smooth profile=GBA$"),
                "%s: Screen size changes fit to 4x crop and back; the next row is not Video filter" %
                context)
        quality = r.evt("display_quality")
        S.check(quality and quality[-1].get("console") == "GBA" and
                quality[-1].get("quality") == "2",
                "%s: one DOWN from Screen size reaches Image quality and changes Original to 2x" %
                context)
        ini = open(os.path.join(r.root, "config.ini")).read()
        expected = {"scale_gba": 1, "filter_gba": 4, "quality_gba": 2,
                    "scale_gb": 0, "filter_gb": 5, "quality_gb": 3,
                    "scale_gbc": 4, "filter_gbc": 2, "quality_gbc": 0}
        S.check(all(re.search(r"^%s = %d$" % (key, value), ini, re.M)
                    for key, value in expected.items()),
                "%s: quality saves under its existing key without changing size, filter or inactive profiles" %
                context)
        textures = [d for d in r.evt("up_tex") if d.get("slot") == "0"]
        S.check(textures and all(d.get("f") == "2" for d in textures),
                "%s: the game actually uses the selected 2x processing factor" % context)
        for name in ("screen_size.png", "image_quality.png", "game.png"):
            S.picture(r, name)


def t_quality(S):
    """Native, 2x and 3x processing share fitted output; manual quality avoids GPU setup."""
    for quality in [1, 2, 3]:
        r = S.run("quality_%d" % quality, "120:shot=game.png\n+20:quit\n",
                  autoboot="gbadhoc-test.gba",
                  config={"scale_gba": 1, "filter_gba": 4, "quality_gba": quality},
                  vita_ini={"gpu_xbrz": 1})
        S.clean(r)
        S.check(r.has(r"display_quality console=GBA quality=%d" % quality),
                "the active GBA profile applies quality %d" % quality)
        textures = [d for d in r.evt("up_tex") if d.get("slot") == "0"]
        S.check((not textures) if quality == 1 else
                bool(textures and all(d.get("f") == str(quality) for d in textures)),
                "quality %d uses its selected CPU intermediate factor, not output zoom" % quality)
        S.check(not r.evt("gpu_xbrz"), "manual quality does not initialize the GPU shader path")
        S.picture(r, "game.png")


def t_favs_recover(S):
    """A favourites save cut short leaves favourites.txt.old and no list:
    it is the list.  Saving writes a new file and moves it in, leaving no
    .tmp or .old behind."""
    r = S.run("favs_recover", "60:select\n+80:shot=starred.png\n+10:square\n"
              "+80:shot=after.png\n+10:quit\n",
              config={"ui_home": 1, "last_rom": "gbadhoc-test.gba"},
              extra={"roms/favourites.txt.old": "gbadhoc-test.gba\n"})
    S.clean(r)
    fav = os.path.join(r.root, "roms", "favourites.txt")
    S.check(r.has(r"^EVT favs_load n=1 "), "the .old list is read back as the list")
    S.check(r.has(r"^EVT favs_save n=0 rc=0"), "untagging saves the list")
    S.check(os.path.exists(fav) and open(fav).read() == "" and
            not os.path.exists(fav + ".old") and not os.path.exists(fav + ".tmp"),
            "the saved list is favourites.txt alone")


def t_art_requeue(S):
    """Coming back to the list from a game (the same scan, from the cache)
    does not hand the whole library to the cover fetcher again."""
    import http.server
    import threading
    import functools
    serve = os.path.join(S.outdir, "art_requeue_serve")
    shutil.rmtree(serve, ignore_errors=True)
    os.makedirs(serve)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

    srv = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(Quiet, directory=serve))
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    script = ("120:cross\n+150:shot=game.png\n" + QUIT_TO_LIST +
              "+200:shot=list.png\n+10:quit\n")
    r = S.run("art_requeue", script, config={"ui_home": 1},
              vita_ini={"art_url": "http://127.0.0.1:%d" % srv.server_address[1]})
    srv.shutdown()
    S.clean(r)
    S.check(r.has(r"game_end reason=game_list") and r.evt("rom_cache_hit"),
            "a game was played and the list came back from the scan cache")
    queued = r.evt("art_queue")
    S.check(len(queued) == 1,
            "the library is handed to the cover fetcher once, not again after "
            "the game (art_queue lines: %d)" % len(queued))


def t_quality_preset(S):
    """In game, TRIANGLE steps the screen size only; an explicit Image quality keeps its processing."""
    for quality, active in ((2, "xBRZ smooth"), (1, "sharp bilinear")):
        r = S.run("quality_preset_%d" % quality,
                  "100:shot=before.png\n+4:triangle\n+40:shot=stretch.png\n"
                  "+4:triangle\n+40:shot=3x.png\n+4:triangle\n+40:shot=fit.png\n+20:quit\n",
                  autoboot="gbadhoc-test.gba",
                  config={"scale_gba": 1, "filter_gba": 4, "quality_gba": quality})
        S.clean(r)
        modes = [(d["scale"], d["_"].split("filter=")[1]) for d in r.evt("video_mode")]
        S.check(modes == [("stretch", active), ("3x", active), ("fit", active)],
                "quality %d: the preset steps stretch, 3x, fit with %s throughout (%s)" %
                (quality, active, modes))
        ini = open(os.path.join(r.root, "config.ini")).read()
        S.check(all(re.search(r"^%s = %d$" % (key, value), ini, re.M) for key, value in
                    (("scale_gba", 1), ("filter_gba", 4), ("quality_gba", quality))),
                "quality %d: the preset saves the size and keeps the quality and requested filter" %
                quality)
        textures = [d for d in r.evt("up_tex") if d.get("slot") == "0"]
        if quality == 1:
            S.check(not textures and r.has(r"sharp_target rc=0"),
                    "Original draws fitted pixels evenly through the sharp target, no enhancement")
        else:
            S.check(textures and all(d.get("f") == "2" for d in textures),
                    "Enhanced 2x keeps its factor at every preset size")
        for name in ("before.png", "stretch.png", "3x.png", "fit.png"):
            S.picture(r, name)


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
    S.check(r.has(r"^EVT gamedb deferred path="),
            "the game list is not read at boot, before the library shows")
    db = [d for d in r.evt("gamedb") if "records" in d]
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


def t_art_progress(S):
    """A held cover response never blocks Play; progress is visible, then covers arrive on return."""
    import http.server
    import io
    import threading
    cfg = {"ui_home": 1, "last_rom": "gbadhoc-test.gba"}
    extra = {"roms/zz-needs-cover.gba": open(OWN_GBA, "rb").read()}
    offline = S.run("art_progress_offline", "90:shot=idle.png\n+10:quit\n",
                    config=cfg, extra=extra)
    S.clean(offline)
    idle = S.picture(offline, "idle.png")

    out = os.path.join(S.outdir, "art_progress")
    shutil.rmtree(out, ignore_errors=True)
    busy_path = os.path.join(out, "busy.png")
    game_path = os.path.join(out, "game.png")
    stop = threading.Event()
    requests = []
    buf = io.BytesIO()
    Image.new("RGB", (64, 64), (240, 24, 24)).save(buf, "PNG")
    payload = buf.getvalue()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            row = {"path": self.path, "before_game": not os.path.exists(game_path),
                   "before_busy": not os.path.exists(busy_path)}
            requests.append(row)
            # No cover response can finish before the game has visibly run.
            until = time.monotonic() + 20
            while not os.path.exists(game_path) and not stop.is_set() and time.monotonic() < until:
                stop.wait(0.005)
            row["response_after_game"] = os.path.exists(game_path)
            if stop.is_set():
                return
            try:
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass  # Starting the game abandons the in-flight cover request.

        def log_message(self, *_):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    serving = threading.Thread(target=srv.serve_forever, daemon=True)
    serving.start()
    script = ("90:shot=busy.png\n+10:cross\n+120:shot=game.png\n" +
              QUIT_TO_LIST + "+90:shot=returned.png\n+180:shot=ready.png\n+10:quit\n")
    try:
        r = S.run("art_progress", script, config=cfg, extra=extra,
                  vita_ini={"art_url": "http://127.0.0.1:%d" % srv.server_port,
                            "art_titles": 0})
    finally:
        stop.set()
        srv.shutdown()
        srv.server_close()
        serving.join()
    S.clean(r)
    busy = S.picture(r, "busy.png")
    S.picture(r, "game.png")
    ready = S.picture(r, "ready.png")
    S.check(bool(requests) and requests[0]["before_busy"] and
            all(row.get("response_after_game") for row in requests),
            "Play runs while the first cover response is still held (%s)" % requests)
    S.check(bool(r.evt("ui_browser_pick")) and bool(r.evt("rom_loaded")) and
            r.shot_evt("game.png") is not None,
            "controller Play reaches an emulated game, not a cover-loading gate")
    if idle and busy:
        S.check(worst_diff(idle, busy, (500, 60, 920, 100)) > 40,
                "the real Home frame visibly shows cover-checking status during a held request")
        S.check(worst_diff(idle, busy, (0, 100, 960, 510)) == 0,
                "background progress leaves the cover row, Play and three footer hints unchanged")
    covers = glob.glob(os.path.join(r.root, "boxart", "*.png"))
    S.check(len(covers) == 2,
            "returning from the game resumes and finishes both controlled cover downloads (%d)" % len(covers))
    if ready:
        red = sum(1 for c in ready.crop((60, 102, 160, 200)).get_flattened_data()
                  if c[0] > 180 and c[1] < 60 and c[2] < 60)
        S.check(red > 8000, "the focused cover appears automatically after returning (%d red pixels)" % red)
    S.summary["art_progress_evidence"] = {"requests": requests, "covers": len(covers)}


def art_settings_requests_ok(requested, failed, stems):
    from collections import Counter
    prefix = "/Nintendo - Game Boy Advance/Named_Boxarts/"
    expected = Counter({prefix + name + ".png": 1 for name in stems})
    actual = Counter(requested)
    if actual == expected:
        return True
    first, selected = prefix + stems[1] + ".png", prefix + stems[0] + ".png"
    expected[first] += 1
    # Selected-first can interrupt a request already in flight at startup.
    # This precise trace also occurs in unmodified R5; do not permit general
    # retries, unknown URLs, or refetches of completed cached covers here.
    return (actual == expected and requested[:3] == [first, selected, first] and
            failed == ['EVT art_fetch failed name="' + stems[1] + '"'])


def art_settings_cache_ok(directory, stems, png, thumb):
    expected_png = {name + ".png" for name in stems}
    expected_thumb = {name + ".t2.565" for name in stems}
    # 1.3: tile thumbnails live in cache/boxart, beside boxart/ (sidecar.h)
    tdir = os.path.join(os.path.dirname(directory), "cache", "boxart")
    try:
        if ({os.path.basename(p) for p in glob.glob(os.path.join(directory, "*.png"))}
                != expected_png or
                {os.path.basename(p) for p in glob.glob(os.path.join(tdir, "*.t2.565"))}
                != expected_thumb):
            return False
        return (all(open(os.path.join(directory, name), "rb").read() == png for name in expected_png)
                and all(open(os.path.join(tdir, name), "rb").read() == thumb for name in expected_thumb))
    except OSError:
        return False


def t_art_settings(S):
    """Artwork keeps downloading in Settings, then stale visible tiles refresh on return."""
    import http.server
    import io
    import threading
    import urllib.parse
    stems = ["gbadhoc-test"] + ["Artwork %02d" % i for i in range(39)]
    out = os.path.join(S.outdir, "art_settings")
    shutil.rmtree(out, ignore_errors=True)
    entered = os.path.join(out, "settings_entered.png")
    finished = os.path.join(out, "settings_done.png")
    returned = os.path.join(out, "after.png")
    root = os.path.join(out, "root")
    buf = io.BytesIO()
    Image.new("RGB", (64, 64), (240, 24, 24)).save(buf, "PNG")
    payload = buf.getvalue()
    cartridge = open(OWN_GBA, "rb").read()
    extra = {"roms/%s.gba" % stem: cartridge for stem in stems[1:]}
    requested, during = [], {}
    stop = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            # The first response cannot arrive before the app is in Settings.
            until = time.monotonic() + 15
            while not os.path.exists(entered) and not stop.is_set() and time.monotonic() < until:
                stop.wait(0.005)
            path = urllib.parse.unquote(self.path)
            stem = os.path.basename(path)[:-4]
            ok = os.path.exists(entered) and stem in stems
            stop.wait(0.02)
            body = payload if ok else b"not found"
            requested.append(path)
            self.send_response(200 if ok else 404)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    def observe():
        while not stop.is_set():
            if os.path.exists(finished):
                during["png"] = len(glob.glob(os.path.join(root, "boxart", "*.png")))
                during["thumbs"] = len(glob.glob(os.path.join(root, "cache", "boxart", "*.t2.565")))
                during["returned"] = os.path.exists(returned)
                return
            stop.wait(0.005)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    serving = threading.Thread(target=srv.serve_forever, daemon=True)
    observer = threading.Thread(target=observe, daemon=True)
    serving.start()
    observer.start()
    script = ("20:shot=before.png\n+10:start\n+10:shot=settings_entered.png\n" +
              "+8:down\n" * 20 + "+360:shot=settings_done.png\n+45:circle\n" +
              "+90:shot=after.png\n+10:quit\n")
    try:
        r = S.run("art_settings", script,
                  config={"ui_home": 1, "wide_gba": 1, "last_rom": "gbadhoc-test.gba"},
                  extra=extra, vita_ini={"art_url": "http://127.0.0.1:%d" % srv.server_port,
                                         "art_titles": 0})
    finally:
        stop.set()
        srv.shutdown()
        srv.server_close()
        serving.join()
        observer.join()
    S.clean(r)
    S.check(during.get("png") == 40 and during.get("thumbs") == 40 and
            during.get("returned") is False,
            "all 40 covers and thumbnails finish before leaving Settings (%s)" % during)
    failed = [e["_"] for e in r.evt("art_fetch") if e["_"].startswith("EVT art_fetch failed ")]
    S.check(art_settings_requests_ok(requested, failed, stems),
            "exact cover requests, allowing only the known initial superseded trace (%d)" % len(requested))
    import struct
    pixel = (240 >> 3) | ((24 >> 2) << 5) | ((24 >> 3) << 11)
    thumb = struct.pack("<4sHHH6x", b"565R", 64, 64, 64) + struct.pack("<H", pixel) * (64 * 64)
    S.check(art_settings_cache_ok(os.path.join(r.root, "boxart"), stems, payload, thumb),
            "exact 40 PNG/thumbnail names and complete contents are persisted")
    S.check(bool(r.evt("art_resync")), "overflow requests one complete artwork-cache resync")
    before = S.picture(r, "before.png")
    after = S.picture(r, "after.png")
    S.picture(r, "settings_done.png")
    if before and after:
        box = (60, 102, 160, 200)
        red = lambda im: sum(1 for c in im.crop(box).get_flattened_data()
                             if c[0] > 180 and c[1] < 60 and c[2] < 60)
        before_red, after_red = red(before), red(after)
        S.check(before_red < 100 and after_red > 8000,
                "the previously cached empty last-game tile reloads its red cover (%d -> %d pixels)" %
                (before_red, after_red))
        evidence = {**during, "requests": len(requested), "before_red": before_red,
                    "after_red": after_red, "resyncs": len(r.evt("art_resync"))}
        S.summary["art_settings_evidence"] = evidence
        print("    artwork evidence " + json.dumps(evidence, sort_keys=True))


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


def cover_png(size, color, corner=None):
    """A PNG of one colour (and, if asked, a different 8x8 corner)."""
    import io
    im = Image.new("RGB", size, color)
    if corner:
        im.paste(Image.new("RGB", (8, 8), corner), (0, 0))
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return buf.getvalue()


def near(px, want, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(px[:3], want))


# Settings > LIBRARY > Cover picture, reached from the top of the browser's
# Settings by wrapping UP: Button mapping, Game order, Game folders, then it.
# start, then up: Button mapping, Game order, Game folders, Sync RetroArch
# saves, Cover picture
COVER_ROW = "60:start\n+30:up\n" + "+8:up\n" * 7


def t_cover_menu(S):
    """Cover picture (cover_custom.h, ui_vita_cover.h): a picture chosen from
    the console's photos (a fake ux0:picture, with a subfolder) is previewed,
    set on X, scaled down with its shape kept, and shown at once; the
    downloaded cover it replaced is kept; a game screenshot can be used too;
    Automatic puts the cover back."""
    out = os.path.join(S.outdir, "cover_menu")
    root = os.path.join(out, "root")
    pics = os.path.join(root, "pics")
    red = cover_png((400, 600), (200, 30, 30))           # portrait, 2:3
    blue = cover_png((300, 200), (30, 30, 200))
    old = cover_png((64, 64), (240, 240, 240))           # the "downloaded" one
    stale = b"R565" + bytes(12) + b"STALE" * 40
    S.make_root(root, {"ui_home": 1}, None, {"pictures": pics},
                {"pics/SCREENSHOT/Red.png": red, "pics/Blue.png": blue,
                 "pics/notes.txt": b"not a picture",
                 "boxart/gbadhoc-test.png": old,
                 "cache/boxart/gbadhoc-test.v565": stale,
                 "cache/boxart/gbadhoc-test.vh565": stale,
                 "boxart/gbadhoc-test.t2.565": stale})
    # newest first: Red is the first picture in the list
    now = time.time()
    os.utime(os.path.join(pics, "SCREENSHOT", "Red.png"), (now, now))
    os.utime(os.path.join(pics, "Blue.png"), (now - 100, now - 100))
    script = COVER_ROW + """+10:cross
+10:shot=cover_page.png
+4:cross
+30:shot=cover_pick.png
+4:cross
+12:shot=cover_sure.png
+4:cross
+12:shot=cover_done.png
+4:circle
+8:circle
+60:shot=cover_home.png
+10:quit
"""
    r = S.run("cover_menu", script, prepared=True)
    S.clean(r)
    pk = r.evt("cover_pick")
    S.check(pk and pk[0].get("pictures") == "2",
            "two pictures are found, the note and the subfolder's "
            "counted right (%s)" % (pk[0]["_"] if pk else "no cover_pick"))
    st = r.evt("cover_set")
    S.check(st and st[0].get("rc") == "0" and st[0].get("from") == "picture",
            "X on the preview sets it (%s)" % (st[0]["_"] if st else "none"))
    png = os.path.join(root, "boxart", "gbadhoc-test.png")
    im = Image.open(png) if os.path.exists(png) else None
    S.check(im is not None and im.size == (341, 512) and
            near(im.convert("RGB").getpixel((100, 100)), (200, 30, 30), 3),
            "the cover is the red picture, 400x600 scaled to 512 high with "
            "its shape kept (%s)" % ((im.size if im else "no file"),))
    S.check(os.path.exists(os.path.join(root, "cache", "covers", "gbadhoc-test.own")),
            "boxart/gbadhoc-test.own says it is the player's")
    dl = os.path.join(root, "cache", "covers", "gbadhoc-test.dl")
    S.check(os.path.exists(dl) and open(dl, "rb").read() == old,
            "the downloaded cover it replaced is kept (.dl)")
    th = os.path.join(root, "cache", "boxart", "gbadhoc-test.t2.565")
    d = open(th, "rb").read() if os.path.exists(th) else b""
    S.check(len(d) == 16 + 160 * 160 * 2 and d[4:8] == bytes([160, 0, 160, 0]),
            "the tile thumbnail was made again, 160 x 160 (%d bytes)" % len(d))
    S.check(d[16 + 2 * (80 * 160 + 80):16 + 2 * (80 * 160 + 80) + 2] ==
            bytes([0x06, 0xC1]) or near(
                [((int.from_bytes(d[16 + 2 * (80 * 160 + 80):16 + 2 * (80 * 160 + 80) + 2], "little") >> k) & m) * (255 // m)
                 for k, m in ((0, 31), (5, 63), (11, 31))], (200, 30, 30), 24),
            "its middle is the red picture's red")
    for ext in ("v565", "vh565"):
        f = os.path.join(root, "boxart", "gbadhoc-test." + ext)
        S.check(not os.path.exists(f) or open(f, "rb").read() != stale,
                "the old %s cache is gone" % ext)
    page = S.picture(r, "cover_page.png")
    sure = S.picture(r, "cover_sure.png")
    if sure:
        S.check(near(sure.getpixel((480, 250)), (200, 30, 30), 40),
                "the preview before confirming shows the red picture (%s)" %
                (sure.getpixel((480, 250)),))
    home = S.picture(r, "cover_home.png")
    if home:
        tile = home.getpixel((110, 150))
        S.check(near(tile, (200, 30, 30), 45),
                "the home tile shows the new cover at once (%s)" % (tile,))
    S.picture(r, "cover_pick.png")

    # a screenshot of the game, then Automatic (the .dl comes back)
    shots = os.path.join(root, "screenshots")
    os.makedirs(shots, exist_ok=True)
    with open(os.path.join(shots, "gbadhoc-test-20260101-120000.png"), "wb") as f:
        f.write(cover_png((240, 160), (30, 200, 200)))
    script = COVER_ROW + """+10:cross
+10:down
+4:cross
+12:cross
+12:shot=cover_shot_done.png
+4:down
+4:down
+4:cross
+12:shot=cover_auto_done.png
+4:circle
+8:circle
+60:shot=cover_home2.png
+10:quit
"""
    r = S.run("cover_menu", script, prepared=True)
    S.clean(r)
    st = r.evt("cover_set")
    S.check(st and st[0].get("from") == "game" and st[0].get("rc") == "0",
            "Use a screenshot of the game takes its screenshot (%s)" %
            (st[0]["_"] if st else "none"))
    au = r.evt("cover_auto")
    S.check(au and au[0].get("rc") == "1",
            "Automatic puts the kept download back (%s)" %
            (au[0]["_"] if au else "none"))
    S.check(os.path.exists(png) and open(png, "rb").read() == old,
            "boxart/gbadhoc-test.png is the downloaded cover again")
    S.check(not os.path.exists(os.path.join(root, "cache", "covers", "gbadhoc-test.own"))
            and not os.path.exists(dl),
            "the player's note and the kept copy are gone")
    d = open(th, "rb").read() if os.path.exists(th) else b""
    S.check(len(d) == 16 + 64 * 64 * 2 or d[4:8] == bytes([64, 0, 64, 0]),
            "its thumbnail is the downloaded picture's (%d bytes)" % len(d))
    home = S.picture(r, "cover_home2.png")
    if home:
        tile = home.getpixel((110, 150))
        S.check(near(tile, (240, 240, 240), 30),
                "the home tile is the downloaded cover again (%s)" % (tile,))
    # the Home's Cover button, by touch
    r = S.run("cover_menu", "100:touch=326/476\n+30:shot=cover_btn.png\n"
              "+4:circle\n+30:quit\n", prepared=True)
    S.clean(r)
    S.check(r.has(r"^EVT ui_screen name=cover from=browser"),
            "the Cover button on the home screen opens the page")
    S.picture(r, "cover_btn.png")


def t_cover_inbox(S):
    """A picture in covers/ named like the game (any case, with or without
    the ROM extension) is taken at the next library refresh and wins over
    a download; a fetch never writes over it; Automatic keeps it off until
    the file changes; the shelf shows it too."""
    import http.server
    import threading
    import functools
    out = os.path.join(S.outdir, "cover_inbox")
    root = os.path.join(out, "root")
    serve = os.path.join(out, "server")
    shutil.rmtree(serve, ignore_errors=True)
    dirn = os.path.join(serve, "Nintendo - Game Boy Advance", "Named_Boxarts")
    os.makedirs(dirn)
    served = cover_png((200, 200), (250, 250, 20))
    with open(os.path.join(dirn, "gbadhoc-test.png"), "wb") as f:
        f.write(served)
    asked = []

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, fmt, *args):
            asked.append(self.path)

    srv = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(Quiet, directory=serve))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d" % srv.server_address[1]
    green = cover_png((900, 500), (30, 190, 40))
    # 1  the file is picked up (no fetching in this run)
    S.make_root(root, {"ui_home": 1}, None, None,
                {"covers/GbAdHoc-Test.GBA.PNG": green,
                 "covers/other.png": cover_png((10, 10), (0, 0, 0))})
    r = S.run("cover_inbox", "200:shot=inbox_home.png\n+10:quit\n",
              prepared=True)
    S.clean(r)
    w = r.evt("cover_inbox")
    S.check(any(d.get("waiting") == "1" for d in w) and
            any(d.get("stem") == "gbadhoc-test" and d.get("rc") == "1"
                for d in w),
            "one waiting file, matched in spite of case and the .gba (%s)" %
            [d["_"] for d in w])
    png = os.path.join(root, "boxart", "gbadhoc-test.png")
    im = Image.open(png) if os.path.exists(png) else None
    S.check(im is not None and im.size == (512, 284),
            "it is copied to boxart/ scaled to 512 wide, shape kept (%s)" %
            ((im.size if im else "no file"),))
    S.check(os.path.exists(os.path.join(root, "covers", "GbAdHoc-Test.GBA.PNG")),
            "the file in covers/ is left alone")
    home = S.picture(r, "inbox_home.png")
    if home:
        tile = home.getpixel((110, 150))
        S.check(near(tile, (30, 190, 40), 45),
                "the home tile shows it (%s)" % (tile,))
    first = open(png, "rb").read() if os.path.exists(png) else b""
    # (prepared runs keep the root's vita.ini: the server's address goes in)
    with open(os.path.join(root, "vita.ini"), "w") as f:
        f.write("art_url = %s\n" % url)
    # 2  again, with the fetcher running and a download on offer: the own
    #    cover is neither asked for nor written over; the list opens
    #    with nothing waiting
    r = S.run("cover_inbox", "300:shot=inbox_home2.png\n+10:quit\n",
              prepared=True)
    S.clean(r)
    S.check(not r.evt("cover_inbox"),
            "a file already taken is not taken again")
    S.check(os.path.exists(png) and open(png, "rb").read() == first,
            "the fetcher did not write over the player's cover")
    S.check(not any("Named_Boxarts/gbadhoc-test" in a for a in asked),
            "and did not even ask for it (asked: %s)" % asked[:5])
    # 3  Automatic: the download comes (there is no kept copy), and the
    #    file in covers/ does not take it back
    script = COVER_ROW + """+10:cross
+10:down
+4:down
+4:cross
+240:shot=inbox_auto.png
+4:circle
+8:circle
+30:quit
"""
    r = S.run("cover_inbox", script, prepared=True)
    S.clean(r)
    au = r.evt("cover_auto")
    S.check(au and au[0].get("rc") == "2",
            "Automatic with no kept copy asks for a download (%s)" %
            (au[0]["_"] if au else "none"))
    S.check(os.path.exists(png) and open(png, "rb").read() == served,
            "the download took the place (asked: %s)" % asked[:5])
    S.check(not os.path.exists(os.path.join(root, "cache", "covers", "gbadhoc-test.own"))
            and os.path.exists(os.path.join(root, "cache", "covers", "gbadhoc-test.auto")),
            "boxart/gbadhoc-test.auto remembers the choice")
    r = S.run("cover_inbox", "300:quit\n", prepared=True)
    S.clean(r)
    S.check(not r.evt("cover_inbox") and open(png, "rb").read() == served,
            "the next start leaves it automatic")
    # 4  a changed file in covers/ is a new wish
    time.sleep(1.1)
    with open(os.path.join(root, "covers", "GbAdHoc-Test.GBA.PNG"), "wb") as f:
        f.write(cover_png((900, 500), (200, 20, 200)))
    r = S.run("cover_inbox", "300:quit\n", prepared=True)
    S.clean(r)
    S.check(r.evt("cover_inbox") and os.path.exists(
                os.path.join(root, "cache", "covers", "gbadhoc-test.own")) and
            Image.open(png).convert("RGB").getpixel((50, 50))[0] > 150,
            "a changed file is taken again")
    # 5  the shelf shows it as well
    S.make_root(root, {"ui_home": 0}, None, None, {"covers/keep": b"x"})
    Image.open(__import__("io").BytesIO(green)).save(
        os.path.join(root, "covers", "gbadhoc-test.jpg"), "JPEG")
    r = S.run("cover_inbox", "200:shot=inbox_shelf.png\n+10:quit\n",
              prepared=True)
    S.clean(r)
    shelf = S.picture(r, "inbox_shelf.png")
    if shelf:
        px = shelf.getpixel((792, 150))
        S.check(near(px, (30, 190, 40), 50),
                "the shelf's cover panel shows a JPG from covers/ (%s)" % (px,))
    srv.shutdown()


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
    # ---- the row: Settings wraps upwards to Button mapping, THIS GAME's
    # Display settings, Clock (RTC), Save type, CPU speed, Keep full speed; CROSS turns it off, and config.ini
    # keeps that
    script = menu_pick(4, wait=100) + "+20:up\n" + "+8:up\n" * 8 + "+10:shot=row.png\n" \
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


def t_smooth_gov(S):
    """The smoothing chosen is never lowered for speed: slow frames while
    xBRZ draws (--slow, simulated; GBADHOC_GOV=1 lets the twin run the
    governor at all) never step it, with Keep full speed on or off."""
    rom = os.path.join(HERE, "testrom", "gbadhoc-scroll.gba")
    if not os.path.exists(rom):
        S.note("no testrom/gbadhoc-scroll.gba: skipped")
        return
    extra = {"roms/gbadhoc-scroll.gba": open(rom, "rb").read()}
    old = os.environ.get("GBADHOC_GOV")
    os.environ["GBADHOC_GOV"] = "1"
    try:
        res = {}
        for on in (1, 0):
            r = S.run("smoothgov_%d" % on, "100:right:300\n", extra=extra,
                      config={"full_speed": on, "scale_gba": 1, "filter_gba": 4,
                              "last_rom": "gbadhoc-scroll.gba"},
                      autoboot="gbadhoc-scroll.gba",
                      args=("--frames", "3000", "--slow", "200:240:30000"))
            S.clean(r)
            res[on] = [e.get("step") for e in r.evt("smooth_gov")]
    finally:
        if old is None:
            os.environ.pop("GBADHOC_GOV", None)
        else:
            os.environ["GBADHOC_GOV"] = old
    S.check(not res[1], "Keep full speed on: smoothing never steps (%s)" % res[1])
    S.check(not res[0], "Keep full speed off: smoothing never steps (%s)" % res[0])


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
    # states go under the app folder, named for the game and its folder
    # (psp/sidecar.h); the extra folder keeps the ROM and its battery save
    st = os.path.join(root, "states", "gba", "Extra Adventure (U) [!].st1")
    S.check(os.path.exists(st) and os.path.getsize(st) > 100000 and
            os.path.exists(st + ".thumb") and os.path.exists(st + ".shot"),
            "slot 2's state and its previews were written under states/ (%s)" %
            os.path.basename(st))
    S.check(only_beside(X, "Extra Adventure (U) [!]",
                        {game, "Extra Adventure (U) [!].sav"}) == [],
            "the extra folder holds the ROM and its .sav and nothing else "
            "(%s)" % only_beside(X, "Extra Adventure (U) [!]",
                                 {game, "Extra Adventure (U) [!].sav"}))
    rs = os.path.join(root, "states", "gba", "Extra Adventure (U) [!].rsm")
    S.check(os.path.exists(rs) and os.path.exists(rs + ".pt"),
            "Continue's point and its patch note are under states/ too")
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
        d3 = [d for d in r3.evt("rom_scan_side") if d["console"] == "1"]
        S.check(d3 and d3[-1]["crc"] == list_crc(gbn),
                "the GB list is complete and in order (listed by the walk the "
                "GBA list made)")
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
    # ten folders (the limit is 24)
    out2 = os.path.join(S.outdir, "extra_roms_ten")
    root2 = os.path.join(out2, "root")
    shutil.rmtree(out2, ignore_errors=True)
    os.makedirs(out2)
    S.make_root(root2, None, None, {"extra_roms": ";".join(d)}, None)
    r = S.run("extra_roms_ten", "40:quit\n", prepared=True, cwd=cwd)
    S.clean(r)
    rr = r.evt("rom_roots")
    # 1.3: up to UI_EXTRA_ROOTS_MAX (24) folders, so all ten count
    S.check(rr and rr[0]["extra"] == "10" and rr[0]["skipped"] == "0" and
            rr[0]["list"] == '"%s"' % ";".join(d[:10]),
            "ten folders: all ten are used (up to 24) (%s)" %
            (rr[0]["_"] if rr else "no rom_roots line"))
    # extra_roms = 0, and extra_roms = (nothing), with the places below there
    cwd = os.path.join(S.outdir, "extra_roms_cwd2")
    shutil.rmtree(cwd, ignore_errors=True)
    for sub in ("ux0:ROMS/GBA", "ux0:ROMS/GB", "ux0:roms/GBA",
                "ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Advance"):
        os.makedirs(os.path.join(cwd, sub), exist_ok=True)
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
            expected = list_oracle(os.path.join(r.root, "roms"),
                                   [os.path.join(cwd, p) for p in fold.split(";")], ".gba")
            S.check(b and int(b[0]["found"]) ==
                    len(expected),
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


def t_old_states(S):
    """A state save that was interrupted leaves only <state>.old.  On the
    home screen: browsing its card never renames it; loading the card
    recovers it first and loads it; deleting the slot removes it; saving
    over it in the game recovers and then replaces it, leaving no .old."""
    make = ("60:cross\n+150:shot=game.png\n" + menu_pick(1) +
            "+20:down\n+10:square\n+30:circle\n+10:circle\n" + EXIT_APP)
    r = S.run("old_states_make", make, config={"ui_home": 1})
    S.clean(r)
    st_rel = os.path.join("states", "gba", "gbadhoc-test.st1")
    st = os.path.join(r.root, st_rel)
    if not S.check(os.path.exists(st), "slot 2 was saved to make the fixture"):
        return
    saved = open(st, "rb").read()

    def interrupted(name, script):
        out = os.path.join(S.outdir, name)
        root = os.path.join(out, "root")
        shutil.rmtree(out, ignore_errors=True)
        shutil.copytree(r.root, root)
        for f in glob.glob(os.path.join(root, "log", "*")):
            os.remove(f)
        os.rename(os.path.join(root, st_rel), os.path.join(root, st_rel + ".old"))
        return S.run(name, script, prepared=True)

    def files(res):
        p = os.path.join(res.root, st_rel)
        return os.path.exists(p), os.path.exists(p + ".old")

    b = interrupted("old_states_browse",
                    "60:shot=home.png\n+2:down\n+60:shot=cards.png\n"
                    "+2:circle\n+40:shot=back.png\n+10:quit\n")
    S.check(b.rc == 0, "browsing ran (rc %s)" % b.rc)
    S.check(files(b) == (False, True) and not b.evt("state_recovered"),
            "browsing the cards leaves the interrupted save as .old, renamed "
            "by nobody (files %s, recovered %d)" % (files(b), len(b.evt("state_recovered"))))

    c = interrupted("old_states_load",
                    "60:down\n+60:cross\n+150:shot=loaded.png\n" + EXIT_APP)
    S.clean(c)
    rec = c.evt("state_recovered")
    S.check(len(rec) == 1 and rec[0].get("file", "").endswith("gbadhoc-test.st1"),
            "X on the card recovers the .old first, once (log: %s)" %
            [x["_"] for x in rec])
    S.check(c.has(r"ui_browser_pick rom=gbadhoc-test.gba state_slot=2") and
            c.has(r"state_load slot=2 via=shelf rc=0"),
            "...and the game starts from that slot")
    present, old = files(c)
    S.check(present and not old and
            open(os.path.join(c.root, st_rel), "rb").read() == saved,
            "the recovered slot is the saved state, byte for byte, and no "
            ".old is left (files %s)" % ((present, old),))

    d = interrupted("old_states_delete",
                    "60:down\n+60:square\n+10:cross\n+40:shot=deleted.png\n"
                    "+10:quit\n")
    S.check(d.rc == 0, "deleting ran (rc %s)" % d.rc)
    S.check(d.has(r"state_delete slot=2 file=") and files(d) == (False, False),
            "SQUARE then X on the card deletes the slot: neither the state "
            "nor its .old is left (files %s)" % (files(d),))

    # Deleting the game's last state empties the shelf: it closes once the
    # deletion is read back, and X then plays the game itself.
    out = os.path.join(S.outdir, "old_states_last")
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(r.root, os.path.join(out, "root"))
    for f in glob.glob(os.path.join(out, "root", "log", "*")):
        os.remove(f)
    g = S.run("old_states_last", "60:down\n+60:square\n+10:cross\n+90:shot=closed.png\n"
              "+10:cross\n+150:shot=game.png\n+10:quit\n", prepared=True)
    S.clean(g)
    # (X is Continue on the Home page: the game played earlier was left
    # with a resume point, so it continues from there -- not from a slot.)
    S.check(g.has(r"state_delete slot=2 file=") and g.has(r"home_shelf_close reason=empty") and
            g.has(r"EVT game rom=") and
            not [e for e in g.evt("state_load") if "slot" in e],
            "deleting the last state closes the shelf, and X then plays the game")

    # The interrupted state is real data: the page asks before replacing it
    # ("Overwrite Slot 2?"), and X answers yes.
    e = interrupted("old_states_save",
                    "60:cross\n+150:shot=game.png\n" + menu_pick(1) +
                    "+20:down\n+10:square\n+20:shot=ask.png\n+10:cross\n"
                    "+30:circle\n+10:circle\n" + EXIT_APP)
    S.clean(e)
    S.check(e.has(r"state_save slot=2 ") and not e.has(r"state_save .*FAILED"),
            "slot 2 saved over the interrupted save, after the overwrite question")
    S.check(len(e.evt("state_recovered")) == 1,
            "the save recovered the .old before replacing it (log: %s)" %
            [x["_"] for x in e.evt("state_recovered")])
    S.check(files(e) == (True, False),
            "saving over it leaves one state and no .old (files %s)" % (files(e),))


class RaServer:
    """A stand-in for retroachievements.org (dorequest.php), on the
    loopback: one user (ra_tester / hunter2pw, token tok-1), one game (the
    test cartridge, by its MD5) with two achievements -- 1001 "First Press"
    when battery RAM byte 1 reads 23 (the test cartridge's first A press,
    testrom/main.c), 1002 one that can never be earned -- and a record of
    every request (the password masked, how often it came kept)."""
    PASSWORD, TOKEN = "hunter2pw", "tok-1"

    def __init__(self, game_md5):
        import http.server
        import threading
        import urllib.parse
        self.md5, self.calls, self.password_seen = game_md5, [], 0
        srv = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                q = dict(urllib.parse.parse_qsl(self.rfile.read(n).decode()))
                pw_ok = q.get("p") == srv.PASSWORD
                if pw_ok:
                    srv.password_seen += 1
                if "p" in q:
                    q["p"] = "*"
                q["_pw_ok"] = pw_ok
                srv.calls.append(q)
                code, body = srv.answer(q)
                data = json.dumps(body).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def url(self):
        return "http://127.0.0.1:%d" % self.port

    def stop(self):
        self.httpd.shutdown()

    def answer(self, q):
        r, ok = q.get("r"), q.get("u") == "ra_tester"
        if r == "login2":
            good = ok and (q.get("t") == self.TOKEN or q["_pw_ok"])
            if not good:
                return 401, {"Success": False, "Code": "invalid_credentials",
                             "Error": "Invalid User/Password combination."}
            return 200, {"Success": True, "User": "ra_tester",
                         "Token": self.TOKEN, "Score": 5,
                         "SoftcoreScore": 0, "Messages": 0}
        if not ok or q.get("t") != self.TOKEN:
            return 401, {"Success": False, "Code": "invalid_token",
                         "Error": "Invalid token"}
        if r == "achievementsets":
            if q.get("m") != self.md5:
                return 200, {"Success": True, "GameId": 0, "Title": "",
                             "ConsoleId": 5, "ImageIconUrl": "", "Sets": []}
            ach = lambda i, t, d, p, m: {
                "ID": i, "Title": t, "Description": d, "Flags": 3,
                "Points": p, "MemAddr": m, "Author": "test",
                "BadgeName": "00001", "Created": 1700000000,
                "Modified": 1700000000, "Type": ""}
            img = "http://127.0.0.1/Images/000001.png"
            return 200, {"Success": True, "GameId": 77,
                         "Title": "GBAdhoc Test", "ConsoleId": 5,
                         "ImageIconUrl": img, "RichPresencePatch": "",
                         "Sets": [{"AchievementSetId": 7, "GameId": 77,
                                   "Title": "GBAdhoc Test", "Type": "core",
                                   "ImageIconUrl": img, "Leaderboards": [],
                                   "Achievements": [
                                       ach(1001, "First Press", "Press A once",
                                           5, "0xH048001=23"),
                                       ach(1002, "Never", "Cannot be earned",
                                           10, "0xH000000=1_0xH000000=2")]}]}
        if r == "startsession":
            return 200, {"Success": True, "Unlocks": [],
                         "HardcoreUnlocks": [], "ServerNow": 1700000000}
        if r == "awardachievement":
            return 200, {"Success": True, "Score": 10, "SoftcoreScore": 5,
                         "AchievementID": int(q.get("a", 0)),
                         "AchievementsRemaining": 1}
        if r == "ping":
            return 200, {"Success": True}
        return 400, {"Success": False, "Error": "unknown request " + str(r)}


def t_ra(S):
    """RetroAchievements (ra_vita.h, a stand-in server on the loopback):
    Settings > Account signs in with the name and password typed on the
    system keyboard and keeps only the token; the next boot signs in with
    it; a game's achievements are fetched by its MD5 and one is earned in
    play, toasted and sent; Settings > Achievements lists them; Hardcore
    refuses a state load and sends its unlock as hardcore; a server that is
    not there costs nothing but a log line."""
    import hashlib
    md5 = hashlib.md5(open(OWN_GBA, "rb").read()).hexdigest()
    srv = RaServer(md5)
    try:
        _t_ra(S, srv)
    finally:
        srv.stop()


def _t_ra(S, srv):
    ini = {"ra_host": srv.url()}
    # The password: Settings (the browser's), up from the top past Button
    # mapping, Game order and Hardcore to Account; X; the name, then the
    # password, each through the keyboard.
    # (1.2: one more UP, for Library > Game folders above Game order)
    r = S.run("ra_login", "60:start\n+30:up\n" + "+8:up\n" * 9 +
              "+10:shot=account.png\n+4:cross\n+2:type=ra_tester\n"
              "+20:type=hunter2pw\n+150:shot=signed_in.png\n+10:circle\n"
              "+30:quit\n", vita_ini=ini)
    S.clean(r)
    S.picture(r, "signed_in.png")
    S.check(r.has(r"^EVT ra_login ok=1 via=password rc=0 user=ra_tester"),
            "signed in with the password")
    S.check(srv.password_seen == 1, "the password went to the server once "
            "(%d)" % srv.password_seen)
    acct = os.path.join(r.root, "retroachievements.txt")
    kept = open(acct).read() if os.path.exists(acct) else ""
    S.check("user=ra_tester" in kept and "token=tok-1" in kept,
            "retroachievements.txt keeps the name and the token")
    leaked = [f for f in glob.glob(os.path.join(r.root, "**", "*"),
                                   recursive=True)
              if os.path.isfile(f) and b"hunter2pw" in open(f, "rb").read()]
    S.check(not leaked, "the password is in no file of the app's (%s)" % leaked)

    # Play: the token signs in, the game is found, A earns First Press, and
    # Settings > Achievements (in game: up past Button mapping and Display
    # settings) lists both.
    keep = {"retroachievements.txt": "user=ra_tester\ntoken=tok-1\n"
                                     "hardcore=0\n"}
    del srv.calls[:]
    r = S.run("ra_play", "60:cross\n+400:shot=loaded.png\n+10:cross:10\n"
              "+40:shot=earned.png\n+160:shot=after.png\n" + menu_pick(4) +
              "+20:up\n" + "+8:up\n" * 7 + "+10:cross\n+20:shot=list.png\n"
              "+10:circle\n+10:circle\n+10:circle\n+60:quit\n",
              vita_ini=ini, extra=keep)
    S.clean(r)
    S.check(r.has(r"^EVT ra_login ok=1 via=token"), "the kept token signs in")
    S.check(r.has(r"^EVT ra_game_begin hash=%s hardcore=0" % srv.md5) and
            r.has(r'^EVT ra_game ok=1 id=77 core=2 unlocked=0 hardcore=0 '
                  r'title="GBAdhoc Test"'),
            "the game is known by its MD5: two achievements")
    S.check(r.has(r'^EVT ra_unlock id=1001 points=5 title="First Press"'),
            "A earns First Press")
    award = [c for c in srv.calls if c.get("r") == "awardachievement"]
    S.check(len(award) == 1 and award[0].get("a") == "1001" and
            award[0].get("h") == "0", "...and it is sent, softcore (%s)" % award)
    S.check(not r.has(r"ra_unlock id=1002"), "the other one is not earned")
    S.check(r.has(r"^EVT ra_page open n=2"), "Settings > Achievements lists both")
    S.picture(r, "earned.png")
    S.picture(r, "list.png")

    # Hardcore: the same, and a state load is refused.
    keep = {"retroachievements.txt": "user=ra_tester\ntoken=tok-1\n"
                                     "hardcore=1\n"}
    del srv.calls[:]
    r = S.run("ra_hardcore", "60:cross\n+400:cross:10\n+200:shot=hc_earned.png\n" +
              menu_pick(1) + "+20:square\n+60:cross\n+30:shot=refused.png\n"
              "+10:circle\n+10:circle\n+60:quit\n", vita_ini=ini, extra=keep)
    S.clean(r)
    S.check(r.has(r"^EVT ra_game ok=1 id=77 core=2 unlocked=0 hardcore=1"),
            "Hardcore is in force for the game")
    S.check(r.has(r"^EVT hardcore_refused what=state_load via=menu") and
            not r.has(r"^EVT state_load slot="), "a state load is refused")
    award = [c for c in srv.calls if c.get("r") == "awardachievement"]
    S.check(len(award) == 1 and award[0].get("h") == "1",
            "the unlock is sent as hardcore (%s)" % award)
    S.picture(r, "refused.png")

    # No server: nothing hangs, the game plays.
    r = S.run("ra_offline", "60:cross\n+300:shot=game.png\n+10:quit\n",
              vita_ini={"ra_host": "http://127.0.0.1:9"}, extra=keep)
    S.clean(r)
    S.check(r.has(r"^EVT ra_http api=login2 status=-2"),
            "the sign-in that cannot reach the server is logged")
    S.picture(r, "game.png")


def t_save_import(S):
    """A save made elsewhere is taken in at the game's first start: from
    import/ (then renamed .imported), Game.srm beside the game, and for a
    Game Boy game Game.sav -- unless a Game.gba owns that name; an existing
    save is never replaced."""
    blob = bytes((i * 7 + 3) & 0xFF for i in range(32768))
    gb_blob = bytes((i * 5 + 1) & 0xFF for i in range(8192))
    # 1. import/ -> roms/gbadhoc-test.sav, the source renamed
    r = S.run("save_import_folder", "60:quit\n", autoboot="gbadhoc-test.gba",
              extra={"import/gbadhoc-test.srm": blob})
    S.clean(r)
    sav = os.path.join(r.root, "roms", "gbadhoc-test.sav")
    S.check(os.path.exists(sav) and open(sav, "rb").read()[:len(blob)] == blob,
            "the imported save is the game's save")
    S.check(os.path.exists(os.path.join(r.root, "import",
                                        "gbadhoc-test.srm.imported")) and
            not os.path.exists(os.path.join(r.root, "import", "gbadhoc-test.srm")),
            "the import/ file is renamed .imported")
    S.check(r.has(r'^EVT save_import from=.*where=import'), "the log says where from")
    S.check(r.index(r"^EVT save_import ") < r.index(r"^EVT sram_load size=32768"),
            "taken in before the core loads the battery")
    # 2. Game.srm beside the game (RetroArch's name)
    r = S.run("save_import_srm", "60:quit\n", autoboot="gbadhoc-test.gba",
              extra={"roms/gbadhoc-test.srm": blob})
    S.clean(r)
    S.check(open(os.path.join(r.root, "roms", "gbadhoc-test.sav"), "rb")
            .read()[:len(blob)] == blob, ".srm beside the game is taken in")
    S.check(os.path.exists(os.path.join(r.root, "roms", "gbadhoc-test.srm")),
            "...and left where it was")
    # 3. an existing save wins
    r = S.run("save_import_keep", "60:quit\n", autoboot="gbadhoc-test.gba",
              extra={"roms/gbadhoc-test.sav": b"\x11" * 32768,
                     "import/gbadhoc-test.sav": blob})
    S.clean(r)
    S.check(not r.has(r"^EVT save_import "), "nothing taken when there is a save")
    S.check(os.path.exists(os.path.join(r.root, "import", "gbadhoc-test.sav")),
            "the import/ file is left alone")
    # 4. Game Boy: Game.sav -> Game.gb.sav; not when Game.gba is beside it
    junk_gb = bytes(0x8000)
    allow = r"boot_failed|boot_refused|notice title|gbcore_create FAILED"
    r = S.run("save_import_gb", "40:cross\n+40:quit\n", autoboot="mono.gb",
              extra={"roms/mono.gb": junk_gb, "roms/mono.sav": gb_blob})
    S.check(os.path.exists(os.path.join(r.root, "roms", "mono.gb.sav")) and
            open(os.path.join(r.root, "roms", "mono.gb.sav"), "rb").read() == gb_blob,
            "a Game Boy game takes Game.sav as Game.gb.sav")
    r = S.run("save_import_gb_zip", "40:cross\n+40:quit\n", autoboot="three.gb",
              extra={"roms/three.gb": junk_gb, "roms/three.zip": b"PK",
                     "roms/three.sav": gb_blob})
    S.check(not os.path.exists(os.path.join(r.root, "roms", "three.gb.sav")),
            "...nor when a zipped Game.zip is beside it")
    r = S.run("save_import_gb_twin", "40:cross\n+40:quit\n", autoboot="two.gb",
              extra={"roms/two.gb": junk_gb, "roms/two.gba": open(OWN_GBA, "rb").read(),
                     "roms/two.sav": gb_blob})
    S.check(not os.path.exists(os.path.join(r.root, "roms", "two.gb.sav")),
            "...but not when Game.gba owns Game.sav")


def t_save_sync(S):
    """A save from RetroArch keeps its size and trailer, goes back to
    RetroArch's file while that is unchanged, is not written over a file
    RetroArch changed, and the newer one is used at the next start (the other
    kept in backups/); Settings > Library > Sync RetroArch saves."""
    rom = "gbadhoc-test"
    ra = "retroarch/savefiles/%s.srm" % rom
    press = "100:a:10\n+200:quit\n"          # the test cartridge writes 64 bytes
    def pat(n, seed):
        return bytes((i * 7 + seed) & 0xFF for i in range(n))
    def rd(r, rel):
        return open(os.path.join(r.root, rel), "rb").read()
    def ours(r):
        return rd(r, "roms/%s.sav" % rom)
    # 1. 131072 bytes (a flash game's size): taken in, played, written back full size
    src = pat(131072, 3)
    r = S.run("save_sync_full", press, autoboot=rom + ".gba", extra={ra: src})
    S.clean(r)
    S.check(r.has(r"^EVT save_import from=.*where=RetroArch"), "taken from RetroArch's folder")
    S.check(r.has(r"^EVT save_sync_link "), "the source is remembered")
    S.check(r.has(r"^EVT save_backup kept .*first time"), "one backup before the first write")
    a, b = ours(r), rd(r, ra)
    S.check(len(a) == 131072 and len(b) == 131072, "full size in both (%d, %d)" % (len(a), len(b)))
    S.check(a[:64] != src[:64] and a == b, "the game's save went back to RetroArch's file")
    S.check(a[64:] == src[64:], "the rest of the save is as it was")
    S.check(len(os.listdir(os.path.join(r.root, "backups"))) == 1, "one backup copy in backups/")
    S.check(not os.path.exists(os.path.join(r.root, "roms", rom + ".sav.bak")),
            "nothing is left beside the game")
    S.check(r.has(r"^EVT save_sync_out ok "), "the log says it was written back")
    # 2. 131088 bytes (RTC trailer): kept in both files
    tail = bytes(range(0xA0, 0xB0))
    src = pat(131072, 9) + tail
    r = S.run("save_sync_tail", press, autoboot=rom + ".gba", extra={ra: src})
    S.clean(r)
    a, b = ours(r), rd(r, ra)
    S.check(len(a) == 131088 and a[-16:] == tail, "the save keeps its 16 trailer bytes")
    S.check(b == a, "and RetroArch's file is the same 131088 bytes")
    # 3. a 32768-byte source stays 32768 (the save itself is the core's 128 KiB)
    src = pat(32768, 5)
    r = S.run("save_sync_32k", press, autoboot=rom + ".gba", extra={ra: src})
    S.clean(r)
    b = rd(r, ra)
    S.check(len(b) == 32768 and b[:64] == ours(r)[:64] and b[64:] == src[64:],
            "a 32 KiB source stays 32 KiB with the new save in it")
    # 4. RetroArch changed its file since: the newer one is used, ours kept in backups/
    root = r.root
    newer = pat(32768, 77)
    p = os.path.join(root, ra)
    open(p, "wb").write(newer)
    t = time.time() + 600
    os.utime(p, (t, t))
    before = ours(r)
    r = S.run("save_sync_32k", "60:quit\n", autoboot=rom + ".gba", prepared=True)
    S.clean(r)
    S.check(r.has(r"^EVT save_sync_conflict .*take=source"), "the log says RetroArch's is taken")
    S.check(ours(r)[:32768] == newer, "RetroArch's newer save is the game's save now")
    kept = [open(os.path.join(root, "backups", f), "rb").read()
            for f in os.listdir(os.path.join(root, "backups"))]
    S.check(before in kept, "the save it replaced is kept in backups/")
    # 5. changed, but older than ours: ours kept, RetroArch's copy backed up, then updated
    old = pat(32768, 99)
    open(p, "wb").write(old)
    t = time.time() - 86400
    os.utime(p, (t, t))
    mine = ours(r)
    r = S.run("save_sync_32k", "60:quit\n", autoboot=rom + ".gba", prepared=True)
    S.clean(r)
    S.check(r.has(r"^EVT save_sync_conflict .*take=ours"), "ours is the newer one")
    S.check(ours(r) == mine, "the game's save is untouched")
    kept = [open(os.path.join(root, "backups", f), "rb").read()
            for f in os.listdir(os.path.join(root, "backups"))]
    S.check(old in kept, "RetroArch's older file is kept in backups/")
    S.check(rd(r, ra) == mine[:32768], "and RetroArch's file now has the newer save")
    # 6. Sync off: written nowhere near RetroArch
    src = pat(32768, 13)
    r = S.run("save_sync_off", press, autoboot=rom + ".gba", extra={ra: src},
              vita_ini={"sync_ra_saves": 0})
    S.clean(r)
    S.check(rd(r, ra) == src, "with the setting off RetroArch's file is not touched")
    S.check(not r.has(r"^EVT save_sync_out "), "...and nothing was written back")
    S.check(ours(r)[64:32768] == src[64:], "the save was still imported")
    # 7. the Settings row (Library, above Game folders): default on, toggles, persists
    walk = "60:start\n+30:up\n" + "+8:up\n" * 6 + "+10:shot=row.png\n"
    r = S.run("save_sync_row", walk + "+4:right\n+20:circle\n+30:quit\n")
    S.clean(r)
    S.check(r.has(r"^EVT sync_ra_saves on=0 saved=1"), "the row turns it off and saves it")
    S.check(re.search(r"^sync_ra_saves = 0$", open(os.path.join(r.root, "vita.ini")).read(), re.M),
            "vita.ini holds it")
    r = S.run("save_sync_row", walk + "+4:right\n+20:circle\n+30:quit\n", prepared=True)
    S.clean(r)
    S.check(r.has(r"^EVT sync_ra_saves on=1 saved=1"), "and back on (it was off when read again)")


def t_save_gb_keep(S):
    """A Game Boy save with bytes after the cartridge RAM (another emulator's
    trailer) keeps them when the game saves; a RetroArch .srm of the plain RAM
    is written back at its own size.  (A tiny cartridge made here: MBC1+RAM+
    battery, writes 0x55 to RAM at once.)"""
    rom = bytearray(0x8000)
    rom[0x100:0x103] = bytes([0xC3, 0x50, 0x01])
    rom[0x150:0x15C] = bytes([0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x55,
                              0xEA, 0x00, 0xA0, 0x18, 0xFE])
    rom[0x147], rom[0x148], rom[0x149] = 0x03, 0, 0x02
    x = 0
    for i in range(0x134, 0x14D):
        x = (x - rom[i] - 1) & 0xFF
    rom[0x14D] = x
    rom = bytes(rom)
    for name, extra in (("short", bytes(range(1, 8))), ("long", bytes(range(100)) * 3)):
        r = S.run("save_gb_keep_" + name, "120:quit\n", autoboot="kept.gb",
                  extra={"roms/kept.gb": rom,
                         "roms/kept.gb.sav": b"\x11" * 8192 + extra})
        S.clean(r)
        d = open(os.path.join(r.root, "roms", "kept.gb.sav"), "rb").read()
        S.check(d[0] == 0x55 and d[1:8192] == b"\x11" * 8191,
                "%s: the game's save was written" % name)
        S.check(d[8192:] == extra, "%s: and the %d bytes after the RAM are kept"
                % (name, len(extra)))
    r = S.run("save_gb_keep_srm", "120:quit\n", autoboot="kept.gb",
              extra={"roms/kept.gb": rom,
                     "retroarch/savefiles/Gambatte/kept.srm": b"\x22" * 8192})
    S.clean(r)
    d = open(os.path.join(r.root, "retroarch", "savefiles", "Gambatte", "kept.srm"), "rb").read()
    S.check(len(d) == 8192 and d[0] == 0x55 and d[1:] == b"\x22" * 8191,
            "a Game Boy .srm of the RAM is written back at 8192 bytes")
def t_save_clock(S):
    """ROM hacks and the Pokemon clock: the cartridge clock is the console's
    (rtc_wall, fe_host.c), a 128 KiB save is read as 128 KiB flash even when
    the ROM says otherwise, and Settings > THIS GAME > Save type / Clock (RTC)
    are remembered per game and change what is detected."""
    own = open(OWN_GBA, "rb").read()
    big = bytes((i * 3 + 1) & 0xFF for i in range(131072))
    # 1. The Vita's clock at start-up reaches the cartridge (test hook:
    #    VITADHOC_FAKE_TIME, the console's local time as seconds since 1970).
    old = os.environ.get("VITADHOC_FAKE_TIME")
    for day, name in ((1791504000, "day1"), (1791504000 + 3 * 86400, "day4")):
        os.environ["VITADHOC_FAKE_TIME"] = str(day)
        try:
            r = S.run("save_clock_" + name, "60:quit\n",
                      autoboot="gbadhoc-test.gba")
        finally:
            if old is None:
                os.environ.pop("VITADHOC_FAKE_TIME", None)
            else:
                os.environ["VITADHOC_FAKE_TIME"] = old
        S.clean(r)
        ev = r.evt("rtc_wall")
        S.check(ev and abs(int(ev[0]["t"]) - day) <= 2,
                "%s: the cartridge clock starts at the console's time (%s)" %
                (name, ev[0]["t"] if ev else "no rtc_wall event"))
    # 2. Save type, Auto.  The test cartridge says "SRAM_V113"; a ROM hack
    #    that says nothing, or says the 64 KB flash library (gpsp#246).
    bare = own.replace(b"SRAM_V113", b"XRAM_V113")
    hack = own.replace(b"SRAM_V113", b"FLASH_V\0\0")
    def go(name, rom, save=None, ini=None):
        x = {"roms/hack.gba": rom}
        if save is not None:
            x["roms/hack.sav"] = save
        if ini:
            x["games/hack.gba.ini"] = ini
        r = S.run(name, "30:quit\n", autoboot="hack.gba", extra=x)
        S.clean(r)
        return r
    S.check(go("save_type_plain", own).has(r"^EVT save_type detected=sram$"),
            "the cartridge's own SRAM string stays SRAM")
    S.check(go("save_type_bare", bare).has(r"^EVT save_type detected=sram-default"),
            "no string and no save: the default")
    S.check(go("save_type_flash64", hack).has(r"^EVT save_type detected=flash64"),
            "a 64 KB flash string and no save: 64 KB flash")
    S.check(go("save_type_flash64_big", hack, big).has(
            r"^EVT save_type detected=flash128"),
            "...but with a 128 KiB save: 128 KB flash")
    S.check(go("save_type_bare_big", bare, big).has(
            r"^EVT save_type detected=flash128"),
            "no string and a 128 KiB save: 128 KB flash")
    S.check(go("save_type_trailer", hack, big + bytes(16)).has(
            r"^EVT save_type detected=flash128"),
            "a 128 KiB save with the 16-byte trailer: 128 KB flash")
    S.check(go("save_type_small", hack, big[:65536]).has(
            r"^EVT save_type detected=flash64"),
            "a 64 KiB save changes nothing")
    S.check(go("save_type_explicit_sram", own, big).has(
            r"^EVT save_type detected=sram$"),
            "an explicit SRAM string is not argued with")
    # 3. The per-game file overrides it, whatever the save looks like.
    for val, want in ((1, "sram"), (2, "flash64"), (3, "flash128"),
                      (4, "eeprom512"), (5, "eeprom8k")):
        r = go("save_type_ini%d" % val, hack, big, "save_type = %d\n" % val)
        S.check(r.has(r"^EVT save_type detected=%s" % want),
                "save_type = %d gives %s" % (val, want))
    # 4. The private Emerald dump (a copy; flash 128 KB and a clock), if it
    #    is on this machine.
    emr = "/w/roms/Pokemon - Emerald Version (USA, Europe).gba"
    esv = "/w/roms/Pokemon Emerald All Shiny Fixed.sav"
    if os.path.exists(emr) and os.path.exists(esv):
        rom, sav = open(emr, "rb").read(), open(esv, "rb").read()
        r = S.run("save_type_emerald", "60:quit\n", autoboot="emerald.gba",
                  extra={"roms/emerald.gba": rom, "roms/emerald.sav": sav})
        S.clean(r)
        S.check(r.has(r"^EVT save_type detected=flash128") and
                r.has(r"^EVT save_type choice=0 file_size=131072"),
                "Emerald with its 131072-byte save: 128 KB flash")
        S.check(open(os.path.join(r.root, "roms", "emerald.sav"), "rb").read()
                [:len(sav)] == sav, "...and the save came back byte for byte")
        r = S.run("save_type_emerald_sram", "60:quit\n", autoboot="emerald.gba",
                  extra={"roms/emerald.gba": rom, "roms/emerald.sav": sav,
                         "games/emerald.gba.ini": "save_type = 1\n"})
        S.check(r.has(r"^EVT save_type detected=sram"),
                "a chosen type beats detection on the same ROM")
    else:
        S.note("no private Emerald copy here: that leg is skipped")
    # 5. The rows: Save type and Clock (RTC) sit at the top of THIS GAME;
    #    from the last row (Display settings) UP twice.  X steps a value.
    r = S.run("save_type_rows", menu_pick(4, wait=60) +
              "+20:up\n" + "+8:up\n" * 6 + "+10:shot=rows.png\n"
              "+4:cross\n+4:cross\n+8:down\n+4:cross\n+10:circle\n"
              "+10:circle\n+30:quit\n", autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.picture(r, "rows.png")
    g = open(os.path.join(r.root, "games", "gbadhoc-test.gba.ini")).read() \
        if os.path.exists(os.path.join(r.root, "games",
                                       "gbadhoc-test.gba.ini")) else ""
    S.check(re.search(r"^save_type = 2$", g, re.M),
            "Save type stepped twice (Auto > SRAM > Flash 64 KB) is saved (%r)"
            % g[:120])
    S.check(re.search(r"^rtc = 1$", g, re.M), "Clock (RTC) On is saved")
    r = S.run("save_type_rows_again", "30:quit\n", autoboot="gbadhoc-test.gba",
              extra={"games/gbadhoc-test.gba.ini": g,
                     "roms/gbadhoc-test.sav": big})
    S.clean(r)
    S.check(r.has(r"^EVT game_settings_clock save_type=2 rtc=1") and
            r.has(r"^EVT save_type detected=flash64"),
            "the next start uses them")


def t_launch_param(S):
    """A launcher's param= starts its game directly, the path taken exactly
    as written (GitHub #3); a missing or wrong file is a notice and the list."""
    own = open(OWN_GBA, "rb").read()
    odd = "Odd & Name %20 +1.gba"
    r = S.run("launch_param", "60:quit\n", args=("--launch-param", odd),
              extra={"roms/" + odd: own})
    S.clean(r)
    S.check(r.has(r'^EVT launch_param raw="Odd & Name %20 \+1\.gba"'),
            "the parameter arrives undecoded")
    S.check(r.has(r"^EVT launch rom=.*Odd & Name %20 \+1\.gba console=GBA"),
            "and starts that game as a GBA game")
    S.check(not r.has(r"^EVT ui_browser "), "without the game list first")
    full = os.path.join(S.outdir, "launch_param", "root", "roms", odd)
    r = S.run("launch_param_full", "60:quit\n", args=("--launch-param", full),
              extra={"roms/" + odd: own})
    S.clean(r)
    S.check(r.has(r"^EVT launch rom=" + re.escape(full)), "a whole path works too")
    for name, why in (("missing.gba", "missing"), ("notes.txt", "not_a_game"),
                      ("x" * 600 + ".gba", "too_long")):
        r = S.run("launch_param_" + why, "40:cross\n+60:quit\n",
                  args=("--launch-param", name), extra={"roms/notes.txt": "x"},
                  autoboot="gbadhoc-test.gba")
        S.clean(r, allow=r"launch_param refused|notice title")
        S.check(r.has(r"^EVT launch_param refused reason=" + why), why)
        S.check(r.has(r"^EVT ui_browser "), why + ": then the game list")
        S.check(not r.has(r"^EVT autoboot rom="),
                why + ": not autoboot.txt's game instead")
        S.check(len([l for l in r.log if "launch_param raw" in l]) <= 1,
                why + ": asked once")


def t_folders_config(S):
    """extra_roms in config.ini (where players look for it) and gba_roms /
    gb_roms / gbc_roms add folders to the game list."""
    out = os.path.join(S.outdir, "folders_config")
    root = os.path.join(out, "root")
    A, B = os.path.join(root, "fa"), os.path.join(root, "fb")
    own = open(OWN_GBA, "rb").read()
    S.make_root(root, {"extra_roms": A, "gbc_roms": B}, None, None,
                {"fa/Folder A.gba": own, "fb/Folder B.gba": own})
    r = S.run("folders_config", "60:quit\n", prepared=True)
    S.clean(r)
    rr = r.evt("rom_roots")
    S.check(rr and rr[0].get("source") == "config" and rr[0].get("extra") == "2",
            "both folders are read (%s)" % (rr[0]["_"] if rr else "none"))


def t_shot_dir(S):
    """vita.ini screenshots = <folder>: screenshots go there (made if
    missing), named after the game, the date and the time."""
    out = os.path.join(S.outdir, "shot_dir")
    root = os.path.join(out, "root")
    want = os.path.join(out, "pictures")
    shutil.rmtree(want, ignore_errors=True)
    r = S.run("shot_dir", "150:l+r+select:3\n+40:quit\n",
              autoboot="gbadhoc-test.gba", vita_ini={"screenshots": want + "/"})
    S.clean(r)
    shots = glob.glob(os.path.join(want, "gbadhoc-test-*.png"))
    S.check(len(shots) == 1 and Image.open(shots[0]).size == (240, 160),
            "one screenshot in the chosen folder (%s)" % shots)
    S.check(not glob.glob(os.path.join(r.root, "screenshots", "*.png")),
            "and none in screenshots/")


def t_zip(S):
    """A game in a .zip (deflated or stored) is listed under its console,
    unpacked once into unzipped/, started, and saves beside the .zip; a zip
    with no game or a broken CRC is refused with a notice."""
    import io, zipfile
    own = open(OWN_GBA, "rb").read()
    def zipped(name, data, method=zipfile.ZIP_DEFLATED):
        b = io.BytesIO()
        with zipfile.ZipFile(b, "w", method) as z:
            z.writestr("readme.txt", "hello")
            z.writestr(name, data)
        return b.getvalue()
    deflated = zipped("Zipped Game (USA).gba", own)
    stored = zipped("Stored.gba", own, zipfile.ZIP_STORED)
    # autoboot the deflated one, press A (battery write), quit
    r = S.run("zip", "150:a:40\n+60:quit\n", autoboot="Zipped.zip",
              extra={"roms/Zipped.zip": deflated, "roms/Stored.zip": stored,
                     "roms/Empty.zip": zipped("notes.txt", b"x")})
    S.clean(r)
    S.check(r.has(r"^EVT zip_unpack rc=0 entry=\"Zipped Game \(USA\)\.gba\" method=8"),
            "the deflated cartridge is unpacked")
    up = os.path.join(r.root, "unzipped", "Zipped.gba")
    S.check(os.path.exists(up) and open(up, "rb").read() == own,
            "unzipped/Zipped.gba is the cartridge, byte for byte")
    S.check(r.has(r"^EVT game rom=.*Zipped\.zip .*save=.*roms/Zipped\.sav"),
            "its save is named after the zip, beside it")
    S.check(os.path.exists(os.path.join(r.root, "roms", "Zipped.sav")),
            "and written there")
    # the list: both zips among the GBA games, the empty one not
    r = S.run("zip_list", "90:quit\n",
              extra={"roms/Zipped.zip": deflated, "roms/Stored.zip": stored,
                     "roms/Empty.zip": zipped("notes.txt", b"x")})
    S.clean(r)
    done = [d for d in r.evt("ui_browser") if d.get("console") == "0"]
    S.check(done and int(done[-1]["roms"]) == len(S.roms(r.root, ".gba")) + 2,
            "the two zipped GBA games are in the GBA list (%s)" %
            (done[-1]["_"] if done else "no list"))
    # stored, reused on the second start
    for i in range(2):
        r = S.run("zip_stored", "60:quit\n", autoboot="Stored.zip",
                  extra={"roms/Stored.zip": stored},
                  prepared=True if i else None)
        S.clean(r)
    S.check(r.has(r"^EVT zip_unpack reused=1"), "the second start reuses the copy")
    # broken CRC
    bad = bytearray(stored)
    k = bad.find(own[0x100:0x110])
    bad[k] ^= 0xFF
    r = S.run("zip_crc", "40:cross\n+40:quit\n", autoboot="Bad.zip",
              extra={"roms/Bad.zip": bytes(bad)})
    S.clean(r, allow=r"boot_refused|notice title|zip_unpack rc=-2")
    S.check(r.has(r"boot_refused reason=zip_crc"), "a damaged zip is refused")


def t_folders_ui(S):
    """Settings > Library > Game folders: a folder typed on the keyboard is
    added (written to vita.ini, the list scanned again with its games), and
    typed again it is removed."""
    out = os.path.join(S.outdir, "folders_ui")
    root = os.path.join(out, "root")
    A = os.path.join(root, "mine")
    own = open(OWN_GBA, "rb").read()
    S.make_root(root, None, None, {"extra_roms": 0},
                {"mine/Folder Game.gba": own})
    # the keyboard takes 63 characters: a short name for the folder
    A = "/tmp/vitadhoc-folders-ui-%d" % os.getpid()
    if os.path.lexists(A):
        os.remove(A)
    os.symlink(os.path.join(root, "mine"), A)
    # START: Settings; UP wraps to Button mapping, then Game order, then
    # Game folders
    walk = "60:start\n+30:up\n+8:up\n+8:up\n+8:up\n+8:up\n+8:up\n+10:shot=row.png\n+4:cross\n"
    r = S.run("folders_ui", walk + "+4:type=%s\n+40:circle\n+90:quit\n" % A,
              prepared=True)
    S.clean(r)
    S.check(r.has(r'^EVT rom_roots_edit folder="%s" rc=1 extra=1' % re.escape(A)),
            "the folder is added")
    ini = open(os.path.join(root, "vita.ini")).read()
    S.check(re.search(r"^extra_roms = %s$" % re.escape(A), ini, re.M),
            "vita.ini holds it (%r)" % ini)
    b = [d for d in r.evt("library_refresh_done") if d.get("console") == "0"]
    S.check(b and int(b[-1]["roms"]) == len(S.roms(root, ".gba")) + 1,
            "its game is in the list after the rescan (%s)" %
            (b[-1]["_"] if b else "none"))
    r = S.run("folders_ui", walk + "+4:type=%s\n+40:circle\n+90:quit\n" % A,
              prepared=True)
    S.clean(r)
    S.check(r.has(r'^EVT rom_roots_edit folder="%s" rc=2 extra=0' % re.escape(A)),
            "typed again, it is removed")
    S.check(re.search(r"^extra_roms = 0$",
                      open(os.path.join(root, "vita.ini")).read(), re.M),
            "vita.ini says no extra folders")


def t_overlay(S):
    """overlays/gba.png is drawn over the whole screen, with its own
    transparency; a game's own overlays/<game>.png wins; overlays = 0 in
    vita.ini turns them off."""
    import io
    def png(color):
        im = Image.new("RGBA", (960, 544), (0, 0, 0, 0))
        im.paste(Image.new("RGBA", (120, 80), color), (0, 0))
        b = io.BytesIO(); im.save(b, "PNG"); return b.getvalue()
    red, green = png((255, 0, 0, 255)), png((0, 255, 0, 255))
    for name, extra, ini, want in (
            ("overlay_console", {"overlays/gba.png": red}, None, (255, 0, 0)),
            ("overlay_game", {"overlays/gba.png": red,
                              "overlays/gbadhoc-test.png": green}, None, (0, 255, 0)),
            ("overlay_off", {"overlays/gba.png": red}, {"overlays": 0}, None)):
        r = S.run(name, "150:shot=game.png\n+10:quit\n", autoboot="gbadhoc-test.gba",
                  extra=extra, vita_ini=ini)
        S.clean(r)
        px = r.shot("game.png").getpixel((20, 20))
        if want:
            S.check(px[:3] == want, "%s: the overlay is on screen (%s)" % (name, px))
            S.check(r.shot("game.png").getpixel((480, 272)) != want,
                    "%s: and the game shows through it" % name)
        else:
            S.check(px[:3] != (255, 0, 0) and not r.has(r"^EVT overlay path="),
                    "%s: no overlay" % name)


def t_ten_slots(S):
    """Ten save-state slots: the page scrolls past the fifth, a state saved
    in slot 8 is Game.st7 with its pictures, and loads back."""
    script = ("150:a:20\n" + menu_pick(1) +
              "+20:down\n" * 7 +                     # slot 8
              "+20:shot=slot8.png\n+10:square\n+60:shot=saved8.png\n"
              "+10:cross\n+60:shot=loaded.png\n+10:quit\n")
    r = S.run("ten_slots", script, autoboot="gbadhoc-test.gba")
    S.clean(r)
    st = os.path.join(r.root, "states", "gba", "gbadhoc-test.st7")
    S.check(os.path.exists(st) and os.path.getsize(st) > 0, "slot 8 is .st7")
    S.check(os.path.exists(st + ".thumb") and os.path.exists(st + ".shot"),
            "with its pictures")
    S.check(r.has(r"^EVT state_save slot=8 ") and
            r.has(r"^EVT state_load slot=8 via=\S+ rc=0"),
            "saved and loaded in slot 8")
    S.picture(r, "slot8.png")


def t_sidecars(S):
    """Game folders stay clean (psp/sidecar.h): states, their pictures,
    Continue and the play-time note go under states/<console>/; a cover's
    picture cache goes under cache/; what older versions left beside the ROM
    is moved there, still loads, and Continue works after it."""
    home = {"ui_home": 1, "last_rom": "gbadhoc-test.gba"}
    # one run: a state in slot 2, a play, leaving to the list (Continue)
    r = S.run("sidecar_make", "60:cross\n+150:a:20\n" + menu_pick(1) +
              "+20:down\n+10:square\n+30:circle\n+10:circle\n+60:right:20\n" +
              QUIT_TO_LIST + "+150:quit\n", config=home)
    S.clean(r)
    stem = "gbadhoc-test"
    roms = os.path.join(r.root, "roms")
    S.check(only_beside(roms, stem, {stem + ".gba", stem + ".sav"}) == [],
            "roms/ holds the ROM and its .sav alone after states, Continue "
            "and play time (%s)" % only_beside(roms, stem,
                                               {stem + ".gba", stem + ".sav"}))
    sd = os.path.join(r.root, "states", "gba")
    want = [stem + ".rsm", stem + ".rsm.pt", stem + ".st1",
            stem + ".st1.shot", stem + ".st1.thumb"]
    have = sorted(os.listdir(sd)) if os.path.isdir(sd) else []
    S.check(all(n in have for n in want),
            "states/gba/ has %s (has %s)" % (want, have))
    S.check(os.path.getsize(os.path.join(sd, stem + ".rsm")) > 100000,
            "Continue's point is a whole state")
    # what is in the screenshot and art folders is the player's own
    S.check(not glob.glob(os.path.join(r.root, "boxart", "*.t2.565")) and
            not glob.glob(os.path.join(r.root, "screenshots", "*.565")),
            "no cache files in boxart/ or screenshots/")

    # the same files as 1.2 left them, beside the ROM
    legacy = {}
    for n in have:
        legacy["roms/" + n] = open(os.path.join(sd, n), "rb").read()
    legacy["boxart/%s.t2.565" % stem] = b"565R" + bytes(12) + b"\x00\x00"
    # (a) Continue from the home screen
    r2 = S.run("sidecar_legacy_continue", "60:cross\n+20:shot=back.png\n+10:quit\n",
               config=home, extra=legacy)
    S.clean(r2)
    S.check(r2.has(r"^EVT state_load path=.*states/gba/gbadhoc-test.rsm via=continue rc=0"),
            "old Continue point beside the ROM: moved, and CROSS continues")
    S.check(only_beside(os.path.join(r2.root, "roms"), stem,
                        {stem + ".gba", stem + ".sav"}) == [],
            "...and nothing of it is left in roms/ (%s)" % only_beside(
                os.path.join(r2.root, "roms"), stem,
                {stem + ".gba", stem + ".sav"}))
    S.check(os.path.exists(os.path.join(r2.root, "states", "gba", stem + ".st1"))
            and os.path.exists(os.path.join(r2.root, "cache", "boxart",
                                            stem + ".t2.565")) and
            not os.path.exists(os.path.join(r2.root, "boxart", stem + ".t2.565")),
            "old states moved under states/, the old cover thumbnail to cache/")
    # (b) a state slot from the menu of a game opened directly
    r3 = S.run("sidecar_legacy_slot", "150:a:20\n" + menu_pick(1) +
               "+20:down\n+10:cross\n+60:quit\n",
               autoboot="gbadhoc-test.gba", extra=legacy)
    S.clean(r3)
    S.check(r3.has(r"^EVT sidecar_migrate rom=.*gbadhoc-test.gba moved=\d+ copied=0 failed=0"),
            "opening the game logs the move")
    S.check(r3.has(r"^EVT state_load slot=2 via=\S+ rc=0"),
            "the moved state in slot 2 loads")
    # (c) a new file is never overwritten by an old one
    keep = dict(legacy)
    r4 = S.run("sidecar_keep", "100:quit\n", autoboot="gbadhoc-test.gba",
               extra=dict(keep, **{"states/gba/" + stem + ".st1": b"mine"}))
    S.clean(r4)
    S.check(open(os.path.join(r4.root, "states", "gba", stem + ".st1"),
                 "rb").read() == b"mine" and
            os.path.exists(os.path.join(r4.root, "roms", stem + ".st1")),
            "a state already under states/ is not overwritten; the old one stays")


def t_display_rows(S):
    """Settings > Display > Grid strength (1-10) and Screen overlay are rows,
    saved to vita.ini and applied at once; Grid strength 10 darkens the LCD
    grid."""
    # browser Settings: 9 DOWN to Screen size, 7 more to Grid strength
    walk = "60:start\n" + "+8:down\n" * 16 + "+20:shot=grid.png\n"
    r = S.run("display_rows", walk + "+4:right\n+10:down\n+10:shot=overlay.png\n"
              "+4:cross\n+20:circle\n+30:quit\n")
    S.clean(r)
    S.check(r.has(r"^EVT grid_strength level=6 saved=1"), "Grid strength 5 -> 6")
    S.check(r.has(r"^EVT overlays on=0 saved=1"), "Screen overlay on -> off")
    ini = open(os.path.join(r.root, "vita.ini")).read()
    S.check(re.search(r"^grid_strength = 6$", ini, re.M) and
            re.search(r"^overlays = 0$", ini, re.M), "both kept in vita.ini (%r)" % ini)
    # Settings > Interface > Screenshot folder, typed on the keyboard
    shots = "/tmp/vad-shots-%d" % os.getpid()
    shutil.rmtree(shots, ignore_errors=True)
    r = S.run("shot_dir_row", "60:start\n" + "+8:down\n" * 23 +
              "+20:shot=row.png\n+4:cross\n+4:type=%s\n+30:circle\n+30:quit\n" % shots)
    S.clean(r)
    S.check(r.has(r'^EVT screenshot_dir set path="%s" saved=1' % re.escape(shots)) and
            os.path.isdir(shots), "the folder is set, made and kept")
    S.check(re.search(r"^screenshots = %s$" % re.escape(shots),
                      open(os.path.join(r.root, "vita.ini")).read(), re.M),
            "vita.ini holds it")
    shutil.rmtree(shots, ignore_errors=True)
    # the look itself: the grid at 1 and at 10 on the same frame
    dark = []
    for level in (1, 10):
        r = S.run("grid_level_%d" % level, "150:shot=g.png\n+5:quit\n",
                  autoboot="gbadhoc-test.gba",
                  config={"filter_gba": 3, "quality_gba": 0, "scale_gba": 1},
                  vita_ini={"grid_strength": level})
        S.clean(r)
        im = r.shot("g.png").convert("L")
        dark.append(sum(im.tobytes()))
    S.check(dark[1] < dark[0], "a stronger grid is darker (%s)" % dark)


def t_cpu_speed(S):
    """Settings > CPU speed (cpuclock.c): 500 MHz by default, 444 as the
    other value; the system's refusal falls back to 444; with PSVshellPlus
    present (stand-in folders, GBADHOC_FAKE_UR0) our 28-byte profile is
    written when none exists, a player's own is never replaced at start, is
    backed up when they switch the row to 500 and put back on 444."""
    import tempfile
    good = bytes.fromhex("50535653" "30333030" "0b000000" "f4010000"
                         "de000000" "de000000" "a6000000")
    # the row, in game: Settings wraps upwards to Button mapping, Display
    # settings, CPU speed
    pick = menu_pick(4, wait=100) + "+20:up\n" + "+8:up\n" * 7 + "+10:shot=row.png\n"
    quit_ = "+4:circle\n+10:circle\n+30:quit\n"

    made = []

    def ur0(profile=None):
        d = tempfile.mkdtemp(prefix="vad-ur0-")
        made.append(d)
        os.makedirs(os.path.join(d, "tai"))
        open(os.path.join(d, "tai", "PSVshellPlus_Kernel.skprx"), "wb").close()
        prof = os.path.join(d, "data", "PSVshell", "profiles", "GBAD00001")
        if profile is not None:
            os.makedirs(os.path.dirname(prof))
            open(prof, "wb").write(profile)
        return d, prof

    # 1. a plugin that lets the app ask: 500 accepted, no profile written
    # (there is no PSVshell), measured clock logged at the start and 8 s in
    r = S.run("cpu_ask", pick + "+600:shot=late.png\n" + quit_, autoboot="gbadhoc-test.gba",
              env={"GBADHOC_FAKE_MHZ": "496"})
    S.clean(r)
    S.check(r.has(r"^EVT clocks: asked for 500 MHz, result 0"), "asks for 500")
    S.check(r.has(r"^EVT clocks: the ARM core measures 496 MHz \(the system "
                  r"reports \d+\) at start"), "the measured clock at start")
    S.check(r.has(r"^EVT clocks: the ARM core measures 496 MHz .* 8 s in"),
            "...and about 8 s in")
    S.picture(r, "row.png")

    # 2. refused, no plugin: falls back to 444, hint in the note
    r = S.run("cpu_refused", pick + quit_, autoboot="gbadhoc-test.gba",
              env={"GBADHOC_FAKE_REFUSE500": "1", "GBADHOC_FAKE_MHZ": "444"})
    S.clean(r)
    S.check(r.has(r"^EVT clocks: asked for 500 MHz, result -1") and
            r.has(r"^EVT clocks: asked for 444 MHz, result 0"),
            "500 refused, 444 asked for")
    S.check(r.has(r'^EVT clocks: note="Needs an overclock '
                  r'plugin such as PSVshell"'), "the plain hint")
    S.picture(r, "row.png")

    # 3. PSVshellPlus present, setting Fast: the profile is written; switching
    # to 444 takes it away again, and vita.ini keeps the setting
    d, prof = ur0()
    env = {"GBADHOC_FAKE_UR0": d, "GBADHOC_FAKE_REFUSE500": "1",
           "GBADHOC_FAKE_MHZ": "444"}
    r = S.run("cpu_profile", pick + "+4:cross\n+10:shot=row_std.png\n" + quit_,
              autoboot="gbadhoc-test.gba", env=env)
    S.clean(r)
    S.check(r.has(r"^EVT cpu_profile written"), "the profile is written")
    S.check(r.has(r"^EVT cpu_mhz setting=444 saved=1"), "444 chosen and saved")
    S.check(r.has(r"^EVT cpu_profile removed"), "our profile is removed")
    S.check(not os.path.exists(prof), "no profile left")
    ini = open(os.path.join(r.root, "vita.ini")).read()
    S.check(re.search(r"^cpu_mhz = 444$", ini, re.M), "vita.ini cpu_mhz = 444")
    S.picture(r, "row_std.png")
    # the first run alone: the bytes
    d1, prof1 = ur0()
    r = S.run("cpu_bytes", "60:quit\n", autoboot="gbadhoc-test.gba",
              env=dict(env, GBADHOC_FAKE_UR0=d1))
    S.clean(r)
    S.check(os.path.exists(prof1) and open(prof1, "rb").read() == good,
            "the profile is the 28 known-good bytes")
    # the saved 444 keeps the next start from writing
    d2, prof2 = ur0()
    r = S.run("cpu_std_boot", "60:quit\n", autoboot="gbadhoc-test.gba",
              vita_ini={"cpu_mhz": 444}, env=dict(env, GBADHOC_FAKE_UR0=d2))
    S.clean(r)
    S.check(r.has(r"^EVT clocks: asked for 444 MHz") and not os.path.exists(prof2),
            "setting 444: asks for 444, writes nothing")

    # 4. a player's own profile: kept at start, saved and replaced when the
    # row goes 444 -> 500, restored when it goes back to 444
    mine = b"PSVS0300" + bytes(range(20))
    d, prof = ur0(mine)
    r = S.run("cpu_keep", "60:quit\n", autoboot="gbadhoc-test.gba",
              env=dict(env, GBADHOC_FAKE_UR0=d))
    S.clean(r)
    S.check(open(prof, "rb").read() == mine and r.has(r"^EVT cpu_profile kept_players"),
            "a player's profile is left alone at start")
    d, prof = ur0(mine)
    r = S.run("cpu_backup", pick + "+4:cross\n+10:cross\n+10:shot=mid.png\n"
              "+4:cross\n+10:" + "shot=end.png\n" + quit_,
              autoboot="gbadhoc-test.gba", env=dict(env, GBADHOC_FAKE_UR0=d))
    S.clean(r)
    # 500 (boot, kept) -> 444 (nothing of ours to take) -> 500 (theirs saved,
    # ours written) -> 444 (theirs back)
    S.check(r.has(r"^EVT cpu_profile written path=\S+ replaced=1") and
            r.has(r"^EVT cpu_profile restored"), "saved, replaced, restored")
    S.check(open(prof, "rb").read() == mine, "their file is back, byte for byte")
    S.check(not os.path.exists(os.path.join(r.root, "psvshell_profile_backup.bin")),
            "the backup is cleared once restored")

    # 5. a place the app may not write (PSVshell's folder is a plain file
    # here, so no folder can be made): one line, a stamp
    d, prof = ur0()
    os.makedirs(os.path.join(d, "data"))
    open(os.path.join(d, "data", "PSVshell"), "wb").close()
    r = S.run("cpu_nowrite", "60:quit\n", autoboot="gbadhoc-test.gba",
              env=dict(env, GBADHOC_FAKE_UR0=d))
    S.clean(r)
    S.check(r.has(r"^EVT cpu_profile not_writable") and
            os.path.exists(os.path.join(r.root, "psvshell_nowrite.stamp")),
            "one line and a stamp")
    for x in made:
        shutil.rmtree(x, ignore_errors=True)


def t_controls_defaults(S):
    """1.3 first impressions: an unstretched default picture, one hint once,
    A and B following the Vita, no stick rewind and no screenshot combo until
    asked, and the three Settings rows that ask."""
    fresh = {"scale_gba": None, "scale_gb": None, "scale_gbc": None,
             "wide_gba": None, "hints": None, "stick_rewind": None,
             "bind_screenshot": None, "ambient_gba": None, "ambient_gb": None,
             "ambient_gbc": None}
    # -- the default picture of a game with no Wide view profile: the
    # console's own 3:2, the whole height, black bars, nothing stretched
    r = S.run("defaults_fresh",
              "150:shot=a.png\n+40:l+r+select:3\n+60:rstick-left:80\n+20:quit\n",
              config=fresh, autoboot="gbadhoc-test.gba")
    S.clean(r)
    S.check(r.has(r"^EVT wide mode=1 fill=0 margin=0 profile=0"),
            "Wide view is on for known games only: no margin for this one, "
            "and the menus' fill is bars")
    S.check(r.has(r"^EVT display_profile .*ambient=2"),
            "the glow beside the picture is on by default")
    im = S.picture(r, "a.png")
    if im:
        side = [sum(im.getpixel((x, y))) for x in (10, 40, 940) for y in
                (100, 300, 450)]
        S.check(sum(1 for v in side if v > 12) >= 6,
                "...so the sides of the screen are filled, not black (%s)" %
                side)
    # the picture itself, with the glow off to see its edges
    r2 = S.run("defaults_geometry", "150:shot=g.png\n170:quit\n",
               config=dict(fresh, ambient_gba=0), autoboot="gbadhoc-test.gba")
    S.clean(r2)
    im = S.picture(r2, "g.png")
    if im:
        row = [sum(im.getpixel((x, 300))) > 24 for x in range(960)]
        xs = [x for x, v in enumerate(row) if v]
        S.check(xs and xs[0] == 72 and xs[-1] == 887,
                "the picture is 816 wide (columns %s..%s of 960)" %
                (xs[0] if xs else "?", xs[-1] if xs else "?"))
        col = [sum(im.getpixel((100, y))) > 24 for y in range(544)]
        S.check(col[0] and col[543], "...and the whole 544 high: 3:2, "
                "not stretched sideways")
    S.check(not glob.glob(os.path.join(r.root, "screenshots", "*.png")),
            "L + R + Select takes no screenshot until Settings turns it on")
    S.check(not r.has(r"^EVT rewind (begin|end)"),
            "the right stick held left does not rewind by default")
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check("hints = 0" in ini and "stick_rewind = 0" in ini and
            "ab_buttons" not in ini,
            "config.ini: the hint is used up, the stick off, and no A/B "
            "choice made for the player")
    S.check(len([l for l in r.log if "hints shown" in l]) == 1,
            "one hint, once")
    r = S.run("defaults_fresh", "100:quit\n", autoboot="gbadhoc-test.gba",
              prepared=True)
    S.check(not r.has(r"hints shown"), "the second launch shows no hint")

    # -- the Rewind button still works with the stick off; the stick, on
    for name, cfg, keys, want in (
            ("defaults_rw_chord", {"stick_rewind": None}, "select+square", 1),
            ("defaults_rw_stick", {"stick_rewind": 1}, "rstick-left", 1),
            ("defaults_rw_none", {"stick_rewind": None}, "rstick-left", 0)):
        r = S.run(name, "300:%s:60\n420:quit\n" % keys, config=cfg,
                  autoboot="gbadhoc-test.gba")
        S.clean(r)
        S.check(bool(r.has(r"^EVT rewind begin")) == bool(want),
                "%s: rewind %s" % (name, "runs" if want else "does not run"))

    # -- A and B: the Vita's Enter button, the player's choice, and a layout
    # the player already made themselves
    def lit(name, cfg, env, keys):
        r = S.run(name, "100:%s:10\n+0:shot=k.png\n+0:%s:10\n+10:quit\n" %
                  (keys, keys), config=cfg, env=env,
                  autoboot="gbadhoc-test.gba")
        S.clean(r)
        return key_boxes(r.shot("k.png")), r
    circle = {"GBADHOC_ENTER_BUTTON": "circle"}
    got, r = lit("ab_cross", {}, None, "cross")
    S.check(got == "A" and r.has(r"^EVT ab_buttons circle_is_a=0 chosen=0"),
            "a Vita whose Enter is Cross: Cross is A (lit: %s)" % got)
    got, r = lit("ab_japan", {}, circle, "cross")
    S.check(got == "B" and r.has(r"^EVT ab_buttons circle_is_a=1 chosen=0"),
            "a Vita whose Enter is Circle: Cross is B (lit: %s)" % got)
    got, r = lit("ab_japan_circle", {}, circle, "circle")
    S.check(got == "A", "...and Circle is A (lit: %s)" % got)
    got, r = lit("ab_chosen_cross", {"ab_buttons": 0}, circle, "cross")
    S.check(got == "A" and r.has(r"chosen=1"),
            "Cross is A, chosen, wins over the Vita's Circle (lit: %s)" % got)
    got, r = lit("ab_chosen_circle", {"ab_buttons": 1}, None, "circle")
    S.check(got == "A", "Circle is A, chosen, on a Cross Vita (lit: %s)" % got)
    got, r = lit("ab_own_layout", {"bind_a": "CIRCLE", "bind_b": "CROSS"},
                 circle, "circle")
    S.check(got == "A" and r.has(r"circle_is_a=0"),
            "a player's own A/B mapping is not swapped by the Vita (lit: %s)"
            % got)
    # the menus follow: on the shelf Cross plays and Circle finds a game;
    # with Circle as A it is the other way round
    for name, cfg, want in (("ab_menu_cross", {"ab_buttons": 0}, "cross"),
                            ("ab_menu_circle", {"ab_buttons": 1}, "circle")):
        r = S.run(name, "60:%s\n+120:quit\n" % want, config=cfg)
        S.check(r.has(r"^EVT session game=1"),
                "%s: %s plays the highlighted game" % (name, want))
    r = S.run("ab_menu_other", "60:cross\n+60:quit\n", config={"ab_buttons": 1})
    S.check(not r.has(r"^EVT session game="),
            "with Circle as A, Cross does not play")

    # -- the three rows, at the end of Settings (UP from the top wraps to
    # Button mapping, then Screenshot button combo, Rewind with right stick,
    # A and B buttons)
    walk = ("60:start\n" + "+8:up\n" * 4 + "+10:shot=ab.png\n+4:right\n"
            "+10:shot=ab2.png\n+8:down\n+4:right\n+10:shot=stick.png\n"
            "+8:down\n+4:right\n+10:shot=combo.png\n+20:quit\n")
    r = S.run("controls_rows", walk, config=fresh)
    S.clean(r)
    S.check(r.has(r"^EVT ab_buttons_setting value=1") and
            r.has(r"^EVT stick_rewind_setting on=1") and
            r.has(r"^EVT screenshot_combo_setting on=1"),
            "each row changes its setting")
    ini = open(os.path.join(r.root, "config.ini")).read()
    S.check(re.search(r"^ab_buttons = 1$", ini, re.M) and
            re.search(r"^stick_rewind = 1$", ini, re.M) and
            re.search(r"^bind_screenshot = SELECT\+L\+R$", ini, re.M),
            "and config.ini keeps all three")
    for n in ("ab.png", "ab2.png", "stick.png", "combo.png"):
        S.picture(r, n)
    # ...and the screenshot row switches it off again
    r = S.run("controls_rows", "60:start\n" + "+8:up\n" * 2 +
              "+4:right\n+10:shot=off.png\n+20:quit\n", prepared=True)
    S.clean(r)
    S.check(r.has(r"^EVT screenshot_combo_setting on=0") and
            re.search(r"^bind_screenshot = NONE$",
                      open(os.path.join(r.root, "config.ini")).read(), re.M),
            "Screenshot button combo off again, kept as NONE")


def t_snes_library(S):
    """SNES games on the card, no setup: every folder under ux0:ROMS (and
    ux0:roms) is a game folder, so "ux0:ROMS/SNES ROM Collection/*.smc" is on
    the SNES shelf; ._ and dot files are left out; picking one starts the SNES
    program (the twin only logs the launch) and never writes anything."""
    cwd = os.path.join(S.outdir, "snes_library_cwd")
    shutil.rmtree(cwd, ignore_errors=True)
    folder = os.path.join(cwd, "ux0:ROMS", "SNES ROM Collection")
    os.makedirs(folder)
    os.makedirs(os.path.join(cwd, "ux0:roms", "Other Name", "Deeper"))
    junk = b"\x00\x05\x16\x07" + bytes(4092)
    tiny = bytes(0x8000)
    for rel, data in (("SNES ROM Collection/Game One (USA).smc", tiny),
                      ("SNES ROM Collection/Game Two (Europe).sfc", tiny),
                      ("SNES ROM Collection/._Game One (USA).smc", junk),
                      ("SNES ROM Collection/.Hidden Game.smc", tiny),
                      ("SNES ROM Collection/Game One (USA).srm", b"keep me"),
                      ("SNES ROM Collection/Game One (USA).zst", b"state")):
        with open(os.path.join(cwd, "ux0:ROMS", rel), "wb") as f:
            f.write(data)
    with open(os.path.join(cwd, "ux0:roms", "Other Name", "Deeper",
                           "Game Three.smc"), "wb") as f:
        f.write(tiny)
    before = {n: open(os.path.join(folder, n), "rb").read()
              for n in os.listdir(folder)}
    r = S.run("snes_library", "90:triangle\n+30:triangle\n+30:triangle\n"
              "+60:shot=snes.png\n+2:cross\n+60:quit\n", cwd=cwd)
    S.clean(r, allow="could not be started")  # the twin cannot start snes.bin
    sw = r.evt("ui_browser_console")
    S.check(sw and sw[-1]["console"] == "3" and int(sw[-1]["roms"]) == 3,
            "the SNES shelf holds the three games and no ._ or dot file (%s)" %
            (sw[-1]["_"] if sw else "no ui_browser_console line"))
    ln = r.evt("snes_launch")
    S.check(ln and "Game" in ln[0]["_"],
            "picking one starts the SNES program (%s)" %
            (ln[0]["_"] if ln else "no snes_launch line"))
    after = {n: open(os.path.join(folder, n), "rb").read()
             for n in os.listdir(folder)}
    S.check(before == after, "the card's files (save, state) are untouched")

    # The Vita's home screen (ui_home = 1): the SNES chip in the top bar
    # names itself, and the button row's hints stay clear of the buttons.
    r = S.run("snes_home", "90:triangle\n+30:triangle\n+30:triangle\n"
              "+90:shot=home_snes.png\n+2:triangle\n+90:shot=home_gba.png\n"
              "+10:quit\n", cwd=cwd, config={"ui_home": 1})
    S.clean(r)
    sw = r.evt("ui_browser_console")
    S.check(sw and any(e["console"] == "3" for e in sw),
            "TRIANGLE reaches the SNES shelf on the home screen (%s)" %
            [e["_"] for e in sw])


TESTS = [("list", t_list), ("snes_library", t_snes_library), ("extra_roms", t_extra_roms),
         ("home", t_home), ("old_states", t_old_states), ("find", t_find), ("library_cache", t_library_cache),
         ("settings_order", t_settings_order), ("quality", t_quality),
         ("quality_preset", t_quality_preset), ("art_requeue", t_art_requeue),
         ("favs_recover", t_favs_recover), ("gb_save_soon", t_gb_save_soon),
         ("vita_ini_once", t_vita_ini_once), ("save_fail_shown", t_save_fail_shown),
         ("ambient_bars", t_ambient_bars), ("sensors", t_sensors),
         ("game_look", t_game_look), ("recent", t_recent), ("resume", t_resume),
         ("patch", t_patch), ("sgb", t_sgb), ("ra", t_ra),
         ("preview", t_preview),
         ("gameinfo", t_gameinfo), ("art_progress", t_art_progress), ("art_settings", t_art_settings),
         ("title_bg", t_title_bg), ("cover_menu", t_cover_menu),
         ("cover_inbox", t_cover_inbox), ("fullspeed", t_fullspeed), ("smooth_gov", t_smooth_gov),
         ("upfade", t_upfade), ("render_thread", t_render_thread),
         ("render_skips", t_render_skips), ("gpu_xbrz", t_gpu_xbrz),
         ("session", t_session),
         ("ff", t_ff),
         ("suspend", t_suspend), ("controls", t_controls), ("turbo", t_turbo),
         ("cheats", t_cheats), ("play", t_play), ("touch", t_touch),
         ("typing", t_typing), ("look", t_look), ("wide", t_wide),
         ("wide_auto", t_wide_auto), ("wide_top", t_wide_top),
         ("switches", t_switches), ("bad", t_bad), ("worker", t_worker), ("cpufix", t_cpufix),
         ("realtime", t_realtime),
         ("save_import", t_save_import), ("save_sync", t_save_sync), ("save_gb_keep", t_save_gb_keep), ("save_clock", t_save_clock), ("launch_param", t_launch_param),
         ("folders_config", t_folders_config), ("shot_dir", t_shot_dir), ("zip", t_zip), ("folders_ui", t_folders_ui), ("overlay", t_overlay), ("ten_slots", t_ten_slots), ("sidecars", t_sidecars), ("display_rows", t_display_rows), ("cpu_speed", t_cpu_speed),
         ("controls_defaults", t_controls_defaults)]


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
