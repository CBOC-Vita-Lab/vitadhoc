#!/usr/bin/env python3
"""link_tests.py -- two (or three) twins of the Vita app, linked over the
loopback network: vita/link_vita.c from the menus to the session and back.

    ./link_tests.py x86               both twins the x86 one
    ./link_tests.py armhf             both the armhf one under qemu-arm (the
                                      ARM dynarec, the Vita's): the core set
    ./link_tests.py mixed             an x86 twin and an armhf twin: they
                                      must refuse to mirror, and may play a
                                      Game Boy link
    options:  --only NAME[,NAME]   some of the tests
              --list               their names

What app_tests.py is to one console this is to two.  Every twin is the whole
app (main_vita.c, the PSP UI, link_vita.c) run with --realtime -- two
processes share the wall clock, as two consoles do -- with its own data
root, driven through the real menus by plat_host.c's input script and
checked by its event log, its screenshots (left in build/linktest/<arch>/
<test>/<twin>/ for looking at) and the files it leaves.

THE NETWORK is the loopback: hostcore/net_host.c's stand-in Wi-Fi says each
twin is on "TwinNet" at 127.0.0.1 (<root>/nethost.ini; a test takes the
Wi-Fi away by rewriting it), transport_lan runs on real UDP sockets, and
ad hoc is netdrv/tests/vadhoc_fake.c's radio cell of loopback sockets.  Each
test has its own port range (vita.ini link_port), so tests and other users
of the machine do not see each other's rooms.

THE LINK IS REAL: the GBA cartridge is tools/linktest/linktest.gba, whose
two machines exchange checked words in ten serial modes, and the GB one
testrom/gblink.gb (make -C testrom gb), 200 checked bytes each way.  Their
own verdicts are read from the machine's memory (vita.ini link_peek) and,
for the Game Boy, from the battery save the session commits; the session's
periodic hashes say that both consoles computed the same two machines.

Not exercised, and said so in the output: Wireless Adapter TRAFFIC (no
homebrew speaks the adapter's protocol; its room, idle link and teardown
are tested), and everything behind a Sony call.
"""
import argparse
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time

from app_tests import HERE, QEMU, Result, Suite, worst_diff

GP = os.path.normpath(os.path.join(HERE, "..", ".."))
LINKTEST = os.path.join(GP, "tools", "linktest", "linktest.gba")
GBLINK = os.path.join(HERE, "testrom", "gblink.gb")
FAKE_IGD = os.path.join(GP, "netdrv", "tests", "fake_igd.py")

# The linktest cartridge's result block (tools/linktest/linktest.h), in
# 32-bit words from 0x02000000.
LT_MAGIC, LT_ROLE, LT_VBL, LT_LOOPS, LT_PHASE, LT_FAIL, LT_PASS = range(7)
LT_SMC_FAIL, LT_TIMEOUT = 13, 14
LT_PEEK = "02000000:48"            # ... and 0x80.., the passes by phase


def exe_of(arch):
    return os.path.join(HERE, "build", arch, "gbadhoc_host")


class Twin:
    """One console: a data root, a script, a process."""

    def __init__(self, T, name, arch, rom, nick, port, script, config=None,
                 vita=None, net=None, refresh=None, extra=None):
        self.T, self.name, self.arch = T, name, arch
        self.out = os.path.join(T.dir, name)
        self.root = os.path.join(self.out, "root")
        self.proc = None
        shutil.rmtree(self.out, ignore_errors=True)
        os.makedirs(os.path.join(self.root, "roms"))
        shutil.copy(rom, os.path.join(self.root, "roms"))
        # GBA_BIOS / GBA_BIOS_<NAME>: the real BIOS in this twin's data root
        # (app_tests.py make_root); GBA_BIOS_A=... alone gives it to one side
        bios = os.environ.get("GBA_BIOS_" + name.upper(),
                              os.environ.get("GBA_BIOS", ""))
        if os.path.isfile(bios):
            shutil.copy(bios, os.path.join(self.root, "gba_bios.bin"))
        self.rom = os.path.basename(rom)
        with open(os.path.join(self.root, "autoboot.txt"), "w") as f:
            f.write(self.rom + "\n")
        cfg = {"nick": nick}
        cfg.update(config or {})
        with open(os.path.join(self.root, "config.ini"), "w") as f:
            for k, v in cfg.items():
                f.write("%s = %s\n" % (k, v))
        vi = {"link_port": port}
        vi.update(vita or {})
        with open(os.path.join(self.root, "vita.ini"), "w") as f:
            for k, v in vi.items():
                f.write("%s = %s\n" % (k, v))
        for rel, data in (extra or {}).items():
            os.makedirs(os.path.dirname(os.path.join(self.root, rel)),
                        exist_ok=True)
            with open(os.path.join(self.root, rel), "wb") as f:
                f.write(data if isinstance(data, bytes) else data.encode())
        self.net = {"wifi": 1, "ssid": "TwinNet"}
        self.net.update(net or {})
        self.write_net()
        self.script = script
        self.refresh = refresh

    def write_net(self, **kv):
        """<root>/nethost.ini: what this twin's Wi-Fi 'is', now."""
        self.net.update(kv)
        tmp = os.path.join(self.root, "nethost.tmp")
        with open(tmp, "w") as f:
            for k, v in self.net.items():
                f.write("%s = %s\n" % (k, v))
        os.replace(tmp, os.path.join(self.root, "nethost.ini"))

    def start(self):
        with open(os.path.join(self.out, "script.txt"), "w") as f:
            f.write(self.script)
        cmd = (QEMU if self.arch.startswith("armhf") else []) + [
            exe_of(self.arch), "--root", self.root, "--realtime",
            "--shot-dir", self.out, "--input-file",
            os.path.join(self.out, "script.txt")]
        if self.refresh:
            cmd += ["--refresh-mhz", str(self.refresh)]
        self.t0 = time.time()
        self.err = open(os.path.join(self.out, "stderr.txt"), "w")
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=self.err,
                                     env=dict(os.environ, TZ="UTC"))
        self.T.procs.append(self.proc)

    def signal(self, sig):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(sig)

    def finish(self, timeout):
        rc = -99
        if self.proc:
            try:
                rc = self.proc.wait(timeout=max(1, timeout))
            except subprocess.TimeoutExpired:
                self.proc.send_signal(signal.SIGCONT)
                self.proc.kill()
                self.proc.wait()
            self.err.close()
        err = open(os.path.join(self.out, "stderr.txt")).read()
        self.r = Result(rc, err, self.root, self.out, time.time() - self.t0)
        return self.r

    def log_now(self):
        try:
            return open(os.path.join(self.root, "log", "gbadhoc.log"),
                        errors="replace").read()
        except OSError:
            return ""

    def wait_log(self, pattern, timeout):
        """Until the (live) log has the pattern; False on time-out or if the
        twin has gone."""
        rx = re.compile(pattern, re.M)
        end = time.time() + timeout
        while time.time() < end:
            if rx.search(self.log_now()):
                return True
            if self.proc.poll() is not None:
                return bool(rx.search(self.log_now()))
            time.sleep(0.1)
        return False


