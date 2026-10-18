#!/usr/bin/env python3
"""clear_stale_handoff.py -- before hw_loop.py starts on a REUSED rig folder,
remove the previous campaign's handoff/RESULT.TXT and handoff/WINDOW.TXT.

    clear_stale_handoff.py --drive F: --role host [--appdir GBADHOC-GBLINK]
                           [--dry-run]

Why: hw_loop (and the pair setup tools) read a present RESULT.TXT as "this
console finished a run and is parked in its handoff window", so a file left
by the last campaign makes the loop collect old logs as a new run and write
CMD.TXT to a console that is not listening.  Run it while the console is at
the XMB with USB connected, BEFORE launching the rig app.

It touches nothing else: only those two files, only under
PSP/GAME/<appdir>/handoff/, never under PSP/GAME/GBADHOC (the owner's
install), and only on a card whose ROLE.TXT names --role.  Each removed
file is copied to --save first (default: beside this run's logs).
"""
import argparse
import hashlib
import os
import shutil
import sys
import time

FILES = ("RESULT.TXT", "WINDOW.TXT")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--drive", required=True)
    ap.add_argument("--role", required=True, choices=("host", "join"))
    ap.add_argument("--appdir", default="GBADHOC-GBLINK")
    ap.add_argument("--save", default=None,
                    help="directory to keep a copy of each removed file in")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.appdir.upper() == "GBADHOC" or not a.appdir.upper().startswith(
            "GBADHOC-"):
        print("REFUSING: %s is not a rig folder" % a.appdir)
        return 2
    root = os.path.join(a.drive + os.sep, "PSP", "GAME", a.appdir)
    try:
        role = open(os.path.join(root, "ROLE.TXT"), encoding="utf-8",
                    errors="replace").read().strip()
    except OSError:
        print("REFUSING: no %s -- not a rig card" % os.path.join(root,
                                                                  "ROLE.TXT"))
        return 2
    if not role.lower().startswith(a.role):
        print("REFUSING: ROLE.TXT on %s says %r, not %s" % (a.drive, role,
                                                            a.role))
        return 2
    print("%s %s: ROLE.TXT %r" % (a.drive, a.appdir, role))
    hd = os.path.join(root, "handoff")
    found = 0
    for n in FILES:
        p = os.path.join(hd, n)
        if not os.path.isfile(p):
            print("  %-10s absent" % n)
            continue
        found += 1
        body = open(p, "rb").read()
        print("  %-10s %s  %d B  md5 %s" % (
            n, time.strftime("%Y-%m-%d %H:%M:%S",
                             time.localtime(os.path.getmtime(p))),
            len(body), hashlib.md5(body).hexdigest()[:8]))
        for ln in body.decode("utf-8", "replace").splitlines()[:8]:
            print("      | " + ln)
        if a.dry_run:
            continue
        if a.save:
            os.makedirs(a.save, exist_ok=True)
            shutil.copy2(p, os.path.join(a.save, "%s-%s-stale-%s" % (
                a.role, a.drive.rstrip(":\\/"), n)))
        os.remove(p)
        print("      removed")
    print("%s: %d stale file(s)%s" % (a.drive, found,
                                      " (dry run: kept)" if a.dry_run and
                                      found else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
