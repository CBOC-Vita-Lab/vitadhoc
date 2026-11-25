#!/usr/bin/env python3
"""Games in a .7z: the reader (sz_rom.c over the LZMA SDK) against archives
that 7z builds here from generated data (nothing is committed).  Needs the
7z / 7za / 7zr program; skips (exit 0) without it.  Also a short fuzz: archives
cut short or with bytes flipped fail cleanly (AddressSanitizer + UBSan), and
whatever extracts equals the original (the CRC says so)."""
import os, random, shutil, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SEVENZ = next((p for p in (shutil.which(x) for x in ("7z", "7za", "7zr")) if p), None)
SAN = "-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined"   # the SDK loads unaligned words on x86 on purpose (CpuArch.h); other CPUs get byte loads


def rd(p):
    with open(p, "rb") as f:
        return f.read()


def wr(p, d):
    with open(p, "wb") as f:
        f.write(d)


def build():
    subprocess.run(["make", "-s", "-f", "archive.mk", "clean"], cwd=HERE)
    r = subprocess.run(["make", "-s", "-f", "archive.mk", "SAN=" + SAN], cwd=HERE,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise RuntimeError(r.stdout)
    return os.path.join(HERE, "sz_cli")


def blob(n, seed):
    """n bytes that compress some, not to nothing"""
    rnd = random.Random(seed)
    words = [bytes(rnd.getrandbits(8) for _ in range(rnd.randint(4, 40))) for _ in range(64)]
    out = bytearray()
    while len(out) < n:
        out += rnd.choice(words)
    return bytes(out[:n])


@unittest.skipUnless(SEVENZ, "no 7z program here")
class Archive7z(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cli = build()
        cls.tmp = tempfile.mkdtemp(prefix="sz-")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def mk(self, name, files, *opts):
        """a .7z of {name: bytes} made with 7z"""
        d = tempfile.mkdtemp(dir=self.tmp)
        src = os.path.join(d, "src")
        os.makedirs(src)
        for n, data in files.items():
            p = os.path.join(src, n)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            wr(p, data)
        out = os.path.join(d, name)
        order = list(files)
        r = subprocess.run([SEVENZ, "a", "-bd", "-y", out] + list(opts) + order, cwd=src,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, 0, r.stdout)
        return out

    def run_cli(self, *a):
        r = subprocess.run([self.cli] + [str(x) for x in a], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        return r.stdout.strip()

    def peek(self, path):
        o = self.run_cli("peek", path).split(" ")
        return int(o[0]), int(o[1]), " ".join(o[2:-2]), int(o[-2]), int(o[-1])

    def extract(self, path):
        out = path + ".out"
        rc = int(self.run_cli("extract", path, out))
        return rc, (rd(out) if rc == 0 else None)

    # ---- what is in it ----
    def test_single_game_per_method(self):
        game = blob(300000, 1)
        for tag, opts in (("lzma2", ["-m0=lzma2"]), ("lzma", ["-m0=lzma"]), ("copy", ["-m0=copy"]),
                          ("bcj", ["-m0=bcj", "-m1=lzma"]), ("delta", ["-m0=delta:4", "-m1=lzma2"]),
                          ("arm", ["-m0=arm", "-m1=lzma"]), ("ppc", ["-m0=ppc", "-m1=lzma"])):
            p = self.mk("g_%s.7z" % tag, {"Game (USA).gba": game}, *opts)
            rc, con, name, us, _ = self.peek(p)
            self.assertEqual((rc, con, name, us), (0, 0, "Game (USA).gba", len(game)), tag)
            rc, data = self.extract(p)
            self.assertEqual((rc, data == game), (0, True), tag)

    def test_first_supported_file_wins_solid(self):
        a, b, c = blob(70000, 2), blob(90000, 3), blob(50000, 4)
        p = self.mk("multi.7z", {"readme.txt": b"hi", "Hack.sfc": b, "Other.gba": a, "x.nes": c}, "-ms=on")
        rc, con, name, us, _ = self.peek(p)
        self.assertEqual((rc, con, name, us), (0, 3, "Hack.sfc", len(b)))
        rc, data = self.extract(p)
        self.assertEqual((rc, data == b), (0, True))
        # a file that is not the first in the block
        p = self.mk("late.7z", {"a.txt": blob(40000, 5), "b.dat": blob(40000, 6), "Late.nes": c}, "-ms=on")
        rc, data = self.extract(p)
        self.assertEqual((rc, data == c), (0, True))

    def test_folders_and_names(self):
        game = blob(0x8000, 7)
        p = self.mk("dir.7z", {"sub/dir/Café ゲーム.md": game})
        rc, con, name, us, _ = self.peek(p)
        self.assertEqual((rc, con, us), (0, 6, len(game)))
        self.assertEqual(name, "sub/dir/Café ゲーム.md")
        # a dot file and a Genesis file under 16 KiB are not games
        p = self.mk("no.7z", {".hidden.gba": blob(5000, 8), "tiny.bin": blob(100, 9), "notes.txt": b"x"})
        self.assertEqual(self.peek(p)[0:2], (-1, -1))
        p = self.mk("bin.7z", {"Game.bin": blob(0x20000, 10)})
        self.assertEqual(self.peek(p)[0:2], (0, 6))

    def test_head(self):
        game = blob(100000, 11)
        p = self.mk("h.7z", {"Game.gba": game})
        o = self.run_cli("head", p, 0x110).split(" ")
        self.assertEqual((int(o[0]), o[1]), (0x110, game[:0x110].hex()))
        o = self.run_cli("head", p, 4096).split(" ")
        self.assertEqual(o[1], game[:4096].hex())

    def test_empty_archive_and_not_an_archive(self):
        p = os.path.join(self.tmp, "text.7z")
        wr(p, b"this is not an archive at all" * 10)
        self.assertEqual(self.peek(p)[0], -1)
        p = os.path.join(self.tmp, "zero.7z")
        wr(p, b"")
        self.assertEqual(self.peek(p)[0], -1)
        p = os.path.join(self.tmp, "sig.7z")
        wr(p, b"7z\xbc\xaf\x27\x1c\x00\x04" + b"\x00" * 100)
        self.assertEqual(self.peek(p)[0], -1)
        self.assertEqual(self.peek(os.path.join(self.tmp, "missing.7z"))[0], -1)

    # ---- the limits ----
    def test_game_over_the_limit_is_refused(self):
        big = b"\x00" * ((32 << 20) + 1)
        p = self.mk("big.7z", {"Big.gba": big})
        self.assertEqual(self.peek(p)[0], -2)
        ok = b"\x00" * (32 << 20)
        p = self.mk("edge.7z", {"Edge.gba": ok})
        rc, con, _, us, _ = self.peek(p)
        self.assertEqual((rc, us), (0, 32 << 20))

    def test_solid_block_over_the_limit_is_refused(self):
        z = b"\x00" * (20 << 20)
        p = self.mk("blk.7z", {"a.dat": z, "b.dat": z, "c.dat": z, "d.dat": z, "Game.gba": blob(200000, 12)}, "-ms=on")
        rc, _, name, _, blk = self.peek(p)
        self.assertEqual((rc, name), (-2, "Game.gba"))
        self.assertGreater(blk, 64 << 20)
        # the same game in a block of its own is fine
        p = self.mk("nonsolid.7z", {"a.dat": z, "b.dat": z, "c.dat": z, "d.dat": z, "Game.gba": blob(200000, 12)}, "-ms=off")
        self.assertEqual(self.peek(p)[0], 0)
        rc, data = self.extract(p)
        self.assertEqual((rc, data == blob(200000, 12)), (0, True))

    def test_encrypted_is_refused(self):
        p = self.mk("enc.7z", {"Game.gba": blob(100000, 13)}, "-psecret", "-mhe=off")
        self.assertEqual(self.peek(p)[0], -2)
        p = self.mk("enc2.7z", {"Game.gba": blob(100000, 13)}, "-psecret", "-mhe=on")
        self.assertIn(self.peek(p)[0], (-1, -2))

    # ---- damaged archives ----
    def test_crc_mismatch_is_not_extracted(self):
        game = blob(60000, 14)
        p = self.mk("crc.7z", {"Game.gba": game}, "-m0=copy")
        raw = bytearray(rd(p))
        k = raw.find(game[1000:1032])
        self.assertGreater(k, 0)
        raw[k] ^= 0x55
        wr(p, bytes(raw))
        rc = int(self.run_cli("peek", p).split()[0])
        self.assertEqual(rc, 0)       # the header is intact
        self.assertEqual(self.extract(p)[0], -2)

    def test_truncated_and_flipped_fail_cleanly(self):
        game = blob(150000, 15)
        base = self.mk("fz.7z", {"readme.txt": b"hello", "Game.gba": game}, "-ms=on")
        raw = rd(base)
        rnd = random.Random(2026)
        cuts = sorted(set([0, 1, 5, 6, 7, 12, 31, 32, 33, 100, len(raw) - 1, len(raw) - 20, len(raw) - 40] +
                          [rnd.randrange(len(raw)) for _ in range(25)]))
        mutants = [raw[:c] for c in cuts]
        for _ in range(120):
            b = bytearray(raw)
            for _ in range(rnd.choice((1, 1, 2, 4))):
                # favour the header (the end) and the start header, where the structure lives
                i = rnd.choice((rnd.randrange(32), len(raw) - 1 - rnd.randrange(min(160, len(raw))),
                                rnd.randrange(len(raw))))
                b[i] = rnd.getrandbits(8)
            mutants.append(bytes(b))
        n_ok = 0
        for m in mutants:
            p = os.path.join(self.tmp, "mut.7z")
            wr(p, m)
            rc = int(self.run_cli("peek", p).split()[0])      # ASan/UBSan abort = nonzero exit = failure
            self.assertIn(rc, (0, -1, -2))
            if rc == 0:
                rc, data = self.extract(p)
                self.assertIn(rc, (0, -1, -2))
                if rc == 0:
                    n_ok += 1
                    self.assertEqual(data, game)               # the CRC allows nothing else
                self.run_cli("head", p, 300)
        self.assertGreater(len(mutants), 100)


if __name__ == "__main__":
    unittest.main(verbosity=2)
