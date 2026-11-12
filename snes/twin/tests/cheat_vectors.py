#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Checks the twin's cheat decoder (snes_host_cheat_decode) against a second implementation written from the
published Game Genie layout: after replacing the letters by the transposition table
  D F 4 7 0 9 1 5 6 B C 8 A 2 3 E  ->  0 1 2 3 4 5 6 7 8 9 A B C D E F
the 8 hex digits are  VV  and the address bits  ijkl qrst opab cdef ghmn uvwx  where the address is  abcdefgh ijklmnop qrstuvwx.
Usage: cheat_vectors.py TWIN
Also checks the accepted and refused spellings of the other code kinds."""
import random, subprocess, sys
twin = sys.argv[1]
GG = "DF4709156BC8A23E"; HEX = "0123456789ABCDEF"

def encode(addr, val):
    bits = format(addr, "024b")
    a = bits[0:8]; i = bits[8:16]; q = bits[16:24]           # abcdefgh ijklmnop qrstuvwx
    A, B, C, D, E_, F, G, H = a; I, J, K, L, M, N, O, P = i; Q, R, S, T, U, V, W, X = q
    n = [format(val >> 4, "x"), format(val & 15, "x"),
         I + J + K + L, Q + R + S + T, O + P + A + B, C + D + E_ + F, G + H + M + N, U + V + W + X]
    d = [x if len(x) == 1 else format(int(x, 2), "X") for x in n]
    d = [x.upper() for x in d]
    out = "".join(GG[HEX.index(x)] for x in d)
    return out[:4] + "-" + out[4:]

def decode(code):
    s = code.replace("-", "")
    h = "".join(HEX[GG.index(c)] for c in s)
    val = int(h[0:2], 16)
    b = [format(int(c, 16), "04b") for c in h[2:]]
    I, J, K, L = b[0]; Q, R, S, T = b[1]; O, P, A, B = b[2]; C, D, E_, F = b[3]; G, H, M, N = b[4]; U, V, W, X = b[5]
    addr = int(A + B + C + D + E_ + F + G + H + I + J + K + L + M + N + O + P + Q + R + S + T + U + V + W + X, 2)
    return addr, val

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
    ln = run(["--cheat-decode", text])[0]
    got = ln.split(" -> ")[1].split(" ")[0]
    if got != want: print("MISMATCH", repr(text), "want", want, "got", ln); bad += 1
# refused
refuse = ["", "hello", "7E0DBE", "7E0DBE6", "7E0DBE635", "7E0DBE=", "=63", "7E0DBE=123", "1234567=12", "ZZZZ-ZZZZ", "C2A6-3D6", "C2A63D6D",
          "00000063", "000000=63", "0=63", "C2A6 3D6D", "7E0DBE : 63", "7E0DBE=63?", "7E0DBE:1?63"]
for text in refuse:
    ln = run(["--cheat-decode", text])[0]
    if " error " not in ln: print("ACCEPTED but should be refused:", repr(text), "->", ln); bad += 1
print("cheat decode vectors: %d generated, %d fixed, %d refused spellings, %d problems" % (len(cases), len(fixed), len(refuse), bad))
sys.exit(1 if bad else 0)
