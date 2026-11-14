#!/usr/bin/env python3
"""Plain unit tests for matrix.py (no network, no emulator): python3 test_matrix.py"""
import hashlib, os, shutil, sys, tempfile, unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matrix as M

GBA = """LOG config loaded
EVT switches autoexit_frames=600 ts_us=1000000
EVT link_boot net=1 fault=0/0/0/0 ts_us=1100000
EVT fps emu=59.78 rendered=49.42 skipped=104 win=600 dt_us=10036157 ts_us=10842022
EVT vita_win loops=600 frames=704 fps_x1000=60164 work_us=3271/90036 late=8 forgiven=0 ts_us=12588588
EVT fps emu=57.00 rendered=57.00 skipped=0 win=600 dt_us=10000000 ts_us=20842022
EVT vita_win loops=600 frames=600 fps_x1000=60000 late=2 ts_us=22588588
EVT handoff: on=1 depth=2/2 produced=55 dropped=1 timeouts=2 ts_us=2000000
EVT handoff: on=1 depth=2/2 produced=30 dropped=0 timeouts=0 ts_us=3000000
EVT look: lvl=0/6 mask=0x0 t=1 ts_us=2100000
EVT look: lvl=3/6 mask=0x7 t=2 ts_us=3100000
EVT vita_jit_worst frame=0 xlat_us=38802 sync_us=293 | win frames=329 xlat_us=115665 sync_us=99999 ts_us=12730096
EVT vita_jit_worst frame=7 xlat_us=112 sync_us=1005 | win frames=1 xlat_us=1 ts_us=22730096
EVT exit ok
"""
SNES = """t=1 fps=60.0 emu_fps=60.0 emu_ms=1
t=2 fps=58.0 emu_fps=58.0 emu_ms=1
A t=0 in=1 out=2 ratio=-100/+200/+300 base=1.0 ur=3/3 drop=0
A t=1 in=1 out=2 ratio=-100/+400/+300 base=1.0 ur=0/5 drop=0
"""


class Parser(unittest.TestCase):
    def test_table_columns(self):
        L = M.Logs(GBA, SNES, "", 60)
        row = dict(zip([c[0] for c in M.COLUMNS], M.summarize_row({"name": "x", "status": "ok"}, L)[2:]))
        self.assertTrue(row["missed/s"].endswith(" L"))          # fallback to late=
        self.assertTrue(row["missed/s"].startswith("0.5"))        # (8/10.0+... ) per second, see below
        self.assertEqual(row["emu fps"], "58.4 (min 57.0)")
        self.assertEqual(row["handoff"], "t2/d1")
        self.assertEqual(row["look"], "3/6")
        self.assertEqual(row["jit xlat/sync us"], "38802/1005")   # only the part before '|'
        self.assertEqual(row["snes fps"], "59.0 (min 58.0)")
        self.assertEqual(row["snes audio"], "r+300 ur5")
        self.assertEqual(row["faults"], "0")

    def test_vc_counter_wins_over_late(self):
        # vc_miss= on the once-a-second handoff: line is that second's count
        # (vita_gxm repeats it and must not be counted twice)
        log = GBA + ("EVT handoff: on=1 vc_miss=4 vc_presents=56 ts_us=3000000\n"
                     "EVT vita_gxm vc_miss=4 ts_us=3000100\n"
                     "EVT handoff: on=1 vc_miss=2 vc_presents=58 ts_us=4000000\n")
        r = M.col_miss(M.Logs(log))
        self.assertFalse(r.endswith(" L"))
        self.assertTrue(r.startswith("3.00 (max 4.00)"), r)   # (4 + 2) / 2

    def test_unknown_and_empty_give_dash(self):
        L = M.Logs("", "", "", 10)
        row = M.summarize_row({"name": "n", "status": "no-log"}, L)
        self.assertEqual(row[2:], ["-"] * len(M.COLUMNS))
        self.assertEqual(M.Logs("LOG only\nEVT unknown a=b\n").evts[0][0], "unknown")
        self.assertEqual(M.col_look(M.Logs("EVT look: weird\n")), "-")

    def test_broken_column_does_not_kill_the_table(self):
        old = M.COLUMNS[:]
        M.COLUMNS.append(("bad", lambda L: 1 / 0))
        try:
            self.assertEqual(M.summarize_row({}, M.Logs())[-1], "-")
        finally:
            M.COLUMNS[:] = old

    def test_faults(self):
        self.assertEqual(M.col_faults(M.Logs("EVT link_boot fault=0/0/0/0\n")), "0")
        self.assertTrue(M.col_faults(M.Logs("EVT link_boot fault=1/0/0/0\n")).startswith("1"))
        self.assertTrue(M.col_faults(M.Logs("EVT exit ok", "", "xx |C| boom\n invalid_access\n")).startswith("2"))
        self.assertEqual(M.col_faults(M.Logs("EVT exit ok", "", "")), "0")


