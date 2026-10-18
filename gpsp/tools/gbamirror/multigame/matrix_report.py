#!/usr/bin/env python3
"""matrix_report.py OUTDIR -- the multigame matrix table from run_matrix.sh's
<fixture>-<cond>.out files: game x condition, PASS/FAIL, end frame, first
divergent frame; then the per-frame core cost of each fixture's cost arm
(loopback, no per-frame hash) relative to AW2.  PPSSPP is NOT a timing
source: the cost column only ranks the games against each other."""
import os
import re
import sys

COND = ["loop", "jitter", "loss"]
COND_NAME = {"loop": "loopback", "jitter": "15+-5 ms", "loss": "3 % loss"}


def result(path):
    try:
        txt = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    m = None
    for m in re.finditer(r"^RESULT (.*)$", txt, re.M):
        pass
    if not m:
        why = re.findall(r"^FAIL: (.*)$", txt, re.M)
        return {"verdict": "FAIL", "frames": "?", "desync": "-",
                "core_ms": "?", "why": why[:1]}
    d = dict(re.findall(r"(\w+)=([^\s]+)", m.group(1)))
    d["why"] = re.findall(r"^FAIL: (.*)$", txt, re.M)[:4]
    return d


def main():
    d = sys.argv[1]
    names = sorted({f.rsplit("-", 1)[0] for f in os.listdir(d)
                    if f.endswith(".out") and "-" in f},
                   key=lambda n: (n != "aw2", n))
    hdr = "%-6s | %-26s | %-26s | %-26s" % ("game", *[COND_NAME[c] for c in COND])
    print(hdr)
    print("-" * len(hdr))
    notes = []
    for n in names:
        cells = []
        for c in COND:
            r = result(os.path.join(d, "%s-%s.out" % (n, c)))
            if r is None:
                cells.append("(not run)")
                continue
            game = any(w.startswith("game:") for w in r.get("why", []))
            other = [w for w in r.get("why", []) if not w.startswith("game:")]
            v = "GAME-FAIL" if (r["verdict"] == "FAIL" and game and not other) else r["verdict"]
            cell = "%s f=%s" % (v, r.get("frames", "?"))
            if r.get("desync", "-") != "-":
                cell += " desync@%s" % r["desync"]
            cells.append(cell)
            for w in r.get("why", []):
                notes.append("%s/%s: %s" % (n, c, w))
        print("%-6s | %-26s | %-26s | %-26s" % (n, *cells))
    print()
    costs = {}
    for n in names:
        r = result(os.path.join(d, "%s-cost.out" % n))
        try:
            costs[n] = float(r["core_ms"]) if r else None
        except (TypeError, ValueError):
            costs[n] = None
    base = costs.get("aw2")
    print("per-frame core cost, both GBAs, PPSSPP (relative ranking only, not hardware timing):")
    for n in sorted(names, key=lambda k: -(costs.get(k) or 0)):
        c = costs.get(n)
        if c is None:
            print("  %-6s (no cost arm)" % n)
        else:
            print("  %-6s %6.2f ms  %s" % (n, c, ("%.2fx AW2" % (c / base)) if base else ""))
    ce = result(os.path.join(d, "control_end.out"))
    if ce is not None:
        print()
        print("end control (AW2 loopback after the matrix): %s" % ce["verdict"])
    if notes:
        print()
        for x in notes:
            print("note " + x)


if __name__ == "__main__":
    main()
