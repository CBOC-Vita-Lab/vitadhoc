#!/usr/bin/env python3
"""make_cheatdb.py -- pack libretro-database's cheat files into one file.

    make_cheatdb.py --db /path/to/libretro-database --out ../assets/cheats.db

Input (a sparse checkout of https://github.com/libretro/libretro-database):
    cht/Nintendo - Game Boy Advance/*.cht
    cht/Nintendo - Game Boy/*.cht
    cht/Nintendo - Game Boy Color/*.cht
    cht/Nintendo - Nintendo Entertainment System/*.cht      (1.4)
    cht/Bandai - WonderSwan/*.cht, cht/Bandai - WonderSwan Color/*.cht (1.4)
    metadat/no-intro/<the same systems>.dat       (name <-> CRC-32 <-> serial)

Output: cheats.db, read on the console by vita/cheatdb.c (the format is
described there and below), and a report beside it (cheats.db.txt).

What it does to the data:
  * several .cht files of one game ("... (USA, Europe) (GameShark).cht",
    "... (Game Genie).cht", the plain one) become ONE list; identical codes
    are kept once; equal names with different codes get the device's name or
    a number appended;
  * entries without a code, "N/A" and the like are dropped;
  * NES and WonderSwan codes (1.4) are not run through cheatcheck (the two
    consoles' engines live in nes/ and wswan/); convert_nes / convert_ws below
    rewrite each entry into exactly the forms those engines take
    (nes_host_cheat_add, wswan_host_cheat_add) and drop what they cannot run,
    counting the reason (see the report).  A NES game's CRC is the No-Intro
    headerless one (the app hashes without the iNES header and trainer), a
    WonderSwan game's the whole file's;
  * with --check (a binary built from vita/tools/cheatcheck.c, built
    automatically when a C compiler is on the path) every code is run
    through the console's own decoder: codes it can never run (not a code at
    all; Xploder GB) are dropped, the rest is counted as usable / needs a
    value / encrypted without its master code;
  * each game is tied to the No-Intro entries of the same title and region,
    which gives the CRC-32 and, for the GBA, the 4-character game code.

File format (all integers little-endian):

  header, 64 bytes
     0  "GBADCHT1"
     8  u32 version (1)        12  u32 games
    16  u32 crc entries        20  u32 offset of the CRC index
    24  u32 serial entries     28  u32 offset of the serial index
    32  u32 name entries       36  u32 offset of the name index
    40  u32 offset of the key strings      44  u32 their size
    48  u32 offset of the game table       52  u32 offset of the first block
    56  u32 file size          60  u32 cheats in all
  game table, 16 bytes a game
        u32 block offset, u32 block size, u16 cheats, u8 system (0 GBA,
        1 GB, 2 GBC, 3 NES, 4 WonderSwan, 5 WonderSwan Color), u8 0,
        u32 CRC-32 of the block
  CRC index, 8 bytes an entry, sorted by CRC:      u32 crc, u32 game
  serial index, 8 bytes, sorted by the 4 bytes:    char[4], u32 game
        (bit 31 of game: every release with this code has this list, so the
        console need not hash the ROM to tell revisions apart)
  name index, 8 bytes, sorted by key string:       u32 key offset, u32 game
        keys (NUL-terminated, in the key strings):
          <s>f<title>(<regions>)[r<rev>]    a full name
          <s>b<title>                       the title alone
        <s> is A (GBA), G (GB and GBC), N (NES) or W (WonderSwan and Color);
        <title> the lower-case letters and
        digits of the name before its first parenthesis; <regions> the
        region names, lower case, sorted, comma separated.
  blocks: the game's name, NUL, then for every cheat  name NUL code NUL
        (UTF-8; a code is libretro's: lines joined by '+').
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from collections import Counter, defaultdict

SYSTEMS = (  # directory, system id, key letter, extensions
    ("Nintendo - Game Boy Advance", 0, "A"),
    ("Nintendo - Game Boy", 1, "G"),
    ("Nintendo - Game Boy Color", 2, "G"),
    ("Nintendo - Nintendo Entertainment System", 3, "N"),
    ("Bandai - WonderSwan", 4, "W"),
    ("Bandai - WonderSwan Color", 5, "W"),
)
SYSTEM_NAMES = {0: "GBA", 1: "GB", 2: "GBC", 3: "NES", 4: "WS", 5: "WSC"}
# The NES catalogue lists every cartridge twice: a headered .nes (its CRC
# includes the 16-byte iNES header) and a headerless one.  The app hashes
# without the header (gamedb_nes_crc_cancel), so only those CRCs are kept --
# the same rule as make_gamedb.py's CRC_EXT.
DAT_SKIP_EXT = {3: (".nes",)}

CHEATS_MAX = 1000          # per game (the console's list holds 1024)
NAME_CAP = 63              # bytes, CHEATS_NAME_CAP - 1
CODE_CAP = 4095            # bytes, CHEATS_CODE_CAP - 1

# ---- names ---------------------------------------------------------------

REGIONS = {
    "usa", "europe", "japan", "world", "germany", "france", "spain", "italy",
    "australia", "korea", "china", "netherlands", "sweden", "brazil", "asia",
    "canada", "taiwan", "hong kong", "uk", "denmark", "norway", "finland",
    "russia", "portugal", "greece", "unknown", "latin america", "scandinavia",
}
GOODTOOLS = {  # (U), (E), (G) ... of the older naming
    "u": ["usa"], "e": ["europe"], "j": ["japan"], "g": ["germany"],
    "f": ["france"], "s": ["spain"], "i": ["italy"], "w": ["world"],
    "a": ["australia"], "k": ["korea"], "c": ["china"],
    "ue": ["usa", "europe"], "eu": ["usa", "europe"],
    "ju": ["japan", "usa"], "uj": ["japan", "usa"],
    "jue": ["japan", "usa", "europe"], "je": ["japan", "europe"],
}
DEVICES = (  # tag (lower case, spaces removed) -> (label, merge order)
    ("codebreaker", ("CodeBreaker", 1)),
    ("gameshark", ("GameShark", 2)),
    ("actionreplay", ("Action Replay", 3)),
    ("gamegenie", ("Game Genie", 4)),
    ("xploder", ("Xploder", 6)),
    ("rumbles", ("Rumbles", 5)),           # NES lists
)
SKIP_DAT = ("beta", "proto", "demo", "sample", "pirate", "kiosk")


def norm_title(s):
    """Lower-case ASCII letters and digits.  cheatdb.c does the same."""
    return "".join(c.lower() for c in s if c.isascii() and c.isalnum())


def parse_name(name):
    """-> (title, regions (sorted tuple), rev, device label, order, diff)"""
    m = re.search(r"[(\[]", name)
    title = (name[:m.start()] if m else name).strip()
    regions, rev, device, order, diff = set(), "", "", 0, False
    for tag in re.findall(r"\(([^)]*)\)", name):
        low = tag.strip().lower()
        flat = low.replace(" ", "")
        hit = False
        for key, (label, o) in DEVICES:
            if flat == key:
                device, order, hit = label, o, True
        if hit:
            continue
        if low == "diff":
            diff = True
            continue
        m2 = re.fullmatch(r"rev ([0-9a-z]+)", low)
        if m2:
            rev = m2.group(1)
            continue
        m2 = re.fullmatch(r"v1\.([0-9])", low)
        if m2:
            rev = m2.group(1) if m2.group(1) != "0" else ""
            continue
        if low in GOODTOOLS:
            regions.update(GOODTOOLS[low])
            continue
        parts = [p.strip() for p in low.split(",")]
        if parts and all(p in REGIONS for p in parts):
            regions.update(parts)
    # "... (USA) Game Shark.cht", "... (Code Breaker)(USA).cht"
    tail = re.sub(r"\([^)]*\)|\[[^\]]*\]", "", name[len(title):]).lower()
    flat = tail.replace(" ", "")
    for key, (label, o) in DEVICES:
        if key in flat and not device:
            device, order = label, o
    regions.discard("unknown")
    return title, tuple(sorted(regions)), rev, device, order, diff


def full_key(letter, title, regions, rev):
    return "%sf%s(%s)%s" % (letter, norm_title(title), ",".join(regions),
                            "r" + rev if rev else "")


def base_key(letter, title):
    return "%sb%s" % (letter, norm_title(title))


# ---- .cht ----------------------------------------------------------------

def read_cht(path):
    """libretro .cht -> [(desc, code)].  Tolerant: BOM, CRLF, quotes or
    none, missing keys, a value that runs over several lines."""
    raw = open(path, "rb").read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    text = raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
    items = {}
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        m = re.match(r"\s*cheat(\d+)_(desc|code)\s*=\s*(.*)$", line)
        if not m:
            continue
        idx, what, val = int(m.group(1)), m.group(2), m.group(3).strip()
        if val.startswith('"'):
            val = val[1:]
            while '"' not in val and i < len(lines):
                val += "\n" + lines[i]
                i += 1
            val = val[:val.rfind('"')] if '"' in val else val
        items.setdefault(idx, {})[what] = val
    out = []
    for idx in sorted(items):
        desc = items[idx].get("desc", "")
        code = items[idx].get("code", "")
        out.append((desc, code))
    return out


def clean_desc(s):
    s = "".join(c if c >= " " and c != "\x7f" else " " for c in s)
    s = s.replace("�", "?").replace('"', "'")
    return re.sub(r"\s+", " ", s).strip()


def clean_code(s):
    s = re.sub(r"[\s]*[\n\r]+[\s]*", "+", s.strip())
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r"\s*\+\s*", "+", s).strip("+ ")
    return s


def canon_code(s):
    return re.sub(r"[^0-9A-Z?*:\-$]+", "+", s.upper()).strip("+")


def clip_utf8(s, cap):
    b = s.encode("utf-8")
    if len(b) <= cap:
        return s
    return b[:cap].decode("utf-8", "ignore").rstrip()


# ---- No-Intro dat ----------------------------------------------------------

def read_dat(path, skip_ext=()):
    """-> [(name, [crc...], serial)].  skip_ext: file extensions whose roms'
    CRCs are left out (the NES's headered .nes)."""
    games = []
    text = open(path, encoding="utf-8", errors="replace").read()
    for block in re.split(r"\ngame \(", text)[1:]:
        m = re.search(r'\n\tname "([^"]*)"', block)
        if not m:
            continue
        if skip_ext:
            crcs = [int(c, 16) for n, c in re.findall(
                r'\brom \( name "([^"]*)".*?\bcrc ([0-9A-Fa-f]{8})\b', block)
                if not n.lower().endswith(skip_ext)]
        else:
            crcs = [int(c, 16) for c in re.findall(
                r"\bcrc ([0-9A-Fa-f]{8})\b", block)]
        s = re.search(r'\n\tserial "([^"]*)"', block)
        games.append((m.group(1), crcs, s.group(1) if s else ""))
    return games


# ---- NES and WonderSwan codes ----------------------------------------------
# What the two consoles' engines take (nes/frontend/host.c nes_host_cheat_add,
# wswan/frontend/host.cpp wswan_host_cheat_add; vita/cheats_vita.c checks the
# same shapes before a code is switched on): codes joined by + , ; . _ or a
# space.

SEP = re.compile(r"[+,;._ ]+")
GG_LETTERS = set("APZLGITYEOXUKSVN")


def convert_nes(code):
    """A libretro NES code -> (the code in the engine's forms, None), or
    (None, why it cannot run).  An entry is all or nothing: a code of several
    parts with one part the engine would refuse is not shipped half."""
    out = []
    par = 0
    for t in SEP.split(code):
        if not t:
            continue
        u = t.upper()
        if len(u) in (6, 8) and set(u) <= GG_LETTERS:
            out.append(u)                     # Game Genie
        elif re.fullmatch(r"[0-9A-F]{4}:[0-9A-F]{2}", u):
            out.append(u)                     # RAM poke AAAA:VV
        elif re.fullmatch(r"[0-9A-F]{4}\?[0-9A-F]{2}:[0-9A-F]{2}", u):
            out.append(u)                     # AAAA?CC:VV (compare value)
        elif re.fullmatch(r"[0-9A-F]{8}", u):
            # Pro Action Rocky: the engine reads byte 1-2 as the address and
            # byte 3 as the value (FCEUI_DecodePAR) -- the same as this poke,
            # in the one form the app's own code check accepts
            out.append("%s:%s" % (u[2:6], u[6:8]))
            par += 1
        elif "?" in u or "*" in u:
            return None, "placeholder"
        else:
            return None, "not a code"
    if not out:
        return None, "not a code"
    return "+".join(out), ("par" if par else None)


def convert_ws(code):
    """A libretro WonderSwan code -> "AAAAA:VV+..." (RAM map address, at most
    five digits), or (None, why).  The libretro files' 7-digit codes
    ("4147262:EB") are ROM patches ('4' and a ROM offset), which the engine
    does not have: only pokes inside the RetroAchievements map (system RAM
    0-0xFFFF, cartridge save RAM to 0x8FFFF) are kept."""
    out = []
    for t in SEP.split(code):
        if not t:
            continue
        m = re.fullmatch(r"([0-9A-Fa-f]+):([0-9A-Fa-f]{2})", t)
        if not m:
            return None, ("placeholder" if "?" in t or "*" in t
                          else "not a code")
        a = int(m.group(1), 16)
        if a > 0x8FFFF:
            return None, "outside the RAM map"
        out.append("%05X:%s" % (a, m.group(2).upper()))
    if not out:
        return None, "not a code"
    return "+".join(out), None


def convert_other(sysid, code):
    return convert_nes(code) if sysid == 3 else convert_ws(code)


# ---- the decoder's verdict ---------------------------------------------------

class Checker:
    """Feeds groups of codes to vita/tools/cheatcheck and reads back, per
    code, (rc, kind, flags) as cheat_engine.c reports them."""

    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)

    def check(self, system, codes):
        w = self.p.stdin
        w.write("G %d\n" % system)
        for c in codes:
            w.write("C %s\n" % c.replace("\n", "+"))
        w.write("E\n")
        w.flush()
        out = []
        for _ in codes:
            rc, kind, flags = self.p.stdout.readline().split()
            out.append((int(rc), int(kind), int(flags)))
        return out

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def build_checker(here):
    cc = shutil.which("cc") or shutil.which("gcc")
    src = os.path.join(here, "cheatcheck.c")
    engine = os.path.join(here, "..", "..", "cheat_engine.c")
    if not cc or not os.path.exists(src):
        return None
    exe = os.path.join(tempfile.mkdtemp(prefix="cheatcheck"), "cheatcheck")
    subprocess.check_call([cc, "-O1", "-I", os.path.join(here, "..", ".."),
                           "-o", exe, src, engine])
    return exe


