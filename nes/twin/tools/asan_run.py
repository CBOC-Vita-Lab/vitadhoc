#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence. Runs an ASan/UBSan build of nes_twin over a ROM tree and collects the sanitizer reports:
# asan_run.py TWIN ROOT FRAMES OUT.json [twin options]
  # build: make -C nes/twin B=/some/dir OPT="-O1 -g" EXTRA="-fsanitize=address,undefined" CXX="g++ -fsanitize=address,undefined"
import os, sys, subprocess, re, json, collections
twin, root, frames, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
extra = sys.argv[5:]
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=0")
issues = collections.OrderedDict(); n = 0; bad_rc = []
roms = []
for d, _, fs in sorted(os.walk(root)):
    for f in sorted(fs):
        if f.lower().endswith(".nes") or f.lower().endswith((".unf", ".fds")): roms.append(os.path.join(d, f))
for r in roms:
    n += 1
    try:
        p = subprocess.run([twin, r, "--frames", frames, "--pad", "30:start,60:a,90:none,120:right+a,200:b,260:none"] + extra, capture_output=True, text=True, errors="replace", timeout=1800, env=env)
    except subprocess.TimeoutExpired:
        issues.setdefault("TIMEOUT", []).append(r); continue
    for ln in p.stderr.splitlines():
        if "runtime error:" in ln or "ERROR: AddressSanitizer" in ln or "ERROR: LeakSanitizer" in ln:
            key = re.sub(r"^.*?/core/", "core/", ln.strip())[:200]
            issues.setdefault(key, []).append(os.path.relpath(r, root))
    if p.returncode not in (0,) and "Sanitizer" in p.stderr: bad_rc.append(os.path.relpath(r, root))
    print(n, os.path.relpath(r, root), p.returncode, flush=True)
json.dump(dict(roms=n, issues={k: sorted(set(v))[:6] + ([str(len(set(v)))+" ROMs"] if len(set(v)) > 6 else []) for k, v in issues.items()}, sanitizer_rc=bad_rc), open(out, "w"), indent=1)
print("done", n, len(issues))