class Tests:
    def __init__(self, mode, hz):
        self.mode = mode
        # who is what: x86 = both x86; armhf = both armhf; mixed = A x86,
        # B armhf
        self.arch_a = "armhf" if mode == "armhf" else "x86"
        self.arch_b = "x86" if mode == "x86" else "armhf"
        self.slow = mode != "x86"
        # A twin under qemu cannot draw sixty pictures a second, let alone
        # run two GBAs at it.  Its display is told to refresh more slowly
        # (the script, the UI and the pacer all count refreshes), so a press
        # still lasts several iterations of the loop and a "second" of the
        # menus' own time limits is longer -- nothing is skipped.
        # --hz does the same for a busy machine: at 30 the x86 twins take
        # half the processor and every test twice as long.
        hz = hz or (12 if self.slow else 60)
        self.refresh = None if hz == 60 else hz * 1000
        self.k = 60.0 / hz               # wall seconds per script second
        global CHORD
        CHORD = 84 if self.slow else 26
        self.S = Suite(mode, False, False)
        self.S.outdir = os.path.join(HERE, "build", "linktest", mode)
        self.procs = []
        self.port = 20000 + (os.getpid() * 64 + random.randrange(64) * 64) % 30000
        self.dir = None

    # ---- plumbing -------------------------------------------------------------
    def begin(self, name):
        self.dir = os.path.join(self.S.outdir, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.port += 32
        self.procs = []
        return self.port

    def twin(self, name, arch, rom, nick, script, **kw):
        return Twin(self, name, arch, rom, nick, self.port, script,
                    refresh=self.refresh, **kw)

    def run(self, twins, seconds, actions=()):
        """Start the twins together, do the timed actions (seconds of SCRIPT
        time from the start, callable), wait for all of them to quit."""
        for t in twins:
            t.start()
        t0 = time.time()
        try:
            for when, fn in sorted(actions, key=lambda a: a[0]):
                d = t0 + (when + SHIFT) * self.k - time.time()
                if d > 0:
                    time.sleep(d)
                fn()
            end = t0 + (seconds + SHIFT) * self.k + 30
            return [t.finish(end - time.time()) for t in twins]
        finally:
            self.kill_all()

    def kill_all(self):
        for p in self.procs:
            if p.poll() is None:
                try:
                    p.send_signal(signal.SIGCONT)
                    p.kill()
                    p.wait(timeout=5)
                except Exception:
                    pass
        self.procs = []

    # ---- checks ---------------------------------------------------------------
    def check(self, cond, what):
        return self.S.check(cond, what)

    def clean(self, r, who, allow=None):
        """The twin left by itself, said goodbye, and nothing in its log
        says a file or a state went wrong."""
        self.check(r.rc == 0, "%s: exit code 0 (got %s) %s" %
                   (who, r.rc, r.err.strip()[-160:] if r.rc else ""))
        self.check(r.has(r"^EVT exit ok"), "%s: the log ends with 'exit ok'" % who)
        bad = [b for b in r.bad_lines()
               if not (allow and re.search(allow, b))]
        self.check(not bad, "%s: a clean log (%s)" % (who, "; ".join(bad[:3])))

    def picture(self, r, who, name):
        try:
            im = r.shot(name)
        except OSError:
            self.check(False, "%s: screenshot %s was taken" % (who, name))
            return None
        self.check(len(im.getcolors(1 << 20) or [0, 0]) > 1,
                   "%s: %s is not a blank picture" % (who, name))
        return im

    def radio_free(self, r, who):
        """Whatever happened, the radio was given back at the end."""
        downs = r.evt("link_net_down")
        self.check(downs and downs[-1].get("radio") == "0",
                   "%s: the radio is free at the end (%s)" %
                   (who, downs[-1]["_"] if downs else "never opened"))

    def peek(self, r, when="end"):
        """The last EVT link_peek of that kind, as a list of words."""
        ws = [d for d in r.evt("link_peek") if d.get("when") == when]
        if not ws:
            return None
        return [int(w, 16) for w in ws[-1]["w"].split(",")]

    def hashes(self, r):
        """(matched, sent) of the session's last gblink_stats line."""
        st = r.evt("gblink_stats")
        if not st:
            return (0, 0)
        m, s = st[-1]["hashes"].split("/")
        return int(m), int(s)

    def same_machines(self, ra, rb):
        """Both consoles computed the same two machines: every periodic hash
        each sent was matched by the other's, and the final hash, taken at
        the agreed end frame, is the same number on both."""
        ma, sa = self.hashes(ra)
        mb, sb = self.hashes(rb)
        self.check(sa > 0 and ma == sa and sb > 0 and mb == sb,
                   "every periodic hash matched on both consoles "
                   "(A %d/%d, B %d/%d)" % (ma, sa, mb, sb))
        fa, fb = ra.evt("gblink_final"), rb.evt("gblink_final")
        self.check(fa and fb and fa[-1]["hash"] == fb[-1]["hash"] and
                   fa[-1]["frame"] == fb[-1]["frame"],
                   "the final hash is the same on both (%s / %s)" %
                   (fa[-1]["hash"] if fa else "-", fb[-1]["hash"] if fb else "-"))
        self.S.summary[self.S.test + ".final_hash"] = fa[-1]["hash"] if fa else ""

    def linktest_verdict(self, r, who, role, when="end", loops=1):
        w = self.peek(r, when)
        if not self.check(w is not None and len(w) >= 48,
                          "%s: the cartridge's result block was read" % who):
            return
        self.check(w[LT_MAGIC] == 0x31544B4C, "%s: linktest magic" % who)
        self.check(w[LT_ROLE] == role, "%s: linktest role %d (%s), got %d" %
                   (who, role, "parent" if role == 1 else "child", w[LT_ROLE]))
        self.check(w[LT_FAIL] == 0 and w[LT_SMC_FAIL] == 0 and
                   w[LT_TIMEOUT] == 0,
                   "%s: no failed check, no time-out (fail=%d smc=%d to=%d, "
                   "passed=%d)" % (who, w[LT_FAIL], w[LT_SMC_FAIL],
                                   w[LT_TIMEOUT], w[LT_PASS]))
        self.check(w[LT_PASS] > 100, "%s: checks passed (%d)" %
                   (who, w[LT_PASS]))
        if loops:
            by_phase = w[32 + 1:32 + 11]
            self.check(w[LT_LOOPS] >= loops and all(by_phase),
                       "%s: %d loop(s) through the ten serial modes, each "
                       "with passed checks (%s)" %
                       (who, w[LT_LOOPS], by_phase))
        self.S.note("%s linktest: role %d, %d checks passed, %d loops, phase "
                    "%d" % (who, w[LT_ROLE], w[LT_PASS], w[LT_LOOPS],
                            w[LT_PHASE]))

    def finish(self):
        return self.S.finish()


# ---- scripts -----------------------------------------------------------------
# A script is absolute refresh numbers; these build the common walks.  The
# in-game menu: 0 Resume, 1 Save states, 2 Play together, 3 Cheats,
# 4 Settings, 5 Quit to game list, 6 Exit GBAdhoc.  Play together: 0 Host a
# game, 1 Join a game, 2 Two players on this Vita, 3 Connection, 4 How to.

# Every time in a test is counted from SHIFT seconds into the run: what the
# EARLY opening of the menu (below) takes longer than a plain one.
SHIFT = 3.0


def at(sec):
    return int((sec + SHIFT) * 60)


def dur(sec):
    return int(sec * 60)


# A press lasts PRESS refreshes and the next comes GAP after it: long enough
# to be seen by a loop that a busy machine (or qemu) runs at a fraction of
# its rate, short of the d-pad's repeat (18 iterations held).
PRESS, GAP = 9, 14
# A screenshot is a PNG written by the loop itself: a fifth of a second and
# more in which no button is read.  The next press waits this long after one.
AFTER_SHOT = 45


def keys(*names, first=GAP):
    return "".join("+%d:%s:%d\n" % (GAP if i else first, n, PRESS)
                   for i, n in enumerate(names))


EARLY = 0      # as a time: "as soon as the game runs"
# START+SELECT must be seen by fifteen turns of the loop.  At 60 Hz that is a
# quarter of a second; an emulated ARM twin running two GBAs on a busy
# machine turns four times a second, and is given seven seconds (Tests sets
# it).  Holding on after the menu is up does nothing.
CHORD = 26
# The chord held from the first moment (open_play(EARLY)): three seconds at
# 60 Hz, fourteen at 12 Hz -- longer than two emulated ARM twins take to
# boot together.  Three of them take longer still: t_room_full raises it.
EARLY_CHORD = 170


def open_play(sec):
    """START+SELECT at `sec`, then Play together: 1.8 s later the page is up.

    EARLY: the chord is held from the first moment for three seconds, so the
    menu opens fifteen frames after the game starts however long the boot
    took on a busy machine.  The linktest cartridge looks for its partner
    during its first 150 frames and plays alone after that; the core is
    paused behind the menu, so a twin that opens it at once is still looking
    when the link forms."""
    if sec == EARLY:
        return ("18:start+select:%d\n" % EARLY_CHORD +
                keys("down", "down", "cross"))
    return ("%d:start+select:%d\n" % (at(sec), CHORD) +
            keys("down", "down", "cross"))


def host(sec, shot=None):
    """Host a game from the Play together page."""
    s = open_play(sec) + "+20:cross:%d\n" % PRESS
    if shot:
        s += "+50:shot=%s\n" % shot
    return s


def join_first(sec, wait=2.0, shot=None, pick=True):
    """Join a game: the list, `wait` seconds for it to fill, the first room."""
    s = open_play(sec) + "+20:down:%d\n" % PRESS + keys("cross")
    s += "+%d:shot=%s\n" % (dur(wait), shot or "join_list.png")
    if pick:
        s += "+%d:cross:%d\n" % (AFTER_SHOT, PRESS)
    return s


def disconnect(sec, shot=None):
    """The session page's second row: Disconnect / Stop hosting / Cancel."""
    s = open_play(sec)
    if shot:
        s += "+25:shot=%s\n" % shot
    return s + keys("down", "cross", first=AFTER_SHOT if shot else GAP)


def close_menu(first=AFTER_SHOT):
    """O on a Play together page, then on the menu's top page."""
    return keys("circle", "circle", first=first)


def quit_at(sec):
    return "%d:quit\n" % at(sec)


def shot(sec, name):
    return "%d:shot=%s\n" % (at(sec), name)


def press(sec, buttons, frames=PRESS):
    return "%d:%s:%d\n" % (at(sec), buttons, frames)


# ---- the tests ----------------------------------------------------------------

def t_wifi_cable(T):
    """Wi-Fi, GBA Link Cable: host, the Join list, the mirror, an agreed end."""
    T.begin("wifi_cable")
    # Both open the menu within the cartridge's first 150 frames (it looks
    # for a partner that long, then runs alone): the core is paused behind
    # the menu, so both machines enter the link still looking.
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY, "host.png") + shot(9.5, "linking.png") +
               shot(14, "running.png") + press(15, "a+up", 40) +
               disconnect(34, "session.png") + shot(37.5, "ended.png") +
               shot(40, "after.png") + quit_at(42),
               vita={"link_peek": LT_PEEK})
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(EARLY, wait=2.5) + "+20:shot=linking.png\n" +
               shot(14, "running.png") + press(17, "b+left", 40) +
               shot(37.5, "ended.png") + shot(40, "after.png") + quit_at(42),
               vita={"link_peek": LT_PEEK})
    ra, rb = T.run([a, b], 42)
    T.clean(ra, "A")
    T.clean(rb, "B")
    # the room, as B's list showed it
    rooms = rb.evt("link_scan_room")
    T.check(rooms and rooms[-1]["title"] == '"linktest"' and
            rooms[-1]["nick"] == '"Alice"' and rooms[-1]["kind"] == "1" and
            rooms[-1]["compatible"] == "1" and rooms[-1]["players"] == "1/2",
            "B's Join list shows A's room: the game, the host's name, 1/2, "
            "compatible (%s)" % (rooms[-1]["_"] if rooms else "no room"))
    T.check(ra.has(r"link_hosting room=\"Room 7\"") and
            rb.has(r"play_join id=\w+ addr=127\.0\.0\.1:%d" % T.port),
            "A hosts Room 7; B picks it from the list by its address")
    for r, who, n in ((ra, "A", 1), (rb, "B", 2)):
        T.check(r.has(r"link_running player=%d" % n),
                "%s: the session reaches 'linked' as Player %d" % (who, n))
        T.check(r.has(r"link_cheats suspended=1") and
                r.index(r"link_cheats suspended=1") <
                r.index(r"gblink_create") and
                r.index(r"link_cheats suspended=0") >
                r.index(r"gblink_resume"),
                "%s: cheats leave before the machines start and return "
                "after the single game is back" % who)
        T.check(r.has(r"gblink_done .*save_committed=131072") and
                r.has(r"gblink_resume how=live_end rc=0"),
                "%s: the link ends by agreement: save committed, the game "
                "carries on from the end frame" % who)
        T.radio_free(r, who)
    T.same_machines(ra, rb)
    T.linktest_verdict(ra, "A", 1)
    T.linktest_verdict(rb, "B", 2)
    # the delay: the host measured, the joiner follows
    da = ra.evt("link_session_begin")[-1]
    ca, cb = ra.evt("gblink_config"), rb.evt("gblink_config")
    T.check(da["how"] == "measured" and da["role"] == "host" and
            ca and cb and ca[-1]["delay"] == cb[-1]["delay"] == da["delay"],
            "the host chose the input delay from a measurement (%s frames) "
            "and the joiner follows it" % da["delay"])
    T.S.note("delay %s, %s; A stalls=%s B stalls=%s" %
             (da["delay"], ra.evt("link_rtt")[-1]["_"][4:] if
              ra.evt("link_rtt") else "no rtt",
              ra.evt("gblink_stats")[-1]["stalls"],
              rb.evt("gblink_stats")[-1]["stalls"]))
    # pictures
    for r, who, names in ((ra, "A", ("host.png", "linking.png", "running.png",
                                     "session.png", "after.png")),
                          (rb, "B", ("join_list.png", "linking.png",
                                     "running.png", "after.png"))):
        for n in names:
            T.picture(r, who, n)
    # the linktest screen: a band, left third the role (red parent, blue
    # child), the rest green until anything fails
    for r, who, ch in ((ra, "A", 0), (rb, "B", 2)):
        im = r.shot("running.png")
        left, right = im.getpixel((200, 50)), im.getpixel((700, 50))
        T.check(left[ch] > 200 and left[2 - ch] < 60 and right[1] > 150 and
                right[0] < 60 and right[2] < 60,
                "%s: the cartridge's own screen while linked: role band %s "
                "(%s), verdict band green %s" %
                (who, left, "red: parent" if ch == 0 else "blue: child",
                 right))
        chip = r.shot("running.png").crop((600, 496, 950, 536))
        T.check(max(hi for _, hi in chip.getextrema()) > 150,
                "%s: the session chip is on the game's picture" % who)


