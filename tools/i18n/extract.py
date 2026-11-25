#!/usr/bin/env python3
"""Moves the user-visible English literals of C sources into lang/en.txt.

  extract.py --area ui [--list] [--apply] FILE...

Every string literal (adjacent literals joined) that looks like text a person
reads becomes tr("area.slug") in the source and a `key<TAB>text` line in
lang/en.txt (sorted; an existing (area, text) keeps its key).  Not touched: log
lines (fe_evt & co.), comparisons (strcmp...), file names and paths, key=value
formats, lowercase identifiers, proper names (NOTRANSLATE).
--list shows what would change; --apply writes the files.
--keys-at FILE:LINE,..  (after --apply, for a literal in a static initializer
the compiler refused) turns tr("k") on those lines into TR_KEY("k").
"""
import os, re, sys, argparse

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EN = os.path.join(ROOT, "lang", "en.txt")

DENY_FN = set("""fe_evt fe_evt_raw strcmp strncmp strcasecmp strncasecmp stricmp strstr strcasestr strchr strrchr strpbrk strspn
strcspn memcmp fopen fopen_rb fopen_wb getenv fe_ini_get fe_ini_set fe_ini_get_int sscanf sizeof strlen open stat mkdir remove rename
opendir access unlink fprintf printf puts sceClibPrintf plat_console assert fputs fwrite fread ends_with has_ext str_ends
fe_log log_line logf plat_log vita_log dbg_log fe_dbg ui_log atoi atof strtol strtoul fe_ini_load fe_ini_save sce_log
fe_ini_get_str strdup fe_config_get fe_file_exists file_exists file_size fe_path_join path_join plat_fopen""".split())
NOTRANSLATE = set("""GBA GB GBC SNES NES WS WSC Genesis SMS GG SG-1000 VitAdhoc GBAdhoc Vita PSP PS TV OLED LCD
Game Boy|Game Boy Advance|Game Boy Color|Super Nintendo|WonderSwan|Sega Genesis / Mega Drive|Master System|Game Gear|Sega CD|Sega CD / Mega CD|
Wi-Fi|USB|CRT|HDMI|RetroArch|RetroAchievements|Discord|GitHub|FPS|MHz|PSVshell|Toniebox|Mega Drive|Mega CD|NES|PS TV""".replace("\n", "|").split("|")) | set("GBA GB GBC SNES NES WS WSC Genesis SMS GG".split())
NOTRANSLATE |= {"GBA", "GB", "GBC", "SNES", "NES", "WS", "Genesis", "SMS", "GG", "SG-1000", "VitAdhoc", "GBAdhoc", "Vita", "PSP",
                  "Game Boy", "Game Boy Advance", "Game Boy Color", "Super Nintendo", "WonderSwan", "Master System", "Game Gear",
                  "Sega CD", "Sega Genesis / Mega Drive", "Sega CD / Mega CD", "Mega Drive", "RetroArch", "RetroAchievements"}


# macros that sit between the literals of one sentence: the Vita's button glyphs (placeholders in the
# catalogue, expanded by the loader) and the words that differ by console (this is the Vita's app)
GLYPH = {"VID_GLYPH_TRI": "{tri}", "VID_GLYPH_O": "{o}", "VID_GLYPH_X": "{x}", "VID_GLYPH_SQ": "{sq}",
         "VID_GLYPH_STAR": "{star}", "VID_GLYPH_STAR_O": "{staro}", "UI_KEY_X": "{x}", "UI_KEY_O": "{o}"}
WORDS = {"UI_DEV": "Vita", "UI_DEVS": "Vitas", "UI_APP": "VitAdhoc", "UI_APP_NAME": "VitAdhoc", "UI_SHELL_NAME": "the LiveArea",
         "UI_RADIO_OFF": "Wi-Fi is off", "ROOM_LABEL_WORD": "Room"}
MACROS = dict(GLYPH); MACROS.update(WORDS)
GBYTES = {b"\x7f": b"{tri}", b"\x80": b"{o}", b"\x81": b"{x}", b"\x82": b"{sq}", b"\x83": b"{star}", b"\x84": b"{staro}"}


