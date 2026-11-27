#!/usr/bin/env python3
"""cheatdb_test.py -- the NES part of the bundled cheat database, run through the
real NES host (nes_twin == nes_host_cheat_add), not through a copy of its grammar.

    cheatdb_test.py TWIN ROMDIR DB

 1. every NES code of the database is accepted by nes_host_cheat_add: tokens
    in == "accepted" out, for the whole database, in chunks
 2. Game Genie codes, decoded here independently, patch the CPU read at the
    decoded address: the value (6 letters), or the value only where the ROM byte
    equals the compare value (8 letters)
 3. RAM pokes AAAA:VV reach the RAM

Copyright CBOC-Vita. MIT licence.
"""
import os
import re
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "..", "..", "gpsp", "vita", "tools"))
from cheatdb_read import read_db  # noqa: E402

twin, romdir, dbpath = sys.argv[1:4]
nrom = os.path.join(romdir, "nrom.nes")
mmc1 = os.path.join(romdir, "mmc1.nes")
GG = "APZLGITYEOXUKSVN"
SEP = re.compile(r"[+,;._ ]+")
fails = 0


def check(ok, what):
    global fails
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok:
        fails += 1


def run(rom, args):
    r = subprocess.run([twin, rom, "--frames", "2"] + args, capture_output=True, text=True)
    return r.stdout + r.stderr


def gg_decode(code):
    """the Game Genie letters -> (address, value, compare or None)"""
    n = [GG.index(c) for c in code]
    a = (0x8000 | ((n[3] & 7) << 12) | ((n[5] & 7) << 8) | ((n[4] & 8) << 8) |
         ((n[2] & 7) << 4) | ((n[1] & 8) << 4) | (n[4] & 7) | (n[3] & 8))
    v = ((n[1] & 7) << 4) | ((n[0] & 8) << 4) | (n[0] & 7)
    if len(n) == 6:
        return a, v | (n[5] & 8), None
    return a, v | (n[7] & 8), ((n[7] & 7) << 4) | ((n[6] & 8) << 4) | (n[6] & 7) | (n[5] & 8)


games, _, _ = read_db(dbpath)
nes = [g for g in games if g["sys"] == 3]
check(len(nes) > 1500, "the database has %d NES games" % len(nes))
tokens = []
for g in nes:
    for _, code in g["cheats"]:
        parts = [p for p in SEP.split(code) if p]
        check_ok = parts and all(
            re.fullmatch(r"[%s]{6}|[%s]{8}|[0-9A-F]{4}:[0-9A-F]{2}|[0-9A-F]{4}\?[0-9A-F]{2}:[0-9A-F]{2}" % (GG, GG), p)
            for p in parts)
        if not check_ok:
            check(False, "grammar: %s: %r" % (g["name"], code))
            continue
        tokens += parts
print("tokens: %d" % len(tokens))

# 1. everything is accepted by the host's own parser
bad = 0
for i in range(0, len(tokens), 150):
    chunk = tokens[i:i + 150]
    args = []
    for t in chunk:
        args += ["--cheat", t]
    out = run(nrom, args)
    got = re.findall(r"cheat '([^']*)': (\d+) accepted", out)
    if [c for c, _ in got] != chunk or any(n != "1" for _, n in got):
        for c, n in got:
            if n != "1":
                bad += 1
                print("  refused: %s" % c)
        if len(got) != len(chunk):
            bad += 1
            print("  chunk %d: %d answers for %d codes" % (i, len(got), len(chunk)))
check(bad == 0, "all %d tokens accepted by nes_host_cheat_add (%d refused)" % (len(tokens), bad))

# 2. Game Genie against an independent decoder
ggs = sorted({t for t in tokens if re.fullmatch("[%s]{6,8}" % GG, t) and len(t) in (6, 8)})
sample = ggs[::max(1, len(ggs) // 120)]
wrong = 0
for t in sample:
    a, v, c = gg_decode(t)
    base = re.search(r"peek [0-9a-f]+=([0-9a-f]+)", run(nrom, ["--peek", "%04X" % a])).group(1)
    got = re.search(r"peek [0-9a-f]+=([0-9a-f]+)", run(nrom, ["--cheat", t, "--peek", "%04X" % a])).group(1)
    want = v if (c is None or c == int(base, 16)) else int(base, 16)
    if int(got, 16) != want:
        wrong += 1
        print("  %s: %04X base %s got %s want %02x" % (t, a, base, got, want))
check(wrong == 0, "%d Game Genie codes patch the read where an independent decoder says (%d wrong)" % (len(sample), wrong))
check(gg_decode("SXIOPO") == (0x91D9, 0xAD, None), "the decoder agrees with the well-known SXIOPO = 91D9:AD")

# 3. pokes reach the RAM: a read of 0100-07FF is patched (what the game sees), and
#    a zero-page poke is written to the RAM itself

pokes = []
for t in tokens:
    m = re.fullmatch(r"(0[3-7][0-9A-F]{2}):([0-9A-F]{2})", t)
    if m and m.group(1) not in [p[0] for p in pokes]:
        pokes.append((m.group(1), m.group(2)))
pokes = pokes[::max(1, len(pokes) // 25)][:25]
wrong = 0
for a, v in pokes:
    out = run(mmc1, ["--cheat", "%s:%s" % (a, v), "--peek", a])
    if not re.search(r"^peek %s=%s$" % (a.lower(), v.lower()), out, re.M):
        wrong += 1
        print("  poke %s:%s: %s" % (a, v, out.strip().splitlines()[-1:]))
check(pokes and wrong == 0, "%d RAM pokes from the database reach the CPU's reads (%d wrong)" % (len(pokes), wrong))
zp = sorted({t for t in tokens if re.fullmatch(r"00[0-9A-F]{2}:[0-9A-F]{2}", t)})
zp = zp[::max(1, len(zp) // 12)][:12]
wrong = 0
for t in zp:
    a, v = t.split(":")
    out = run(mmc1, ["--cheat", t, "--ra", "%s,1" % a])
    if not re.search(r"^ra %s n=1: %s$" % (a.lower(), v.lower()), out, re.M):
        wrong += 1
        print("  zero page %s: %s" % (t, out.strip().splitlines()[-2:]))
check(zp and wrong == 0, "%d zero-page pokes from the database reach the RAM (%d wrong)" % (len(zp), wrong))
sys.exit(1 if fails else 0)