def t_fast_link(T):
    """Fast link (GBAdhoc 4.0): the HOST's setting sends two frames of
    buttons a packet; the guest, set otherwise, runs the host's.  The link
    must still be exact (the cartridge's own checks, the session's hashes),
    with the cable's fast path and riding acks on as everywhere."""
    T.begin("fast_link")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY, "host.png") + shot(14, "running.png") +
               press(15, "a+up", 40) + disconnect(30, "session.png") +
               quit_at(36),
               config={"fast_link": 1}, vita={"link_peek": LT_PEEK})
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(EARLY, wait=2.5) + shot(14, "running.png") +
               press(17, "b+left", 40) + quit_at(36),
               config={"fast_link": 0}, vita={"link_peek": LT_PEEK})
    ra, rb = T.run([a, b], 36)
    T.clean(ra, "A")
    T.clean(rb, "B")
    for r, who, n in ((ra, "A", 1), (rb, "B", 2)):
        T.check(r.has(r"link_running player=%d" % n),
                "%s: the session reaches 'linked' as Player %d" % (who, n))
        T.check(r.has(r"gblink_fast_link on=1"),
                "%s: the session runs with Fast link on (%s)" %
                (who, "its own setting" if who == "A" else "the host's"))
        T.check(r.has(r"gbadual_session_start .*fast=3/3"),
                "%s: the cable's fast path with the lean hooks" % who)
    T.check(ra.has(r"fast_link_setting own=1 role=host") and
            rb.has(r"fast_link_setting own=0 role=guest"),
            "the two settings differed, and the host's won")
    T.same_machines(ra, rb)
    T.linktest_verdict(ra, "A", 1)
    T.linktest_verdict(rb, "B", 2)


def t_share(T, replace=False):
    """Share game data (GBAdhoc 4.0) over Wi-Fi: A shares from the game
    list's Settings, B joins the share on the same Room, opens the game they
    both have and takes A's save -- checked byte for byte."""
    T.begin("share_replace" if replace else "share")
    sav = bytes((i * 7 + 3) & 0xFF for i in range(32768))
    mine = bytes((i * 13 + 5) & 0xFF for i in range(32768))
    # replace: B has a save of its own, so taking A's asks first ("Replace
    # your save?", X replace) and keeps B's as linktest.sav.bak.
    ask = press(21, "cross") if replace else ""
    late = 2 if replace else 0
    down7 = keys(*(["down"] * 7))
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               press(2, "start") + down7 + "+20:shot=settings.png\n" +
               keys("cross", first=AFTER_SHOT) + shot(8, "sharing.png") +
               shot(26, "shared.png") + press(28, "circle") +
               shot(30, "after.png") + quit_at(32),
               extra={"roms/linktest.sav": sav})
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               press(5, "start") + down7 + keys("down") +
               "+20:shot=settings.png\n" + keys("cross", first=AFTER_SHOT) +
               shot(14, "list.png") + press(15, "cross") +
               shot(18, "their.png") + press(19, "cross") +
               (shot(20, "ask.png") if replace else "") + ask +
               shot(24 + late, "done.png") + press(25 + late, "circle") +
               press(26 + late, "circle") + shot(28 + late, "after.png") +
               quit_at(32 + late),
               extra={"roms/linktest.sav": mine} if replace else None)
    for t in (a, b):
        os.remove(os.path.join(t.root, "autoboot.txt"))   # the game list
    ra, rb = T.run([a, b], 32 + late)
    T.clean(ra, "A")
    T.clean(rb, "B")
    T.check(ra.has(r"share_net_up host=1 group=GS07DATA rc=0"),
            "A opens the share room GS07DATA on Wi-Fi")
    T.check(ra.has(r"share_host_up group=GS07DATA .*saves=1"),
            "A shares one save")
    T.check(rb.has(r"share_scan_room name=GS07DATA kind=4") and
            rb.has(r"share_net_up host=0 group=GS07DATA rc=0"),
            "B finds the room on the network and joins it")
    T.check(rb.has(r"share_join_welcome"),
            "B is welcomed and reads the list of what A shares")
    got = os.path.join(rb.root, "roms", "linktest.sav")
    data = open(got, "rb").read() if os.path.exists(got) else b""
    T.check(data == sav, "B now has A's save, byte for byte (%d bytes)" %
            len(data))
    T.check(not any(n.endswith(".gsp") for n in
                    os.listdir(os.path.join(rb.root, "roms"))),
            "no temporary file left behind")
    if replace:
        bak = os.path.join(rb.root, "roms", "linktest.sav.bak")
        old = open(bak, "rb").read() if os.path.exists(bak) else b""
        T.check(old == mine, "B's own save is kept as linktest.sav.bak "
                "(%d bytes)" % len(old))
        return
    for r, who, names in ((ra, "A", ("settings.png", "sharing.png")),
                          (rb, "B", ("settings.png", "list.png",
                                     "their.png", "done.png"))):
        for n in names:
            T.picture(r, who, n)


def t_share_replace(T):
    """Share game data: B takes A's save over one of its own, which is kept
    as a backup."""
    t_share(T, replace=True)


MGIFT_FIXTURES = os.path.join(HERE, "..", "..", "tools", "fixtures", "mgift")


def t_mgift(T):
    """Mystery Gift (the PSP's since 3.0): SELECT+DOWN in a GBA game starts
    the listener; a phone's parcel (the Mystery Gift Station app's own
    Emerald fixture, old-sea-map.mgc2) arrives over Wi-Fi and is accepted
    with the protocol's ACK; a corrupted parcel is answered "invalid"; a
    second SELECT+DOWN stops it.  (The game's side of the gift needs
    FireRed, LeafGreen or Emerald: tools/test_mgift.c covers the cart.)"""
    import socket
    T.begin("mgift")
    port = T.port + 40
    fx = open(os.path.join(MGIFT_FIXTURES, "old-sea-map.mgc2"), "rb").read()
    bad = bytearray(fx)
    bad[100] ^= 1
    acks = {}

    def send(name, data):
        sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sk.settimeout(0.25)
        try:
            for _ in range(40):
                sk.sendto(bytes(data), ("127.0.0.1", port))
                try:
                    r, _ = sk.recvfrom(64)
                except socket.timeout:
                    continue
                if r[:4] == b"MGA2":
                    acks[name] = r
                    return
        finally:
            sk.close()

    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               press(3, "select+down", 20) + shot(4.5, "listening.png") +
               shot(12, "gift.png") + press(13, "select+down", 20) +
               shot(14.5, "off.png") + quit_at(16),
               vita={"mgift_port": port})
    ra, = T.run([a], 16, actions=[(6, lambda: send("bad", bad)),
                                  (8, lambda: send("good", fx))])
    T.clean(ra, "A")
    T.check(ra.has(r"mg_start ip=127\.0\.0\.1 game=\w+ port=%d" % port),
            "the shortcut starts the listener on Wi-Fi, on the port")
    T.check("bad" in acks and acks["bad"][8] == 2 and
            ra.has(r"mg_parcel_invalid"),
            "a corrupted parcel is answered 'invalid' (%s)" %
            (acks.get("bad", b"")[8:9].hex() or "no answer"))
    T.check("good" in acks and acks["good"][8] == 0 and
            acks["good"][4:8] == fx[4:8],
            "the Station's parcel is accepted: MGA2, its id, result 0 (%s)" %
            (acks.get("good", b"").hex() or "no answer"))
    T.check(ra.has(r"mg_parcel id=%08x frlg=0 emerald=\d+ rc=0" %
                   int.from_bytes(fx[4:8], "little")),
            "the cart holds the gift (Emerald's script, no FR/LG one)")
    T.check(ra.has(r"mg_stop "), "the second SELECT+DOWN stops it")
    for n in ("listening.png", "gift.png", "off.png"):
        T.picture(ra, "A", n)


