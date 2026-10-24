#!/usr/bin/env python3
"""score_gbalink_hw.py -- verdict and numbers for the GBA link hardware pair
rig (prepare_gbalink_hw_rig.py; docs/GBA-LINK-MIRROR.md section 15).

    score_gbalink_hw.py --logs DIR --rig RIG [--awscore score_awlink.py]

DIR is hw_loop.py's --logs: autoNNN-<arm>-<role>.log (+ .meta.json).
Every cycle with both consoles' logs is scored with the scorer its arm
needs; a cycle whose card EBOOT (meta.json eboot_crc32) is not the one
MANIFEST.txt lists for that arm and role is INVALID (the first run after
the owner's launch is whatever was staged before, logged arm "x").

  mirror arms (B, A): score_session.py -- both consoles exit 0 after
      gblink_done, every periodic HASH matched, FINAL hashes equal at the
      same frame: the 3000 and the 1000 computed the same two GBAs.
  fake arm (F): score_awlink.py -- the battle completed on both consoles
      and their state_watch sequences agree.

Then the numbers: tools/gblink/perf_report.py over the mirror runs (fps,
work, core, over-budget and late frames, stalls, audio starvation, hash
cost, absorbed stalls, underruns, ROM pages read from the stick a minute)
and its window-matched core difference A vs B.
Exit 0 only if every valid run passes and each arm has at least one.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = ("F",)          # every other arm is a mirror session


def manifest_crcs(rig):
    out = {}
    for ln in open(os.path.join(rig, "MANIFEST.txt"), encoding="utf-8"):
        m = re.match(r"# arm (\w+) (host|join) eboot crc32 ([0-9a-f]{8})", ln)
        if m:
            out[(m.group(1), m.group(2))] = m.group(3)
    return out


def meta_crc(log):
    try:
        return json.load(open(log[:-4] + ".meta.json")).get("eboot_crc32")
    except (OSError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", required=True)
    ap.add_argument("--rig", required=True)
    ap.add_argument("--vs", default="B",
                    help="control arm for the window-matched core difference")
    ap.add_argument("--awscore", default=os.path.join(
        HERE, "..", "..", "..", "gbalink-baseline-worktree", "tools",
        "awlink", "score_awlink.py"))
    a = ap.parse_args()
    want = manifest_crcs(a.rig)
    cycles = {}
    for f in glob.glob(os.path.join(a.logs, "auto*-*-*.log")):
        m = re.match(r"auto(\d+)-(\w+)-(host|join)\.log$", os.path.basename(f))
        if m:
            cycles.setdefault((int(m.group(1)), m.group(2)), {})[m.group(3)] = f
    ok_all, passed, mirror_logs = True, {}, []
    tmp = tempfile.mkdtemp(prefix="gbahw-score-")
    print("cycle arm verdict  detail")
    for (n, arm), roles in sorted(cycles.items()):
        if set(roles) != {"host", "join"}:
            print("%5d %-3s MISSING  only %s" % (n, arm, ",".join(roles)))
            continue
        bad = [r for r in ("host", "join")
               if want.get((arm, r)) != meta_crc(roles[r])]
        if bad:
            print("%5d %-3s INVALID  card EBOOT %s not the rig's for arm %s"
                  % (n, arm, "/".join("%s=%s" % (r, meta_crc(roles[r]))
                                     for r in bad), arm))
            continue
        d = os.path.join(tmp, "%03d-%s" % (n, arm))
        os.makedirs(d)
        if arm not in FAKE:
            shutil.copy2(roles["host"], os.path.join(d, "host.log"))
            shutil.copy2(roles["join"], os.path.join(d, "join.log"))
            p = subprocess.run([sys.executable,
                                os.path.join(HERE, "score_session.py"), d],
                               capture_output=True, text=True)
            mirror_logs += [roles["host"], roles["join"]]
        else:
            p = subprocess.run([sys.executable, a.awscore, roles["host"],
                                roles["join"]], capture_output=True, text=True)
        good = p.returncode == 0
        ok_all &= good
        passed.setdefault(arm, []).append(good)
        lines = [ln for ln in p.stdout.splitlines() if ln.strip()]
        key = [ln for ln in lines if re.search(
            r"final hashes|FAIL|^host:|^join:|PASS|frame time", ln)]
        print("%5d %-3s %-8s %s" % (n, arm, "PASS" if good else "FAIL",
                                    (key or lines or ["(no output)"])[0]))
        for ln in key[1:6]:
            print("              %s" % ln)
    for arm in sorted({k[0] for k in want}):
        if not passed.get(arm):
            ok_all = False
            print("arm %s: no valid run" % arm)
        else:
            print("arm %s: %d/%d PASS" % (arm, sum(passed[arm]),
                                          len(passed[arm])))
    if mirror_logs:
        print("\nperf_report (mirror runs; window-matched core vs %s):" % a.vs + "")
        pr = os.path.join(HERE, "..", "gblink", "perf_report.py")
        sys.stdout.flush()
        subprocess.run([sys.executable, pr] + sorted(mirror_logs) +
                       ["--vs", a.vs])
    shutil.rmtree(tmp, ignore_errors=True)
    print("\nVERDICT %s" % ("PASS" if ok_all else "FAIL"))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
