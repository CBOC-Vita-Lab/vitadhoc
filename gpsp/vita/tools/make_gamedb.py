#!/usr/bin/env python3
"""make_gamedb.py -- assets/gamedb.txt: the game list's names and blurbs.

Built once on a computer with internet (the app never needs it):

  1. the No-Intro catalogues of GBA, GB and GBC cartridges, from
     libretro-database (metadat/no-intro/*.dat): every cartridge's full
     name, CRC32 and (GBA) the four-letter game code in its header;
  2. Wikidata: every video game released for those three consoles, with
     its English Wikipedia article and its names (label and aliases);
  3. English Wikipedia: the first sentences of each matched article
     (TextExtracts, plain text, at most three sentences).

The output is one text file, sorted, one record a line, TAB-separated:

  C<crc32 hex>  <No-Intro name>   a cartridge by its checksum
  S<code>       <No-Intro name>   a GBA cartridge by its header's code
  Z<code>       <bytes>           ...and that cartridge's size (homebrew
                                  borrows codes: a file much smaller than
                                  the cartridge is not it)
  T<title key>  <article>         a game by its title (title_key below)
  W<article>    <blurb>           an English Wikipedia article's first lines

The app finds a cartridge's No-Intro name (by file name, header code or
checksum) to ask the libretro thumbnail server for its box art, and its
blurb by title key, then article.  A GBA game's other regions (the same
first three letters of the code: BPEE, BPEJ, BPED...) share the article
the one with an English name found.  Wikipedia text is CC BY-SA 4.0: the
app and the README credit it.

  python3 make_gamedb.py OUTDIR      (downloads into OUTDIR/cache)
"""
import json
import os
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request

UA = "VitAdhoc-gamedb/1.0 (a personal PS Vita emulator front end; " \
     "builds an offline list of game descriptions once)"
SYSTEMS = [
    ("Nintendo - Game Boy Advance", "Q188642", "A"),
    ("Nintendo - Game Boy", "Q186437", "B"),
    ("Nintendo - Game Boy Color", "Q203992", "C"),
]


def get(url, cache=None, accept=None):
    if cache and os.path.exists(cache):
        with open(cache, "rb") as f:
            return f.read()
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    if accept:
        req.add_header("Accept", accept)
    for attempt in range(8):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
            break
        except Exception as e:          # pragma: no cover (network)
            if attempt == 7:
                raise
            # Wikidata's query service may allow one query a minute
            busy = getattr(e, "code", 0) in (429, 503)
            time.sleep(65 if busy else 3 * (attempt + 1))
    if cache:
        with open(cache, "wb") as f:
            f.write(data)
    return data


def title_key(s):
    """The same in the app (gamedb.c title_key): accents off, lower case,
    'The' moved off either end, '&' = 'and', anything not a letter or
    digit a single space."""
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not unicodedata.combining(c))
    s = s.lower().replace("&", " and ")
    s = re.sub(r"[^a-z0-9]+", " ", s).strip()
    # "Legend of Zelda, The - ..." / "The Legend of Zelda: ..."
    s = re.sub(r"^the ", "", s)
    s = re.sub(r" the$", "", s)
    s = s.replace(" the ", " the ")
    return s


def base_title(nointro):
    """'Legend of Zelda, The - The Minish Cap (USA)' -> 'The Legend of
    Zelda - The Minish Cap'."""
    t = re.split(r" \(", nointro, 1)[0].strip()
    m = re.match(r"^(.*?), (The|A|An)( - .*)?$", t)
    if m:
        t = "%s %s%s" % (m.group(2), m.group(1), m.group(3) or "")
    return t


def variants(key):
    """Other keys the same game may be known by (No-Intro adds words
    Wikipedia leaves off)."""
    out = {key}
    for suffix in (" version", " edition", " advance"):
        if key.endswith(suffix):
            out.add(key[: -len(suffix)])
    out.add(re.sub(r"^(disney s|disney|tom clancy s|sid meier s) ", "", key))
    return out