def t_three_sessions(T):
    """Three sessions in one boot: host, end, join, end, host again."""
    T.begin("three_sessions")
    # 1: A hosts, B joins, A ends.  2: B hosts, A joins, B ends.  3: as 1.
    # An emulated ARM twin on a busy machine spends most of a short session
    # setting it up: each one runs e seconds longer there, so that every
    # session reaches at least one hash check.
    e = 8 if T.slow else 0
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + disconnect(16 + e) +
               join_first(26 + e, wait=2.5, shot="join2.png") +
               shot(34 + e, "second.png") +
               host(48 + 2 * e) + shot(58 + 3 * e, "third.png") +
               disconnect(62 + 3 * e) +
               shot(68 + 3 * e, "after.png") + quit_at(70 + 3 * e))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(EARLY, wait=2.5) +
               host(21 + e) + disconnect(40 + 2 * e) +
               join_first(50 + 2 * e, wait=2.5) +
               shot(58 + 3 * e, "third.png") +
               shot(68 + 3 * e, "after.png") + quit_at(70 + 3 * e))
    ra, rb = T.run([a, b], 70 + 3 * e)
    T.clean(ra, "A")
    T.clean(rb, "B")
    for r, who, roles in ((ra, "A", ["host", "guest", "host"]),
                          (rb, "B", ["guest", "host", "guest"])):
        got = [d["role"] for d in r.evt("link_session_begin")]
        T.check(got == roles, "%s: three sessions, as %s (got %s)" %
                (who, "/".join(roles), "/".join(got)))
        done = r.evt("gblink_done")
        T.check(len(done) == 3, "%s: each ended by agreement, save "
                "committed (%d of 3)" % (who, len(done)))
        st = [d["hashes"] for d in r.evt("gblink_done")]
        T.check(all(h.split("/")[0] == h.split("/")[1] and h != "0/0"
                    for h in st), "%s: every hash of every session matched "
                "(%s)" % (who, " ".join(st)))
        downs = r.evt("link_net_down")
        T.check(len(downs) == 3 and all(d["radio"] == "0" for d in downs),
                "%s: the radio was given back after each" % who)
        T.check(len(r.evt("link_cheats")) == 6, "%s: cheats suspended and "
                "returned three times" % who)
    fa = [d["hash"] for d in ra.evt("gblink_final")]
    fb = [d["hash"] for d in rb.evt("gblink_final")]
    T.check(fa == fb and len(fa) == 3,
            "the three final hashes agree between the consoles")
    for r, who in ((ra, "A"), (rb, "B")):
        T.picture(r, who, "third.png")


def gb_verdict(T, r, who, role):
    """The GB cartridge's own word: from the machine's memory at the end,
    and from the battery save the session committed."""
    w = T.peek(r, "end")
    if T.check(w is not None and len(w) >= 2,
               "%s: the cartridge's memory was read" % who):
        got = (w[0] & 255, (w[0] >> 8) & 255, (w[0] >> 16) & 255,
               (w[0] >> 24) & 255)
        T.check(got == (role, 200, 200, 0),
                "%s: %s, 200 bytes exchanged, 200 checks passed, none "
                "failed (role/n/ok/bad = %s)" %
                (who, "master" if role == 1 else "slave", got))
    try:
        sav = open(os.path.join(r.root, "roms", "gblink.gb.sav"), "rb").read()
    except OSError:
        sav = b""
    T.check(sav[:2] == b"GL" and len(sav) >= 8 and sav[7] == 0x5A and
            tuple(sav[2:6]) == (role, 200, 200, 0),
            "%s: the battery save the session committed holds the verdict "
            "(%s)" % (who, sav[:8].hex()))


def t_wifi_gb(T):
    """Wi-Fi, Game Boy Link Cable: two Game Boys per console, bytes each way."""
    T.begin("wifi_gb")
    a = T.twin("a", T.arch_a, GBLINK, "Alice",
               host(2, "host.png") + shot(11, "linking.png") +
               press(15, "a", 20) + shot(17, "running.png") +
               disconnect(26, "session.png") + shot(33, "after.png") +
               quit_at(35), vita={"link_peek": "C000:2"})
    b = T.twin("b", T.arch_b, GBLINK, "Bob",
               join_first(4, wait=3) + shot(17, "running.png") +
               shot(33, "after.png") + quit_at(35),
               vita={"link_peek": "C000:2"})
    ra, rb = T.run([a, b], 35)
    T.clean(ra, "A")
    T.clean(rb, "B")
    rooms = rb.evt("link_scan_room")
    T.check(rooms and rooms[-1]["kind"] == "2" and
            rooms[-1]["compatible"] == "1",
            "B's list shows a Game Boy Link Cable room")
    for r, who, n in ((ra, "A", 1), (rb, "B", 2)):
        T.check(r.has(r"link_running player=%d" % n),
                "%s: linked as Player %d" % (who, n))
        T.check(r.has(r"gblink_done ") and r.has(r"gblink_save_commit .*ok=1"),
                "%s: agreed end, save committed" % who)
        T.radio_free(r, who)
        T.picture(r, who, "running.png")
    T.same_machines(ra, rb)
    gb_verdict(T, ra, "A", 1)
    gb_verdict(T, rb, "B", 2)
    # the cartridge's screen is one shade: light grey master, dark grey slave
    sa = ra.shot("running.png").getpixel((480, 272))
    sb = rb.shot("running.png").getpixel((480, 272))
    T.check(sum(sa) > sum(sb) + 60 and sum(sb) > 60,
            "the two screens: master lighter than slave, neither black "
            "(%s / %s)" % (sa, sb))


GBLOCAL = os.path.join(HERE, "testrom", "gblocal.gb")


def t_gb_two_carts(T):
    """Game Boy link with two DIFFERENT cartridges, neither console holding
    the other's: each streams its cartridge to the other for the session
    (memory only, checked by SHA-1, never written to the card) -- "only one
    of you needs the cartridge"."""
    T.begin("gb_two_carts")
    a = T.twin("a", T.arch_a, GBLINK, "Alice",
               host(2, "host.png") + shot(14, "linking.png") +
               shot(20, "running.png") + disconnect(26, "session.png") +
               shot(33, "after.png") + quit_at(35))
    b = T.twin("b", T.arch_b, GBLOCAL, "Bob",
               join_first(4, wait=3) + shot(14, "linking.png") +
               shot(20, "running.png") + shot(33, "after.png") +
               quit_at(35))
    ra, rb = T.run([a, b], 35)
    T.clean(ra, "A")
    T.clean(rb, "B")
    for r, who, n in ((ra, "A", 1), (rb, "B", 2)):
        T.check(r.has(r"link_running player=%d" % n),
                "%s: linked as Player %d with the other's cartridge" % (who, n))
        T.check(r.has(r"gblink_rom_send done size=\d+") and
                r.has(r"gblink_rom_received size=\d+ ok=1"),
                "%s: sent its cartridge and received the other's" % who)
        T.check(r.has(r"gblink_done "), "%s: an agreed end" % who)
        T.radio_free(r, who)
    T.same_machines(ra, rb)
    for who, root in (("A", ra.root), ("B", rb.root)):
        roms = sorted(os.listdir(os.path.join(root, "roms")))
        T.check(not any(n.startswith("gblocal") for n in roms) if who == "A"
                else not any(n.startswith("gblink.gb") for n in roms),
                "%s: the other's cartridge was not written to the card (%s)" %
                (who, ", ".join(roms)))


def t_gb_extra_folder(T):
    """Game Boy link where the partner's cartridge is in a folder of the
    other console that is not roms/ (vita.ini extra_roms, as the game list
    reads it): it is found there and checked by SHA-1 -- nothing is streamed
    to this console."""
    T.begin("gb_extra_folder")
    a = T.twin("a", T.arch_a, GBLINK, "Alice",
               host(2, "host.png") + shot(14, "linking.png") +
               shot(20, "running.png") + disconnect(26, "session.png") +
               shot(33, "after.png") + quit_at(35))
    extra = os.path.join(T.dir, "b", "root", "other")
    b = T.twin("b", T.arch_b, GBLOCAL, "Bob",
               join_first(4, wait=3) + shot(14, "linking.png") +
               shot(20, "running.png") + shot(33, "after.png") +
               quit_at(35),
               vita={"extra_roms": extra},
               extra={"other/Other Cartridge.gb": open(GBLINK, "rb").read(),
                      "other/._Other Cartridge.gb": bytes(4096)})
    ra, rb = T.run([a, b], 35)
    T.clean(ra, "A")
    T.clean(rb, "B")
    for r, who, n in ((ra, "A", 1), (rb, "B", 2)):
        T.check(r.has(r"link_running player=%d" % n),
                "%s: linked as Player %d" % (who, n))
        T.check(r.has(r"gblink_done "), "%s: an agreed end" % who)
        T.radio_free(r, who)
    T.check(rb.has(r"gblink_partner_rom source=library path=%s" %
                   re.escape(extra + "/Other Cartridge.gb")),
            "B: the partner's cartridge was found in its extra folder")
    T.check(not rb.has(r"gblink_rom_received"),
            "B: so it was not streamed over the air")
    T.check(ra.has(r"gblink_partner_rom source=transfer") and
            ra.has(r"gblink_rom_received size=\d+ ok=1"),
            "A has no such folder: it received B's cartridge as before")
    T.check(not any(n.startswith("gblink")
                    for n in os.listdir(os.path.join(rb.root, "roms"))),
            "B: nothing was copied into roms/")


