#!/usr/bin/env python3
"""score_session.py OUT -- verdict for one GBA link-cable session between two
consoles (PPSSPP pair or hardware pair): OUT/host.log + OUT/join.log, and if
present OUT/{host,join}/gbadual_hash.txt (both machines, every frame).

PASS needs, on BOTH consoles:
  - `EVT exit code=0`, `gblink_done`, no gblink_desync / final_mismatch /
    gblink_failed / gbadual_guest_fault / gbadual_step FAILED;
  - every periodic HASH matched (hashes=N/N, N > 0);
  - the two FINAL hashes equal (same frame);
  - when both hash files exist: identical over their common prefix, and the
    prefix reaches the agreed end frame.
It also prints the session's costs: the mean/p95/max core time per frame
(core_prof windows during the session), stalls, and the wire bandwidth
(netdrv tx/rx bytes over the running window).
Exit 0 = PASS.
"""
import os
import re
import sys


def evts(path, name):
    out = []
    try:
        for line in open(path, encoding="utf-8", errors="replace"):
            if line.startswith("EVT " + name + " ") or line.strip() == "EVT " + name:
                out.append(line.rstrip("\n"))
    except OSError:
        pass
    return out


def kv(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def t_us(line):
    m = re.search(r"t_us=(\d+)", line)
    return int(m.group(1)) if m else None


def side(out, s):
    L = os.path.join(out, s + ".log")
    r = {"ok": True, "why": []}
    ex = evts(L, "exit")
    r["exit"] = kv(ex[-1]).get("code") if ex else None
    if r["exit"] != "0":
        r["ok"] = False
        r["why"].append("exit=%s" % r["exit"])
    done = evts(L, "gblink_done")
    if not done:
        r["ok"] = False
        r["why"].append("no gblink_done")
    for bad in ("gblink_desync", "gblink_final_mismatch", "gblink_failed",
                "gbadual_guest_fault", "gblink_stall_timeout",
                "gblink_final_timeout"):
        if evts(L, bad):
            r["ok"] = False
            r["why"].append("%s: %s" % (bad, evts(L, bad)[0][4:120]))
    if any("FAILED" in l for l in evts(L, "gbadual_step")):
        r["ok"] = False
        r["why"].append("gbadual_step FAILED")
    st = evts(L, "gblink_stats")
    r["stats"] = kv(st[-1]) if st else {}
    hs = r["stats"].get("hashes", "0/0")
    m, n = (int(x) for x in hs.split("/")) if "/" in hs else (0, 0)
    r["hashes"] = (m, n)
    if n == 0 or m != n:
        r["ok"] = False
        r["why"].append("hashes %d/%d" % (m, n))
    fin = evts(L, "gblink_final")
    r["final"] = kv(fin[-1]) if fin else {}
    endat = evts(L, "gblink_end_at")
    r["end_at"] = int(kv(endat[-1]).get("frame", "0")) if endat else 0
    # cost: core_prof windows between gblink_start and gblink_final
    lines = open(L, encoding="utf-8", errors="replace").read().splitlines()
    on = False
    core = []
    nets = []
    for ln in lines:
        if ln.startswith("EVT gblink_start "):
            on = True
        elif ln.startswith("EVT gblink_final ") or ln.startswith("EVT gblink_close"):
            on = False
        if on and ln.startswith("EVT core_prof "):
            mm = re.search(r"core=(\d+)/(\d+)/(\d+)", ln)
            if mm:
                core.append(tuple(int(x) for x in mm.groups()))
        if on and ln.startswith("EVT net_stats "):
            nets.append(ln)
    r["core"] = core
    hb = [ln for ln in lines if ln.startswith("EVT heartbeat ")]
    r["nets"] = nets
    r["hb"] = hb
    return r


def hash_cmp(out):
    a = os.path.join(out, "host", "gbadual_hash.txt")
    b = os.path.join(out, "join", "gbadual_hash.txt")
    if not (os.path.isfile(a) and os.path.isfile(b)):
        return None, "no per-frame hash files (nohash arm)"
    A = open(a).read().splitlines()
    Bl = open(b).read().splitlines()
    n = min(len(A), len(Bl))
    for i in range(n):
        if A[i] != Bl[i]:
            return False, "first difference at line %d: host %r join %r" % (
                i + 1, A[i], Bl[i])
    last = A[n - 1] if n else ""
    f0 = max((int(x) for x in re.findall(r"s=0 f=(\d+)", "\n".join(A[:n]))),
             default=0)
    return True, "identical over %d lines (host %d, join %d); slot 0 reaches f=%d (%s)" % (
        n, len(A), len(Bl), f0, last[:60]), f0


def main():
    out = sys.argv[1]
    H, J = side(out, "host"), side(out, "join")
    ok = H["ok"] and J["ok"]
    for name, r in (("host", H), ("join", J)):
        print("%s: exit=%s hashes=%d/%d stalls=%s streak_max=%s episodes=%s "
              "frames=%s/%s %s" % (
                  name, r["exit"], r["hashes"][0], r["hashes"][1],
                  r["stats"].get("stalls"), r["stats"].get("streak_max"),
                  r["stats"].get("episodes"), r["stats"].get("f0"),
                  r["stats"].get("f1"), "OK" if r["ok"] else "FAIL: " + "; ".join(r["why"])))
    fh, fj = H["final"].get("hash"), J["final"].get("hash")
    ff = (H["final"].get("frame"), J["final"].get("frame"))
    if fh and fh == fj and ff[0] == ff[1]:
        print("final hashes equal: %s at frame %s" % (fh, ff[0]))
    else:
        ok = False
        print("FAIL: final hashes %s@%s vs %s@%s" % (fh, ff[0], fj, ff[1]))
    hc = hash_cmp(out)
    if hc[0] is None:
        print("per-frame: %s" % hc[1])
    elif hc[0]:
        print("per-frame: %s" % hc[1])
        end = max(H["end_at"], J["end_at"])
        if end and hc[2] < end:
            ok = False
            print("FAIL: the common per-frame prefix stops at f=%d, before the end frame %d" % (hc[2], end))
    else:
        ok = False
        print("FAIL per-frame: %s" % hc[1])
    for name, r in (("host", H), ("join", J)):
        if r["core"]:
            means = [c[0] for c in r["core"]]
            print("%s cost: core_prof windows=%d mean_of_means=%.2f ms  max_window_max=%.2f ms" % (
                name, len(means), sum(means) / len(means) / 1000.0,
                max(c[1] for c in r["core"]) / 1000.0))
        nets = r["nets"]
        if len(nets) >= 2:
            a, b = kv(nets[0]), kv(nets[-1])
            ta, tb = None, None
            # net_stats has no timestamp: use the ~5 s cadence
            dt = 5.0 * (len(nets) - 1)
            try:
                txb = int(b.get("tx_bytes", 0)) - int(a.get("tx_bytes", 0))
                rxb = int(b.get("rx_bytes", 0)) - int(a.get("rx_bytes", 0))
                txf = int(b["tx"]) - int(a["tx"])
                rxf = int(b["rx"]) - int(a["rx"])
                print("%s wire: %.0f B/s tx, %.0f B/s rx (netdrv frames incl. header), "
                      "%.1f/%.1f datagrams/s, retx=%s srtt_us=%s (over ~%.0f s)" % (
                          name, txb / dt, rxb / dt, txf / dt, rxf / dt,
                          b.get("retx"), b.get("srtt_us"), dt))
            except (KeyError, ValueError):
                pass
    print("VERDICT %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
