#!/usr/bin/env python3
"""lang/pk_en.txt -> playerkit/src/pk_strings_en.inc (the English table compiled into the players).
   gen_strings.py [--check]   --check: fail when the .inc is out of date."""
import os, sys, re
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
src = os.path.join(root, "lang", "pk_en.txt")
dst = os.path.join(root, "playerkit", "src", "pk_strings_en.inc")

def parse(path):
    out = []
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        k, sep, v = line.partition("\t")
        if not sep or not re.fullmatch(r"[A-Za-z0-9._-]+", k) or not v:
            sys.exit("%s:%d: not key<TAB>text" % (path, n))
        out.append((k, v))
    keys = [k for k, _ in out]
    if keys != sorted(keys, key=lambda s: s.encode()):
        sys.exit("%s: keys are not sorted" % path)
    if len(set(keys)) != len(keys):
        sys.exit("%s: duplicate key" % path)
    return out

def cstr(v):
    # pk_en.txt escapes: \n \t \\ ; everything else is literal UTF-8
    b = []
    i = 0
    while i < len(v):
        c = v[i]
        if c == "\\":
            i += 1
            b.append({"n": "\\n", "t": "\\t", "\\": "\\\\"}[v[i]])
        elif c == '"':
            b.append('\\"')
        elif c == "?":
            b.append("\\?")          # no trigraphs
        elif ord(c) < 0x80:
            b.append(c)
        else:
            b.append("".join("\\%03o" % x for x in c.encode()))
        i += 1
    return '"' + "".join(b) + '"'

def make():
    lines = ["/* pk_strings_en.inc -- GENERATED from lang/pk_en.txt by playerkit/tools/gen_strings.py: do not edit. */",
             "static const struct { const char *key, *val; } kEn[] = {"]
    for k, v in parse(src):
        lines.append("\t{ \"%s\", %s }," % (k, cstr(v)))
    lines.append("};")
    return "\n".join(lines) + "\n"

text = make()
if "--check" in sys.argv:
    if not os.path.exists(dst) or open(dst).read() != text:
        sys.exit("playerkit/src/pk_strings_en.inc is out of date: run playerkit/tools/gen_strings.py")
    print("pk_strings_en.inc is current (%d keys)" % len(parse(src)))
else:
    open(dst, "w").write(text)
    print("wrote %s (%d keys)" % (dst, len(parse(src))))