def t_wifi_adapter(T):
    """Wi-Fi, GBA Wireless Adapter: the room, an idle link, both ways out."""
    T.begin("wifi_adapter")
    lm = {"roms/linkmode.txt": "linktest.gba = adapter\n"}
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(2, "host.png") + shot(11, "linked.png") +
               press(12, "cross") + shot(14, "game.png") +
               disconnect(26) + shot(30, "after.png") + quit_at(32), extra=lm)
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(5, wait=3) + "+12:shot=joining.png\n" +
               shot(12, "joined.png") + disconnect(20) +
               shot(24, "after.png") + quit_at(32), extra=lm)
    ra, rb = T.run([a, b], 32)
    T.clean(ra, "A")
    T.clean(rb, "B")
    rooms = rb.evt("link_scan_room")
    T.check(rooms and rooms[0]["kind"] == "0" and
            rooms[0]["players"] == "1/5" and rooms[0]["compatible"] == "1",
            "B's list shows a Wireless Adapter room, 1/5")
    T.check(ra.has(r"net_up role=host proto=\"gpSP") and
            rb.has(r"net_up role=join proto=\"gpSP"),
            "both run the core's own adapter protocol over netdrv")
    T.check(rb.has(r"session_start id=1") and ra.has(r"peer_connected id=1"),
            "the joiner is in the host's session")
    T.check(ra.has(r"link_room .*players=2/5 open=1"),
            "the room's announcement follows: 2/5")
    T.check(ra.has(r"link_cheats suspended=1") and
            rb.has(r"link_cheats suspended=1"),
            "cheats are off on both while in the room")
    T.check(rb.has(r"link_disconnect") and ra.has(r"peer_disconnected id=1")
            and ra.has(r"link_room .*players=1/5"),
            "the joiner leaves: the host sees it and the room is 1/5 again")
    for r, who in ((ra, "A"), (rb, "B")):
        T.radio_free(r, who)
        T.check(r.evt("link_cheats")[-1]["suspended"] == "0",
                "%s: cheats are back afterwards" % who)
    for r, who, names in ((ra, "A", ("host.png", "linked.png", "game.png",
                                     "after.png")),
                          (rb, "B", ("joined.png", "after.png"))):
        for n in names:
            T.picture(r, who, n)
    T.S.note("Wireless Adapter: bring-up, an idle link and teardown only -- "
             "no homebrew speaks the adapter, so no RFU traffic crossed")


# ---- Internet ------------------------------------------------------------------

class FakeRouter:
    """netdrv/tests/fake_igd.py: an SSDP responder and the HTTP side of an
    Internet Gateway Device on the loopback."""

    def __init__(self, T, *flags):
        self.log = os.path.join(T.dir, "router.log")
        self.p = subprocess.Popen([sys.executable, FAKE_IGD, "--log", self.log]
                                  + list(flags), stdout=subprocess.PIPE,
                                  text=True)
        T.procs.append(self.p)
        line = self.p.stdout.readline()
        m = re.match(r"PORTS (\d+) (\d+)", line)
        self.ssdp = int(m.group(1)) if m else 0

    def calls(self):
        try:
            return open(self.log).read()
        except OSError:
            return ""


def t_internet(T):
    """Internet: the host's public address by UPnP, a join by address."""
    port = T.begin("internet")
    # how long the pretend router may take over one answer: an emulated ARM
    # twin asks once a refresh, twelve times a second
    ms = 8000 if T.slow else 1500
    router = FakeRouter(T)
    T.check(router.ssdp > 0, "the pretend router is up")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + "+6:shot=host_asking.png\n" +
               shot(6, "host_public.png") + shot(16, "running.png") +
               disconnect(26) + shot(31, "after.png") + quit_at(36),
               config={"net_conn": 1},
               vita={"link_peek": LT_PEEK, "link_upnp_ms": ms,
                     "link_upnp": "127.0.0.1:%d" % router.ssdp})
    # B: Join a game opens the address page on the last address used, the
    # cursor on its Join key; then the list of that one address.
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               open_play(EARLY) + "+20:down:%d\n" % PRESS + keys("cross") +
               "%d:shot=address.png\n" % at(7.5) + press(8, "cross") +
               "%d:shot=join_list.png\n" % at(10.5) + press(11.4, "cross") +
               shot(16, "running.png") + shot(31, "after.png") + quit_at(36),
               config={"net_conn": 1, "net_addr1": "127.0.0.1:%d" % port},
               vita={"link_peek": LT_PEEK})
    ra, rb = T.run([a, b], 36)
    T.clean(ra, "A")
    T.clean(rb, "B")
    T.check(ra.has(r"link_upnp mapped addr=203\.0\.113\.7:%d port=%d private=0"
                   % (port, port)),
            "A: the router opened the port and said the public address")
    calls = router.calls()
    T.check("AddPortMapping" in calls and "UDP" in calls,
            "the router saw AddPortMapping for UDP (%s)" %
            calls.strip().splitlines()[-1:] )
    T.check(ra.has(r"link_upnp removed") and "DeletePortMapping" in calls,
            "the mapping was removed when the room closed")
    T.check(rb.has(r"link_scan open target=127\.0\.0\.1:%d" % port) and
            rb.has(r"link_scan_room .*addr=127\.0\.0\.1:%d .*compatible=1" % port),
            "B: the typed address is probed first and its room listed")
    for r, who, n in ((ra, "A", 1), (rb, "B", 2)):
        T.check(r.has(r"link_net_up .*arq=30000/30000") and
                r.has(r"link_running player=%d" % n),
                "%s: linked as Player %d on the Internet profile" % (who, n))
        T.radio_free(r, who)
    d = ra.evt("link_session_begin")[-1]
    T.check(int(d["delay"]) >= 3, "the Internet's input delay is at least 3 "
            "frames (%s, %s)" % (d["delay"], d["how"]))
    T.same_machines(ra, rb)
    T.linktest_verdict(ra, "A", 1)
    T.linktest_verdict(rb, "B", 2)
    for r, who, names in ((ra, "A", ("host_asking.png", "host_public.png",
                                     "running.png")),
                          (rb, "B", ("address.png", "join_list.png",
                                     "running.png"))):
        for n in names:
            T.picture(r, who, n)

    # No router answers: the page says what to do by hand.
    T.begin("internet_norouter")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + shot(12, "host_closed.png") + press(14, "down") +
               keys("cross") + shot(18, "after.png") + quit_at(19),
               config={"net_conn": 1}, vita={"link_upnp": "127.0.0.1:9"})
    ra, = T.run([a], 19)
    T.clean(ra, "A")
    T.check(ra.has(r"link_upnp failed"), "A: no gateway answered, and the "
            "log says why (%s)" % (ra.evt("link_upnp")[-1]["_"]
                                   if ra.evt("link_upnp") else ""))
    T.check(ra.has(r"link_hosting") and ra.has(r"link_disconnect phase=3"),
            "the room was open all the while, and closes when asked")
    T.picture(ra, "A", "host_closed.png")
    T.radio_free(ra, "A")

    # The router opens the port and will not say its own address: the page
    # says the way in is open, and where to look the address up.
    port = T.begin("internet_noaddr")
    router = FakeRouter(T, "--no-extip")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + shot(12, "host_noaddr.png") + press(14, "down") +
               keys("cross") + shot(18, "after.png") + quit_at(19),
               config={"net_conn": 1},
               vita={"link_upnp": "127.0.0.1:%d" % router.ssdp,
                     "link_upnp_ms": ms})
    ra, = T.run([a], 19)
    T.clean(ra, "A")
    T.check(ra.has(r"link_upnp mapped addr=\? port=%d private=0" % port),
            "A: the port is open, the address unknown")
    T.check(ra.has(r"link_upnp removed") and
            "DeletePortMapping" in router.calls(),
            "the mapping was removed when the room closed")
    T.picture(ra, "A", "host_noaddr.png")
    T.radio_free(ra, "A")


# ---- Ad hoc (the fake radio cell) ---------------------------------------------

def adhoc_net(T, station, peers, **kw):
    net = {"wifi": 1, "adhoc_station": station,
           "adhoc_peers": ",".join(str(p) for p in peers),
           "adhoc_base": T.port + 16, "adhoc_dialog_frames": 40}
    net.update(kw)
    return net


def adhoc_go(sec, join=False, shot1=None):
    """Play together > Host (or Join) > the explanation page > Continue."""
    s = open_play(sec) + "+20:%s:%d\n" % ("down" if join else "cross", PRESS)
    if join:
        s += keys("cross")
    if shot1:
        s += "+20:shot=%s\n" % shot1
    return s + "+%d:cross:%d\n" % (AFTER_SHOT if shot1 else GAP + 8, PRESS)


