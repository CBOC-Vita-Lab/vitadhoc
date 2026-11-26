#!/usr/bin/env python3
"""Checks every translated catalogue against the English one.

  check_catalogues.py [--verbose]

For each lang/<code>.txt (against lang/en.txt) and lang/pk_<code>.txt (against
lang/pk_en.txt):
  - the file is UTF-8 without a byte-order mark, LF line ends, a final LF;
  - every line is a comment, empty, or key<TAB>text with a well-formed key, the
    known escapes only (\\n \\t \\\\) and no control characters;
  - the keys are those of English, none missing, none extra, none twice, in
    the English order;
  - each text has the printf conversions of the English one, in the same order,
    by the loaders' rule (gpsp/vita/i18n.c, playerkit/src/pk_i18n.cpp: "%%" and
    a percent sign followed by a space are text; positional forms are refused);
  - the button glyphs ({x} {o} {tri} {sq} {star} {staro}) of English are all
    there, as many of each (a main-app text may add a glyph where English names
    the button in words, "Circle is A"; the players have no glyphs at all);
  - the line breaks (\\n) are as many as in English;
  - ui.date_order / states.date_order are dmy, mdy or ymd.
The English files themselves must be sorted (the loaders search them) and match
the compiled tables (gen_lang_h.py --check, playerkit's gen_strings.py --check
where it has one).  Standard library only; exits 1 on any problem.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LANG = os.path.join(ROOT, "lang")
KEY_RE = re.compile(r"[A-Za-z0-9._-]+")
GLYPH_RE = re.compile(r"\{(x|o|tri|sq|star|staro)\}")
DATE_KEYS = ("ui.date_order", "states.date_order")


def conv_sig(s):
    """The loaders' conv_sig: the type letters in order, None for a refused form."""
    out, i, n = [], 0, len(s)
    while i < n:
        if s[i] != "%":
            i += 1
            continue
        i += 1
        if i < n and s[i] in "% ":
            i += 1
            continue
        while i < n and s[i] in "-+ #0123456789.*hlLzjtq":
            if s[i] == "*" or s[i] in "hlLzjtq":
                out.append(s[i])
            i += 1
        if i >= n or s[i] not in "diouxXcsfFeEgGp":
            return None
        out.append(s[i] + ",")
        i += 1
    return "".join(out)


def read(path, problems):
    """[(line number, key, raw text)] of a catalogue; format problems appended."""
    raw = open(path, "rb").read()
    name = os.path.relpath(path, ROOT)
    if raw.startswith(b"\xef\xbb\xbf"):
        problems.append("%s: byte-order mark" % name)
        raw = raw[3:]
    if b"\r" in raw:
        problems.append("%s: CR in the file (LF line ends only)" % name)
    if raw and not raw.endswith(b"\n"):
        problems.append("%s: no LF at the end" % name)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        problems.append("%s: not UTF-8 (%s)" % (name, e))
        return []
    out = []
    for n, line in enumerate(text.split("\n"), 1):
        line = line.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        k, sep, v = line.partition("\t")
        where = "%s:%d" % (name, n)
        if not sep or not KEY_RE.fullmatch(k):
            problems.append("%s: not key<TAB>text" % where)
            continue
        if not v:
            problems.append("%s: %s is empty" % (where, k))
        if re.search(r"\\[^nt\\]|\\$", v.replace("\\\\", "")):
            problems.append("%s: %s has an unknown escape" % (where, k))
        if any(ord(c) < 0x20 and c != "\t" for c in v) or "\t" in v:
            problems.append("%s: %s has a control character" % (where, k))
        out.append((n, k, v))
    return out


def check_pair(en_path, tr_path, problems, players):
    en_rows = read(en_path, problems)
    rows = read(tr_path, problems)
    name = os.path.relpath(tr_path, ROOT)
    en = {k: v for _, k, v in en_rows}
    en_order = [k for _, k, _ in en_rows]
    seen = {}
    for n, k, v in rows:
        if k in seen:
            problems.append("%s:%d: %s twice (first on line %d)" % (name, n, k, seen[k]))
        seen[k] = n
    missing = [k for k in en_order if k not in seen]
    extra = [k for k in seen if k not in en]
    for k in missing:
        problems.append("%s: missing %s" % (name, k))
    for k in extra:
        problems.append("%s: %s is not an English key" % (name, k))
    order = [k for _, k, _ in rows if k in en]
    if not missing and not extra and order != en_order:
        first = next(i for i, (a, b) in enumerate(zip(order, en_order)) if a != b)
        problems.append("%s: keys not in the English order (from %s)" % (name, order[first]))
    for n, k, v in rows:
        e = en.get(k)
        if e is None:
            continue
        where = "%s:%d: %s" % (name, n, k)
        se, sv = conv_sig(e), conv_sig(v)
        if sv is None or se != sv:
            problems.append("%s: printf conversions %r, English %r" % (where, sv, se))
        ge, gv = sorted(GLYPH_RE.findall(e)), sorted(GLYPH_RE.findall(v))
        short = [g for g in set(ge) if gv.count(g) < ge.count(g)]
        if short or (players and ge != gv):
            problems.append("%s: glyphs %s, English %s" % (where, gv, ge))
        if v.count("\\n") != e.count("\\n"):
            problems.append("%s: %d line breaks, English %d" % (where, v.count("\\n"), e.count("\\n")))
        if k in DATE_KEYS and v not in ("dmy", "mdy", "ymd"):
            problems.append("%s: must be dmy, mdy or ymd" % where)
    return len(rows)


def main():
    verbose = "--verbose" in sys.argv
    problems = []
    for en_name in ("en.txt", "pk_en.txt"):
        rows = read(os.path.join(LANG, en_name), problems)
        keys = [k for _, k, _ in rows]
        if keys != sorted(keys, key=lambda s: s.encode()):
            problems.append("lang/%s: keys are not sorted" % en_name)
        if len(set(keys)) != len(keys):
            problems.append("lang/%s: a key twice" % en_name)
        for n, k, v in rows:
            if conv_sig(v) is None:
                problems.append("lang/%s:%d: %s has a conversion the loaders refuse" % (en_name, n, k))
    codes = sorted(f[:-4] for f in os.listdir(LANG)
                   if f.endswith(".txt") and not f.startswith("pk_") and f != "en.txt")
    for code in codes:
        for en_name, tr_name in (("en.txt", code + ".txt"), ("pk_en.txt", "pk_" + code + ".txt")):
            tr_path = os.path.join(LANG, tr_name)
            if not os.path.exists(tr_path):
                problems.append("lang/%s: missing (lang/%s.txt has no player catalogue)" % (tr_name, code))
                continue
            n = check_pair(os.path.join(LANG, en_name), tr_path, problems, en_name == "pk_en.txt")
            if verbose:
                print("lang/%s: %d texts" % (tr_name, n))
    for tool in (["tools/i18n/gen_lang_h.py", "--check"], ["playerkit/tools/gen_strings.py", "--check"]):
        r = subprocess.run([sys.executable] + tool, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
        if r.returncode:
            problems.append("%s: %s" % (tool[0], r.stdout.strip()))
    for p in problems:
        print(p)
    print("check_catalogues: %d languages, %d problems" % (len(codes), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
