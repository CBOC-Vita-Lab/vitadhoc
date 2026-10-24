#!/usr/bin/env python3
"""stage_first_arm.py -- put a rig's FIRST arm onto a card whose console is
at the XMB in USB mode (not parked in the rig app), so the owner's launch of
the rig app runs it, and hw_loop takes over from its first handoff.

    stage_first_arm.py --rig RIG --arm P --drive F: --role host [--dry-run]
        [--appdir GBADHOC-GBLINK] [--save DIR]

Steps, all inside PSP/GAME/<appdir> only (never PSP/GAME/GBADHOC):
  1. refuse unless ROLE.TXT names --role;
  2. stale handoff files from the last campaign -- RESULT.TXT, WINDOW.TXT
     and CMD.TXT -- are copied to --save and removed (a stale RESULT.TXT
     reads as "parked", a stale CMD.TXT is obeyed at the first window);
  3. the arm's stage directory is mirrored with hw_loop's own staging
     (host-/join- prefixes, subdirectories) and the ini stamped with
     run_id/arm as hw_loop does; the golden save/state restored into roms/;
  4. every staged file is read back and md5-compared.
The first cycle hw_loop collects is logged "x" (it did not stage it).
"""
import argparse
import hashlib
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "rig"))
import hw_loop  # noqa: E402

STALE = ("RESULT.TXT", "WINDOW.TXT", "CMD.TXT")


def md5(p):
    h = hashlib.md5()
    with open(p, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rig", required=True)
    ap.add_argument("--arm", required=True)
    ap.add_argument("--drive", required=True)
    ap.add_argument("--role", required=True, choices=("host", "join"))
    ap.add_argument("--appdir", default="GBADHOC-GBLINK")
    ap.add_argument("--save", default=None)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.appdir.upper() == "GBADHOC" or not a.appdir.upper().startswith(
            "GBADHOC-"):
        print("REFUSING: %s is not a rig folder" % a.appdir)
        return 2
    hw_loop.APPDIR = a.appdir
    root = hw_loop.card_root(a.drive)
    ok, why = hw_loop.check_role_file(a.drive, a.role)
    if not ok:
        print("REFUSING: %s: %s" % (a.drive, why))
        return 2
    st = os.path.join(a.rig, "stage-" + a.arm)
    if not os.path.isdir(st):
        print("REFUSING: no %s" % st)
        return 2
    print("%s %s ROLE.TXT %r: arm %s from %s%s" % (
        a.drive, a.appdir, why, a.arm, st, " (dry run)" if a.dry_run else ""))
    hd = os.path.join(root, "handoff")
    for n in STALE:
        p = os.path.join(hd, n)
        if os.path.isfile(p):
            print("  stale %s (%s)" % (n, time.strftime(
                "%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(p)))))
            if not a.dry_run:
                if a.save:
                    os.makedirs(a.save, exist_ok=True)
                    shutil.copy2(p, os.path.join(a.save, "%s-%s" % (a.role,
                                                                     n)))
                os.remove(p)
    if a.dry_run:
        return 0
    run_id = "first-%d" % int(time.time())
    staged = hw_loop.stage_arm({a.arm: st}, a.arm, root, a.role, run_id)
    restored = hw_loop.restore_save(os.path.join(a.rig, "golden"), root,
                                    a.role)
    print("  staged: %s" % ", ".join(staged))
    print("  golden restored: %s" % ", ".join(restored))
    bad = 0
    for cur, _d, files in os.walk(st):
        rel_dir = os.path.relpath(cur, st)
        rel_dir = "" if rel_dir == "." else rel_dir
        for name in files:
            t = name
            for pfx in ("host-", "join-"):
                if name.startswith(pfx):
                    t = name[len(pfx):] if pfx[:-1] == a.role else None
                    break
            if t is None or t == ".gpsp-harness.ini":
                continue
            dst = os.path.join(root, rel_dir, t)
            if md5(dst) != md5(os.path.join(cur, name)):
                print("  MISMATCH %s" % dst)
                bad += 1
    hw_loop.flush(a.drive)
    print("  verified (md5) %s, flushed" % ("FAILED" if bad else "OK"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
