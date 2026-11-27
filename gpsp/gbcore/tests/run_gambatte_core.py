#!/usr/bin/env python3
"""Builds and runs gambatte_core.cpp (the Accurate core's wrapper: states, Quick response image, battery with clock,
refused cartridges, cheats' memory) on the host.  usage: run_gambatte_core.py [ROM]  (default: blargg cpu_instrs)"""
import os, subprocess, sys, tempfile
from pathlib import Path
HERE = Path(__file__).resolve().parent
REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(HERE))
import run_suites as r  # noqa: E402

def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ.get("GATE_GB_TEST_ROMS") or str(REPO / "roms/gb-test-roms"),
        "cpu_instrs/individual/01-special.gb",
    )
    if not Path(rom).is_file():
        sys.stderr.write("run_gambatte_core: ROM not found: %s (pass ROM arg or set GATE_GB_TEST_ROMS)\n" % rom)
        return 2
    with tempfile.TemporaryDirectory(prefix="gam-core-") as td:
        exe = r.build_gambatte(Path(td), "gambatte_core.cpp")
        p = subprocess.run([str(exe), rom], capture_output=True, text=True)
        sys.stdout.write(p.stdout)
        return p.returncode
sys.exit(main())
