#!/usr/bin/env python3
"""make_wiki_attribution.py -- notices/WIKIPEDIA-ATTRIBUTION.md's list of
articles: one line (article and history links) for every W record of
assets/gamedb.txt, so the credit is complete by construction.

  python3 make_wiki_attribution.py            (rewrites the file)
  python3 make_wiki_attribution.py --check    (exit 1 if it is out of date)
"""
import os
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, "..", "assets", "gamedb.txt")
OUT = os.path.join(HERE, "..", "..", "..", "notices", "WIKIPEDIA-ATTRIBUTION.md")
MARK = "\n- ["        # the list starts at the first bullet


def build():
    with open(OUT, encoding="utf-8") as f:
        text = f.read()
    head = text[:text.index(MARK) + 1]
    arts = set()
    with open(DB, encoding="utf-8") as f:
        for line in f:
            if line[0] == "W":
                arts.add(line[1:].split("\t", 1)[0])
    rows = []
    for a in sorted(arts):
        q = urllib.parse.quote(a, safe="")
        rows.append("- [%s](https://en.wikipedia.org/wiki/%s) | [contributors and history]"
                    "(https://en.wikipedia.org/w/index.php?title=%s&action=history)\n"
                    % (a, q, q))
    return head + "".join(rows)


if __name__ == "__main__":
    new = build()
    if "--check" in sys.argv:
        with open(OUT, encoding="utf-8") as f:
            sys.exit(0 if f.read() == new else 1)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(new)
