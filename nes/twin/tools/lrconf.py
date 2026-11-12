#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence. Runs lrhead over a ROM tree: lrconf.py LRHEAD CORE.so OUT.json FRAMES [--filter X]
# (NES_TESTROMS = the tree; LR_PATCH_NES2=1 adds an 8 KiB PRG-RAM NES 2.0 header so cores that follow the header expose $6000).
"""lrconf.py LRHEAD CORE.so OUT.json FRAMES [--filter X] [--root DIR]   (headless libretro run, no pad input)"""
import os, sys, subprocess, hashlib, json, re, tempfile
head, core, out, frames = sys.argv[1:5]; frames = int(frames)
root, filt = os.environ.get("NES_TESTROMS", ""), ""
a = sys.argv[5:]
while a:
    k = a.pop(0)
    if k == "--filter": filt = a.pop(0)
    elif k == "--root": root = a.pop(0)
res = {}; tmp = tempfile.mkdtemp(prefix="lrconf")
PATCH = os.environ.get("LR_PATCH_NES2") == "1"
def patched(r):
    d = bytearray(open(r, "rb").read())
    if d[:4] == b"NES\x1a" and not (d[7] & 0x0C):
        d[7] = (d[7] & 0xF3) | 8; d[10] = 0x07
        if d[5] == 0: d[11] = 0x07
    q = os.path.join(tmp, "p.nes"); open(q, "wb").write(d); return q
def one(r, n):
    if PATCH: r = patched(r)
    h, rm = os.path.join(tmp, "h"), os.path.join(tmp, "r")
    p = subprocess.run([head, core, r, str(n), "--hash", h, "--ram", rm, "--status", "--text", "--nt", "--summary"], capture_output=True, text=True, errors="replace", timeout=3600)
    o = p.stdout + p.stderr
    m = re.search(r"peek \$6000=([0-9a-f]{2}) sig=([0-9a-f]{6})", o)
    t = o.split("text:", 1)[1].split("fps=")[0] if "text:" in o else ""
    sp = re.search(r"fps=\s*([0-9.]+)", o)
    nt = o.split("nt:", 1)[1].split("\n")[0] if "nt:" in o else ""
    ram = hashlib.md5(open(rm, "rb").read()).hexdigest() if os.path.exists(rm) and os.path.getsize(rm) else None
    return dict(rc=p.returncode, st=m.group(1) if m else None, sig=m.group(2) if m else None, text=" ".join(t.split())[:200], nt=" ".join(nt.split())[:300], ram=ram, fps=float(sp.group(1)) if sp else 0, load="LOADFAIL" in o)
for d, _, fs in sorted(os.walk(root)):
    for f in sorted(fs):
        r = os.path.join(d, f)
        if not f.lower().endswith(".nes") or filt not in r: continue
        rel = os.path.relpath(r, root)
        try:
            x = one(r, frames)
            nt = x.get("nt", "").lower()
            if x["sig"] == "deb061" and x["st"] == "80": x = one(r, frames * 4)
        except subprocess.TimeoutExpired:
            x = dict(rc=-9, st=None)
        res[rel] = x; print(rel, x.get("st"), flush=True)
json.dump(res, open(out, "w"), indent=0, sort_keys=True)
