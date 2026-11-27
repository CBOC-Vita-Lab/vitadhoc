"""Checks the twin's cheat decoder (snes_host_cheat_decode) against an independent Python implementation of the
Game Genie code layout.  The layout is the one Snes9x uses (core/cheats2.cpp, S9xGameGenieToRaw): the 8 letters map
to hex digits through DF4709156BC8A23E -> 0123456789ABCDEF; the first two digits are the value and the other six
are a 24-bit address that is scrambled by a fixed bit permutation.  The permutation is taken from the formula there
and inverted here (it is a bijection, checked below), so the reference is not a copy of the C++ code.
Usage: cheat_vectors.py TWIN [--encode ADDR VAL]
Also checks the accepted and refused spellings of the other code kinds."""
import random, subprocess, sys
twin = sys.argv[1]
GG = "DF4709156BC8A23E"; HEX = "0123456789ABCDEF"

def scramble(a):   # the address transform of S9xGameGenieToRaw, bit by bit
    return (((a & 0x003c00) << 10) + ((a & 0x00003c) << 14) + ((a & 0xf00000) >> 8) + ((a & 0x000003) << 10) +
            ((a & 0x00c000) >> 6) + ((a & 0x0f0000) >> 12) + ((a & 0x0003c0) >> 6))
PERM = [scramble(1 << i) for i in range(24)]
assert all(p & (p - 1) == 0 and p for p in PERM) and len(set(PERM)) == 24, "not a bit permutation"
INV = {p: i for i, p in enumerate(PERM)}

def unscramble(a):
    r = 0
    for j in range(24):
        if a >> j & 1: r |= 1 << INV[1 << j]
    return r

def encode(addr, val):
    d = "%02X%06X" % (val, unscramble(addr))
    out = "".join(GG[HEX.index(c)] for c in d)
    return out[:4] + "-" + out[4:]

def decode(code):
    s = code.replace("-", "")
    h = "".join(HEX[GG.index(c)] for c in s)
    return scramble(int(h[2:], 16)) & 0xFFFFFF, int(h[0:2], 16)

if len(sys.argv) == 5 and sys.argv[2] == "--encode":    # cheat_vectors.py TWIN --encode ADDR VAL: print the Game Genie code
    print(encode(int(sys.argv[3], 16), int(sys.argv[4], 16)))
    sys.exit(0)

def run(args):
    p = subprocess.run([twin, "-"] + args, capture_output=True, text=True)
    return p.stdout.splitlines()

random.seed(7)
cases = [(0x7E0DBE, 0x63), (0x00800D, 0x77), (0x000001, 0x00), (0xFFFFFF, 0xFF), (0x7E0010, 0xAA)]
cases += [(random.randrange(1, 1 << 24), random.randrange(256)) for _ in range(300)]
codes = [encode(a, v) for a, v in cases]
for (a, v), c in zip(cases, codes):
    assert decode(c) == (a, v), "reference self-test failed for %s" % c
args = []
for c in codes: args += ["--cheat-decode", c]
lines = run(args)
bad = 0
assert len(lines) == len(codes), "expected %d lines, got %d" % (len(codes), len(lines))
for (a, v), c, ln in zip(cases, codes, lines):
    want = "%06X=%02X" % (a, v)
    if ln.split(" -> ")[1].split(" ")[0] != want: print("MISMATCH", c, "want", want, "got", ln); bad += 1
# a few fixed vectors (the reference above produced them; the twin must agree)
fixed = [("7E0DBE=63", "7E0DBE=63"), ("7e0dbe:63", "7E0DBE=63"), ("7E0DBE63", "7E0DBE=63"), ("7e0dbe=9?63", "7E0DBE=09?63"),
         ("7E0DBE63+7E0DBF05", "7E0DBE=63+7E0DBF=05"), ("7E0DBE63 7E0DBF05", "7E0DBE=63+7E0DBF=05"), ("7E0DBE=63\n7E0DBF=5", "7E0DBE=63+7E0DBF=05"),
         ("  c2a6-3d6d ", "%06X=%02X" % decode("C2A6-3D6D"))]
for text, want in fixed:
    ln = "\n".join(run(["--cheat-decode", text]))   # a text with a line end echoes over two lines
    got = ln.split(" -> ")[1].split(" ")[0]
    if got != want: print("MISMATCH", repr(text), "want", want, "got", ln); bad += 1
# refused
refuse = ["", "hello", "7E0DBE", "7E0DBE6", "7E0DBE635", "7E0DBE=", "=63", "7E0DBE=123", "1234567=12", "ZZZZ-ZZZZ", "C2A6-3D6", "C2A63D6G",
          "00000063", "000000=63", "0=63", "C2A6 3D6D", "7E0DBE : 63", "7E0DBE=63?", "7E0DBE:1?63"]
for text in refuse:
    ln = "\n".join(run(["--cheat-decode", text]))   # a text with a line end echoes over two lines
    if " error " not in ln: print("ACCEPTED but should be refused:", repr(text), "->", ln); bad += 1
print("cheat decode vectors: %d generated, %d fixed, %d refused spellings, %d problems" % (len(cases), len(fixed), len(refuse), bad))
sys.exit(1 if bad else 0)
