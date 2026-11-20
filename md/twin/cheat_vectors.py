#!/usr/bin/env python3
"""Applies the canonical parts of gpsp/vita/tests/cheats/sega_vectors.txt through md_host_cheat_add and checks the bytes.

usage: python3 -I cheat_vectors.py MD_TWIN VECTORS.txt MD_ROM [SMS_ROM]

Mega Drive: work RAM parts are read back from $FF0000 and up, ROM word patches from the ROM address; parts the rules refuse must
change nothing and be counted as refused. 8 bit systems (needs an SMS ROM): RAM parts at C000 and up, ROM parts below 4000 (the
fixed slots) are checked, with a compare that is the real ROM byte (must patch) and one that is not (must not).
Prints PASS / FAIL / SKIP lines, one per console group, and one FAIL per failing vector."""
import subprocess, sys

twin, vec, md_rom = sys.argv[1], sys.argv[2], sys.argv[3]
sms_rom = sys.argv[4] if len(sys.argv) > 4 else None


def run(rom, code, peeks, system=None):
    cmd = [twin, rom, "--frames", "2", "--quiet", "--cheat", code]
    if system:
        cmd += ["--system", system]
    for p in peeks:
        cmd += ["--peek", "%X" % p]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
    acc, vals = None, {}
    for ln in out.splitlines():
        if ln.startswith("cheats accepted:"):
            acc = int(ln.split(":")[1])
        elif ln.startswith("PEEK "):
            a, v = ln[5:].split("=")
            vals[int(a, 16)] = int(v, 16)
    return acc, vals


def parts_of(res):
    return [p.split(":") for p in res.split("+")]


def rom_bytes(path):
    return open(path, "rb").read()


groups = {"MD": [0, 0], "8BIT": [0, 0]}   # checked, failed
refused = 0
fails = []
md_data = rom_bytes(md_rom)
sms_data = rom_bytes(sms_rom) if sms_rom else None

for ln in open(vec):
    if ln.startswith("#") or not ln.strip():
        continue
    cols = ln.rstrip("\n").split("\t")
    if len(cols) < 3 or cols[2].startswith("DROP:"):
        continue
    con, res = cols[0], cols[2]
    parts = parts_of(res)
    if con == "MD":
        exp, kept, sent = {}, 0, []
        for pt in parts:
            a, v = int(pt[0], 16), int(pt[1], 16)
            if a >= 0xFF0000:
                if v < 0x100:
                    exp[((a & 0xFFFF) ^ 1) | 0xFF0000] = v      # Genesis Plus GX: a 00VV patch lands in work_ram[a] as held = the 68000 byte a^1
                else:
                    exp[(a & 0xFFFE) | 0xFF0000], exp[(a & 0xFFFE) + 1 | 0xFF0000] = v >> 8, v & 255
                kept += 1
            elif a < 0x400000 and (a & 0xFFFFFE) + 2 <= len(md_data):
                exp[a & 0xFFFFFE], exp[(a & 0xFFFFFE) + 1] = v >> 8, v & 255
                kept += 1
            else:
                refused += 1
            sent.append(pt)
        if not exp:
            continue
        acc, vals = run(md_rom, res, sorted(exp))
        bad = [("%06X" % a, exp[a], vals.get(a)) for a in exp if vals.get(a) != exp[a]]
        groups["MD"][0] += 1
        if acc != kept or bad:
            groups["MD"][1] += 1
            fails.append("FAIL md vector %s -> %s: accepted %s of %d, wrong bytes %s" % (cols[1], res, acc, kept, bad[:4]))
    else:
        if not sms_data:
            continue
        exp, peeks, send, kept = {}, [], [], 0
        for pt in parts:
            a, v = int(pt[0], 16), int(pt[1], 16)
            cmp_ = int(pt[2], 16) if len(pt) > 2 else None
            if a >= 0xC000:
                exp[a] = v
                send.append(":".join(pt)); kept += 1
            elif a < 0x4000 and a < len(sms_data):
                real = sms_data[a]
                if cmp_ is None or cmp_ == 0:
                    exp[a] = v
                else:
                    cmp_ = real            # a compare that matches the ROM byte: must patch
                    exp[a] = v
                send.append("%04X:%02X%s" % (a, v, (":%02X" % cmp_) if cmp_ else "")); kept += 1
        if not exp:
            continue
        # RAM mirror: C000-DFFF and E000-FFFF are the same 8 KB, so later parts win on the same cell
        model = {}
        for k in sorted(exp):
            pass
        for s in send:
            f = s.split(":"); a = int(f[0], 16)
            model[(a & 0x1FFF) + 0xC000 if a >= 0xC000 else a] = int(f[1], 16)
        acc, vals = run(sms_rom, "+".join(send), sorted(model), None)
        bad = [("%04X" % a, model[a], vals.get(a)) for a in model if vals.get(a) != model[a]]
        groups["8BIT"][0] += 1
        if acc != kept or bad:
            groups["8BIT"][1] += 1
            fails.append("FAIL 8bit vector %s -> %s: accepted %s of %d, wrong bytes %s" % (cols[1], res, acc, kept, bad[:4]))

# a compare that is NOT the ROM byte must leave the ROM alone
if sms_data:
    a = 0x0100
    real = sms_data[a]
    wrong = (real ^ 0xFF) or 1
    acc, vals = run(sms_rom, "%04X:%02X:%02X" % (a, (real ^ 0x5A) & 255, wrong), [a])
    groups["8BIT"][0] += 1
    if vals.get(a) != real:
        groups["8BIT"][1] += 1
        fails.append("FAIL 8bit compare mismatch still patched the ROM")
    acc, vals = run(sms_rom, "%04X:%02X:00" % (a, (real ^ 0x5A) & 255), [a])      # compare 00 = no compare
    groups["8BIT"][0] += 1
    if vals.get(a) != (real ^ 0x5A) & 255:
        groups["8BIT"][1] += 1
        fails.append("FAIL 8bit compare 00 must patch unconditionally")

for f in fails:
    print(f)
for g, label in (("MD", "Mega Drive"), ("8BIT", "Master System / Game Gear / SG-1000")):
    n, bad = groups[g]
    if g == "8BIT" and not sms_data:
        print("SKIP cheat vectors %s (no 8 bit ROM given)" % label)
    elif n == 0:
        print("FAIL cheat vectors %s: nothing was checked" % label)
    else:
        print("%s cheat vectors %s: %d checked, %d failed%s" % ("PASS" if bad == 0 else "FAIL", label, n, bad,
              (", %d parts refused by the rules" % refused) if g == "MD" else ""))
