#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence. Instructions per frame under callgrind: ir.py TWIN OUT.json ROM... (delta of a 60 and a 260 frame run).
"""ir.py TWIN OUT.json ROM... : instructions per frame (callgrind Ir), delta of two runs (60 and 260 frames)"""
import subprocess, re, sys, json, os
twin, out = sys.argv[1:3]; res = {}
def ir(rom, n, pad):
    p = subprocess.run(["valgrind", "--tool=callgrind", "--callgrind-out-file=/dev/null", twin, rom, "--frames", str(n), "--pad", pad], capture_output=True, text=True, errors="replace")
    m = re.search(r"Collected : (\d+)", p.stderr)
    return int(m.group(1)) if m else None
pad = "30:start,60:a,90:none,120:right+a,200:b,260:none"
for rom in sys.argv[3:]:
    a, b = ir(rom, 60, pad), ir(rom, 260, pad)
    res[rom] = dict(ir_per_frame=(b - a) / 200.0 if a and b else None, startup_ir=a)
    print(rom, res[rom], flush=True)
json.dump(res, open(out, "w"), indent=1)
