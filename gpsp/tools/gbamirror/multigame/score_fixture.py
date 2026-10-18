#!/usr/bin/env python3
"""score_fixture.py OUT FIXTURE SESSION_RC -- the multigame verdict for one
two-console run (run_fixture_ppsspp.sh), on top of score_session.py's
(SESSION_RC: its exit code, 0 = PASS).

PASS needs, besides score_session's PASS:
  - the autopilots named in DONE (default "host join") DONE, no ap_fail
    on either (AW2's join script is still playing when the host ends);
  - every marker in the fixture's MARKS logged by BOTH consoles' scripts
    (each console's autopilot reads its own machine, so a marker gated on
    RAM proves the game got there on that machine);
  - SWAP="pre post": a trade oracle -- the host's `post` value is the
    join's `pre` and vice versa (logram of party slot 0, each console's
    own machine);
  - CROSS="mine theirs": a battle oracle -- the host's `mine` is the join's
    `theirs` and vice versa;
  - the golden: both machines' per-frame hash lines at GOLDEN_FRAME equal to
    GOLDEN_S0 / GOLDEN_S1, on both consoles (skipped for a --nohash arm).
    An empty GOLDEN_S0 prints the values to record instead.

On a per-frame difference it prints the first divergent FRAME, slot and
which hashed region differs (r = registers, i = IWRAM, e = EWRAM,
io = I/O, p = palette, o = OAM, v = VRAM).

The last line is machine-readable, for run_matrix.sh:
  RESULT name=.. verdict=PASS|FAIL frames=.. desync=-|<frame> core_ms=..
"""
import os
import re
import sys

FIELDS = ("r", "i", "e", "io", "p", "o", "v")


def fixture(path):
    d = {}
    for ln in open(path, encoding="utf-8"):
        m = re.match(r"\s*([A-Z0-9_]+)=(.*)$", ln.rstrip("\n"))
        if m:
            v = m.group(2).strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
                v = v[1:-1]
            d[m.group(1)] = v
    return d


def lines(path, prefix):
    out = []
    try:
        for ln in open(path, encoding="utf-8", errors="replace"):
            if ln.startswith(prefix):
                out.append(ln.rstrip("\n"))
    except OSError:
        pass
    return out


def hash_lines(path):
    """{(slot, frame): 'r=.. i=.. ..'}"""
    d = {}
    try:
        for ln in open(path, encoding="utf-8", errors="replace"):
            m = re.match(r"s=(\d) f=(\d+) (.*)$", ln.strip())
            if m:
                d[(int(m.group(1)), int(m.group(2)))] = m.group(3)
    except OSError:
        return None
    return d


def kv(s):
    return dict(re.findall(r"(\w+)=([^\s]+)", s))


def first_divergence(a, b):
    """first (frame, slot) where both have a line and they differ"""
    common = sorted(set(a) & set(b), key=lambda k: (k[1], k[0]))
    for k in common:
        if a[k] != b[k]:
            ka, kb = kv(a[k]), kv(b[k])
            regions = [f for f in FIELDS if ka.get(f) != kb.get(f)]
            return k[1], k[0], regions
    return None


