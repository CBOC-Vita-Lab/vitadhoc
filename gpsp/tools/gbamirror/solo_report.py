#!/usr/bin/env python3
"""solo_report.py LOGDIR... -- per arm: the one-console bench's emulated fps
and core time (EVT fps / core_prof; first window dropped) on the console the
rig ran on.  Logs autoNNN-<arm>-<role>.log."""
import glob, os, re, statistics as st, sys
rows = {}
for d in sys.argv[1:]:
    for f in sorted(glob.glob(os.path.join(d, "auto*-*-*.log"))):
        m = re.match(r"auto(\d+)-(\w+)-(host|join)\.log$", os.path.basename(f))
        if not m or m.group(2) == "x":
            continue
        core, fps = [], []
        for ln in open(f, errors="replace"):
            if ln.startswith("EVT core_prof"):
                core.append(int(re.search(r"core=(\d+)", ln).group(1)))
            elif ln.startswith("EVT fps "):
                fps.append(float(re.search(r"emu=([\d.]+)", ln).group(1)))
        if len(core) > 2:
            rows.setdefault(m.group(2), []).append(
                (os.path.basename(f), st.mean(core[1:]) / 1000, st.mean(fps[1:])))
print("%-4s %4s %8s %7s   runs" % ("arm", "n", "core ms", "fps"))
for arm, rs in sorted(rows.items()):
    print("%-4s %4d %8.2f %7.1f   %s" % (arm, len(rs), st.mean(r[1] for r in rs),
          st.mean(r[2] for r in rs), "  ".join("%.2f/%.1f" % (r[1], r[2]) for r in rs)))