class Tok:
    __slots__ = ("kind", "s", "a", "b", "line", "text")
    def __init__(s, kind, text, a, b, line):
        s.kind, s.text, s.a, s.b, s.line = kind, text, a, b, line


def lex(src):
    """tokens: 'str' (a string literal, raw body in text), 'id', 'p' (punctuation), 'pp' (a preprocessor line start)"""
    toks, i, n, line = [], 0, len(src), 1
    at_line_start = True
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1; i += 1; at_line_start = True; continue
        if c in " \t\r":
            i += 1; continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += src.count("\n", i, j); i = j; continue
        if src.startswith("//", i):
            j = src.find("\n", i); i = n if j < 0 else j; continue
        if c == "#" and at_line_start:
            # a preprocessor line: its directive word is a token, its body is lexed on (continuations folded)
            m = re.match(r"#\s*(\w+)", src[i:])
            toks.append(Tok("pp", m.group(1) if m else "", i, i + 1, line))
            i += 1; at_line_start = False; continue
        at_line_start = False
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            toks.append(Tok("str", src[i + 1:j], i, j + 1, line))
            line += src.count("\n", i, j); i = j + 1; continue
        if c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            toks.append(Tok("chr", src[i:j + 1], i, j + 1, line)); i = j + 1; continue
        m = re.match(r"[A-Za-z_]\w*", src[i:])
        if m:
            toks.append(Tok("id", m.group(0), i, i + len(m.group(0)), line)); i += len(m.group(0)); continue
        m = re.match(r"\d[\w.]*", src[i:])
        if m:
            toks.append(Tok("num", m.group(0), i, i + len(m.group(0)), line)); i += len(m.group(0)); continue
        if c == "\\" and i + 1 < n and src[i + 1] == "\n":      # a continuation line
            line += 1; i += 2; continue
        toks.append(Tok("p", c, i, i + 1, line)); i += 1
    return toks


def dead_lines(src):
    """1-based line numbers inside a branch the Vita build never compiles (#ifndef VITA ... #else/#endif, #ifdef VITA ... #else)."""
    dead, stack = set(), []          # stack of [state] state: 'live', 'dead' (this branch), or 'other'
    for n, line in enumerate(src.split("\n"), 1):
        m = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
        if m:
            d, rest = m.group(1), re.sub(r"/\*.*?\*/|//.*$", "", m.group(2)).strip()
            if d in ("ifdef", "ifndef", "if"):
                is_vita = re.fullmatch(r"VITA|defined\s*\(?\s*VITA\s*\)?", rest) is not None
                if d == "ifdef" and is_vita: stack.append(["live", "dead"])
                elif d == "ifndef" and rest == "VITA": stack.append(["dead", "live"])
                elif d == "if" and is_vita: stack.append(["live", "dead"])
                elif d == "if" and re.fullmatch(r"!\s*defined\s*\(?\s*VITA\s*\)?", rest): stack.append(["dead", "live"])
                else: stack.append(["other", "other"])
                stack[-1].append(0)
            elif d in ("else", "elif"):
                if stack:
                    stack[-1][2] = 1
            elif d == "endif" and stack:
                stack.pop()
        if any(st[st[2]] == "dead" for st in stack):
            dead.add(n)
    return dead


def decode(body):
    """C literal body -> text, or None when it holds something this scheme does not carry."""
    out = bytearray(); i = 0
    while i < len(body):
        c = body[i]
        if c == "\\":
            i += 1; e = body[i]
            if e == "n": out += b"\n"
            elif e == "t": out += b"\t"
            elif e == "\\": out += b"\\"
            elif e == '"': out += b'"'
            elif e == "'": out += b"'"
            elif e == "%": out += b"%"
            elif e == "x":
                m = re.match(r"[0-9a-fA-F]{1,2}", body[i + 1:])
                if not m: return None
                out.append(int(m.group(0), 16)); i += len(m.group(0))
            elif e in "01234567":
                m = re.match(r"[0-7]{1,3}", body[i:]); out.append(int(m.group(0), 8)); i += len(m.group(0)) - 1
            else:
                return None
            i += 1
        else:
            out += c.encode(); i += 1
    for b, ph in GBYTES.items():
        out = out.replace(b, ph)
    try:
        t = out.decode("utf-8")
    except UnicodeDecodeError:
        return None
    if any(ord(ch) < 32 and ch not in "\n\t" for ch in t):
        return None
    return t


