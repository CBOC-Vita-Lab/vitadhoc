#!/usr/bin/env python3
"""Maps the GLOBDIFF lines of md_twin (MD_TWIN_GLOBDIFF=N, --state-roundtrip) to variable names.
   usage: MD_TWIN_GLOBDIFF=1 build/x86/md_twin ROM --state-roundtrip | python3 -I tools/globdiff.py build/x86/md_twin
Copyright CBOC-Vita. MIT licence."""
import subprocess, sys, bisect, collections
exe = sys.argv[1]
syms = []
for line in subprocess.run(["nm", "-n", "-S", exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in "bBdDrR":
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    elif len(p) == 3 and p[1] in "bBdD":
        syms.append((int(p[0], 16), 0, p[2]))
base = None
for line in subprocess.run(["nm", exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[2] == "md_host_run_frame":
        base = int(p[0], 16)
addrs = [s[0] for s in syms]
cnt = collections.OrderedDict()
for line in sys.stdin:
    if not line.startswith("GLOBDIFF"):
        continue
    _, what, off, n = line.split()
    a = base + int(off)
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][2] if i >= 0 else "?"
    key = "%s+%d" % (name, a - syms[i][0]) if "-o" in sys.argv else name
    cnt[key] = cnt.get(key, 0) + int(n)
for k, v in cnt.items():
    print("%-28s %d bytes" % (k, v))
