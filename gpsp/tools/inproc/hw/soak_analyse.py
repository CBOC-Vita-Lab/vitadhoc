#!/usr/bin/env python3
"""soak_analyse.py -- score the in-process return soak (docs/INPROC-SOAK.md).

    python soak_analyse.py <hw logs dir> [--ref <ppsspp smoke dir>] [--json out]

<hw logs dir> is what hw_loop collected for ONE console: autoNNN-<arm>-<role>.log
plus autoNNN-<arm>-<role>-shash.txt / -heartbeat.txt / -stall.txt and .meta.json.
<ppsspp smoke dir> holds smoke.sh's per-arm dirs (frontend.log + shash.txt).

Per run: sessions, returns, refusals (and why), census exactness (every
pre_select / post_select / post_unload equal to the run's first pre_select),
post_load repeatability per game, ME up / HOME up per session, badjump /
bjrec / stall lines, evt drops, `ui_return fade_done ms`.

Identity: every session is compared, frame by frame, with the FRESH-BOOT
session of the same game on the same console (session 0 of a run: S1's is
aw2, S2's hns, Fr/Fe/Fc/Fh the others), on vhash (the presented frame) and,
for GBA, shash (CPU regs + IWRAM/EWRAM/IO/palette/OAM/VRAM; c= a= n= excluded,
as x20_analyse).  Reported over all frames and over frames after the state
load (f > 30), since frames 4..30 of a GBA boot differ even between two fresh
boots (docs/INPROC-RETURN.md §5).  With --ref the same is done against the
PPSSPP fresh boots of the same build.
"""
import glob
import json
import os
import re
import sys

POST = 30   # the harness loads slot 1 at frame 30


def game_of(path):
    # lower case: PPSSPP upper-cases some names, the card does not
    return os.path.basename(path.strip()).lower()


def shash_sessions(p):
    out, cur, last = [], None, 0
    if not os.path.isfile(p):
        return out
    for l in open(p, errors="replace"):
        if not l.startswith("f="):
            continue
        parts = l.split()
        try:
            f = int(parts[0][2:])
        except ValueError:
            continue
        if cur is None or f <= last:
            cur = {}
            out.append(cur)
        last = f
        cur[f] = " ".join(x for x in parts[1:] if not x.startswith(("c=", "a=", "n=")))
    return out


