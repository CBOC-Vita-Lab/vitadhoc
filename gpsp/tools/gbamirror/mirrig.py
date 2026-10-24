#!/usr/bin/env python3
"""mirrig.py -- the GBA link mirror's single-console hardware bench: ONE rig
app on ONE console, driven by the USB handoff loop (launched once from the
XMB, never N apps).  docs/GBA-LINK-MIRROR.md section 10.

    python tools/gbamirror/mirrig.py stage   --out STAGE --build DIR --fx FX --bios BIOS [--plan P]
    python tools/gbamirror/mirrig.py plan    --stage STAGE
    python tools/gbamirror/mirrig.py install --stage STAGE --drive F: --backup DIR
    python tools/gbamirror/mirrig.py run     --stage STAGE --drive F: --logs LOGS

What a cycle is: the AW2 link session of the desktop prototype on this PSP
alone -- slot 0 (the player's machine, drawn by the ME) driven by the
autopilot, slot 1 (the mirror partner, headless) by a frame-stamped script,
the cable between them, no radio.  It measures what two cores cost on this
model (core_prof, EVT gbadual) and whether this console computes the same
two machines as PPSSPP did (log/gbadual_hash.txt vs the golden).

Arms differ ONLY in harness keys (one EBOOT, the harness64 GBA_LINK build,
relabelled "GBA MIRROR RIG"), as in tools/rig/resrig.py:
    M-SOLO     the player's machine alone (no partner): what the session's
               scenes cost one core, up to the link lobby where it waits
    M-DUAL     the pair, lookahead 2464 cycles (two scanlines)
    M-DUAL10K  the pair, lookahead 10000 (62 switches a frame instead of 410)
    M-OWN      the pair, partner with its OWN copy of the ROM (64 MiB only)

Built on harness-kit/hw_loop.py (parse_result, flush, eject, copy_retry,
card_root), imported unchanged, like tools/drprof/drrig.py and resrig.py.

SAFETY.  Writes only under <drive>/PSP/GAME/GBADHOC-GBAMIRROR.  `install` to
a drive letter refuses without tools/rig/backup_sticks.py's MANIFEST.verified
for that volume serial (or a volume with no user files).  A PSP Go must run
it from the M2 card, never internal storage (the in-app export is the Memory
Stick driver; see drrig.py).  A missing RESULT.TXT is a result (froze): logged,
then the loop waits for the owner's relaunch.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.environ.get("RESRIG_HWLOOP_DIR") or os.path.normpath(
    os.path.join(HERE, "..", "..", "..", "..", "harness-kit"))
sys.path.insert(0, KIT)
sys.path.insert(0, os.path.join(HERE, ".."))
import hw_loop  # noqa: E402
from relabel_pbp import relabel  # noqa: E402

APPDIR = "GBADHOC-GBAMIRROR"
TITLE = "GBA MIRROR RIG"
ROUNDS = 2
AUTOEXIT = "9100"

# fixture: the AW2 link session (owner's AW2 save + state on BOTH machines)
FX_FILES = {"aw2.gba": "aw2.gba", "aw2.sav": "aw2.sav", "aw2.st0": "aw2.st0"}
FX_SCRIPTS = {"script.txt": "host30.txt", "partner.txt": "aw2_link_join.txt",
              # the baseline's RAM-gated 10-round battle (claude/gbalink-
              # baseline tools/awlink/scripts), host side delayed 30 frames
              # for the frame-30 state autoload like host30.txt
              "battle_host.txt": "battle_host30.txt",
              "battle_join.txt": "aw2_battle_join.txt"}

# lookahead explicit: the pair's default changed to half a frame after the
# 3000's first sessions; these arms keep the meaning they were measured with
DUAL = {"gba_dual": "1", "gba_dual_script": "partner.txt", "gba_dual_hash": "1",
        "gba_dual_lookahead": "2464"}
ARM_TABLE = [
    ("M-SOLO", {}),
    ("M-DUAL", dict(DUAL)),
    ("M-DUAL10K", dict(DUAL, gba_dual_lookahead="10000")),
    ("M-OWN", dict(DUAL, gba_dual_own_rom="1")),
    # the whole battle: both machines on the baseline's RAM-gated scripts
    ("M-BATTLE", {"gba_dual": "1", "gba_dual_autopilot": "battle_join.txt",
                  "gba_dual_lookahead": "2464",
                  "gba_dual_hash": "1", "script": "battle_host.txt",
                  "autoexit_frames": "17000"}),
]
# lookahead sweep (2026-10-01, after the 3000's first run: 409 switches a
# frame cost ~48 us each there -- a full I-cache refill).  Desktop: the
# battle is hash-identical to lookahead 2464 up to 140448 (half a frame).
for _n, _l in (("50K", "50000"), ("140K", "140448")):
    ARM_TABLE.append(("M-BATTLE" + _n, dict(ARM_TABLE[4][1], gba_dual_lookahead=_l)))
    ARM_TABLE.append(("M-DUAL" + _n, dict(DUAL, gba_dual_lookahead=_l)))
# -NH: the same arms WITHOUT gba_dual_hash.  The 3000's first session showed
# a ~13 ms per-frame cost that switching does not explain: the hash of both
# machines (~0.8 MB a frame, inside the timed frame) is the suspect.  Cost
# comes from these; the oracle stays on shash (the player's machine).
for _name, _x in list(ARM_TABLE):
    if _name in ("M-DUAL", "M-DUAL50K", "M-DUAL140K", "M-BATTLE140K"):
        ARM_TABLE.append((_name + "-NH", {k: v for k, v in _x.items()
                                          if k != "gba_dual_hash"}))
# The hang (docs/GBA-LINK-MIRROR.md section 13): the PSP-3000 hung once in
# M-BATTLE140K-NH.  -PAR is the same arm with every cache sync of BOTH cores
# made whole D+I (harness cache_paranoid; B inherits it since the hang-study
# build): the one switch that removes every coherency hole at once.
ARM_TABLE.append(("M-BATTLE140K-NH-PAR",
                  dict(dict(ARM_TABLE)["M-BATTLE140K-NH"],
                       cache_paranoid="1")))
PLANS = {
    # PSP-1000 (32 MiB): no M-OWN (a second 8 MiB copy does not fit beside
    # the statically linked partner core)
    "p1000": ["M-BATTLE", "M-DUAL", "M-SOLO", "M-DUAL10K"],
    # 64 MiB consoles
    "p64": ["M-BATTLE", "M-DUAL", "M-SOLO", "M-DUAL10K", "M-OWN"],
}

BASE_INI = [
    ("rom", "aw2.gba"),
    ("script", "script.txt"),
    ("load_state", "1"),
    ("shash", "1"),
    ("heartbeat_s", "5"),
    ("me_mode", "1"),
    ("log_input", "0"),
    ("autoexit_frames", AUTOEXIT),
    # hang study (section 13): a stall record after 15 s without a main-loop
    # iteration (with the machine pair's position), and a relaunch after a
    # freeze hands the frozen run's logs off before running anything
    ("stall_watch_s", "15"),
    ("freeze_rig_recover", "1"),
    ("handoff", "1"),
    ("handoff_window_s", "90"),
    ("handoff_park_s", "0"),
    ("handoff_max_runs", "100000"),
    ("handoff_total_s", "900"),
]


def log(msg):
    hw_loop.log(msg)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for blk in iter(lambda: fh.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def crc32(path):
    c = 0
    with open(path, "rb") as fh:
        for blk in iter(lambda: fh.read(1 << 20), b""):
            c = zlib.crc32(blk, c)
    return "%08x" % (c & 0xFFFFFFFF)


def arm_extra(arm):
    for name, extra in ARM_TABLE:
        if name == arm:
            return extra
    raise KeyError(arm)


def order_for(plan, rounds=ROUNDS):
    """Round 1 = every arm once; the repeat round in REVERSE (ABBA)."""
    names = PLANS[plan]
    out = []
    for r in range(rounds):
        out.extend(names if r % 2 == 0 else names[::-1])
    return out


def render_ini(arm, run_id):
    extra = arm_extra(arm)
    lines = ["# mirrig: GBA link mirror single-console bench (docs/GBA-LINK-MIRROR.md)"]
    lines += ["%s = %s" % kv for kv in BASE_INI if kv[0] not in extra]
    lines += ["%s = %s" % kv for kv in sorted(extra.items())]
    lines += ["run_id = %s" % run_id, "arm = %s" % arm]
    return "\n".join(lines) + "\n"


def staged_keys(ini_text):
    out = {}
    for line in ini_text.splitlines():
        if line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


# --------------------------------------------------------------- stage ----
def cmd_stage(a):
    if os.path.exists(a.out) and os.listdir(a.out):
        print("REFUSING: %s is not empty" % a.out)
        return 2
    base = os.path.join(a.out, "base")
    os.makedirs(os.path.join(base, "roms"))
    with open(os.path.join(a.build, "EBOOT.PBP"), "rb") as fh:
        pbp = relabel(fh.read(), TITLE)
    with open(os.path.join(base, "EBOOT.PBP"), "wb") as fh:
        fh.write(pbp)
    shutil.copy2(os.path.join(a.build, "gbadhoc_me.prx"),
                 os.path.join(base, "gbadhoc_me.prx"))
    shutil.copy2(a.bios, os.path.join(base, "gba_bios.bin"))
    for dst, src in FX_FILES.items():
        shutil.copy2(os.path.join(a.fx, src), os.path.join(base, "roms", dst))
    for dst, src in FX_SCRIPTS.items():
        shutil.copy2(os.path.join(a.fx, src), os.path.join(base, dst))
    man = {"created": time.strftime("%Y-%m-%d %H:%M:%S"), "appdir": APPDIR,
           "plan": a.plan, "arms": PLANS[a.plan], "order": order_for(a.plan),
           "base_ini": BASE_INI, "arm_keys": dict(ARM_TABLE), "files": {}}
    for cur, _d, files in os.walk(base):
        for n in files:
            p = os.path.join(cur, n)
            rel = os.path.relpath(p, base).replace(os.sep, "/")
            man["files"][rel] = {"md5": md5(p), "crc32": crc32(p),
                                 "size": os.path.getsize(p)}
    with open(os.path.join(a.out, "MANIFEST.json"), "w") as fh:
        json.dump(man, fh, indent=1)
    print("staged %d files -> %s; EBOOT crc32=%s" % (
        len(man["files"]), a.out, man["files"]["EBOOT.PBP"]["crc32"]))
    return 0


def load_manifest(stage):
    with open(os.path.join(stage, "MANIFEST.json")) as fh:
        return json.load(fh)


def verify_stage(stage, man):
    for rel, want in man["files"].items():
        p = os.path.join(stage, "base", rel)
        if not os.path.isfile(p) or md5(p) != want["md5"]:
            print("STAGE CHANGED since it was made: %s" % rel)
            return False
    return True


def cmd_ini(a):
    """The arm's harness ini without the handoff keys: a single PPSSPP run of
    the staged files (golden_ppsspp.sh) makes the golden hashes."""
    text = render_ini(a.arm, "golden-%s" % a.arm)
    keep = [ln for ln in text.splitlines()
            if not ln.split("=")[0].strip().startswith("handoff")]
    for ln in keep:
        print(ln)
    return 0


def cmd_plan(a):
    man = load_manifest(a.stage)
    for i, arm in enumerate(man["order"], 1):
        print("cycle %2d  %-10s %s" % (i, arm, arm_extra(arm) or "(no partner)"))
    return 0


# ------------------------------------------------------------- install ----
def volume_serial(drive):
    try:
        out = subprocess.run(["cmd", "/c", "vol", drive], capture_output=True,
                             text=True, timeout=20).stdout
        for line in out.splitlines():
            if "Serial Number" in line:
                return line.split()[-1]
    except Exception:
        pass
    return "unknown"


def backup_ok(backup, serial):
    if not os.path.isfile(os.path.join(backup, "MANIFEST.verified")):
        return False, "no MANIFEST.verified in %s" % backup
    try:
        with open(os.path.join(backup, "MANIFEST.txt"), encoding="utf-8") as fh:
            body = fh.read()
    except OSError as e:
        return False, str(e)
    if "serial=%s" % serial not in body:
        return False, "backup manifest has no file from volume %s" % serial
    return True, "backup verified for volume %s" % serial


def user_files(drive, cap=1):
    n = 0
    for cur, dirs, files in os.walk(drive + os.sep):
        dirs[:] = [d for d in dirs if d.lower() not in (
            "system volume information", "$recycle.bin")]
        n += len(files)
        if n >= cap:
            return n
    return n


def stage_arm(root, arm, run_id):
    text = render_ini(arm, run_id)
    with open(os.path.join(root, ".gpsp-harness.ini"), "w", encoding="utf-8",
              newline="\n") as fh:
        fh.write(text)
    return text


def cmd_install(a):
    man = load_manifest(a.stage)
    if not verify_stage(a.stage, man):
        return 2
    is_card = len(a.drive) == 2 and a.drive[1] == ":"
    if is_card and not os.path.isdir(a.drive + os.sep):
        print("REFUSING: %s is not mounted" % a.drive)
        return 2
    if is_card:
        serial = volume_serial(a.drive)
        ok, why = backup_ok(a.backup, serial)
        if not ok:
            nfiles = user_files(a.drive)
            if nfiles == 0:
                why = "volume %s holds no user files: nothing to back up" % serial
            else:
                print("REFUSING to write to %s: %s (and the volume holds %d user "
                      "file(s))" % (a.drive, why, nfiles))
                return 2
    else:
        serial, why = "dir", "directory target (dry run): backup check skipped"
    log(why)
    root = hw_loop.card_root(a.drive)
    if os.path.exists(root) and not a.reinstall:
        print("REFUSING: %s exists (pass --reinstall to overwrite the rig "
              "folder; nothing outside it is touched)" % root)
        return 2
    base = os.path.join(a.stage, "base")
    for cur, _d, files in os.walk(base):
        rel = os.path.relpath(cur, base)
        out = root if rel == "." else os.path.join(root, rel)
        os.makedirs(out, exist_ok=True)
        for n in files:
            if not hw_loop.copy_retry(os.path.join(cur, n), os.path.join(out, n),
                                      why="install"):
                return 1
    for sub in ("handoff", "log"):
        d = os.path.join(root, sub)
        os.makedirs(d, exist_ok=True)
        for n in os.listdir(d):
            try:
                os.remove(os.path.join(d, n))
            except OSError:
                pass
    run_id = "1-%d" % int(time.time())
    stage_arm(root, man["order"][0], run_id)
    with open(os.path.join(root, "RIG.TXT"), "w") as fh:
        fh.write("mirrig install %s serial=%s plan=%s first=%s run_id=%s\n"
                 % (time.strftime("%Y-%m-%d %H:%M:%S"), serial, man["plan"],
                    man["order"][0], run_id))
    for rel, want in man["files"].items():
        if md5(os.path.join(root, rel)) != want["md5"]:
            print("INSTALL VERIFY FAILED: %s" % rel)
            return 1
    if is_card:
        hw_loop.flush(a.drive)
    log("installed %d files to %s (verified); cycle 1 = %s run_id=%s"
        % (len(man["files"]), root, man["order"][0], run_id))
    return 0


# ----------------------------------------------------------------- run ----
def collect(root, dest, cycle, arm, run_id, man, drive, res, ini_text, froze):
    os.makedirs(dest, exist_ok=True)
    got = {}
    for sub in ("log", "handoff"):
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if not os.path.isfile(p) or n == "CMD.TXT":
                continue
            out = os.path.join(dest, n if sub == "log" else "handoff-" + n)
            if hw_loop.copy_retry(p, out, why="collect"):
                got[os.path.basename(out)] = md5(out)
                if sub == "log":
                    try:
                        os.remove(p)
                    except OSError:
                        pass
    eboot = os.path.join(root, "EBOOT.PBP")
    meta = {"drive": drive, "role": "solo", "arm": arm, "run_id": run_id,
            "cycle": cycle, "result": res or "unknown",
            "eboot_crc32": crc32(eboot) if os.path.isfile(eboot) else "unknown",
            "eboot_expected_crc32": man["files"]["EBOOT.PBP"]["crc32"],
            "staged_ini": staged_keys(ini_text), "collected": got,
            "t": time.strftime("%Y-%m-%d %H:%M:%S"), "froze_before": froze}
    with open(os.path.join(dest, "meta.json"), "w") as fh:
        json.dump(meta, fh, indent=1)
    return meta


def restore_golden(stage, root):
    src = os.path.join(stage, "base", "roms")
    done = []
    for n in sorted(os.listdir(src)):
        if n.lower().endswith((".sav", ".st0")):
            if hw_loop.copy_retry(os.path.join(src, n), os.path.join(root, "roms", n),
                                  why="golden"):
                done.append(n)
    return done


def cmd_run(a):
    man = load_manifest(a.stage)
    if not verify_stage(a.stage, man):
        return 2
    order = man["order"]
    root = hw_loop.card_root(a.drive)
    os.makedirs(a.logs, exist_ok=True)
    state_p = os.path.join(a.logs, "RIGSTATE.json")
    st = {"next": 0, "cycles": []}
    if os.path.isfile(state_p):
        with open(state_p) as fh:
            st = json.load(fh)
    real = len(a.drive) == 2 and a.drive[1] == ":"
    log("mirrig: %d cycles, %s, logs -> %s (resuming at %d)"
        % (len(order), root, a.logs, st["next"] + 1))
    while st["next"] < len(order):
        i = st["next"]
        arm = order[i]
        res_p = os.path.join(root, "handoff", "RESULT.TXT")
        ini_p = os.path.join(root, ".gpsp-harness.ini")
        log("cycle %d/%d: waiting for %s (RESULT.TXT)" % (i + 1, len(order), arm))
        t0, warned = time.time(), False
        while hw_loop.parse_result(res_p) is None:
            if not warned and time.time() - t0 > a.freeze_s:
                log("  NO RESULT after %ds -- cycle %d (%s) froze or hung. Owner: "
                    "hold POWER to switch off, power on, relaunch GBA MIRROR RIG. "
                    "The loop keeps waiting." % (a.freeze_s, i + 1, arm))
                with open(os.path.join(a.logs, "FREEZE-c%03d-%s.txt" % (i + 1, arm)),
                          "w") as fh:
                    fh.write("cycle=%d arm=%s no RESULT.TXT after %ds at %s\n"
                             % (i + 1, arm, a.freeze_s,
                                time.strftime("%Y-%m-%d %H:%M:%S")))
                warned = True
            if time.time() - t0 > a.timeout:
                log("TIMEOUT waiting for cycle %d -- stopping (resume later with "
                    "the same command)" % (i + 1))
                return 1
            time.sleep(a.poll)
        res = hw_loop.parse_result(res_p)
        try:
            with open(ini_p, encoding="utf-8") as fh:
                ini_text = fh.read()
        except OSError:
            ini_text = ""
        keys = staged_keys(ini_text)
        run_id = keys.get("run_id", "unknown")
        if keys.get("arm") != arm:
            log("  WARNING: card ini says arm=%s, plan says %s -- collecting under "
                "the card's value" % (keys.get("arm"), arm))
            arm = keys.get("arm", "unknown")
        if res.get("status") == "parked" and not warned:
            log("  console was PARKED -- sending RUN for %s (not counted)" % arm)
            more = True
        else:
            dest = os.path.join(a.logs, "auto%03d-%s-solo" % (i + 1, arm))
            meta = collect(root, dest, i + 1, arm, run_id, man, a.drive, res,
                           ini_text, warned)
            log("  collected %d files -> %s  status=%s exit=%s reason=%s frames=%s"
                % (len(meta["collected"]), os.path.basename(dest),
                   res.get("status"), res.get("exit"), res.get("reason"),
                   res.get("frames", "unknown")))
            st["cycles"].append({"cycle": i + 1, "arm": arm, "run_id": run_id,
                                 "froze": warned, "dir": os.path.basename(dest)})
            st["next"] = i + 1
            log("  golden restored: %s" % ", ".join(restore_golden(a.stage, root)))
            more = st["next"] < len(order)
            if more:
                nxt = order[st["next"]]
                nrid = "%d-%d" % (st["next"] + 1, int(time.time()))
                stage_arm(root, nxt, nrid)
                log("  staged cycle %d: %s run_id=%s" % (st["next"] + 1, nxt, nrid))
        with open(os.path.join(root, "handoff", "CMD.TXT"), "w") as fh:
            fh.write("RUN\n" if more else "STOP\n")
        try:
            os.remove(res_p)
        except OSError:
            pass
        with open(state_p, "w") as fh:
            json.dump(st, fh, indent=1)
        if real:
            hw_loop.eject(a.drive, attempts=3)
    log("all %d cycles collected. Report: python tools/gbamirror/mir_report.py "
        "--rig %s --golden <PPSSPP golden dir>" % (len(order), a.logs))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("stage")
    s.add_argument("--out", required=True)
    s.add_argument("--build", required=True, help="a GBA_LINK harness64 build dir")
    s.add_argument("--fx", required=True,
                   help="dir with aw2.gba/.sav/.st0, host30.txt, aw2_link_join.txt")
    s.add_argument("--bios", required=True)
    s.add_argument("--plan", default="p64", choices=sorted(PLANS))
    p = sp.add_parser("plan")
    p.add_argument("--stage", required=True)
    i = sp.add_parser("ini")
    i.add_argument("--arm", required=True, choices=[n for n, _e in ARM_TABLE])
    for name in ("install", "run"):
        q = sp.add_parser(name)
        q.add_argument("--stage", required=True)
        q.add_argument("--drive", required=True,
                       help="the console's MEMORY STICK drive letter, or a "
                            "directory standing in for one (PPSSPP dry run)")
        q.add_argument("--appdir", default=APPDIR)
        if name == "install":
            q.add_argument("--backup", default="")
            q.add_argument("--reinstall", action="store_true")
        else:
            q.add_argument("--logs", required=True)
            q.add_argument("--freeze-s", type=int, default=600)
            q.add_argument("--timeout", type=int, default=4 * 3600)
            q.add_argument("--poll", type=float, default=2.0)
    a = ap.parse_args()
    if getattr(a, "appdir", APPDIR) != APPDIR:
        print("REFUSING: --appdir must be exactly %s" % APPDIR)
        return 2
    hw_loop.APPDIR = APPDIR
    return {"stage": cmd_stage, "plan": cmd_plan, "install": cmd_install,
            "run": cmd_run, "ini": cmd_ini}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
