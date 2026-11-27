#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence. Runs nes_twin over a ROM tree and records status, text, md5 of the per-frame picture hashes,
# of the WAV and of the RAM dump: runset.py TWIN OUT.json FRAMES [--prefix "qemu-arm -cpu max"] [--filter X] [--pad SCRIPT]
# Two such files (x86_64 vs armhf under qemu, or before vs after a change) are compared key by key.
"""runset.py TWIN OUTJSON FRAMES [--prefix 'qemu-arm -cpu max'] [--filter X] [--root DIR] [--pad SCRIPT]
per ROM: status/sig/text, md5 of per-frame hash file, md5 of wav, md5 of ram dump, last frame hash."""
import os, sys, subprocess, hashlib, json, tempfile, shlex, re
twin, out, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
prefix, filt, root, pad, extra = [], "", os.environ.get("NES_TESTROMS", ""), "30:start,60:a,90:none,120:right+a,200:b,260:none", []
a = sys.argv[4:]
while a:
    k = a.pop(0)
    if k == "--prefix": prefix = shlex.split(a.pop(0))
    elif k == "--filter": filt = a.pop(0)
    elif k == "--root": root = a.pop(0)
    elif k == "--pad": pad = a.pop(0)
    elif k == "--extra": extra = shlex.split(a.pop(0))
res = {}
roms = []
for d, _, fs in sorted(os.walk(root)):
    for f in sorted(fs):
        if f.lower().endswith((".nes",)) and filt in os.path.join(d, f): roms.append(os.path.join(d, f))
tmp = tempfile.mkdtemp(prefix="runset", dir=os.environ.get("TMPDIR", "/tmp"))
def md5(p):
    try: return hashlib.md5(open(p, "rb").read()).hexdigest()
    except OSError: return None
for r in roms:
    rel = os.path.relpath(r, root)
    h, w, rd = [os.path.join(tmp, x) for x in ("h", "w", "r")]
    cmd = prefix + [twin, r, "--frames", str(frames), "--pad", pad, "--hashes", h, "--wav", w, "--ram-dump", rd, "--status", "--text", "--summary"] + extra
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=1800)
        o = p.stdout + p.stderr
        m = re.search(r"peek \$6000=([0-9a-f]{2}) sig=([0-9a-f]{6})", o)
        t = o.split("text:", 1)[1].split("\n")[0] if "text:" in o else ""
        last = open(h).read().strip().splitlines()[-1] if os.path.exists(h) and os.path.getsize(h) else ""
        res[rel] = dict(rc=p.returncode, st=m.group(1) if m else None, sig=m.group(2) if m else None, text=" ".join(t.split())[:200],
                        hashes=md5(h), wav=md5(w), ram=md5(rd), last=last.split()[-1] if last else "")
    except subprocess.TimeoutExpired:
        res[rel] = dict(rc=-9, st=None)
    for x in (h, w, rd):
        if os.path.exists(x): os.unlink(x)
    print(rel, res[rel].get("st"), flush=True)
json.dump(res, open(out, "w"), indent=0, sort_keys=True)