def main():
    out, fxp, src = sys.argv[1], sys.argv[2], int(sys.argv[3])
    fx = fixture(fxp)
    name = fx.get("NAME", os.path.basename(fxp))
    ok = src == 0
    why = [] if ok else ["score_session FAIL"]
    hl, jl = os.path.join(out, "host.log"), os.path.join(out, "join.log")
    for side, L in (("host", hl), ("join", jl)):
        if side in fx.get("DONE", "host join").split() and not lines(L, "EVT ap_done"):
            ok = False
            why.append("%s: autopilot not DONE" % side)
        f = lines(L, "EVT ap_fail")
        if f:
            ok = False
            why.append("%s: %s" % (side, f[0][4:120]))
        marks = {kv(x).get("text") for x in lines(L, "EVT ap_mark")}
        for m in fx.get("MARKS", "").split():
            if m not in marks:
                ok = False
                why.append("%s: marker %s missing" % (side, m))
    # SWAP="pre post": a trade -- each console's machine logged its party
    # slot before (pre) and after (post); a completed trade swaps them
    sw = fx.get("SWAP", "").split()
    if len(sw) == 2:
        val = {}
        for side, L in (("host", hl), ("join", jl)):
            for x in lines(L, "EVT ap_val"):
                d = kv(x)
                val[(side, d.get("name"))] = d.get("val")
        a, b = sw
        hp, hq = val.get(("host", a)), val.get(("host", b))
        jp, jq = val.get(("join", a)), val.get(("join", b))
        if hp and jp and hq == jp and jq == hp and hp != jp:
            print("trade oracle: host %s %s -> %s, join %s -> %s (swapped)" % (a, hp, hq, jp, jq))
        else:
            ok = False
            why.append("trade oracle: host %s=%s %s=%s, join %s=%s %s=%s (not swapped)" % (
                a, hp, b, hq, a, jp, b, jq))
    # CROSS="mine theirs": a battle -- each console's machine logged its own
    # side's value and the opponent's; the host's `mine` is the join's
    # `theirs` and vice versa (both machines see the same battle)
    cr = fx.get("CROSS", "").split()
    if len(cr) == 2:
        val = {}
        for side, L in (("host", hl), ("join", jl)):
            for x in lines(L, "EVT ap_val"):
                d = kv(x)
                val[(side, d.get("name"))] = d.get("val")
        a, b = cr
        hm, ht = val.get(("host", a)), val.get(("host", b))
        jm, jt = val.get(("join", a)), val.get(("join", b))
        if hm and jm and hm == jt and jm == ht:
            print("battle oracle: host %s=%s %s=%s, join %s=%s %s=%s (crossed)" % (
                a, hm, b, ht, a, jm, b, jt))
        else:
            ok = False
            why.append("battle oracle: host %s=%s %s=%s, join %s=%s %s=%s (not crossed)" % (
                a, hm, b, ht, a, jm, b, jt))
    if fx.get("GAME_FAIL"):
        ok = False
        why.append("game: " + fx["GAME_FAIL"])
    # frames reached / end frame
    fin = lines(hl, "EVT gblink_final ")
    frames = kv(fin[-1]).get("frame", "?") if fin else "?"
    # per-frame
    ha = hash_lines(os.path.join(out, "host", "gbadual_hash.txt"))
    hb = hash_lines(os.path.join(out, "join", "gbadual_hash.txt"))
    desync = "-"
    if ha is not None and hb is not None and ha and hb:
        d = first_divergence(ha, hb)
        if d:
            ok = False
            desync = str(d[0])
            why.append("consoles diverge at frame %d slot %d in %s" % (
                d[0], d[1], ",".join(d[2]) or "?"))
        g = int(fx.get("GOLDEN_FRAME", "0") or 0)
        if g:
            for slot in (0, 1):
                want = fx.get("GOLDEN_S%d" % slot, "")
                for side, H in (("host", ha), ("join", hb)):
                    got = H.get((slot, g))
                    if got is None:
                        ok = False
                        why.append("%s: no slot %d line at golden frame %d" % (side, slot, g))
                    elif not want:
                        if side == "host":
                            print("golden to record: GOLDEN_S%d=\"%s\"" % (slot, got))
                    elif got != want:
                        ok = False
                        ka, kb = kv(got), kv(want)
                        why.append("%s: slot %d at golden frame %d differs from the golden in %s" % (
                            side, slot, g, ",".join(f for f in FIELDS if ka.get(f) != kb.get(f))))
            # where it left the reference, if one was kept (MG_REF dir)
            ref = os.path.join(os.environ.get("MG_REF", ""), name + ".gbadual_hash.txt")
            if any("golden" in w for w in why) and os.path.isfile(ref):
                d = first_divergence(hash_lines(ref), ha)
                if d:
                    why.append("left the reference run at frame %d slot %d in %s" % (
                        d[0], d[1], ",".join(d[2])))
                    if desync == "-":
                        desync = "ref@%d" % d[0]
    else:
        print("per-frame: none (nohash arm)")
    # cost: core_prof means while the session ran (both consoles)
    cost = []
    for L in (hl, jl):
        on = False
        for ln in open(L, encoding="utf-8", errors="replace"):
            if ln.startswith("EVT gblink_start "):
                on = True
            elif ln.startswith("EVT gblink_final ") or ln.startswith("EVT gblink_close"):
                on = False
            if on and ln.startswith("EVT core_prof "):
                m = re.search(r"core=(\d+)/(\d+)/(\d+)", ln)
                if m:
                    cost.append(int(m.group(1)))
    # drop the first window (setup) when there are enough
    if len(cost) > 6:
        cost = cost[1:]
    core = "%.2f" % (sum(cost) / len(cost) / 1000.0) if cost else "?"
    for w in why:
        print("FAIL: " + w)
    print("fixture %s: %s (end frame %s, core %s ms/frame)" % (
        name, "PASS" if ok else "FAIL", frames, core))
    print("RESULT name=%s verdict=%s frames=%s desync=%s core_ms=%s" % (
        name, "PASS" if ok else "FAIL", frames, desync, core))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
