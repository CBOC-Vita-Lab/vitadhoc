#!/usr/bin/env python3
"""Focused real-writer GBA state regressions; --baseline proves v4.0.0 gaps."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT.parent / "audit/reliability/gba-core"
OUT.mkdir(parents=True, exist_ok=True)
os.chdir(ROOT)
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
FLAGS = ["-std=gnu99", "-O1", "-g", "-w", "-DINLINE=inline",
         "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
         "-I.", "-Ilibretro/libretro-common/include", "-Ifrontend-common"]
records = []


def execute(name, cmd, expected=0, marker=None):
    cmd = list(map(str, cmd))
    p = subprocess.run(cmd, text=True, capture_output=True, env=ENV)
    (OUT / (name + ".log")).write_text(p.stdout + p.stderr)
    clean = expected != 0 or not any(s in p.stderr for s in ("runtime error:", "Sanitizer"))
    ok = p.returncode == expected and clean and (marker is None or marker in p.stdout + p.stderr)
    records.append(dict(name=name, command=cmd, exit=p.returncode, expected=expected, passed=ok))
    (OUT / "state-results.json").write_text(json.dumps(records, indent=2) + "\n")
    print(("PASS " if ok else "FAIL ") + name, flush=True)
    if not ok:
        print(p.stdout + p.stderr)
        raise SystemExit(1)


def build(directory, name, sources, extra=()):
    exe = directory / name
    execute(name + "-build", ["gcc", *FLAGS, *extra, *sources,
                              "-Wl,--gc-sections", "-Wl,--allow-multiple-definition", "-o", exe])
    return exe


with tempfile.TemporaryDirectory(prefix="gba-state-") as directory:
    t = Path(directory)
    preflight = [ROOT / "tools/test_savestate_preflight_loops.c",
                 *[ROOT / n for n in ("input.c", "main.c", "savestate.c", "gba_memory.c", "sound.c")]]
    if "--baseline" in sys.argv:
        old = t / "v4"; old.mkdir()
        for name in ("input.c", "main.c", "savestate.c", "gba_memory.c", "sound.c"):
            blob = subprocess.check_output(["git", "show", "v4.0.0:gpsp/" + name])
            (old / name).write_bytes(blob)
        exe = build(t, "types-baseline", [preflight[0], *[old / p.name for p in preflight[1:]]],
                    ["-DEXPECT_UNSAFE_TYPES"])
        execute("types-baseline", [exe, "types"], marker="BASELINE accepts all 22")
        exe = build(t, "timer-baseline", [ROOT / "tools/tests/test_gba_timer_state.c", old / "main.c"],
                    ["-DEXPECT_UNSAFE"])
        execute("timer-baseline", [exe], marker="reload=0 accepted=1")
        test = (ROOT / "tools/tests/test_gba_memory_state.c").read_text()
        test = test.replace('"../../gba_memory.c"', '"' + str(old / "gba_memory.c") + '"')
        test = test.replace('"../../savestate.c"', '"' + str(ROOT / "savestate.c") + '"')
        (t / "memory-old-test.c").write_text(test)
        exe = build(t, "memory-baseline", [t / "memory-old-test.c"], ["-DEXPECT_UNSAFE"])
        execute("memory-baseline-overrun", [exe, "consume"], 1, "AddressSanitizer: global-buffer-overflow")
    for rewind in (False, True):
        suffix = "-rewind" if rewind else ""
        extra = ["-DREWIND", "-DTIMER_LAZY"] if rewind else []
        exe = build(t, "memory-candidate" + suffix,
                    [ROOT / "tools/tests/test_gba_memory_state.c"], extra)
        execute("memory-candidate" + suffix, [exe])
        exe = build(t, "timer-candidate" + suffix,
                    [ROOT / "tools/tests/test_gba_timer_state.c", ROOT / "main.c"], extra)
        execute("timer-candidate" + suffix, [exe])
        exe = build(t, "preflight-candidate" + suffix, preflight, extra)
        execute("preflight-candidate" + suffix, [exe])
print("All focused GBA state checks passed; expected baseline failures are recorded separately.")