def parse_log(p):
    r = {"sessions": [], "census": [], "returns": 0, "refused": [], "mismatch": [],
         "exit": None, "fade_ms": [], "me_up": 0, "me_other": [], "home_up": 0,
         "home_other": [], "badjump": [], "bjrec": 0, "evt_drop": None,
         "post_load": [], "autoload": [], "picks": [], "model": None,
         "me_rend_on": 0}
    sess = None
    for l in open(p, errors="replace"):
        l = l.rstrip("\n")
        m = re.match(r"EVT inproc_session n=(\d+) rom=(.*)$", l)
        if m:
            sess = {"n": int(m.group(1)), "rom": game_of(m.group(2)), "vh": {},
                    "me": None, "home": None, "rend": None}
            r["sessions"].append(sess)
            continue
        m = re.match(r"EVT vhash f=(\d+) h=([0-9a-f]+)", l)
        if m:
            if sess is not None:
                sess["vh"][int(m.group(1))] = m.group(2)
            continue
        if l.startswith("EVT heap_census"):
            at = re.search(r"at=(\w+)", l).group(1)
            body = l.split(" ", 3)[3]
            r["census"].append((at, body, len(r["sessions"]) - 1))
            if at == "post_load" and sess is not None:
                r["post_load"].append((sess["rom"], body))
            continue
        if l.startswith("EVT inproc_return"):
            r["returns"] += 1
        elif l.startswith("EVT inproc_refused"):
            r["refused"].append(l[4:])
        elif l.startswith("EVT inproc_census_mismatch"):
            r["mismatch"].append(l[4:])
        elif l.startswith("EVT exit"):
            r["exit"] = l[4:]
        elif "ui_return fade_done" in l:
            m = re.search(r"mode=(\d+) ms=(\d+)", l)
            r["fade_ms"].append((int(m.group(1)), int(m.group(2)),
                                 sess["rom"] if sess else "-"))
        elif l.startswith("EVT me_init"):
            if "state=up" in l:
                r["me_up"] += 1
                if sess is not None:
                    sess["me"] = "up"
            else:
                r["me_other"].append(l[4:])
                if sess is not None:
                    sess["me"] = l[4:]
        elif l.startswith("EVT me_boot") and sess is not None:
            m = re.search(r"rend=(\d)", l)
            sess["rend"] = m.group(1) if m else None
            if m and m.group(1) == "1":
                r["me_rend_on"] += 1
        elif l.startswith("EVT home_service state="):
            st = re.search(r"state=(\w+)", l).group(1)
            if st == "up":
                r["home_up"] += 1
                if sess is not None:
                    sess["home"] = "up"
            elif st not in ("stopped",):
                r["home_other"].append(l[4:])
        elif "badjump" in l and l.startswith("EVT badjump"):
            r["badjump"].append(l[4:])
        elif l.startswith("EVT core_health"):
            m = re.search(r"bjrec=(\d+)", l)
            if m:
                r["bjrec"] = max(r["bjrec"], int(m.group(1)))
            m = re.search(r" badjump=(\d+)", l)
            if m and int(m.group(1)):
                r["badjump"].append(l[4:])
        elif l.startswith("EVT evt_drop total="):
            r["evt_drop"] = int(l.split("=")[-1])
        elif l.startswith("EVT autoload_state"):
            r["autoload"].append(l[4:])
        elif l.startswith("EVT ui_browser_pick"):
            r["picks"].append(l[4:])
        elif l.startswith("EVT psp_model") and r["model"] is None:
            m = re.search(r"name=(\S+)", l)
            r["model"] = m.group(1) if m else l
    return r


def census_summary(r):
    by = {}
    for at, body, _s in r["census"]:
        by.setdefault(at, []).append(body)
    if not by.get("pre_select"):
        return "no census", False
    first = by["pre_select"][0]
    ok = True
    parts = []
    for at in ("pre_select", "post_select", "post_unload"):
        vals = by.get(at, [])
        same = all(v == first for v in vals)
        ok &= same
        parts.append("%s %d/%d" % (at, sum(v == first for v in vals), len(vals)))
    return ", ".join(parts), ok


def post_load_summary(r):
    per = {}
    for rom, body in r["post_load"]:
        m = re.search(r"free=(\d+) top=\d+ largest=(\d+)", body)
        per.setdefault(rom, set()).add((m.group(1), m.group(2)) if m else body)
    return {k: len(v) for k, v in per.items()}, per


def cmp(a, b, after=0):
    common = sorted(f for f in set(a) & set(b) if f > after)
    diff = [f for f in common if a[f] != b[f]]
    return len(common), diff


def fmt(c):
    n, d = c
    if not n:
        return "  -   "
    return "%d/%d%s" % (n - len(d), n, (" (f%d..%d)" % (d[0], d[-1])) if d else "")


def load_runs(d):
    runs = []
    for p in sorted(glob.glob(os.path.join(d, "auto*-*-*.log"))):
        if p.endswith(".prev.log"):
            continue
        stem = p[:-4]
        m = re.match(r"auto(\d+)-(\w+)-(host|join)$", os.path.basename(stem))
        if not m:
            continue
        meta = {}
        if os.path.isfile(stem + ".meta.json"):
            meta = json.load(open(stem + ".meta.json"))
        r = parse_log(p)
        r["cycle"] = int(m.group(1))
        r["arm"] = m.group(2)
        r["meta"] = meta
        r["shash"] = shash_sessions(stem + "-shash.txt")
        r["stall"] = open(stem + "-stall.txt").read().splitlines() \
            if os.path.isfile(stem + "-stall.txt") else []
        r["prev"] = os.path.isfile(stem + ".prev.log")
        runs.append(r)
    return runs


