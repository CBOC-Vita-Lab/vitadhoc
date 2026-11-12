#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Property test of the orientation detector (wswan/frontend/host.cpp, orient_update) against the rules in INTEGRATION.md.

Randomised sequences of key-group scan patterns (Y group, X group, both, none) and LCD icon states (vertical, horizontal,
both, none, plus unused port bits set at random) are written by a ROM (make_roms.build_orient_table) frame by frame. An
independent model of the documented rules predicts, for every frame, the detected orientation, the keys seen and the icon
class; the twin's --hashes output must agree on every frame. Then the same sequences are run with the manual override
(--orient h / v): the reported orientation must be the override on every frame, while detection (switch count) still
follows the model.

Rules modelled (INTEGRATION.md, "Detection"):
  keys: only Y scanned in a frame -> vertical evidence, only X -> horizontal, both or neither -> no key evidence
  icon: when there is no key evidence, exactly one icon bit set decides (vertical bit 1, horizontal bit 2)
  a switch needs CONSECUTIVE frames of the same evidence (same source, same direction): 6 for key evidence, 2 for icons;
  a frame with no evidence does not reset the count; a frame whose evidence agrees with the current answer does
  usage: orient_prop.py [--seeds N] [--frames N] [--twin BIN] [--work DIR]
"""
import os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_roms as mr

H, V = 0, 1
KEYS, ICON = "keys", "icon"
NEED = {KEYS: 6, ICON: 2}


def gen_entries(rng, n):
    """Runs of patterns and icons; run lengths 1..14, so both short and long streaks of each kind occur."""
    out = []
    pat_choices = [0, 1, 2, 3]
    pat_w = [2, 3, 3, 2]
    icon_vals = [0, 2, 4, 6]            # horizontal icon / vertical icon / both / none (bit 1 = vertical, bit 2 = horizontal)
    while len(out) < n:
        p = rng.choices(pat_choices, pat_w)[0]
        ic = rng.choice(icon_vals)
        run = rng.randint(1, 14)
        for _ in range(run):
            if len(out) >= n: break
            noise = rng.choice([0, 0, 0, 1, 8, 9, 0x10, 0x3F]) if rng.random() < 0.3 else 0
            # bits 0, 3..7 are not icon bits: they must not change the answer
            out.append((p, (ic | noise) & 0xFF))
    return out


def icon_class(raw):
    v = (raw >> 1) & 1
    hz = (raw >> 2) & 1
    return 1 if (v and not hz) else 2 if (hz and not v) else 0


def model(entries, footer_vertical, mode):
    """Per frame records (orient, keys, icon) as the twin prints them, plus the switch count. Frame 0 has no evidence."""
    detected = V if footer_vertical else H
    cand, cand_src, cand_n = detected, None, 0
    switches = 0
    rows = []
    for f in range(len(entries) + 1):
        if f == 0:
            pat, raw = 0, 0
        else:
            pat, raw = entries[f - 1]
        keys = (1 if pat & 1 else 0) | (2 if pat & 2 else 0)
        icls = icon_class(raw)
        want, src = None, None
        if keys == 1: want, src = V, KEYS
        elif keys == 2: want, src = H, KEYS
        elif icls: want, src = (V if icls == 1 else H), ICON
        if want is not None:
            if want == detected:
                cand_n = 0
                cand_src = None
            else:
                if (want, src) == (cand, cand_src):
                    cand_n += 1
                else:
                    cand, cand_src, cand_n = want, src, 1
                if cand_n >= NEED[src]:
                    detected = want; switches += 1; cand_n = 0; cand_src = None
        rows.append((detected, keys, icls))
    return rows, switches


def run_twin(twin, rom, frames, hashes, extra=()):
    cmd = [twin, rom, "--frames", str(frames), "--hashes", hashes, "--quiet"] + list(extra)
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=600)
    summary = (p.stdout.strip().splitlines() or [""])[-1]
    return p.returncode, summary


def read_hashes(path):
    rows = []
    with open(path) as f:
        for line in f:
            t = line.split()
            rows.append((int(t[2]), int(t[3]), int(t[4])))
    return rows


def main():
    seeds, frames, twin, work = 24, 1200, os.environ.get("WSWAN_TWIN", os.path.join(HERE, "..", "build", "wswan_twin")), None
    a = sys.argv[1:]
    while a:
        k = a.pop(0)
        if k == "--seeds": seeds = int(a.pop(0))
        elif k == "--frames": frames = int(a.pop(0))
        elif k == "--twin": twin = a.pop(0)
        elif k == "--work": work = a.pop(0)
        else: print(__doc__); return 2
    if not os.path.isfile(twin):
        print("twin not found:", twin); return 2
    work = work or tempfile.mkdtemp(prefix="orient_prop_")
    os.makedirs(work, exist_ok=True)
    fails = 0; checks = 0; switches_total = 0; switching_runs = 0
    for seed in range(seeds):
        rng = random.Random(1000 + seed)
        entries = gen_entries(rng, frames)
        footer_v = seed % 3 == 2             # some runs start from the vertical footer flag
        rom = os.path.join(work, "orient_%02d.ws" % seed)
        with open(rom, "wb") as fh: fh.write(mr.build_orient_table(entries, vertical_flag=footer_v, cart_id=0x80 + seed))
        rows, sw = model(entries, footer_v, 0)
        switches_total += sw; switching_runs += 1 if sw else 0
        hp = os.path.join(work, "orient_%02d.h" % seed)
        rc, summ = run_twin(twin, rom, frames + 1, hp)
        got = read_hashes(hp)
        bad = None
        if rc != 0 or len(got) != frames + 1:
            bad = "twin rc=%d rows=%d" % (rc, len(got))
        else:
            for f, (exp, g) in enumerate(zip(rows, got)):
                checks += 1
                if (exp[0], exp[1], exp[2]) != (g[0], g[1], g[2]):
                    bad = "frame %d: model orient/keys/icon %s, twin %s" % (f, exp, g); break
        if bad:
            fails += 1; print("FAIL seed %d (footer %s): %s" % (seed, "V" if footer_v else "H", bad))
        # manual override: the answer is the override on every frame; the detector runs on and its switches agree
        for mode, flag, want_o in (("h", "--orient", H), ("v", "--orient", V)):
            hp2 = os.path.join(work, "orient_%02d_%s.h" % (seed, mode))
            rc2, summ2 = run_twin(twin, rom, frames + 1, hp2, (flag, mode))
            g2 = read_hashes(hp2)
            ob = [r for r in g2 if r[0] != want_o]
            sw_twin = int(summ2.split("switches=")[1].split()[0]) if "switches=" in summ2 else -1
            if rc2 != 0 or len(g2) != frames + 1:
                fails += 1; print("FAIL seed %d override %s: rc=%d rows=%d" % (seed, mode, rc2, len(g2)))
            elif ob:
                fails += 1; print("FAIL seed %d override %s: %d frames not forced (first %s)" % (seed, mode, len(ob), ob[0]))
            elif sw_twin != sw:
                fails += 1; print("FAIL seed %d override %s: detector switches twin %d model %d" % (seed, mode, sw_twin, sw))
    print("orientation property: seeds=%d frames/seed=%d checks=%d switch_runs=%d switches=%d failures=%d"
          % (seeds, frames, checks, switching_runs, switches_total, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
