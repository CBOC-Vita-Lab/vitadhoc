#!/usr/bin/env python3
"""lang/en.txt -> gpsp/vita/lang_en.h (the English table compiled into the app).
   gen_lang_h.py [--check]   --check: fail when the header is out of date."""
import os, sys, re
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
src = os.path.join(root, "lang", "en.txt")
dst = os.path.join(root, "gpsp", "vita", "lang_en.h")

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

GLYPHS = {"{tri}": 0o177, "{o}": 0o200, "{x}": 0o201, "{sq}": 0o202, "{star}": 0o203, "{staro}": 0o204}

def cstr(v):
    for k, b in GLYPHS.items():
        v = v.replace(k, "\x00%03o" % b)
    # en.txt escapes: \n \t \\ ; everything else is literal UTF-8
    b = []
    i = 0
    while i < len(v):
        c = v[i]
        if c == "\\":
            i += 1
            c = {"n": "\\n", "t": "\\t", "\\": "\\\\"}[v[i]]
            b.append(c)
        elif c == "\x00":
            b.append("\\" + v[i + 1:i + 4]); i += 3
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
    lines = ["/* lang_en.h -- GENERATED from lang/en.txt by tools/i18n/gen_lang_h.py: do not edit. */",
             "static const struct { const char *key, *val; } i18n_en[] = {"]
    for k, v in parse(src):
        lines.append("   { \"%s\", %s }," % (k, cstr(v)))
    lines.append("};")
    return "\n".join(lines) + "\n"

text = make()
if "--check" in sys.argv:
    if not os.path.exists(dst) or open(dst).read() != text:
        sys.exit("gpsp/vita/lang_en.h is out of date: run tools/i18n/gen_lang_h.py")
    print("lang_en.h is current (%d keys)" % len(parse(src)))
else:
    open(dst, "w").write(text)
    print("wrote %s (%d keys)" % (dst, len(parse(src))))