def t_adhoc(T):
    """Ad hoc: the system's dialog, then PDP -- over the fake radio cell."""
    T.begin("adhoc")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               adhoc_go(EARLY, shot1="explain.png") + "+8:shot=dialog.png\n" +
               shot(5, "hosting.png") + shot(18, "running.png") +
               disconnect(26) + shot(31, "after.png") + quit_at(34),
               config={"net_conn": 2}, net=adhoc_net(T, 1, [2]),
               vita={"link_peek": LT_PEEK})
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               adhoc_go(EARLY, join=True) + "%d:shot=wait.png\n" % at(6) +
               press(7, "cross") + "+14:shot=dialog.png\n" +
               shot(18, "running.png") + shot(31, "after.png") + quit_at(34),
               config={"net_conn": 2}, net=adhoc_net(T, 2, [1]),
               vita={"link_peek": LT_PEEK})
    # B's page waits on "Continue" until 7 s; A is hosting by then.
    b.script = (open_play(EARLY) + "+20:down:%d\n" % PRESS + keys("cross") +
                "%d:shot=explain.png\n" % at(6) + press(7, "cross") +
                "+14:shot=dialog.png\n" + shot(18, "running.png") +
                shot(31, "after.png") + quit_at(34))
    ra, rb = T.run([a, b], 34)
    T.clean(ra, "A")
    T.clean(rb, "B")
    for r, who in ((ra, "A"), (rb, "B")):
        T.check(r.has(r"link_phase 0 -> 1 ") and
                r.has(r"link_adhoc_dialog state=2 abort=0"),
                "%s: the connection screen ran and connected" % who)
        T.check(r.has(r"link_net_up .*arq=40000/30000"),
                "%s: netdrv on the ad hoc profile" % who)
        T.check(r.has(r"vadhoc_stats tx=\d+"), "%s: PDP datagrams flowed "
                "(%s)" % (who, r.evt("vadhoc_stats")[-1]["_"][4:60]
                          if r.evt("vadhoc_stats") else ""))
    T.check(ra.has(r"link_hosting room=\"Room 7\" kind=2"),
            "A hosts by Link Cable in the cell (no room list on ad hoc)")
    T.check(rb.has(r"link_running player=2") and
            ra.has(r"link_running player=1"), "the two link: P1 and P2")
    d = ra.evt("link_session_begin")[-1]
    T.check(d["how"] == "kind" and d["delay"] == "3",
            "ad hoc has no measurement: the delay is the kind's, 3 (%s/%s)" %
            (d["delay"], d["how"]))
    T.same_machines(ra, rb)
    T.linktest_verdict(ra, "A", 1)
    T.linktest_verdict(rb, "B", 2)
    for r, who in ((ra, "A"), (rb, "B")):
        downs = r.evt("link_net_down")
        T.check(downs and downs[-1]["radio"] == "0",
                "%s: the radio is free again (ad hoc given back)" % who)
    for r, who, names in ((ra, "A", ("explain.png", "dialog.png",
                                     "hosting.png", "running.png")),
                          (rb, "B", ("explain.png", "dialog.png",
                                     "running.png"))):
        for n in names:
            T.picture(r, who, n)


# ---- when it does not work -------------------------------------------------------

def still_linked(r, name="end.png"):
    """The twin's session was up when it took its last picture, long after
    the third console was turned away.  (Both then quit at the same moment,
    and whichever is a little later may see the other go: that is not the
    third console's doing.)"""
    if not r.has(r"link_running") or not r.has(r"host_shot .*file=%s" % name):
        return False
    return (not r.has(r"link_ended") or
            r.index(r"link_ended") > r.index(r"host_shot .*file=%s" % name))


def t_room_full(T):
    """A third console: the list says full; with no list, netdrv refuses."""
    global EARLY_CHORD
    EARLY_CHORD = 300 if T.slow else 170
    try:
        room_full(T)
    finally:
        EARLY_CHORD = 170


def room_full(T):
    T.begin("room_full")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + shot(28, "end.png") + quit_at(30))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(EARLY, wait=2.5) + shot(28, "end.png") + quit_at(30))
    c = T.twin("c", T.arch_a, LINKTEST, "Carol",
               join_first(12, wait=3, shot="list.png") +
               "+30:shot=refused.png\n" + close_menu() + keys("circle") +
               shot(24, "after.png") + quit_at(30))
    ra, rb, rc = T.run([a, b, c], 30)
    T.clean(rc, "C")
    rooms = rc.evt("link_scan_room")
    T.check(rooms and rooms[-1]["players"] == "2/2" and
            rooms[-1]["open"] == "0",
            "C's list shows the room with its two players, not open (%s)" %
            (rooms[-1]["_"][4:] if rooms else "no room"))
    T.check(rc.has(r"join_refused id=\w+ why=\".*(full|middle of a game)"),
            "C picks it and is told why not (%s)" %
            (rc.evt("join_refused")[-1]["why"] if rc.evt("join_refused")
             else "nothing"))
    T.check(not rc.has(r"link_action"), "nothing was started on C")
    T.check(still_linked(ra) and still_linked(rb),
            "A and B's session was not disturbed")
    T.picture(rc, "C", "list.png")
    T.picture(rc, "C", "refused.png")

    # Ad hoc has no list: the third console asks and netdrv turns it away.
    T.begin("room_full_adhoc")
    cfg = {"net_conn": 2}
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               adhoc_go(EARLY) + shot(30, "end.png") + quit_at(32),
               config=cfg, net=adhoc_net(T, 1, [2, 3]))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               adhoc_go(4, join=True) + shot(30, "end.png") + quit_at(32),
               config=cfg, net=adhoc_net(T, 2, [1, 3]))
    c = T.twin("c", T.arch_a, LINKTEST, "Carol",
               adhoc_go(14, join=True) + shot(22, "full.png") +
               press(24, "circle") + shot(27, "after.png") + quit_at(32),
               config=cfg, net=adhoc_net(T, 3, [1, 2]))
    ra, rb, rc = T.run([a, b, c], 32)
    T.clean(rc, "C")
    T.check(rc.has(r"link_join_refused why=1 stop=3") and
            rc.has(r"link_fail_screen code=2"),
            "C: netdrv refused the JOIN (the room has its two) and the "
            "screen says the room is full")
    T.check(rc.has(r"gblink_resume how=restore_prelink rc=0") and
            rc.evt("link_cheats")[-1]["suspended"] == "0",
            "C: back in its own game where it was, cheats returned")
    T.check(still_linked(ra) and still_linked(rb),
            "A and B's session was not disturbed")
    T.picture(rc, "C", "full.png")
    downs = rc.evt("link_net_down")
    T.check(downs and downs[-1]["radio"] == "0", "C: the radio is free")


def t_different_build(T):
    """Another release, another CPU backend: listed, greyed, never linked."""
    T.begin("different_build")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice", host(EARLY) + quit_at(20))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(3, wait=3, shot="list.png") +
               "+30:shot=refused.png\n" + close_menu() + keys("circle") +
               quit_at(20), vita={"link_build": "3.6.0"})
    ra, rb = T.run([a, b], 20)
    T.clean(ra, "A")
    T.clean(rb, "B")
    rooms = rb.evt("link_scan_room")
    T.check(rooms and rooms[-1]["compatible"] == "0",
            "B (another release) lists A's room as not compatible")
    T.check(rb.has(r"join_refused .*different version") and
            not rb.has(r"link_action"),
            "B is told 'a different version of GBAdhoc' and nothing starts")
    T.check(not ra.has(r"peer_connected"), "A never saw a joiner")
    T.picture(rb, "B", "list.png")
    T.picture(rb, "B", "refused.png")

    # With no list (ad hoc) the two never answer each other: the protocol
    # string carries the build.
    T.begin("different_build_adhoc")
    cfg = {"net_conn": 2}
    a = T.twin("a", T.arch_a, LINKTEST, "Alice", adhoc_go(EARLY) + quit_at(36),
               config=cfg, net=adhoc_net(T, 1, [2]))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               adhoc_go(4, join=True) + shot(12, "finding.png") +
               shot(29, "noanswer.png") + press(30, "circle") +
               shot(33, "after.png") + quit_at(36),
               config=cfg, net=adhoc_net(T, 2, [1]),
               vita={"link_build": "3.6.0"})
    ra, rb = T.run([a, b], 36)
    T.clean(ra, "A")
    T.clean(rb, "B")
    pa = ra.evt("link_net_up")[-1]["proto"]
    pb = rb.evt("link_net_up")[-1]["proto"]
    T.check(pa != pb and pa.startswith('"GBAdhoc cable v1/'),
            "the two builds speak different protocol strings (%s / %s)" %
            (pa, pb))
    T.check(not ra.has(r"peer_connected") and
            rb.has(r"link_join_refused why=2") and
            rb.has(r"link_fail_screen code=1"),
            "B is never answered and says so after its time limit")
    T.picture(rb, "B", "finding.png")
    T.picture(rb, "B", "noanswer.png")
    downs = rb.evt("link_net_down")
    T.check(downs and downs[-1]["radio"] == "0", "B: the radio is free")


def t_host_quits(T):
    """The host quits to the game list in the middle of a session."""
    T.begin("host_quits")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + "%d:start+select:24\n" % at(16) +
               keys("down", "down", "down", "down", "down", "cross", "cross") +
               shot(21, "list.png") + quit_at(30))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(EARLY, wait=2.5) + shot(14, "running.png") +
               shot(19.5, "lost.png") + shot(27, "after.png") + quit_at(30))
    ra, rb = T.run([a, b], 30)
    T.clean(ra, "A")
    T.clean(rb, "B")
    T.check(ra.has(r"game_end reason=game_list") and
            ra.has(r"link_session_abandoned") and
            ra.has(r"gblink_resume how=carry_on rc=0"),
            "A: quit to the game list; the session is abandoned and its own "
            "game kept as it stood")
    T.check(ra.index(r"link_net_down radio=0") < ra.index(r"game_unload"),
            "A: the radio was given back before the game was unloaded")
    T.picture(ra, "A", "list.png")
    T.check(rb.has(r"link_ended state=4 ran=1 err=peer_lost") and
            rb.has(r"gblink_resume how=carry_on rc=0"),
            "B: 'link lost', and its game carries on")
    T.radio_free(rb, "B")
    e1, e2 = rb.shot_evt("lost.png"), rb.shot_evt("after.png")
    T.check(e1 and e2 and int(e2["emu"]) > int(e1["emu"]) + 200,
            "B: the game runs on afterwards (%s -> %s frames)" %
            (e1 and e1["emu"], e2 and e2["emu"]))
    T.picture(rb, "B", "lost.png")