BUTTONS = set("START SELECT HOME L R L1 R1 UP DOWN LEFT RIGHT STICK REAR TL TR BL BR CROSS CIRCLE TRIANGLE SQUARE X O DPAD PS".split())


def buttons_only(t):
    w = [x for x in re.split(r"[ +/,:-]+", t) if x]
    return bool(w) and all(x in BUTTONS for x in w)


def visible(t):
    if buttons_only(t):
        return False
    s = re.sub(r"%[-+ #0-9.*]*[hlzjt]*[a-zA-Z%]", " ", t)
    if not re.search(r"[^\W\d_]{2}", s):
        return False
    if t in NOTRANSLATE:
        return False
    if re.fullmatch(r"[a-z0-9_.:=/+\-]+", t):               # a lowercase token: an id, a file name, a log word
        return False
    if re.search(r"\w=%|\w=\w+ |^\.\w+$|\.(png|txt|ini|gba|gb|gbc|bin|dat|cht|sav|ips|zip|log|json|apng|wav)\b|^%s/|/%s|^[a-z_]+:", t):
        return False
    if "/" in t and " " not in t:                            # a path
        return False
    if re.fullmatch(r"[A-Z0-9]+", t) and re.search(r"\d", t):   # an alphabet, a code
        return False
    if re.match(r"^(/|ovr\.)|ux0:|uma0:|^%(\.\*)?s(%(\.\*)?s)?\.\w+$|^%s\.|^%s%s", t):
        return False
    if re.match(r"^(GBAdhoc|gpSP|# VitAdhoc)|\|%|\.srm|\.shot|^%s\|", t):
        return False
    if re.match(r"^(GPSP|ge_)", t):
        return False
    if re.fullmatch(r"[A-Z0-9_]+", t) and len(t) <= 4:     # BIOS, RTC...: a code
        return False
    if re.fullmatch(r"[A-Z_]{2,}[0-9]*", t) and "_" in t:   # an enum-like name
        return False
    return True


def slug(t):
    s = re.sub(r"%[-+ #0-9.*]*[hlzjt]*[a-zA-Z%]", " ", t)
    words = re.findall(r"[A-Za-z0-9]+", s.lower())
    s = "_".join(words[:6]) or "text"
    return s[:44].rstrip("_")


def load_en():
    ent, ctx, pend = {}, {}, []
    if os.path.exists(EN):
        for line in open(EN, encoding="utf-8"):
            line = line.rstrip("\n")
            if line.startswith("#"):
                pend.append(line); continue
            if not line: continue
            k, _, v = line.partition("\t")
            ent[k] = v; ctx[k] = pend; pend = []
    return ent, ctx


def save_en(ent, ctx):
    with open(EN, "w", encoding="utf-8") as f:
        for k in sorted(ent, key=lambda s: s.encode()):
            for c in ctx.get(k, []):
                f.write(c + "\n")
            f.write("%s\t%s\n" % (k, ent[k]))


