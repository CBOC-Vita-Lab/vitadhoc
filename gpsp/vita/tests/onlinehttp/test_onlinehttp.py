#!/usr/bin/env python3
"""Tests of online_http.{h,cpp}, the room client (plan package H).

    python3 -I gpsp/vita/tests/onlinehttp/test_onlinehttp.py [-v]

Builds tests/onlinehttp/runner.cpp twice with the app's C++ flags (-fno-exceptions -fno-rtti): without TLS, and (when the host64 Mbed TLS
prefix exists: $MBEDTLS_HOST, default /tls/mbedtls/host64) with it.  A fake of the service (fake_service.py) listens on the
loopback in plain HTTP and, for the TLS cases, behind a private test CA that reaches the client ONLY through its test hook
(tls::SetExtraTestRoots).  Nothing leaves 127.0.0.1.  Without the prefix the TLS cases are SKIPPED, loudly.  Against the real service code
(online/service/dev/server.mjs, package S) the `flow ... real` run is made when that file exists, else SKIPPED."""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
VITA = HERE.parents[1]            # gpsp/vita
REPO = HERE.parents[3]
NH = VITA / "nethttps"
MBED = Path(os.environ.get("MBEDTLS_HOST", "/tls/mbedtls/host64"))
CXXF = ["g++", "-x", "c++", "-std=gnu++14", "-O1", "-g", "-fno-exceptions", "-fno-rtti", "-w", "-DNP_HTTPS_TEST_HOOKS",
        f"-I{VITA}", f"-I{VITA.parent / 'frontend-common'}"]
SRC = [str(HERE / "runner.cpp"), str(VITA / "online_http.cpp"), str(NH / "np_https.cpp"), str(NH / "np_https_sys.cpp"),
       str(NH / "np_https_tls_mbedtls.cpp"), str(NH / "np_http_plain.cpp")]
UA = "VitAdhoc/2.0.0 (PlayStation Vita)"
HAVE_TLS = (MBED / "lib" / "libmbedtls.a").exists() and (MBED / "include" / "mbedtls").is_dir()


def run(args, env=None, timeout=120):
    e = dict(os.environ)
    e.update(env or {})
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, env=e)


class Base(unittest.TestCase):
    tmp = None
    fake = None
    ports = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="onlinehttp-"))
        r = run(CXXF + ["-o", str(cls.tmp / "runner_plain")] + SRC + ["-lpthread"])
        assert r.returncode == 0, r.stderr
        cls.tls_bin = None
        if HAVE_TLS:
            r = run(CXXF + ["-DNP_HAVE_MBEDTLS", f"-I{MBED / 'include'}", "-o", str(cls.tmp / "runner_tls")] + SRC + [str(NH / "np_https_selftest.cpp"),
                    f"-L{MBED / 'lib'}", "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-lpthread"])
            assert r.returncode == 0, r.stderr
            cls.tls_bin = cls.tmp / "runner_tls"
        args = [sys.executable, "-I", str(HERE / "fake_service.py"), str(cls.tmp / "fk")]
        if HAVE_TLS:
            args += ["--tls-dir", str(cls.tmp / "pki")]
        cls.fake = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(200):
            if (cls.tmp / "fk" / "ports.json").exists():
                break
            time.sleep(0.05)
        cls.ports = json.loads((cls.tmp / "fk" / "ports.json").read_text())

    @classmethod
    def tearDownClass(cls):
        if cls.fake:
            cls.fake.send_signal(signal.SIGTERM)
            cls.fake.wait(10)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def stats(self):
        return json.load(urllib.request.urlopen(f"http://127.0.0.1:{self.ports['plain']}/__stats"))

    def runner(self, *args, tls=False, env=None, timeout=120):
        exe = self.tls_bin if tls else self.tmp / "runner_plain"
        r = run([str(exe)] + [str(a) for a in args], env=env, timeout=timeout)
        fails = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
        self.assertEqual(r.returncode, 0, "\n".join(fails) + "\n" + r.stderr[-800:])
        self.assertFalse(fails)
        return r

    @property
    def base(self):
        return f"http://127.0.0.1:{self.ports['plain']}"


class Pure(Base):
    def test_json_reader_and_desc(self):
        r = self.runner("json")
        self.assertGreater(len([l for l in r.stdout.splitlines() if l.startswith("ok")]), 30)