class Playlist(unittest.TestCase):
    TEXT = """# comment
@base core4=off look_gov=0 t.core4=off
Golden Sun (UE) [!].gba | 60s | | baseline   # trailing comment
Super Metroid.smc | 30 | t.spin=100 auto_xbrz=0 | snes
@base
Fire Red.gba | 3600f | xbrz=4
Mario Golf.gba
"""

    def test_parse(self):
        e = M.parse_playlist(self.TEXT, default_secs=45)
        self.assertEqual(len(e), 4)
        self.assertEqual((e[0].game, e[0].secs, e[0].label), ("Golden Sun (UE) [!].gba", 60.0, "baseline"))
        self.assertEqual(e[0].vita, {"core4": "off", "look_gov": "0"})
        self.assertEqual(e[0].threads, {"core4": "off"})
        self.assertTrue(e[1].is_snes)
        self.assertEqual(e[1].threads, {"core4": "off", "spin": "100"})
        self.assertEqual(e[2].secs, 60.0)                          # 3600 frames
        self.assertEqual(e[2].vita, {"xbrz": "4"})                 # @base cleared
        self.assertEqual(e[3].secs, 45.0)

    def test_errors_carry_line_numbers(self):
        for bad in ("a.gba | 5x", "a.gba | 5 | novalue", "| 5", "a|1|b=c|d|e"):
            with self.assertRaises(M.PlaylistError) as c:
                M.parse_playlist(bad)
            self.assertIn("line 1", str(c.exception))


class FakeApp(M.DirBackend):
    """a DirBackend whose 'app' writes logs on launch, or fails/interrupts"""
    startup_s = 0

    def __init__(self, root, mode="ok"):
        super().__init__(root)
        self.mode, self.stopped = mode, 0

    def launch(self):
        if self.mode == "fail":
            raise RuntimeError("launch refused")
        if self.mode == "int":
            raise KeyboardInterrupt()
        self.put(M.GBA_LOG, GBA.encode())
        if self.get(M.AUTOBOOT).startswith(b"S"):
            self.put(M.SNES_LOG, SNES.encode())

    def running(self):
        return False

    def stop(self):
        self.stopped += 1


def tree_hash(root):
    h = hashlib.sha256()
    for d, _, fs in sorted(os.walk(root)):
        for f in sorted(fs):
            p = os.path.join(d, f)
            h.update(os.path.relpath(p, root).encode())
            h.update(open(p, "rb").read())
    return h.hexdigest()