def esc(t):
    return t.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def process(path, area, ent, ctx, rev, apply_, listing, ranges=()):
    area0 = area
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    toks = lex(src)
    deadset = dead_lines(src)
    # 1. enclosing call and function of every token index
    stack, depth, func, cand, in_pp = [], 0, "", "", False
    info = {}
    pp_line = -1
    for idx, t in enumerate(toks):
        if t.kind == "pp":
            pp_line = t.line; stack_save = None
        if t.kind == "p":
            if t.s if False else t.text == "(":
                prev = toks[idx - 1] if idx else None
                name = prev.text if prev and prev.kind == "id" else ""
                stack.append(name)
                if depth == 0 and name and not stack[:-1]:
                    cand = name
            elif t.text == ")":
                if stack: stack.pop()
            elif t.text == "{":
                depth += 1
                if depth == 1 and cand: func = cand
            elif t.text == "}":
                depth = max(0, depth - 1)
                if depth == 0: func = ""; stack = []
            elif t.text == ";" and depth == 0:
                stack = []
        if t.kind == "str":
            info[idx] = (list(stack), func or cand)
    # 2. groups of adjacent literals
    edits = []
    i = 0
    isgrp = lambda t: t.kind == "str" or (t.kind == "id" and t.text in MACROS)
    while i < len(toks):
        if toks[i].kind != "str":
            i += 1; continue
        # back up over macros just before the first literal
        j0 = i
        while j0 > 0 and toks[j0 - 1].kind == "id" and toks[j0 - 1].text in MACROS:
            if j0 > 1 and toks[j0 - 2].kind == "id" and toks[j0 - 2].text == "define":
                break                      # the macro's own definition
            j0 -= 1
        j = i
        while j + 1 < len(toks) and isgrp(toks[j + 1]):
            j += 1
        while toks[j].kind != "str":
            j -= 1
        grp = toks[j0:j + 1]
        calls, fn = info[i]
        i = j + 1
        if j0 < i and any(g.kind == "id" for g in grp) is False:
            pass
        # a #include / #error / #pragma / #if line
        ppt = None
        k = toks.index(grp[0]) if False else None
        parts = []
        for g in grp:
            parts.append(decode(g.text) if g.kind == "str" else MACROS[g.text])
        text = None if None in parts else "".join(parts)
        if text is None or not visible(text.strip() if text.strip() else text):
            continue
        if any(c in DENY_FN for c in calls):
            continue
        ln = grp[0].line
        # the directive this literal is on, if any
        lineno = src.count("\n", 0, grp[0].a)
        ls = src.rfind("\n", 0, grp[0].a) + 1
        if re.match(r"\s*#\s*(include|error|warning|pragma|if|ifdef|elif|line)\b", src[ls:grp[0].a]):
            continue
        if re.match(r"\s*#\s*define\s+(%s)\b" % "|".join(MACROS), src[ls:grp[0].a]):
            continue                       # the word macros themselves
        if ln in deadset:
            continue
        edits.append((grp[0].a, grp[-1].b, text, ln, fn, calls[-1] if calls else ""))
    out, last = [], 0
    new = 0
    for a, b, text, ln, fn, call in edits:
        area = next((ar for lo, hi, ar in ranges if lo <= ln <= hi), area0)
        if call in ("footer", "browser_footer", "ov_footer"):
            area = "hint"
        elif call in ("osd_toast", "toast", "ui_toast", "toast_now"):
            area = "toast"
        elif call in ("app_notice", "ui_notice", "notice"):
            area = "notice"
        k = rev.get((area, text))
        if not k:
            base = "%s.%s" % (area, slug(text)); k = base; n = 2
            while k in ent and ent[k] != text:
                k = "%s_%d" % (base, n); n += 1
            ent[k] = text; rev[(area, text)] = k; new += 1
            where = os.path.basename(path) + (", " + fn if fn else "")
            ctx[k] = ["# context: %s%s" % (call + " in " if call else "", where)]
        if listing:
            print("%s:%d  %-22s %-28s %s" % (os.path.basename(path), ln, call[:22], k[:28], text.replace("\n", "\\n")[:70]))
        out.append(src[last:a])
        out.append('tr("%s")' % k + "\n" * src.count("\n", a, b))
        last = b
    out.append(src[last:])
    if apply_:
        open(path, "w", encoding="utf-8", errors="surrogateescape").write("".join(out))
    return len(edits), new


def keys_at(spec):
    for item in spec.split(","):
        f, _, ln = item.rpartition(":")
        lines = open(f, encoding="utf-8", errors="surrogateescape").read().split("\n")
        l = lines[int(ln) - 1]
        lines[int(ln) - 1] = re.sub(r'\btr\("([^"]+)"\)', r'TR_KEY("\1")', l)
        open(f, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(lines))


