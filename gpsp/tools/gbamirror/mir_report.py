#!/usr/bin/env python3
"""mir_report.py -- report for a mirrig (GBA link mirror, single console) run.

    python tools/gbamirror/mir_report.py --rig LOGS [--golden GOLD] [--split F]

LOGS holds the collected cycles (autoNNN-<arm>-solo/, from mirrig.py run);
GOLD the PPSSPP golden (one dir per arm, from golden_ppsspp.sh).  Per cycle:

  cost   the frame's core microseconds (shash.txt c=: the whole
         fe_host_run_frame, i.e. BOTH machines in a dual arm), from the state
         load on, split at frame --split (default 1500: before = title and
         menus, after = the link lobby and the battle); mean / median / p95 /
         max and the share of frames over the 16,683 us of a 59.94 Hz frame
  pair   the last EVT gbadual line: switches per frame, blocked runs,
         multi-player transfers, late events
  oracle gbadual_hash.txt (both machines, every frame) and shash.txt (the
         player's machine; c=, a= and n= ignored -- audio is compared within
         one platform only) against the golden of the same arm: IDENTICAL,
         or the first differing line
"""
import argparse
import os
import re
import statistics

BUDGET = 16683


def read_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return [ln.rstrip("\n") for ln in fh]
    except OSError:
        return None


def shash_rows(path):
    rows = []
    for ln in read_lines(path) or []:
        if not ln.startswith("f="):
            continue
        kv = dict(p.split("=", 1) for p in ln.split() if "=" in p)
        rows.append(kv)
    return rows


def pct(xs, q):
    if not xs:
        return 0
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * (len(xs) - 1) + 0.5))]


def cost_stats(xs):
    if not xs:
        return "no frames"
    over = sum(1 for x in xs if x > BUDGET)
    return "n=%d mean=%.2f med=%.2f p95=%.2f max=%.2f ms over=%.1f%%" % (
        len(xs), statistics.mean(xs) / 1000, statistics.median(xs) / 1000,
        pct(xs, 0.95) / 1000, max(xs) / 1000, 100.0 * over / len(xs))


def compare(a_path, b_path, key=None):
    a, b = read_lines(a_path), read_lines(b_path)
    if a is None or b is None:
        return "missing (%s)" % ("hw" if a is None else "golden")
    if key:
        a, b = [key(x) for x in a], [key(x) for x in b]
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return "DIFF at line %d: hw '%s' golden '%s'" % (i + 1, a[i][:90], b[i][:90])
    if len(a) != len(b):
        return "IDENTICAL for %d lines (hw %d, golden %d)" % (n, len(a), len(b))
    return "IDENTICAL (%d lines)" % n


def shash_key(ln):
    if not ln.startswith("f="):
        return ""
    return " ".join(p for p in ln.split() if not p.startswith(("c=", "a=", "n=")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rig", required=True)
    ap.add_argument("--golden")
    ap.add_argument("--split", type=int, default=1500)
    ap.add_argument("--from-frame", type=int, default=32,
                    help="first frame counted (after the frame-30 state load)")
    a = ap.parse_args()
    cycles = sorted(d for d in os.listdir(a.rig) if re.match(r"auto\d+-", d))
    by_arm = {}
    for d in cycles:
        p = os.path.join(a.rig, d)
        arm = re.match(r"auto\d+-(.+)-solo$", d)
        arm = arm.group(1) if arm else d
        rows = shash_rows(os.path.join(p, "shash.txt"))
        c = [(int(r["f"]), int(r.get("c", "0"))) for r in rows if "c" in r]
        pre = [x for f, x in c if a.from_frame <= f < a.split]
        post = [x for f, x in c if f >= a.split]
        fe = (read_lines(os.path.join(p, "frontend.log")) or
              read_lines(os.path.join(p, "frontend.prev.log")) or [])
        ex = next((ln for ln in fe if ln.startswith("EVT exit")), "EVT exit ?")
        gd = [ln for ln in fe if ln.startswith("EVT gbadual frames=")]
        print("== %s  [%s]" % (d, ex[4:]))
        print("   pre-link  %s" % cost_stats(pre))
        print("   link      %s" % cost_stats(post))
        if gd:
            kv = dict(x.split("=", 1) for x in gd[-1][4:].split()[1:] if "=" in x)
            fr = int(kv["frames"].split("/")[0]) or 1
            print("   pair      switches/frame=%.0f blocked=%s mp=%s late=%s "
                  "late_max=%s pages=%s" % (int(kv["switches"]) / fr, kv["blocked"],
                                            kv["mp"], kv["late"], kv["late_max"],
                                            kv.get("pages", "?")))
        # hang study (section 13): the pair's guard, derails, cache calls
        hl = [ln for ln in fe if ln.startswith("EVT gbadual_health")]
        if hl:
            print("   health    %s" % hl[-1][len("EVT gbadual_health "):])
        for ln in fe:
            if ln.startswith(("EVT gbadual_step FAILED", "EVT gbadual_stop",
                              "EVT gbadual_guest_fault", "EVT guest_fault",
                              "EVT freeze_rig_recover")):
                print("   EVENT     %s" % ln[4:])
        for name in ("stall.txt", "heartbeat.txt"):
            st = read_lines(os.path.join(p, name)) or []
            if st and (name == "stall.txt" or not ex.startswith("EVT exit code=0")):
                print("   %-9s %s" % (name[:-4], st[-1][:200]))
        days = [ln for ln in fe if "ap_val name=day" in ln]
        if days or any("gbadual_autopilot" in ln for ln in fe):
            done = sum(1 for ln in fe if ln.startswith("EVT ap_done"))
            last = [ln for ln in fe if ln.startswith("EVT gbadual_autopilot status=")]
            print("   battle    day lines=%d (both machines: 2 per day) ap_done=%d "
                  "partner=%s" % (len(days), done,
                                  last[-1].split("=")[-1] if last else "?"))
        if a.golden:
            g = os.path.join(a.golden, arm)
            for suf in ("-NH-PAR", "-NH"):
                if not os.path.isdir(g) and arm.endswith(suf):
                    g = os.path.join(a.golden, arm[:-len(suf)])   # same guest
            print("   oracle    shash    %s" % compare(os.path.join(p, "shash.txt"),
                                                    os.path.join(g, "shash.txt"),
                                                    shash_key))
            if os.path.isfile(os.path.join(p, "gbadual_hash.txt")):
                print("   oracle    gbadual  %s" % compare(
                    os.path.join(p, "gbadual_hash.txt"),
                    os.path.join(g, "gbadual_hash.txt")))
        by_arm.setdefault(arm, []).append((pre, post))
    print()
    for arm, runs in sorted(by_arm.items()):
        allpost = [x for _p, post in runs for x in post]
        print("%-10s rounds=%d link window: %s" % (arm, len(runs), cost_stats(allpost)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