def parse_dat(text):
    games = []
    for block in re.findall(r"game \((.*?)\n\)", text, re.S):
        name = re.search(r'\tname "(.*?)"', block)
        rom = re.search(r'rom \( name ".*?" size (\d+) crc ([0-9A-Fa-f]{8})', block)
        serial = re.search(r'\tserial "(.*?)"', block)
        if not name:
            continue
        games.append({"name": name.group(1),
                      "size": int(rom.group(1)) if rom else 0,
                      "crc": rom.group(2).upper() if rom else None,
                      "serial": serial.group(1) if serial else None})
    return games


def wikidata(qid, cachedir):
    """(name, article) for every game Wikidata says came out on console
    `qid` and that has an English Wikipedia article: its label, its
    aliases and the article's own title.  Through the plain API (a search
    for the statement, then the items fifty at a time): the SPARQL service
    may be rate-limited to a query a minute."""
    ids, off = [], 0
    while True:
        url = ("https://www.wikidata.org/w/api.php?action=query&format=json"
               "&list=search&srprop=&srlimit=500&srsearch=haswbstatement:P400=%s"
               "&sroffset=%d" % (qid, off))
        d = json.loads(get(url, os.path.join(cachedir, "wds_%s_%d.json" %
                                              (qid, off))))
        ids += [x["title"] for x in d["query"]["search"]]
        if "continue" not in d:
            break
        off = d["continue"]["sroffset"]
    rows = []
    for i in range(0, len(ids), 50):
        url = ("https://www.wikidata.org/w/api.php?action=wbgetentities"
               "&format=json&props=labels|aliases|sitelinks&languages=en"
               "&sitefilter=enwiki&ids=" + "|".join(ids[i:i + 50]))
        d = json.loads(get(url, os.path.join(cachedir, "wde_%s_%05d.json" %
                                              (qid, i))))
        for e in d.get("entities", {}).values():
            sl = e.get("sitelinks", {}).get("enwiki")
            if not sl:
                continue
            art = sl["title"]
            lab = e.get("labels", {}).get("en", {}).get("value")
            if lab:
                rows.append((lab, art))
            for a in e.get("aliases", {}).get("en", []):
                rows.append((a["value"], art))
            rows.append((re.sub(r" \(.*\)$", "", art), art))
    return rows


ASCII = {"\u2013": "-", "\u2014": " - ", "\u2018": "'", "\u2019": "'",
         "\u201c": '"', "\u201d": '"', "\u2026": "...", "\u00d7": "x",
         "\u00a0": " ", "\u2022": "-", "\u00b7": "-", "\u2122": "",
         "\u00ae": "", "\u00e6": "ae", "\u00c6": "AE", "\u00df": "ss",
         "\u0153": "oe", "\u00f8": "o", "\u00d8": "O", "\u2032": "'"}


def ascii_text(s):
    """The app's fonts are ASCII: accents off, typographic punctuation to
    its plain form, anything else left out."""
    s = "".join(ASCII.get(c, c) for c in s)
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not unicodedata.combining(c))
    return "".join(c for c in s if 32 <= ord(c) < 127)


def clean(text):
    text = text.split("\n")[0]          # the first paragraph
    # pronunciations and foreign titles in the first parentheses
    text = re.sub(r" ?\((?:[^()]|\([^()]*\))*?(?:Japanese|Hepburn|lit\.|"
                  r"Korean|Chinese|known as|stylized|abbreviated)[^()]*?"
                  r"(?:\([^()]*\)[^()]*)*\)", "", text)
    text = ascii_text(re.sub(r"\s+", " ", text).strip())
    # what is left of a foreign title once its letters are gone: "( !)"
    text = re.sub(r" ?\([^A-Za-z0-9()]*\)", "", text).replace(" ,", ",")
    text = re.sub(r"\s+", " ", text).strip()
    # a list or a disambiguation is not about the game
    if re.match(r"^(The following is a list|.{0,60} (may )?refers? to )", text):
        return ""
    # the home screen has three lines: whole sentences up to ~200
    # characters (a longer first sentence is cut there by the app)
    sentences = re.split(r"(?<=[.!?]) (?=[A-Z])", text)
    out = sentences[0]
    for s in sentences[1:]:
        if len(out) + 1 + len(s) > 200:
            break
        out += " " + s
    if len(out) < 40:
        return ""
    return out.replace("\t", " ")