def lost_run(T, name, how):
    """B is killed (or frozen) mid-session; how long until A says so."""
    T.begin(name)
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + shot(14, "running.png") + shot(18.5, "waiting.png") +
               shot(29, "after.png") + quit_at(40))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(EARLY, wait=2.5) + quit_at(40))
    t = {}

    def strike():
        t["ok"] = a.wait_log(r"link_running", 5)
        b.signal(signal.SIGKILL if how == "kill" else signal.SIGSTOP)
        t["at"] = time.time()
        t["seen"] = a.wait_log(r"link_ended ", 30 * T.k)
        t["took"] = time.time() - t["at"]

    def revive():
        b.signal(signal.SIGCONT)

    actions = [(16, strike)]
    if how == "stop":
        actions.append((32, revive))
    ra, rb = T.run([a, b], 40, actions)
    T.clean(ra, "A")
    T.check(t.get("ok"), "the session was running when B was struck")
    T.check(t.get("seen") and t["took"] < 12 * T.k,
            "A says the link is lost %.1f s after B %s (netdrv's limit is "
            "8 s of silence)" % (t.get("took", -1),
                                 "was killed" if how == "kill" else "froze"))
    T.check(ra.has(r"link_stall_note"),
            "A: 'Waiting for the other Vita...' while it waits")
    T.check(ra.has(r"link_ended state=4 ran=1 err=peer_lost") and
            ra.has(r"gblink_resume how=carry_on rc=0"),
            "A: link lost, its game carries on from where it stands")
    T.radio_free(ra, "A")
    e1, e2 = ra.shot_evt("after.png"), None
    T.check(e1 and int(e1["emu"]) > 0 and ra.has(r"^EVT exit ok"),
            "A plays on and leaves cleanly")
    T.picture(ra, "A", "waiting.png")
    T.picture(ra, "A", "after.png")
    if how == "stop":
        T.check(rb.rc == 0 and rb.has(r"link_ended state=4 ran=1") and
                rb.has(r"link_net_down radio=0"),
                "B, thawed 16 s later: finds the link gone, ends it, plays "
                "on and leaves cleanly (exit %s)" % rb.rc)
    return t.get("took", -1)


def t_link_lost(T):
    """The other console dies, or freezes: said within the time limit."""
    k = lost_run(T, "lost_kill", "kill")
    s = lost_run(T, "lost_stop", "stop")
    T.S.note("link lost noticed after %.1f s (killed), %.1f s (frozen)" %
             (k, s))


def t_lossy(T):
    """A bad network: loss, latency, jitter.  The session holds, in step."""
    variants = (
        ("loss5", {"link_loss": 5}, 24),
        ("loss20", {"link_loss": 20}, 40),
        ("slow", {"link_latency": 80, "link_jitter": 30}, 26),
        ("bad", {"link_loss": 5, "link_dup": 2, "link_latency": 40,
                 "link_jitter": 40}, 26))
    if T.slow:
        # Under qemu: two of them.  At 20% loss the states cross one
        # datagram a turn of the loop (fe_gblink halves its rate on loss),
        # some 1500 turns: 25 s at 60 Hz, two minutes at the 12 Hz of an
        # emulated ARM twin -- which is given the time.
        variants = (("loss20", {"link_loss": 20}, 64),
                    (variants[3][0], variants[3][1], 34))
    for name, fault, secs in variants:
        T.begin("lossy_" + name)
        vi = dict(fault, link_peek=LT_PEEK)
        a = T.twin("a", T.arch_a, LINKTEST, "Alice",
                   host(EARLY) + shot(secs - 6, "running.png") +
                   disconnect(secs) + quit_at(secs + 10), vita=vi)
        b = T.twin("b", T.arch_b, LINKTEST, "Bob",
                   join_first(EARLY, wait=2.5) + shot(secs - 6, "running.png") +
                   quit_at(secs + 10), vita=vi)
        ra, rb = T.run([a, b], secs + 10)
        T.clean(ra, "A " + name)
        T.clean(rb, "B " + name)
        T.check(ra.has(r"gblink_done ") and rb.has(r"gblink_done "),
                "%s: the session ran and ended by agreement on both" % name)
        T.same_machines(ra, rb)
        T.linktest_verdict(ra, "A " + name, 1, loops=0)
        T.linktest_verdict(rb, "B " + name, 2, loops=0)
        sa, sb = ra.evt("gblink_stats")[-1], rb.evt("gblink_stats")[-1]
        la = ra.evt("lan_stats")[-1] if ra.evt("lan_stats") else {}
        d = ra.evt("link_session_begin")[-1]
        f0 = int(sa["f0"])
        # (an emulated ARM twin on a busy machine runs five frames a second)
        T.check(f0 > (150 if T.slow else 300),
                "%s: the machines ran (%d frames)" % (name, f0))
        if "link_latency" in fault:
            T.check(d["how"] == "measured" and int(d["delay"]) >=
                    2 + fault["link_latency"] // 17,
                    "%s: the measured delay covers the latency (%s frames "
                    "for %d ms each way)" % (name, d["delay"],
                                             fault["link_latency"]))
        T.S.note("%s: delay %s (%s), %d frames, stalls A %s B %s (longest "
                 "%s/%s), resend requests %s/%s, shim dropped/delayed/dup %s"
                 % (name, d["delay"], d["how"], f0, sa["stalls"], sb["stalls"],
                    sa["streak_max"], sb["streak_max"], sa["nacks"],
                    sb["nacks"], la.get("fault", "-")))


def t_wifi_lost(T):
    """The Wi-Fi goes away: before hosting, while hosting, during a session."""
    # before: the page's own answer (no Wi-Fi), nothing started
    T.begin("wifi_before")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY, "nowifi.png") + close_menu() + quit_at(8),
               net={"wifi": 0})
    ra, = T.run([a], 8)
    T.clean(ra, "A")
    T.check(not ra.has(r"link_action") and not ra.has(r"play_host"),
            "no Wi-Fi: Host a game is not even asked for")
    T.picture(ra, "A", "nowifi.png")

    # while hosting: the room closes, the page says why; when the Wi-Fi is
    # back, hosting works again and a session forms
    T.begin("wifi_hosting")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + shot(5, "hosting.png") + shot(10, "closed.png") +
               close_menu() + host(16) + shot(26, "running.png") +
               quit_at(40), vita={"link_peek": LT_PEEK})
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(18, wait=3) + shot(26, "running.png") + quit_at(40))
    ra, rb = T.run([a, b], 40, [(6, lambda: a.write_net(wifi=0)),
                               (12, lambda: a.write_net(wifi=1))])
    T.clean(ra, "A")
    T.check(ra.has(r"link_lost why=4 phase=3"),
            "A, hosting: the Wi-Fi's loss is noticed")
    T.check(ra.index(r"link_net_down radio=0") > ra.index(r"link_lost why=4"),
            "A: the room is closed and the radio released")
    T.picture(ra, "A", "closed.png")
    T.check(len(ra.evt("link_hosting")) == 2 and ra.has(r"link_running") and
            rb.has(r"link_running"),
            "with the Wi-Fi back A hosts again and B joins: a session")

    # during a session
    T.begin("wifi_session")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + shot(19.5, "lost.png") + shot(26, "after.png") +
               quit_at(30))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(EARLY, wait=2.5) + shot(22, "lost.png") + quit_at(30))
    ra, rb = T.run([a, b], 30, [(16, lambda: a.write_net(wifi=0))])
    T.clean(ra, "A")
    T.clean(rb, "B")
    T.check(ra.has(r"link_lost why=4 phase=5 session=1") and
            ra.has(r"link_ended state=4 ran=1 err=peer_lost .*why=4") and
            ra.has(r"gblink_resume how=carry_on rc=0"),
            "A: Wi-Fi lost in a session: the link ends, saying so, and the "
            "game carries on")
    T.radio_free(ra, "A")
    T.check(rb.has(r"link_ended state=4 ran=1") and
            rb.has(r"gblink_resume how=carry_on"),
            "B: link lost, its game carries on")
    T.picture(ra, "A", "lost.png")
    e1, e2 = ra.shot_evt("lost.png"), ra.shot_evt("after.png")
    T.check(e1 and e2 and int(e2["emu"]) > int(e1["emu"]) + 200,
            "A: the game runs on afterwards")


