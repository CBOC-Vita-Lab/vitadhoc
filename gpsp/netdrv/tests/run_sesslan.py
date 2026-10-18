#!/usr/bin/env python3
"""run_sesslan.py OUTDIR -- real GB link sessions over transport_lan.

Builds the two-console session stack exactly as tools/gblink/build.sh builds
sesssim (two complete fe_gblink stacks, four TGB Dual instances), links it
with netdrv.c + transport_lan.c and netdrv/tests/sesslan.c instead of
sesssim's simulated network, generates the two test cartridges of
tools/tests/gblink_session_tests.py (no commercial ROM), and runs sessions
in real time over loopback UDP:

  lan        the same cartridge on both consoles, a clean LAN
  wifi       1 % loss, 2 ms latency, 4 ms jitter
  transfer   different cartridges, each sent to the partner over the BULK
             lane (1037-byte datagrams) with 5 % loss, 20 ms, 10 ms jitter;
             SHA-1 checked by the receiver
  ordered    different cartridges with no bulk lane: the transfers ride
             netdrv's ordered channel (560-byte payloads)
  internet   2 % loss, 60 ms latency, 20 ms jitter, input delay 8, redundant
             input datagrams + resend requests + stall absorption
  busy       3 % loss, 5 ms, 25 ms jitter

Each must end DONE on both consoles with every periodic hash matched (both
consoles ran both Game Boys to the same state) and both saves committed.
"""
from __future__ import annotations

import importlib.util
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
GPSP = HERE.parent.parent


def sh(*cmd: str, cwd: Path = GPSP) -> None:
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit("FAIL build: " + " ".join(cmd[:3]))


def build(out: Path) -> Path:
    sh("bash", "tools/gblink/build.sh", str(out))
    o = out / "obj"
    objs = [str(o / n) for n in (
        "dual1.o", "dual2.o", "gblink1.o", "gblink2.o", "fe_util.o",
        "gbcore_a.o", "gbcore_b.o", "gbcore_c.o", "gbcore_d.o",
        "tgb_shared.o", "ap_a.o", "ap_b.o")]
    exe = out / "sesslan"
    sh("gcc", "-O2", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
       "-Wno-unused-function", "-Wno-unused-variable", "-pthread",
       "-Igbcore", "-Ifrontend-common", "-Inetdrv",
       "-o", str(exe), "netdrv/tests/sesslan.c", "netdrv/netdrv.c",
       "netdrv/transport_lan.c", *objs)
    return exe


def roms(t: Path) -> None:
    spec = importlib.util.spec_from_file_location(
        "gst", GPSP / "tools" / "tests" / "gblink_session_tests.py")
    gst = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gst)
    (t / "dmg.gb").write_bytes(gst.rom(False, 0x21, 0x5A, 0x8000))
    (t / "cgb.gbc").write_bytes(gst.rom(True, 0x93, 0xC3, 0x20000))
    (t / "a.sav").write_bytes(bytes(range(256)) * 32)
    (t / "b.sav").write_bytes(bytes(255 - (i & 255) for i in range(8192)))