class Plain(Base):
    def test_every_route_over_plain_http(self):
        before = self.stats()
        r = self.runner("flow", self.base)
        after = self.stats()
        self.assertEqual(after["bad"], [], "the fake refused a request of the client: " + str(after["bad"]))
        self.assertEqual(set(after["ua"][len(before["ua"]):]), {UA})
        paths = after["paths"][len(before["paths"]):]
        self.assertIn("POST /v1/rooms", paths)
        self.assertIn("POST /v1/rooms/K7M2Q/endpoint", paths)
        self.assertFalse([p for p in paths if "etc" in p or ".." in p], "a malformed code must not reach the network")
        rids = after["rids"][len(before["rids"]):]
        self.assertEqual(rids[:2], ["create 0123456789abcdef", "join fedcba9876543210"], "the request id rides in the body, unchanged")
        self.assertFalse([x for x in rids if "NOT" in x.upper() and "HEX" in x.upper()], "a malformed request id reached the network")
        self.assertIn("join -", rids, "a call without an id sends no rid member")

    def test_error_codes_map_as_2_3_says(self):
        self.runner("errors", self.base)
        self.assertEqual(self.stats()["bad"], [])

    def test_build_label_is_never_sent_as_the_version(self):
        s0 = self.stats()
        self.runner("badver", self.base)
        s1 = self.stats()
        self.assertEqual(s1["bad"], [])
        self.assertEqual(s1["ua"][-1], "VitAdhoc/1.2.3.4 (PlayStation Vita)")
        m = re.search(r'#define\s+ONLINE_APP_VERSION\s+"([^"]+)"', (VITA / "online_vita.h").read_text())
        fallback_ua = f"VitAdhoc/{m.group(1)} (PlayStation Vita)" if m else UA
        self.assertIn(fallback_ua, s1["ua"][len(s0["ua"]):])

    def test_three_in_flight_and_drop(self):
        self.runner("three", self.base)

    def test_server_down(self):
        self.runner("down", 1)

    def test_no_answer_is_net_after_the_limit(self):
        self.runner("blackhole", self.ports["blackhole"], timeout=60)

    def test_drop_never_blocks_the_frame_thread(self):
        self.runner("dropstuck", timeout=60)

    def test_https_without_tls_in_the_build(self):
        self.runner("nohttps")

    @unittest.skipUnless((REPO / "online/service/dev/server.mjs").exists(), "SKIPPED: online/service/dev/server.mjs (package S) is not in this tree")
    def test_against_the_real_service_code(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("SKIPPED: node is not installed")
        p = subprocess.Popen([node, str(REPO / "online/service/dev/server.mjs"), "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             env=dict(os.environ, ALLOW_PRIVATE_ADDR="1"))
        try:
            port = None
            t0 = time.time()
            while time.time() - t0 < 10 and port is None:
                line = p.stdout.readline()
                m = re.search(r"READY (\d+)", line)
                if m:
                    port = int(m.group(1))
            if port is None:
                self.skipTest("SKIPPED: could not read the dev server's port")
            self.runner("flow", f"http://127.0.0.1:{port}", "real")
        finally:
            p.terminate()


class Tls(Base):
    def setUp(self):
        if not HAVE_TLS:
            self.skipTest(f"SKIPPED: no host64 Mbed TLS prefix at {MBED}")

    def tls(self, name, expect, env=None):
        e = {"TEST_ROOT_PEM": str(self.tmp / "pki" / "rootA.pem")}
        e.update(env or {})
        return self.runner("tls", f"https://localhost:{self.ports['tls'][name]}", expect, tls=True, env=e, timeout=90)

    def conns(self, stats, name):
        return [c for c in stats["tls"] if c["server"] == name]

    def check_ok(self, name, version, cipher_part):
        s0 = self.stats()
        self.tls(name, "ok")
        s1 = self.stats()
        good = self.conns(s1, name)
        self.assertTrue(good and all(c.get("version") == version for c in good), good)
        self.assertIn(cipher_part, good[0]["cipher"])
        self.assertEqual(s1["bad"], [])
        self.assertLessEqual(len(good), 2, "the polls of one wait should reuse a TLS connection")
        self.assertGreater(s1["requests"] - s0["requests"], 8)

    def test_ecdsa_tls12(self):
        self.check_ok("ec12", "TLSv1.2", "ECDSA")

    def test_ecdsa_tls13(self):
        self.check_ok("ec13", "TLSv1.3", "TLS_")

    def test_rsa_tls12(self):
        self.check_ok("rsa12", "TLSv1.2", "RSA")

    def test_rsa_tls13(self):
        self.check_ok("rsa13", "TLSv1.3", "TLS_")

    def test_wrong_host_name_is_cert(self):
        self.tls("wrong", "cert")
        self.assertTrue(all(c.get("failed") for c in self.conns(self.stats(), "wrong")))

    def test_untrusted_chain_is_cert(self):
        self.tls("untrusted", "cert")
        self.assertTrue(all(c.get("failed") for c in self.conns(self.stats(), "untrusted")))

    def test_test_root_is_needed(self):
        """Without the hook the client trusts only its own roots: the same good server is refused."""
        r = run([str(self.tls_bin), "tls", f"https://localhost:{self.ports['tls']['ec13']}", "cert"], env={k: v for k, v in os.environ.items() if k != "TEST_ROOT_PEM"})
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_clock_before_the_floor_sends_nothing(self):
        before = len(self.conns(self.stats(), "ecany"))
        self.tls("ecany", "clock")
        self.assertEqual(len(self.conns(self.stats(), "ecany")), before, "a connection was made although the clock is wrong")

    def test_roots_are_per_host(self):
        """The four Sectigo roots anchor api.github.com alone; the eight others anchor every host but that one."""
        r = self.runner("scope", tls=True)
        self.assertEqual(len([l for l in r.stdout.splitlines() if l.startswith("ok")]), 14, r.stdout)

    def test_a_host_with_its_own_roots_trusts_no_other(self):
        """A real handshake: the good server, named as the host with roots of its own, is refused although the general list (with
        the test root) would accept it -- the handshake's list replaces the general one, it does not add to it."""
        before = len([c for c in self.conns(self.stats(), "ecany") if c.get("failed")])
        self.tls("ecany", "cert", env={"TEST_ONLY_HOST": "localhost"})
        self.assertGreater(len([c for c in self.conns(self.stats(), "ecany") if c.get("failed")]), before)
        self.tls("ecany", "ok", env={"TEST_ONLY_HOST": "elsewhere.example"})

    def test_selftest_runs(self):
        r = run([str(self.tls_bin), "selftest"], env={"TEST_ROOT_PEM": str(self.tmp / "pki" / "rootA.pem"),
                "SMG_NET_SELFTEST": f"https://localhost:{self.ports['tls']['ec13']}/v1/info?v=2.0.0"}, timeout=60)
        self.assertIn("selftest result: OK", r.stderr, r.stderr[-600:])


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[a for a in sys.argv if a != "-v"])