def clit(t):
    o = t.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
    for k, g in {"{tri}": "\\x7f", "{o}": "\\x80", "{x}": "\\x81", "{sq}": "\\x82", "{star}": "\\x83", "{staro}": "\\x84"}.items():
        o = o.replace(k, '" "%s" "' % g if False else g)
    return '"' + o + '"'


def untr(spec):
    """Back to the plain English literal on the given lines (a table the translation must not touch)."""
    ent, ctx = load_en()
    for item in spec.split(","):
        f, _, ln = item.rpartition(":")
        lines = open(f, encoding="utf-8", errors="surrogateescape").read().split("\n")
        l = lines[int(ln) - 1]
        lines[int(ln) - 1] = re.sub(r'\b(?:TR_KEY|tr)\("([^"]+)"\)', lambda m: clit(ent[m.group(1)]), l)
        open(f, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(lines))


def untr_buttons(files):
    ent, ctx = load_en()
    bad = {k for k, v in ent.items() if buttons_only(v)}
    for f in files:
        src = open(f, encoding="utf-8", errors="surrogateescape").read()
        src = re.sub(r'\b(?:TR_KEY|tr)\("([^"]+)"\)', lambda m: clit(ent[m.group(1)]) if m.group(1) in bad else m.group(0), src)
        open(f, "w", encoding="utf-8", errors="surrogateescape").write(src)
    print("%d button-name keys back to literals" % len(bad))


def untr_dead(files):
    ent, ctx = load_en()
    n = 0
    for f in files:
        src = open(f, encoding="utf-8", errors="surrogateescape").read()
        dead = dead_lines(src)
        lines = src.split("\n")
        for ln in dead:
            l = lines[ln - 1]
            nl = re.sub(r'\b(?:TR_KEY|tr)\("([^"]+)"\)', lambda m: clit(ent[m.group(1)]) if m.group(1) in ent else m.group(0), l)
            if nl != l:
                n += 1; lines[ln - 1] = nl
        open(f, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(lines))
    print("%d dead-branch lines back to literals" % n)


def prune(files):
    """Drops the keys of en.txt no source mentions any more."""
    ent, ctx = load_en()
    used = set()
    for f in files:
        used |= set(re.findall(r'"([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+)"', open(f, encoding="utf-8", errors="surrogateescape").read()))
    gone = [k for k in ent if k not in used]
    for k in gone:
        del ent[k]; ctx.pop(k, None)
    save_en(ent, ctx)
    print("pruned %d unused keys, %d left" % (len(gone), len(ent)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--area", default="ui")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--keys-at")
    ap.add_argument("--untr", help="FILE:LINE,...  back to the English literal")
    ap.add_argument("--untr-dead", action="store_true")
    ap.add_argument("--untr-buttons", action="store_true")
    ap.add_argument("--prune", action="store_true", help="drop keys no FILE mentions")
    ap.add_argument("--ranges", default="", help="LO-HI:area,LO-HI:area  other key prefixes by line")
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    if a.untr:
        untr(a.untr); sys.exit(0)
    if a.untr_dead:
        untr_dead(a.files); sys.exit(0)
    if a.untr_buttons:
        untr_buttons(a.files); sys.exit(0)
    if a.prune:
        prune(a.files); sys.exit(0)
    if a.keys_at:
        keys_at(a.keys_at); sys.exit(0)
    ent, ctx = load_en()
    rev = {}
    for k, v in ent.items():
        rev.setdefault((k.split(".")[0], v), k)
    tot = 0
    for f in a.files:
        rg = [(int(x.split("-")[0]), int(x.split("-")[1].split(":")[0]), x.split(":")[1]) for x in a.ranges.split(",") if x]
        n, new = process(f, a.area, ent, ctx, rev, a.apply, a.list, rg)
        print("%s: %d literals, %d new keys" % (f, n, new), file=sys.stderr)
    if a.apply:
        save_en(ent, ctx)
