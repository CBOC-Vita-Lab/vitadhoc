#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""blargg-style conformance run for the NES host glue.

  NES_TESTROMS=/path/to/nes-test-roms conformance.py [--frames N] [--filter TEXT] [--md FILE] [--opt NAME=VAL]

Runs every .nes under NES_TESTROMS (for example a clone of
https://github.com/christopherpow/nes-test-roms) in the Linux twin and reads the
result the way the tests report it: $6001-$6003 = DE B0 61 marks the protocol,
$6000 = 80 while running, 81 = the test wants the console's reset button pressed (the twin cannot, so
those are listed as "needs-reset" and are neither passes nor failures), 00 = passed, anything else = failed,
and the text at $6004 explains. ROMs that do not use the protocol are listed as "no-protocol"
(they report on screen only) and are not counted as passes or failures.
A test still running after N frames is run again with 4 times as many.
Without NES_TESTROMS the script prints a notice and exits 0. Nothing is downloaded.
Exit status is 0 whatever the results: the list is the result, except with --expect FILE: a file of ROM paths (relative to
NES_TESTROMS, one per line, '#' comments) that must pass; any that does not is printed as REGRESSION and the exit status is 1.
--write-expect FILE writes the current passes in that format (conformance_expect.txt is the list kept in this tree).
"""
import os, re, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
twin = os.environ.get("NES_TWIN", os.path.join(here, "build", "nes_twin"))
root = os.environ.get("NES_TESTROMS", "")
args = sys.argv[1:]
frames, filt, md, opts, expect, wexpect = 3000, "", None, [], None, None
while args:
    a = args.pop(0)
    if a == "--frames": frames = int(args.pop(0))
    elif a == "--filter": filt = args.pop(0)
    elif a == "--md": md = args.pop(0)
    elif a == "--opt": opts += ["--opt", args.pop(0)]
    elif a == "--expect": expect = args.pop(0)
    elif a == "--write-expect": wexpect = args.pop(0)
    else:
        print("usage: conformance.py [--frames N] [--filter TEXT] [--md FILE] [--opt NAME=VAL] [--expect FILE] [--write-expect FILE]"); sys.exit(2)
if not root or not os.path.isdir(root):
    print("NES_TESTROMS is not set (or not a directory): nothing to run. See nes/README.md.")
    sys.exit(0)

def run(rom, n):
    p = subprocess.run([twin, rom, "--frames", str(n), "--status", "--text", "--summary"] + opts,
                       capture_output=True, text=True, errors="replace", timeout=300)
    out = p.stdout + p.stderr
    m = re.search(r"peek \$6000=([0-9a-f]{2}) sig=([0-9a-f]{6})", out)
    fps = re.search(r"fps=\s*([0-9.]+)", out)
    txt = ""
    if "text:" in out:
        txt = out.split("text:", 1)[1]
        txt = txt.split("\n/")[0]
    return (m.group(1), m.group(2)) if m else (None, None), (float(fps.group(1)) if fps else 0.0), txt, out

rows, counts = [], {"pass": 0, "fail": 0, "needs-reset": 0, "timeout": 0, "no-protocol": 0, "load-error": 0}
speeds = []
for d, _, files in sorted(os.walk(root)):
    for f in sorted(files):
        path = os.path.join(d, f)
        if not f.lower().endswith(".nes") or filt not in path:
            continue
        rel = os.path.relpath(path, root)
        try:
            (st, sig), fps, txt, out = run(path, frames)
            if st is not None and sig == "deb061" and st == "80":
                (st, sig), fps, txt, out = run(path, frames * 4)
        except subprocess.TimeoutExpired:
            rows.append((rel, "timeout", "", 0.0)); counts["timeout"] += 1; continue
        if st is None:
            kind = "load-error"; txt = out.strip().splitlines()[0] if out.strip() else ""
        elif sig != "deb061":
            kind = "no-protocol"; txt = ""
        elif st == "80":
            kind = "timeout"
        elif st == "81":
            kind = "needs-reset"
        elif st == "00":
            kind = "pass"
        else:
            kind = "fail"
        counts[kind] += 1
        if fps: speeds.append(fps)
        line = " ".join(txt.split())[:100]
        rows.append((rel, kind + ("" if kind != "fail" else " ($6000=" + st + ")"), line, fps))
        print("%-12s %s  %s" % (rows[-1][1].split()[0].upper(), rel, line if kind not in ("pass", "no-protocol") else ""), flush=True)

print()
print("pass %d  fail %d  needs-reset %d  still-running %d  no-protocol %d  load-error %d  (of %d ROMs)" % (
    counts["pass"], counts["fail"], counts["needs-reset"], counts["timeout"], counts["no-protocol"], counts["load-error"], len(rows)))
if speeds:
    print("twin speed on these ROMs: mean %.0f fps, min %.0f fps" % (sum(speeds) / len(speeds), min(speeds)))
if md:
    with open(md, "w") as o:
        o.write("| ROM | result | message | fps |\n|---|---|---|---|\n")
        for r in rows:
            o.write("| %s | %s | %s | %.0f |\n" % (r[0], r[1], r[2].replace("|", "/"), r[3]))
        o.write("\npass %d, fail %d, needs reset %d, still running %d, no protocol %d, load error %d, of %d ROMs\n" % (
            counts["pass"], counts["fail"], counts["needs-reset"], counts["timeout"], counts["no-protocol"], counts["load-error"], len(rows)))
if wexpect:
    with open(wexpect, "w") as o:
        o.write("# ROMs of nes-test-roms (commit 95d8f621) that pass in this core; conformance.py --expect checks them\n")
        for r in rows:
            if r[1] == "pass": o.write(r[0] + "\n")
if expect:
    want = [l.strip() for l in open(expect) if l.strip() and not l.startswith("#")]
    got = {r[0]: r[1] for r in rows if not filt or filt in r[0]}
    bad = [w for w in want if (not filt or filt in w) and got.get(w) != "pass"]
    for w in bad: print("REGRESSION: %s is %s" % (w, got.get(w, "missing")))
    print("expected passes: %d, regressions: %d" % (len(want), len(bad)))
    sys.exit(1 if bad else 0)