class Restore(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp()
        self.data = os.path.join(self.t, "data")
        os.makedirs(os.path.join(self.data, "roms"))
        open(os.path.join(self.data, "roms", "x.gba"), "wb").write(b"rom")
        self.pl = os.path.join(self.t, "p.txt")
        open(self.pl, "w").write("Golden.gba | 5s | k=v\nSuper.smc | 5s | t.spin=1\n")
        self._mk = M.make_backend

    def tearDown(self):
        M.make_backend = self._mk
        shutil.rmtree(self.t)

    def go(self, mode, extra=()):
        M.make_backend = lambda a: FakeApp(self.data, mode)
        return M.main([self.pl, "--backend", "dir", "--data-dir", self.data,
                       "--out", os.path.join(self.t, "out")] + list(extra))

    def seed_owner(self):
        for rel, d in ((M.VITA_INI, b"mine=1\n"), (M.THREADS_INI, b"spin=9\n"), (M.AUTOBOOT, b"Old.gba\n"),
                       (M.GBA_LOG, b"old log\n"), (M.SNES_LOG, b"old snes\n")):
            M.DirBackend(self.data).put(rel, d)

    def test_files_existed(self):
        self.seed_owner()
        before = tree_hash(self.data)
        self.assertEqual(self.go("ok"), 0)
        self.assertEqual(tree_hash(self.data), before)
        out = os.path.join(self.t, "out")
        self.assertTrue(os.path.isfile(os.path.join(out, "01-Golden-gba", "gbadhoc.log")))
        self.assertTrue(os.path.isfile(os.path.join(out, "02-Super-smc", "snes-log.txt")))
        self.assertIn("faults", open(os.path.join(out, "summary.txt")).read())

    def test_files_did_not_exist(self):
        before = tree_hash(self.data)
        self.assertEqual(self.go("ok"), 0)
        self.assertEqual(tree_hash(self.data), before)
        for rel in M.MANAGED:
            self.assertIsNone(M.DirBackend(self.data).get(rel), rel)

    def test_written_files_during_run(self):
        # what the entry really wrote (checked with a backend that records it)
        seen = {}

        class Rec(FakeApp):
            def launch(s):
                seen[s.get(M.AUTOBOOT)] = (s.get(M.VITA_INI), s.get(M.THREADS_INI))
                super().launch()
        M.make_backend = lambda a: Rec(self.data)
        M.main([self.pl, "--backend", "dir", "--data-dir", self.data, "--out", os.path.join(self.t, "o2")])
        self.assertEqual(seen[b"Golden.gba\n"], (b"k=v\nautoexit_frames=300\n", None))
        self.assertEqual(seen[b"Super.smc\n"], (b"autoexit_frames=300\n", b"spin=1\n"))

    def test_failure_restores(self):
        self.seed_owner()
        before = tree_hash(self.data)
        self.assertEqual(self.go("fail"), 2)
        self.assertEqual(tree_hash(self.data), before)

    def test_ctrl_c_restores(self):
        self.seed_owner()
        before = tree_hash(self.data)
        self.assertEqual(self.go("int"), 130)
        self.assertEqual(tree_hash(self.data), before)

    def test_restore_command_after_a_killed_run(self):
        self.seed_owner()
        before = tree_hash(self.data)
        be = M.DirBackend(self.data)
        sess = M.Session(be, os.path.join(self.t, "out", "backup"))
        sess.backup()
        be.put(M.VITA_INI, b"clobbered\n")
        be.delete(M.AUTOBOOT)
        be.put(M.SNES_LOG, b"new\n")
        self.assertEqual(M.main(["--restore", os.path.join(self.t, "out"), "--backend", "dir",
                                 "--data-dir", self.data]), 0)
        self.assertEqual(tree_hash(self.data), before)

    def test_restore_reports_problem(self):
        be = M.DirBackend(self.data)
        be.put(M.VITA_INI, b"a\n")
        sess = M.Session(be, os.path.join(self.t, "b"))
        sess.backup()

        class Stuck(M.DirBackend):
            def put(s, rel, data):
                pass
        be.put(M.VITA_INI, b"b\n")
        self.assertTrue(M.Session(Stuck(self.data), os.path.join(self.t, "b")).restore())



class ConfigMergeTest(unittest.TestCase):
    def test_merge_keeps_owner_lines_and_sets_keys(self):
        owner = b"theme = dark\nquality_gba = 0\n# note = x\n"
        out = M.merge_config(owner, {"quality_gba": "3", "core4": "off"}).decode()
        self.assertIn("theme = dark", out)
        self.assertIn("quality_gba = 3", out)
        self.assertNotIn("quality_gba = 0", out)
        self.assertIn("# note = x", out)
        self.assertIn("core4 = off", out)

    def test_playlist_c_keys(self):
        es = M.parse_playlist("G.gba | 10s | look_gov=0 c.quality_gba=5 t.core4=off | x\n", 60) \
            if hasattr(M, "parse_playlist") else None
        if es is None:
            self.skipTest("parser has another name")
        self.assertEqual(es[0].config, {"quality_gba": "5"})
        self.assertEqual(es[0].threads, {"core4": "off"})
        self.assertEqual(es[0].vita, {"look_gov": "0"})

if __name__ == "__main__":
    unittest.main()