def extracts(titles, cachedir):
    out = {}
    titles = sorted(set(titles))
    for i in range(0, len(titles), 20):
        batch = titles[i:i + 20]
        url = ("https://en.wikipedia.org/w/api.php?action=query&format=json"
               "&prop=extracts&exintro=1&explaintext=1&exsentences=3"
               "&exlimit=20&redirects=1&titles=" +
               urllib.parse.quote("|".join(batch)))
        data = json.loads(get(url, os.path.join(cachedir, "wp%05d.json" % i)))
        q = data.get("query", {})
        back = {}
        for n in q.get("normalized", []) + q.get("redirects", []):
            back[n["to"]] = back.get(n["from"], n["from"])
        for p in q.get("pages", {}).values():
            t = p.get("title")
            ex = p.get("extract", "")
            if t and ex:
                out[back.get(t, t)] = clean(ex)
                out[t] = clean(ex)
        time.sleep(0.2)
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    cache = os.path.join(outdir, "cache")
    os.makedirs(cache, exist_ok=True)
    recs = {}
    keys_wanted = {}            # title key -> article
    names = []                  # (key, base title) per cartridge
    for dat, qid, tag in SYSTEMS:
        text = get("https://raw.githubusercontent.com/libretro/libretro-database/"
                   "master/metadat/no-intro/" + urllib.parse.quote(dat) + ".dat",
                   os.path.join(cache, dat + ".dat")).decode("utf-8", "replace")
        games = parse_dat(text)
        wd = wikidata(qid, cache)
        index = {}
        for label, art in wd:
            index.setdefault(title_key(label), art)
        hit = 0
        family = {}             # GBA: code[:3] -> article
        for g in games:
            if g["crc"]:
                recs["C" + g["crc"]] = g["name"]
            code = g["serial"] if g["serial"] and tag == "A" and \
                re.match(r"^[A-Z0-9]{4}$", g["serial"]) else None
            if code:
                recs.setdefault("S" + code, g["name"])
                recs.setdefault("Z" + code, str(g["size"]))
            k = title_key(base_title(g["name"]))
            g["key"], g["code"] = k, code
            if k in keys_wanted:
                art = keys_wanted[k]
            else:
                art = next((index[v] for v in variants(k) if v in index), None)
                if art:
                    keys_wanted[k] = art
                    hit += 1
            if art and code:
                family.setdefault(code[:3], art)
        for g in games:         # the other regions of a game found once
            if g["code"] and g["key"] not in keys_wanted and \
                    g["code"][:3] in family:
                keys_wanted[g["key"]] = family[g["code"][:3]]
                hit += 1
        print("%s: %d cartridges, %d Wikidata names, %d titles matched" %
              (dat, len(games), len(wd), hit), file=sys.stderr)
    ex = extracts(keys_wanted.values(), cache)
    n = 0
    for k, art in keys_wanted.items():
        if ex.get(art):
            recs["T" + k] = art
            recs["W" + art] = ex[art]
            n += 1
    print("%d titles with a blurb, %d articles" %
          (n, sum(1 for k in recs if k[0] == "W")), file=sys.stderr)
    with open(os.path.join(outdir, "gamedb.txt"), "w", encoding="utf-8") as f:
        for k in sorted(recs):
            f.write("%s\t%s\n" % (k, recs[k]))


if __name__ == "__main__":
    main()
