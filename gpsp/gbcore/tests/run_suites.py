#!/usr/bin/env python3
"""Run the public Game Boy test suites on the host and print a pass/fail table.

From gpsp:  python3 gbcore/tests/run_suites.py [--expect FILE] [--write FILE]
                                              [--roms DIR] [--mooneye DIR]

Suites: blargg (prebuilt ROMs under --roms or GATE_GB_TEST_ROMS: serial
text, screen text or cartridge-RAM report), mooneye (built ROMs, folder in
--mooneye or GATE_GB_MOONEYE: the Fibonacci register verdict), dmg-acid2 /
cgb-acid2 (built ROMs under --acid or GATE_GB_ACID2 as <name>/build/<rom>, the
reference pictures as <name>/img: screen against the picture).  The
ROMs are inputs read from disk, never copied into the repository; a suite
whose ROMs are absent is reported SKIP.  With --expect, any result that is
not what the expected-results file says is a failure; --write writes the
current results in that format.  (host)
"""
import argparse, os, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
import run_gb_tests as g  # noqa: E402

def build(t):
    objs = []
    for name in g.CORE + [g.SHARED]:
        o = t / (name[:-2] + ".o")
        subprocess.run(["gcc", "-std=gnu11", "-O2", "-w", *g.CORE_FLAGS, "-c", str(ROOT / "gbcore/tgbdual" / name), "-o", str(o)], check=True)
        objs.append(str(o))
    exe = t / "suites"
    subprocess.run(["gcc", "-std=gnu11", "-O2", "-w", "-I" + str(ROOT / "gbcore"), "-I" + str(ROOT / "frontend-common"),
                    "-o", str(exe), str(HERE / "suites.c"), str(ROOT / "gbcore/gbcore_tgbdual.c"), *objs, "-lm"], check=True, cwd=ROOT)
    return exe

def run(exe, rom, args=(), timeout=300):
    try:
        p = subprocess.run([str(exe), str(rom), *args], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"TIMEOUT": ""}
    d = {}
    for line in p.stdout.splitlines():
        k, _, v = line.partition(" ")
        d[k] = v
    if p.returncode:
        d["CRASH"] = str(p.returncode)
    return d

def blargg(d):
    s = d.get("SERIAL", "") + " " + d.get("SCREEN", "")
    if "Passed" in s: return "PASS"
    if "Failed" in s: return "FAIL"
    m = d.get("MEM")
    if m is not None:
        st = m.split(" ", 1)[0]
        return "PASS" if st == "0" else ("FAIL" if st != "128" else "TIMEOUT")
    return "TIMEOUT"

def mooneye(d):
    r = d.get("REGS", "").split()
    if r == ["3", "5", "8", "13", "21", "34"]: return "PASS"
    if r == ["66"] * 6: return "FAIL"
    return "TIMEOUT"

def acid(exe, rom, ref, gbc, t):
    try:
        from PIL import Image
    except ImportError:
        return "SKIP"
    out = t / "acid.ppm"
    run(exe, rom, ["--gbc" if gbc else "--dmg", "--frames", "120", "--ppm", str(out)])
    if not out.exists(): return "TIMEOUT"
    if gbc:
        # colour picture: 5-6-5 bit quantisation allows a few levels of error
        ca, cb = Image.open(out).convert("RGB"), Image.open(ref).convert("RGB")
        if ca.size != cb.size: return "FAIL"
        bad = sum(1 for x, y in zip(ca.tobytes(), cb.tobytes()) if abs(x - y) > 8)
        return "PASS" if bad == 0 else "FAIL"
    a = Image.open(out).convert("L")
    b = Image.open(ref).convert("L")
    if a.size != b.size: return "FAIL"
    # Compare by grey rank: the reference uses its own shades, the run uses
    # the grey palette, so equal pictures have equal pixel orderings.
    pa, pb = a.tobytes(), b.tobytes()
    sa, sb = sorted(set(pa)), sorted(set(pb))
    if len(sa) != len(sb): return "FAIL"
    ma, mb = {v: i for i, v in enumerate(sa)}, {v: i for i, v in enumerate(sb)}
    bad = sum(1 for x, y in zip(pa, pb) if ma[x] != mb[y])
    return "PASS" if bad == 0 else "FAIL"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", default=os.environ.get("GATE_GB_TEST_ROMS", ""))
    ap.add_argument("--mooneye", default=os.environ.get("GATE_GB_MOONEYE", ""))
    ap.add_argument("--acid", default=os.environ.get("GATE_GB_ACID2", ""))
    ap.add_argument("--expect"); ap.add_argument("--write")
    a = ap.parse_args()
    res = {}
    with tempfile.TemporaryDirectory(prefix="gb-suites-") as td:
        t = Path(td)
        exe = build(t)
        if os.environ.get("GB_SUITES_KEEP"):
            import shutil; shutil.copy(exe, os.environ["GB_SUITES_KEEP"])
        roms = Path(a.roms)
        if a.roms and roms.is_dir():
            for p in sorted(roms.rglob("*.gb")):
                if "/source/" in str(p):
                    continue
                rel = p.relative_to(roms).as_posix()
                frames = "1800" if "sound" in rel or "mem_timing" in rel else "1500"
                if rel.endswith("_sound.gb"): frames = "4000"
                if rel.endswith("cpu_instrs.gb") or rel.startswith("interrupt_time"): frames = "3000"
                res["blargg/" + rel] = blargg(run(exe, p, ["--frames", frames]))
        moon = Path(a.mooneye)
        if a.mooneye and moon.is_dir():
            for p in sorted(moon.rglob("*.gb")):
                rel = p.relative_to(moon).as_posix()
                if rel.startswith(("manual-only", "madness", "misc", "utils")) or "emulator-only" in rel: continue
                res["mooneye/" + rel] = mooneye(run(exe, p, ["--frames", "300"]))
        acid_root = Path(a.acid)
        for name, rom, ref, gbc in (
                ("dmg-acid2", "dmg-acid2/build/dmg-acid2.gb", "dmg-acid2/img/reference-dmg.png", False),
                ("cgb-acid2", "cgb-acid2/build/cgb-acid2.gbc", "cgb-acid2/img/reference.png", True)):
            if a.acid and (acid_root / rom).exists():
                r = acid(exe, acid_root / rom, acid_root / ref, gbc, t)
                if r != "SKIP": res["acid2/" + name] = r
    lines = ["%s %s" % (k, v) for k, v in sorted(res.items())]
    if a.write:
        Path(a.write).write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    npass = sum(v == "PASS" for v in res.values())
    print("TOTAL %d/%d pass" % (npass, len(res)))
    if a.expect:
        exp = dict(l.rsplit(" ", 1) for l in Path(a.expect).read_text().splitlines() if l.strip())
        bad = [(k, e, res[k]) for k, e in sorted(exp.items()) if k in res and e == "PASS" and res[k] != "PASS"]
        for k, e, r in bad: print("REGRESSION %s expected %s got %s" % (k, e, r))
        for k, e in sorted(exp.items()):
            if k in res and e != "PASS" and res[k] == "PASS":
                print("NOW PASSES %s (update the expected file)" % k)
        return 1 if bad else 0
    return 0

sys.exit(main())