# ---- main ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True, help="libretro-database checkout")
    ap.add_argument("--out", required=True)
    ap.add_argument("--check", help="cheatcheck binary (default: build it)")
    ap.add_argument("--no-check", action="store_true")
    args = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))

    checker = None
    if not args.no_check:
        exe = args.check or build_checker(here)
        if exe:
            checker = Checker(exe)
        else:
            print("no C compiler: codes are not checked", file=sys.stderr)

    stats = Counter()
    # ---- 1. the .cht files, grouped by game --------------------------------
    groups = {}      # key -> dict
    for sysdir, sysid, letter in SYSTEMS:
        d = os.path.join(args.db, "cht", sysdir)
        if not os.path.isdir(d):
            continue            # a tree without this console's lists
        for fn in sorted(os.listdir(d)):
            path = os.path.join(d, fn)
            if not fn.lower().endswith(".cht") or not os.path.isfile(path):
                continue
            stats["files %d" % sysid] += 1
            title, regions, rev, device, order, diff = parse_name(fn[:-4])
            entries = read_cht(path)
            stats["entries %d" % sysid] += len(entries)
            key = (letter, norm_title(title), regions, rev)
            g = groups.setdefault(key, dict(
                letter=letter, sysid=sysid, title=title, regions=regions,
                rev=rev, files=[], names=[]))
            g["sysid"] = max(g["sysid"], sysid)
            clean = re.sub(r"\s*\((code ?breaker|game ?shark|game genie|"
                           r"xploder|action replay|diff)\)", "", fn[:-4],
                           flags=re.I).strip()
            g["names"].append((order + (10 if diff else 0), clean))
            g["files"].append((order + (10 if diff else 0), fn, device,
                               [(d_, c_, (sysid, fn, n_))
                                for n_, (d_, c_) in enumerate(entries)]))

    # ---- 2. which lists a cartridge gets ------------------------------------
    # One .cht "group" is one (title, regions, revision).  A cartridge gets
    # every group of its title whose regions meet its own -- the plain
    # "(USA, Australia)" file AND the "(USA, Europe) (GameShark)" one -- and
    # of several revisions' groups only its own when there is one.
    by_title = defaultdict(list)
    for key, g in groups.items():
        by_title[(g["letter"], norm_title(g["title"]))].append(g)
        g["id"] = len(by_title)  # placeholder, replaced below
    for i, g in enumerate(groups.values()):
        g["id"] = i

    def region_fit(gr, dr):
        gr, dr = set(gr), set(dr)
        if gr == dr:
            return 2
        if not gr or not dr or gr & dr or "world" in gr | dr:
            return 1
        return 0

    def lists_for(letter, title, regions, rev):
        cands = [(region_fit(g["regions"], regions), g)
                 for g in by_title.get((letter, norm_title(title)), ())]
        cands = [(f, g) for f, g in cands if f]
        if any(g["rev"] == rev for f, g in cands):
            cands = [(f, g) for f, g in cands if g["rev"] == rev]
        cands.sort(key=lambda c: (-c[0], c[1]["regions"], c[1]["rev"]))
        return tuple(g["id"] for f, g in cands)

    # targets: every No-Intro release, and every group under its own name
    targets = []     # (letter, title, regions, rev, name, crcs, serial, is_dat)
    dat_total = 0
    for sysdir, sysid, letter in SYSTEMS:
        path = os.path.join(args.db, "metadat", "no-intro", sysdir + ".dat")
        if not os.path.exists(path):
            print("missing", path, file=sys.stderr)
            continue
        for name, crcs, serial in read_dat(path, DAT_SKIP_EXT.get(sysid, ())):
            dat_total += 1
            low = name.lower()
            if any("(%s" % s in low for s in SKIP_DAT):
                continue
            title, regions, rev, _, _, _ = parse_name(name)
            targets.append((letter, title, regions, rev, name, crcs, serial, 1))
    for g in groups.values():
        targets.append((g["letter"], g["title"], g["regions"], g["rev"],
                        sorted(g["names"])[0][1], [], "", 0))

    by_id = {g["id"]: g for g in groups.values()}
    outcome = {}     # (system, file, entry) -> what became of it
    RANK = {"duplicate": 0, "empty": 1, "too long": 1, "not a code": 2,
            "xploder": 2, "placeholder": 2, "outside the RAM map": 2, "over the limit": 3,
            "unsupported type": 4,
            "encrypted, no master code": 4, "needs a value": 4, "usable": 5,
            "unchecked": 5}
    games = {}       # tuple of group ids -> dict
    for t in targets:
        ids = lists_for(*t[:4])
        if not ids:
            continue
        gm = games.setdefault(ids, dict(ids=ids, targets=[]))
        gm["targets"].append(t)

    # ---- 3. merge each game's files, check, name -----------------------------
    for ids, gm in games.items():
        files = []
        for rank, gid in enumerate(ids):
            g = by_id[gid]
            for order, fn, device, entries in g["files"]:
                files.append((order, rank, fn, device, entries))
        files.sort(key=lambda f: f[:3])
        gm["sysid"] = max(by_id[i]["sysid"] for i in ids)
        gm["letter"] = by_id[ids[0]]["letter"]
        seen, used, cheats = set(), {}, []

        def note(src, what):
            # one verdict per source entry: being kept anywhere wins
            if RANK[what] > RANK[outcome.get(src, "duplicate")] or src not in outcome:
                outcome[src] = what

        for order, rank, fn, device, entries in files:
            for desc, code, src in entries:
                code = clean_code(code)
                desc = clean_desc(desc)
                if not code or canon_code(code) in ("", "N/A", "NA"):
                    note(src, "empty")
                    continue
                if len(code.encode()) > CODE_CAP:
                    note(src, "too long")
                    continue
                if src[0] >= 3:
                    code, why = convert_other(src[0], code)
                    if code is None:
                        note(src, why)
                        continue
                    if why == "par":
                        stats["PAR codes rewritten as pokes"] += 1
                c = canon_code(code)
                if c in seen:
                    note(src, "duplicate")
                    continue
                seen.add(c)
                cheats.append([desc, code, device, src])
        if gm["sysid"] >= 3:
            cheats = [c + ["usable"] for c in cheats]   # converted above
        elif checker and cheats:
            verdicts = checker.check(gm["sysid"], [c[1] for c in cheats])
            kept = []
            for ch, (rc, kind, flags) in zip(cheats, verdicts):
                xploder = "$" in ch[1] or (ch[2] == "Xploder" and rc == -2)
                if rc == 0:
                    reason = "usable"
                elif rc == -4:
                    reason = "needs a value"
                elif rc == -5:
                    reason = "encrypted, no master code"
                elif xploder:
                    reason = "xploder"
                elif rc == -2:
                    reason = "unsupported type"
                else:
                    reason = "not a code"
                # What can never run on this console does not ship; what the
                # player can make run (fill in a value, add the master code)
                # does, greyed.
                if reason in ("xploder", "not a code"):
                    note(ch[3], reason)
                    continue
                kept.append(ch + [reason])
            cheats = kept
        else:
            cheats = [c + ["unchecked"] for c in cheats]
        for ch in cheats[CHEATS_MAX:]:
            note(ch[3], "over the limit")
        cheats = cheats[:CHEATS_MAX]
        for ch in cheats:
            note(ch[3], ch[4])
        # names: never empty, never twice
        n = 0
        for ch in cheats:
            n += 1
            name = clip_utf8(ch[0], NAME_CAP) or "Cheat %d" % n
            if name.lower() in used:
                base = name
                cand = "%s (%s)" % (base, ch[2]) if ch[2] else ""
                k = 2
                while (not cand or cand.lower() in used or
                       len(cand.encode()) > NAME_CAP):
                    suffix = " (%d)" % k
                    cand = clip_utf8(base, NAME_CAP - len(suffix)) + suffix
                    k += 1
                name = cand
            used[name.lower()] = 1
            ch[0] = name
        gm["cheats"] = cheats
        # the name shown: a No-Intro release's when the game has one
        first = by_id[ids[0]]
        dats = sorted((t for t in gm["targets"] if t[7]),
                      key=lambda t: (-region_fit(first["regions"], t[2]),
                                     t[3] != first["rev"], len(t[4]), t[4]))
        own = sorted(t[4] for t in gm["targets"] if not t[7])
        gm["name"] = clip_utf8(dats[0][4] if dats else own[0], 120)

    live = [gm for gm in games.values() if gm["cheats"]]
    stats["games without a usable entry"] = len(games) - len(live)
    # the GBA, then GB and GBC, then (1.4) the NES and WonderSwan: the games of
    # the first three keep the numbers they had without the other two
    live.sort(key=lambda g: (g["sysid"] > 2, g["sysid"] != 0,
                             g["name"].lower(), g["ids"]))
    for i, g in enumerate(live):
        g["index"] = i
    crc_index, serial_owner, serial_all = {}, {}, defaultdict(set)
    crc_letter = {}
    name_index, base_rank = {}, {}
    for g in live:
        for letter, title, regions, rev, name, crcs, serial, is_dat in g["targets"]:
            for crc in crcs:
                # one CRC, one game: a CRC two consoles share is left to its
                # first console (the name lookups still find the other)
                if crc_letter.setdefault(crc, letter) != letter:
                    stats["CRC shared by two consoles"] += 1
                    continue
                crc_index.setdefault(crc, g["index"])
            if letter == "A" and re.fullmatch(r"[A-Z0-9]{4}", serial or ""):
                serial_all[serial].add(g["index"])
                # the first release's list for a code several share
                if serial not in serial_owner or (rev == "" and is_dat):
                    serial_owner[serial] = g["index"]
            name_index.setdefault(full_key(letter, title, regions, rev),
                                  g["index"])
            # the title alone: the USA / World release, the first revision,
            # then the longest list
            r = set(regions)
            rank = (("usa" in r or "world" in r or not r), rev == "",
                    len(g["cheats"]))
            bk = base_key(letter, title)
            if bk not in base_rank or rank > base_rank[bk]:
                base_rank[bk] = rank
                name_index[bk] = g["index"]

    # ---- 4. write -----------------------------------------------------------
    blocks, table = [], []
    for g in live:
        b = bytearray(g["name"].encode("utf-8") + b"\0")
        for name, code, device, src, reason in g["cheats"]:
            b += name.encode("utf-8") + b"\0" + code.encode("utf-8") + b"\0"
        blocks.append(bytes(b))
        stats["games %d" % g["sysid"]] += 1
        stats["cheats %d" % g["sysid"]] += len(g["cheats"])
        stats["dat %d" % g["sysid"]] += any(t[7] for t in g["targets"])
        for name, code, device, src, reason in g["cheats"]:
            stats["%s %d" % (reason, g["sysid"])] += 1

    keys = sorted(name_index, key=lambda k: k.encode())
    keyblob, keyoff = bytearray(), []
    for k in keys:
        keyoff.append(len(keyblob))
        keyblob += k.encode() + b"\0"

    HEADER = 64
    off_games = HEADER
    off_crc = off_games + 16 * len(live)
    off_serial = off_crc + 8 * len(crc_index)
    off_name = off_serial + 8 * len(serial_owner)
    off_keys = off_name + 8 * len(keys)
    off_blocks = (off_keys + len(keyblob) + 3) & ~3
    pos = off_blocks
    for g, b in zip(live, blocks):
        table.append(struct.pack("<IIHBBI", pos, len(b), len(g["cheats"]),
                                 g["sysid"], 0, zlib.crc32(b)))
        pos += len(b)
    total = sum(len(g["cheats"]) for g in live)
    out = bytearray()
    out += struct.pack("<8sIIIIIIIIIIIIII", b"GBADCHT1", 1, len(live),
                       len(crc_index), off_crc, len(serial_owner), off_serial,
                       len(keys), off_name, off_keys, len(keyblob), off_games,
                       off_blocks, pos, total)
    assert len(out) == HEADER
    out += b"".join(table)
    for crc in sorted(crc_index):
        out += struct.pack("<II", crc, crc_index[crc])
    for serial in sorted(serial_owner):
        unique = len(serial_all[serial]) == 1
        out += serial.encode() + struct.pack(
            "<I", serial_owner[serial] | (0x80000000 if unique else 0))
    for k, o in zip(keys, keyoff):
        out += struct.pack("<II", o, name_index[k])
    out += keyblob
    out += b"\0" * (off_blocks - len(out))
    out += b"".join(blocks)
    assert len(out) == pos
    with open(args.out, "wb") as f:
        f.write(out)

    # ---- 5. report --------------------------------------------------------
    names = SYSTEM_NAMES
    rep = []
    rep.append("cheats.db: %d bytes, %d games, %d cheats" % (pos, len(live), total))
    rep.append("index: %d CRC-32, %d GBA game codes, %d names; No-Intro "
               "entries read: %d" % (len(crc_index), len(serial_owner),
                                     len(keys), dat_total))
    rep.append("games tied to at least one No-Intro release: %d of %d; GBA "
               "game codes that need the CRC to tell revisions apart: %d" %
               (sum(stats["dat %d" % s] for s in range(6)), len(live),
                sum(1 for s in serial_all.values() if len(s) > 1)))
    rep.append("CRC-32 shared by games of two consoles (left to the first): %d" %
               stats["CRC shared by two consoles"])
    rep.append("a game here is one distinct list: the .cht files of a title "
               "that fit a release's")
    rep.append("region and revision, merged (so a file can serve several "
               "games, and the per-system")
    rep.append("cheat counts below count such a file's entries once per "
               "game).")
    rep.append("largest list: %d cheats (%s)" % max(
        (len(g["cheats"]), g["name"]) for g in live))
    rep.append("largest block: %d bytes" % max(len(b) for b in blocks))
    rep.append("")
    rep.append("%-5s %7s %7s %8s %8s %8s %8s %8s" % (
        "", ".cht", "entries", "games", "cheats", "usable", "value?", "encr."))
    sids = [sid for sid in range(6) if stats["files %d" % sid]]
    for sid in sids:
        rep.append("%-5s %7d %7d %8d %8d %8d %8d %8d" % (
            names[sid], stats["files %d" % sid], stats["entries %d" % sid],
            stats["games %d" % sid], stats["cheats %d" % sid],
            stats["usable %d" % sid], stats["needs a value %d" % sid],
            stats["encrypted, no master code %d" % sid]))
    rep.append("")
    ncrc = Counter(crc_letter[c] for c in crc_index)
    for sid in sids:
        rep.append("%-5s games tied to a No-Intro release (CRC-32 lookup): "
                   "%d of %d" % (names[sid], stats["dat %d" % sid],
                                 stats["games %d" % sid]))
    rep.append("CRC-32 index entries by console: GBA %d, GB+GBC %d, NES %d, "
               "WonderSwan+Color %d" % (ncrc["A"], ncrc["G"], ncrc["N"],
                                        ncrc["W"]))
    rep.append("")
    oc = Counter((src[0], what) for src, what in outcome.items())
    rep.append("every entry of the .cht files, once (it may be in several "
               "games' lists):")
    cols = ("usable", "needs a value", "encrypted, no master code",
            "unsupported type", "xploder", "not a code", "empty",
            "duplicate", "over the limit", "too long", "placeholder",
            "outside the RAM map")
    heads = ("usable", "value?", "encr.", "type", "xploder", "garbage",
             "empty", "dup.", ">1000", "long", "no-val", "no-RAM")
    rep.append("%-5s %7s  %s" % ("", "entries", "  ".join(heads)))
    for sid in sids:
        rep.append("%-5s %7d  %s" % (
            names[sid], sum(v for (s_, w), v in oc.items() if s_ == sid),
            "  ".join("%*d" % (len(h), oc[(sid, c)])
                      for c, h in zip(cols, heads))))
    rep.append("")
    rep.append("usable: the console's decoder runs it.  value?: the code has"
               " ?? / XX where the")
    rep.append("player chooses a value (shipped, greyed).  encr.: CodeBreaker "
               "lines encrypted with a")
    rep.append("seed code the database does not have (shipped, greyed).  "
               "type: a code type the")
    rep.append("decoder does not run (shipped, greyed).  Not shipped: "
               "xploder (Xploder GB: its")
    rep.append("byte substitution is not known here), garbage (not a code: "
               "typos, \"N/A\", a form the")
    rep.append("console's engine does not take), empty (no code "
               "at all), dup. (the same code again in the same list), >1000 "
               "(two German Pokemon")
    rep.append("files have 2575 and 1194 entries; a list holds 1000).  NES "
               "and WonderSwan only: no-val")
    rep.append("(a ?? or * where the player would choose a value: not "
               "shipped), no-RAM (a WonderSwan")
    rep.append("poke outside the RetroAchievements map -- the libretro files' "
               "7-digit codes are ROM")
    rep.append("patches, which the engine does not have: not shipped).  An "
               "entry of several parts is")
    rep.append("shipped whole or not at all.  PAR codes rewritten as AAAA:VV "
               "pokes (NES): %d (counted per game)." %
               stats["PAR codes rewritten as pokes"])
    rep.append("")
    rep.append("titles whose every entry was left out (no list shipped): %d" %
               stats["games without a usable entry"])
    text = "\n".join(rep) + "\n"
    with open(args.out + ".txt", "w") as f:
        f.write(text)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