def field(line: str, key: str) -> str:
    m = re.search(r"\b%s=(\S+)" % re.escape(key), line)
    return m.group(1) if m else ""


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "sesslan-obj").resolve()
    only = sys.argv[2:]
    exe = build(out)
    scenarios = [
        ("lan", "dmg.gb", "dmg.gb", ["delay=3"]),
        ("wifi", "dmg.gb", "dmg.gb", ["delay=3", "loss=1", "lat=2", "jitter=4",
                                      "rto_first=30000", "rto_min=30000",
                                      "rto_max=2500000"]),
        ("transfer", "cgb.gbc", "dmg.gb", ["delay=4", "bulk=1037", "loss=5",
                                          "lat=20", "jitter=10",
                                          "rto_first=30000", "rto_min=30000",
                                          "rto_max=2500000"]),
        ("ordered", "cgb.gbc", "dmg.gb", ["delay=3", "loss=1", "lat=2",
                                         "jitter=4"]),
        ("internet", "dmg.gb", "dmg.gb", ["delay=8", "bulk=1037", "copies=4",
                                         "nack=1", "absorb=1", "loss=2",
                                         "lat=60", "jitter=20",
                                         "rto_first=30000", "rto_min=30000",
                                         "rto_max=2500000"]),
        ("busy", "dmg.gb", "dmg.gb", ["delay=4", "bulk=1037", "copies=4",
                                     "nack=1", "absorb=1", "loss=3", "lat=5",
                                     "jitter=25", "rto_first=30000",
                                     "rto_min=30000", "rto_max=2500000"]),
    ]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="sesslan-") as tmp:
        t = Path(tmp)
        roms(t)
        for k, (name, rh, rg, extra) in enumerate(scenarios):
            if only and name not in only:
                continue
            args = [str(exe), "rom_h=%s" % (t / rh), "rom_g=%s" % (t / rg),
                    "sav_h=%s" % (t / "a.sav"), "sav_g=%s" % (t / "b.sav"),
                    "out_h=%s" % (t / (name + "-h.sav")),
                    "out_g=%s" % (t / (name + "-g.sav")),
                    "run=600", *extra]
            r = subprocess.run(args, capture_output=True, text=True, timeout=300)
            o = r.stdout
            res = [l for l in o.splitlines() if "EVT result console=" in l]
            net = [l for l in o.splitlines() if "EVT net console=" in l]
            ok = (r.returncode == 0 and len(res) == 2 and
                  o.count("gblink_final ") >= 2 and
                  all(field(l, "state") == "3" and field(l, "committed") == "1"
                      for l in res) and
                  (t / (name + "-h.sav")).exists() and
                  (t / (name + "-g.sav")).exists())
            for l in res:
                h = field(l, "hashes").split("/")
                ok = ok and len(h) == 2 and h[0] == h[1] and int(h[0]) >= 8
            if rh != rg:
                rx = [l for l in o.splitlines() if "gblink_rom_received" in l]
                ok = ok and len(rx) == 2 and all("ok=1" in l for l in rx)
                if "bulk=1037" in extra:
                    ok = ok and all("chunk=1024" in l for l in rx)
            if "transport_invariant_broken" in o:
                ok = False
            if not ok:
                failed += 1
                print(o[-4000:])
                print(r.stderr[-1000:])
                print("FAIL sesslan %s (exit %d)" % (name, r.returncode))
                continue
            setup = re.search(r"running setup_ms=(\d+)", o)
            print("sesslan %-9s DONE on both: frames=%s hashes=%s stalls H/G=%s/%s"
                  " (episodes %s/%s) setup=%s ms" % (
                      name, field(res[0], "frames"), field(res[0], "hashes"),
                      field(res[0], "stalls"), field(res[1], "stalls"),
                      field(res[0], "episodes"), field(res[1], "episodes"),
                      setup.group(1) if setup else "?"))
            for l in net:
                print("    %s: netdrv tx=%s rx=%s retx=%s srtt=%s us | lan tx=%s"
                      " rx=%s bulk tx/rx=%s/%s shim drop/delay=%s/%s" % (
                          field(l, "console"), field(l, "nd_tx"),
                          field(l, "nd_rx"), field(l, "retx"),
                          field(l, "srtt_us"), field(l, "lan_tx"),
                          field(l, "lan_rx"), field(l, "bulk_tx"),
                          field(l, "bulk_rx"), field(l, "fault_drop"),
                          field(l, "fault_delay")))
            if rh != rg:
                for l in [x for x in o.splitlines() if "gblink_rom_received" in x]:
                    print("    cartridge: size=%s ok=%s us=%s chunk=%s rx=%s dup=%s" % (
                        field(l, "size"), field(l, "ok"), field(l, "us"),
                        field(l, "chunk"), field(l, "rx"), field(l, "dup")))
    if failed:
        print("FAILED: %d scenario(s)" % failed)
        return 1
    print("ALL SESSLAN SESSIONS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