def load_ref(d):
    """PPSSPP smoke dirs -> [run dict with arm]."""
    out = []
    for arm in sorted(os.listdir(d)) if d and os.path.isdir(d) else []:
        p = os.path.join(d, arm, "frontend.log")
        if not os.path.isfile(p):
            continue
        r = parse_log(p)
        r["arm"] = arm
        r["shash"] = shash_sessions(os.path.join(d, arm, "shash.txt"))
        out.append(r)
    return out


def fresh_refs(runs):
    """game -> (vhash, shash or None, where) from session 0 of every run."""
    ref = {}
    for r in runs:
        if not r["sessions"]:
            continue
        s0 = r["sessions"][0]
        if s0["rom"] in ref:
            continue
        sh = r["shash"][0] if r["shash"] else None
        ref[s0["rom"]] = (s0["vh"], sh, "%s s0" % r["arm"])
    return ref


def main():
    d = sys.argv[1]
    refdir = None
    jout = None
    if "--ref" in sys.argv:
        refdir = sys.argv[sys.argv.index("--ref") + 1]
    if "--json" in sys.argv:
        jout = sys.argv[sys.argv.index("--json") + 1]
    runs = load_runs(d)
    hw_ref = fresh_refs([r for r in runs if r["arm"] != "U"])
    pp_ref = fresh_refs(load_ref(refdir)) if refdir else {}
    tot = {"sessions": 0, "returns": 0, "refused": 0, "census_bad": 0,
           "vh_bad_post": 0, "sh_bad_post": 0, "vh_cmp": 0, "sh_cmp": 0,
           "badjump": 0, "stall": 0, "fade": []}
    print("logs: %s" % d)
    for r in runs:
        meta = r["meta"].get("result", {})
        cs, cok = census_summary(r)
        pl_n, _pl = post_load_summary(r)
        print("\n== cycle %d arm %s  model=%s  eboot=%s  %s  frames=%s evt_drop=%s(log %s)"
              % (r["cycle"], r["arm"], r["model"], (r["meta"].get("eboot_md5") or "?")[:8],
                 r["exit"], meta.get("frames"), meta.get("evt_drop"), r["evt_drop"]))
        print("   sessions=%d returns=%d refused=%s mismatch=%d prev_log=%s"
              % (len(r["sessions"]), r["returns"], r["refused"] or 0,
                 len(r["mismatch"]), r["prev"]))
        print("   census: %s -> %s" % (cs, "EXACT" if cok else "NOT EXACT"))
        print("   post_load distinct per game: %s" % pl_n)
        nses = len(r["sessions"])
        me_up = sum(1 for s in r["sessions"] if s["me"] == "up")
        rend = sum(1 for s in r["sessions"] if s["rend"] == "1")
        home = sum(1 for s in r["sessions"] if s["home"] == "up")
        print("   ME up %d/%d sessions (rend=1 in %d), HOME up %d/%d; me other: %s; home other: %s"
              % (me_up, nses, rend, home, nses, sorted(set(r["me_other"]))[:3],
                 sorted(set(r["home_other"]))[:3]))
        print("   badjump lines=%d bjrec_max=%d stall lines=%d"
              % (len(r["badjump"]), r["bjrec"], len(r["stall"])))
        if r["picks"]:
            print("   picks: %s" % " | ".join(p.split("rom=")[1] for p in r["picks"][:6])
                  + (" ..." if len(r["picks"]) > 6 else ""))
        if r["fade_ms"]:
            ms = [m for _mode, m, _g in r["fade_ms"]]
            print("   fade_done ms: n=%d min=%d median=%d max=%d"
                  % (len(ms), min(ms), sorted(ms)[len(ms) // 2], max(ms)))
            tot["fade"] += r["fade_ms"]
        tot["sessions"] += nses
        tot["returns"] += r["returns"]
        tot["refused"] += len(r["refused"])
        tot["census_bad"] += 0 if cok else 1
        tot["badjump"] += len(r["badjump"]) + r["bjrec"]
        tot["stall"] += len(r["stall"])
        # identity
        bad = []
        # The UI arm loads its state from the shelf, not at the harness's
        # frame 30, so its references are its own first session of each game
        # (self-consistency across returns); everything else is compared with
        # the console's fresh boots.
        own = {}
        if r["arm"] == "U":
            for i, s in enumerate(r["sessions"]):
                if s["rom"] not in own:
                    own[s["rom"]] = (s["vh"], r["shash"][i] if i < len(r["shash"])
                                     else None, "U s%d" % i, i)
        for i, s in enumerate(r["sessions"]):
            ref = own.get(s["rom"]) if r["arm"] == "U" else hw_ref.get(s["rom"])
            if ref and r["arm"] == "U" and ref[3] == i:
                # (the UI arm's first session of each game is its reference)
                fr = hw_ref.get(s["rom"])
                if i == 0 and fr:
                    print("   s000 %-14s (U reference) vs %s: vhash post %s"
                          % (s["rom"], fr[2], fmt(cmp(fr[0], s["vh"], POST))))
                continue
            sh = r["shash"][i] if i < len(r["shash"]) else None
            line = "   s%03d %-14s" % (i, s["rom"])
            if ref:
                va, vp = cmp(ref[0], s["vh"]), cmp(ref[0], s["vh"], POST)
                line += " hw vhash all %-18s post %-18s" % (fmt(va), fmt(vp))
                tot["vh_cmp"] += 1
                if vp[1] or not vp[0]:
                    tot["vh_bad_post"] += 1
                    bad.append(i)
                if s["rom"].endswith(".gba") and ref[1] is not None and sh is not None:
                    sa, sp = cmp(ref[1], sh), cmp(ref[1], sh, POST)
                    line += " shash all %-18s post %-18s" % (fmt(sa), fmt(sp))
                    tot["sh_cmp"] += 1
                    if sp[1] or not sp[0]:
                        tot["sh_bad_post"] += 1
                        if i not in bad:
                            bad.append(i)
            pr = pp_ref.get(s["rom"])
            if pr:
                line += " | ppsspp vh post %s" % fmt(cmp(pr[0], s["vh"], POST))
                if s["rom"].endswith(".gba") and pr[1] is not None and sh is not None:
                    line += " sh post %s" % fmt(cmp(pr[1], sh, POST))
            if i < 3 or i in bad or i == nses - 1 or "-v" in sys.argv:
                print(line)
        if bad:
            print("   NOT IDENTICAL after the state load: sessions %s" % bad)
        else:
            print("   identity: every session identical to its fresh boot after the state load")
    print("\n== TOTAL sessions=%d returns=%d refused=%d census-not-exact runs=%d "
          "vhash-post-diff sessions=%d/%d shash-post-diff sessions=%d/%d badjump=%d stall=%d"
          % (tot["sessions"], tot["returns"], tot["refused"], tot["census_bad"],
             tot["vh_bad_post"], tot["vh_cmp"], tot["sh_bad_post"], tot["sh_cmp"],
             tot["badjump"], tot["stall"]))
    if tot["fade"]:
        per = {}
        for _mode, ms, g in tot["fade"]:
            per.setdefault(g, []).append(ms)
        allms = sorted(ms for _m, ms, _g in tot["fade"])
        print("   return time (hold -> browser interactive): n=%d min=%d median=%d max=%d ms"
              % (len(allms), allms[0], allms[len(allms) // 2], allms[-1]))
        for g, v in sorted(per.items()):
            v = sorted(v)
            print("     after %-14s n=%2d median=%d max=%d" % (g, len(v), v[len(v) // 2], v[-1]))
    print("   fresh refs (hw): %s" % {k: v[2] for k, v in hw_ref.items()})
    if jout:
        json.dump(tot, open(jout, "w"), indent=1)


if __name__ == "__main__":
    main()
