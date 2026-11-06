#!/usr/bin/env python3
"""Build the real GB core with a host compiler and run gbaudio.c.
From gpsp: python3 gbcore/tests/run_gbaudio.py"""
import importlib.util, os, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
spec = importlib.util.spec_from_file_location("gb_tests", ROOT / "tools/run_gb_tests.py")
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
with tempfile.TemporaryDirectory(prefix="gb-audio-") as d:
    t = Path(d); objs = []
    for name in g.CORE + [g.SHARED]:
        o = t / (name + ".o")
        subprocess.check_call(["gcc", "-std=gnu11", "-O1", "-g", *g.CORE_FLAGS, "-c",
                               str(ROOT / "gbcore/tgbdual" / name), "-o", str(o)])
        objs.append(str(o))
    glue = t / "adapter.o"
    subprocess.check_call(["gcc", "-std=gnu11", "-O1", "-g", "-c", "gbcore/gbcore_tgbdual.c", "-o", str(glue)])
    exe = t / "gbaudio"
    subprocess.check_call(["gcc", "-std=gnu11", "-O1", "-g", *g.CORE_FLAGS, "-I.", "-Ifrontend-common",
                           "-o", str(exe), "gbcore/tests/gbaudio.c", str(glue), *objs, "-lm"])
    sys.exit(subprocess.call([str(exe)]))