def t_cancel(T):
    """Cancel at every step of a bring-up; the next attempt works."""
    # 1. the host stops hosting; 2. the joiner cancels while the session
    # sets up -- the host keeps its room -- and joins again
    T.begin("cancel_wifi")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + keys("down", "cross") + "+30:shot=stopped.png\n" +
               close_menu() + host(6) + shot(15, "kept.png") +
               shot(26, "running.png") + quit_at(30))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(8, wait=2.5) + "+40:circle:%d\n" % PRESS +
               "+50:shot=cancelled.png\n" +
               join_first(17, wait=2.5) + shot(26, "running.png") +
               quit_at(30))
    ra, rb = T.run([a, b], 30)
    T.clean(ra, "A")
    T.clean(rb, "B")
    T.check(ra.has(r"link_disconnect phase=3") and
            len(ra.evt("link_hosting")) == 2,
            "A: Stop hosting closes the room; hosting again opens it")
    T.check(rb.has(r"link_cancel ticks=") and
            rb.has(r"link_ended state=4 ran=0 err=cancelled .*why=3") and
            rb.has(r"gblink_resume how=restore_prelink rc=0"),
            "B: O on the LINKING screen cancels; its game is where it was")
    T.check(ra.has(r"link_host_keeps_room") and ra.has(r"link_host_clear"),
            "A: the visitor left before the link started; the room stays")
    T.check(ra.has(r"link_running") and rb.has(r"link_running") and
            len(rb.evt("link_session_begin")) == 2,
            "B joins again and the session forms")
    for r, who, names in ((ra, "A", ("stopped.png", "kept.png")),
                          (rb, "B", ("cancelled.png",))):
        for n in names:
            T.picture(r, who, n)

    # 3. ad hoc: Cancel on the waiting page while the system's screen is
    # up; then the player closes that screen himself; then it works
    T.begin("cancel_adhoc")
    cfg = {"net_conn": 2}
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               adhoc_go(EARLY) + "+30:shot=dialog.png\n" +
               keys("down", "cross", first=AFTER_SHOT) +
               "+40:shot=cancelled.png\n" + close_menu() +
               adhoc_go(9) + shot(15, "closed.png") + close_menu() +
               adhoc_go(20) + shot(26, "hosting.png") + quit_at(40),
               config=cfg, net=adhoc_net(T, 1, [2], adhoc_dialog_frames=400))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               adhoc_go(9, join=True) + shot(15, "closed.png") +
               press(16, "circle") + shot(18, "back.png") +
               adhoc_go(27, join=True) + shot(36, "running.png") +
               quit_at(40), config=cfg,
               net=adhoc_net(T, 2, [1], adhoc_dialog_result=1))
    ra, rb = T.run([a, b], 40, [
        (7, lambda: a.write_net(adhoc_dialog_frames=40, adhoc_dialog_result=1)),
        (18, lambda: (a.write_net(adhoc_dialog_result=0),
                      b.write_net(adhoc_dialog_result=0)))])
    T.clean(ra, "A")
    T.clean(rb, "B")
    dl = [d["_"] for d in ra.evt("link_adhoc_dialog")]
    T.check(len(dl) == 3 and "abort=1" in dl[0] and "state=3 abort=0" in dl[1]
            and "state=2" in dl[2],
            "A: cancelled from the waiting page; closed by the player; "
            "connected (%s)" % " | ".join(x[22:40] for x in dl))
    downs = ra.evt("link_net_down")
    T.check(ra.has(r"link_failed code=9 host=1"),
            "A: 'the connection screen was closed' is the page's reason")
    T.check(rb.has(r"link_failed code=9 host=0") and
            rb.has(r"link_phase 6 -> 0"),
            "B: the same on the LINKING screen, until O")
    T.check(rb.has(r"link_running") and ra.has(r"link_running"),
            "the third time both connect and the session forms")
    for r, who, names in ((ra, "A", ("dialog.png", "cancelled.png",
                                     "closed.png")),
                          (rb, "B", ("closed.png", "back.png"))):
        for n in names:
            T.picture(r, who, n)

    # 4. a Wireless Adapter join nobody answers: O cancels; left alone it
    # gives up by itself
    T.begin("cancel_adapter")
    lm = {"roms/linkmode.txt": "linktest.gba = adapter\n"}
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               adhoc_go(EARLY, join=True) + shot(5, "joining.png") +
               press(6, "circle") + shot(8, "cancelled.png") +
               adhoc_go(9, join=True) + shot(32, "noanswer.png") +
               press(33, "circle") + shot(35, "after.png") + quit_at(36),
               config={"net_conn": 2}, net=adhoc_net(T, 2, [1]), extra=lm)
    rb, = T.run([b], 36)
    T.clean(rb, "B")
    T.check(rb.has(r"link_phase 1 -> 2 kind=1") and
            rb.has(r"link_cancel phase=joining"),
            "B: 'Joining by Wireless Adapter', cancelled with O")
    T.check(rb.has(r"link_failed code=1 host=0 phase=2"),
            "B: the second time nobody answers and it says so by itself")
    downs = rb.evt("link_net_down")
    T.check(len(downs) == 2 and all(d["radio"] == "0" for d in downs),
            "B: the radio is free after each")
    T.check(rb.evt("link_cheats")[-1]["suspended"] == "0",
            "B: cheats are back")
    for n in ("joining.png", "cancelled.png", "noanswer.png", "after.png"):
        T.picture(rb, "B", n)


def t_suspend(T):
    """The console sleeps in the middle of a session."""
    T.begin("suspend")
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(EARLY) + "%d:suspend\n" % at(16) + shot(17, "wake.png") +
               press(18, "cross") + shot(19.5, "note.png") +
               shot(26, "after.png") + quit_at(30))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(EARLY, wait=2.5) + shot(19, "lost.png") + quit_at(30))
    ra, rb = T.run([a, b], 30)
    T.clean(ra, "A")
    T.clean(rb, "B")
    T.check(ra.has(r"link_suspend phase=5 session=1") and
            ra.index(r"link_net_down radio=0") < ra.index(r"power resume"),
            "A: the session is ended and the radio released before sleeping")
    T.check(ra.has(r"gblink_resume how=carry_on rc=0") and
            ra.has(r"link_resume note=sleep"),
            "A: its game is kept, and it says why the link ended on waking")
    T.check(rb.has(r"link_ended state=4 ran=1 err=peer_lost"),
            "B is told at once (BYE), not after a time-out")
    T.picture(ra, "A", "wake.png")
    T.picture(ra, "A", "note.png")
    e1, e2 = ra.shot_evt("note.png"), ra.shot_evt("after.png")
    T.check(e1 and e2 and int(e2["emu"]) > int(e1["emu"]) + 200,
            "A: the game runs on after waking")


def t_mixed_refuse(T):
    """An x86 console and an ARM one: no mirror between different backends."""
    T.begin("mixed_refuse")
    # A (x86) hosts by Link Cable; B (ARM) sees the room and may not enter.
    # Then B hosts and A may not enter.
    a = T.twin("a", T.arch_a, LINKTEST, "Alice",
               host(2) + press(13, "down") + keys("cross") + close_menu() +
               join_first(24, wait=3, shot="list.png") +
               "+40:shot=refused.png\n" + close_menu() + keys("circle") +
               quit_at(36))
    b = T.twin("b", T.arch_b, LINKTEST, "Bob",
               join_first(5, wait=3, shot="list.png") +
               "+40:shot=refused.png\n" + close_menu() + keys("circle") +
               host(17) + quit_at(36))
    ra, rb = T.run([a, b], 36)
    T.clean(ra, "A")
    T.clean(rb, "B")
    ca = ra.evt("link_compat")[-1]
    cb = rb.evt("link_compat")[-1]
    T.check(ca["backend"] != cb["backend"] and ca["cable"] != cb["cable"],
            "the two backends have different Link Cable words (%s %s / %s %s)"
            % (ca["backend"], ca["cable"], cb["backend"], cb["cable"]))
    T.check(ca["gb"] == cb["gb"] and ca["adapter"] == cb["adapter"],
            "and the same Game Boy and Wireless Adapter words")
    pa = ra.evt("link_net_up")[-1]["proto"]
    pb = rb.evt("link_net_up")[-1]["proto"]
    T.check(pa != pb, "their netdrv protocol strings differ too: with no "
            "list (ad hoc) neither answers the other (%s / %s)" % (pa, pb))
    for r, who, other in ((rb, "B (ARM)", "A"), (ra, "A (x86)", "B")):
        rooms = r.evt("link_scan_room")
        T.check(rooms and rooms[-1]["kind"] == "1" and
                rooms[-1]["compatible"] == "0",
                "%s lists %s's Link Cable room as not compatible" %
                (who, other))
        T.check(r.has(r"join_refused .*different version") and
                not r.has(r"link_action role=join"),
                "%s is told so and no join starts" % who)
    T.check(not ra.has(r"peer_connected") and not rb.has(r"peer_connected")
            and not ra.has(r"gblink_create") and not rb.has(r"gblink_create"),
            "no session was ever attempted")
    T.picture(rb, "B", "refused.png")


TESTS = [("wifi_cable", t_wifi_cable), ("fast_link", t_fast_link), ("share", t_share), ("share_replace", t_share_replace), ("mgift", t_mgift), ("gb_two_carts", t_gb_two_carts), ("gb_extra_folder", t_gb_extra_folder), ("three_sessions", t_three_sessions),
         ("wifi_gb", t_wifi_gb), ("wifi_adapter", t_wifi_adapter),
         ("internet", t_internet), ("adhoc", t_adhoc),
         ("room_full", t_room_full), ("different_build", t_different_build),
         ("host_quits", t_host_quits), ("link_lost", t_link_lost),
         ("lossy", t_lossy), ("wifi_lost", t_wifi_lost),
         ("cancel", t_cancel), ("suspend", t_suspend),
         ("mixed_refuse", t_mixed_refuse)]
# Which tests a mode runs when none are named.  x86: everything but the
# mixed pair's.  armhf (under qemu, five times the wall time): the sessions
# and what depends on the machines or on timing; the menus' own refusals
# and cancels are the same code on any CPU and stay with x86.  mixed: the
# refusal, and the two kinds of link that do not need matching backends.
RUNS = {
    "x86": [n for n, _ in TESTS if n != "mixed_refuse"],
    "armhf": ["wifi_cable", "three_sessions", "wifi_gb", "wifi_adapter",
              "internet", "adhoc", "room_full", "host_quits", "link_lost",
              "lossy", "suspend"],
    "mixed": ["mixed_refuse", "wifi_gb", "wifi_adapter"],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", nargs="?", default="x86",
                    choices=("x86", "armhf", "mixed"))
    ap.add_argument("--only")
    ap.add_argument("--hz", type=int, default=0,
                    help="the twins' display rate (60; 12 under qemu)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        print(" ".join(n for n, _ in TESTS))
        return
    T = Tests(a.mode, a.hz)
    for arch in {T.arch_a, T.arch_b}:
        if not os.path.exists(exe_of(arch)):
            sys.exit("no %s: make ARCH=%s first" % (exe_of(arch), arch))
    for f in (LINKTEST, GBLINK):
        if not os.path.exists(f):
            sys.exit("no %s (make -C testrom gb RGBDS=...)" % f)
    only = a.only.split(",") if a.only else None
    try:
        for name, fn in TESTS:
            if only and name not in only:
                continue
            if not only and name not in RUNS[a.mode]:
                continue
            T.S.test = name
            t0 = time.time()
            print("%s: %s" % (name, fn.__doc__.split("\n")[0]), flush=True)
            before = len(T.S.fails)
            try:
                fn(T)
            except Exception as e:
                import traceback
                traceback.print_exc()
                T.S.check(False, "the test itself raised %r" % (e,))
            finally:
                T.kill_all()
            print("    %s  (%.0f s)" % ("ok" if len(T.S.fails) == before
                                        else "FAILED", time.time() - t0),
                  flush=True)
    finally:
        T.kill_all()
    sys.exit(T.finish())


if __name__ == "__main__":
    main()
