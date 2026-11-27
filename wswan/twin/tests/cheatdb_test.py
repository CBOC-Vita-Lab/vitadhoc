#!/usr/bin/env python3
"""cheatdb_test.py -- the WonderSwan part of the bundled cheat database, run through
the real host (wswan_host_cheat_add), not a copy of its grammar.

    cheatdb_test.py TWIN ROMDIR DB

 1. every WonderSwan / Color code of the database is AAAAA:VV and is accepted
 2. its value is read back from the address it names after a few frames
 3. the codes the converter had to drop (a ROM patch, a 7-digit address) are
    refused by the host as well, which is why they are not in the database

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
rom = os.path.join(romdir, "h_sram.ws")
fails = 0


def check(ok, what):
    global fails
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok:
        fails += 1


def run(code, peek, work):
    h = os.path.join(work, "h.txt")
    r = subprocess.run([twin, rom, "--frames", "4", "--cheat", code, "--peek", peek, "--hashes", h],
                       capture_output=True, text=True)
    m = re.search(r"cheats accepted: (\d+)", r.stdout + r.stderr)
    last = open(h).read().split("\n")[-2].split() if os.path.exists(h) else []
    return int(m.group(1)) if m else -1, (int(last[5]) if len(last) > 5 else None)


games, _, names = read_db(dbpath)
ws = [g for g in games if g["sys"] in (4, 5)]
codes = [(g["name"], t, c) for g in ws for t, c in g["cheats"]]
check(len(codes) >= 1, "the database has %d WonderSwan code(s) in %d game(s)" % (len(codes), len(ws)))
check(any(k.startswith("W") for k in names), "WonderSwan games are keyed by name (W...)")
work = os.environ.get("TMPDIR", "/tmp")
import tempfile
with tempfile.TemporaryDirectory(dir=work) as w:
    for name, title, code in codes:
        m = re.fullmatch(r"([0-9A-F]{1,5}):([0-9A-F]{2})", code)
        check(bool(m), "%s / %s: %s is AAAAA:VV" % (name, title, code))
        if not m:
            continue
        acc, got = run(code, m.group(1), w)
        check(acc == 1, "%s: accepted by wswan_host_cheat_add (%d)" % (code, acc))
        check(got == int(m.group(2), 16), "%s: reads back %s (got %s)" % (code, m.group(2), got))
    # what the converter drops is also what the host refuses
    for code in ("401234:C3", "4000123:EB", "ZZZZ:00"):
        acc, _ = run(code, "0", w)
        check(acc == 0, "dropped form %s refused by the host (%d)" % (code, acc))
sys.exit(1 if fails else 0)
