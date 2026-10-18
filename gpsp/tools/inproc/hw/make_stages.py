#!/usr/bin/env python3
"""make_stages.py -- hw_loop arm directories for the in-process return soak
(docs/INPROC-SOAK.md), one set per console.

    python make_stages.py --out <dir> --fx <fixtures> --builds <dir> \
        --backup1000 <freezerig backup> --backup3000 <freezerig backup>

Writes <out>/<model>/<arm>/ for model 1000 (role join, harness build, 32 MiB
layout) and 3000 (role host, harness64 build), plus <out>/<model>/golden/
(the GB-link rig's own top-level saves, restored by hw_loop after every run so
this soak never leaves them changed).

Every soak file lives in roms/soak/ under an s_ name, so nothing the GB/GBA
link rig keeps in roms/ is overwritten and the browser lists the soak games
apart from the rig's.

Arms
  S1  100 in-process returns, harness quit, slot-1 state at frame 30,
      aw2 -> red -> emerald -> crystal -> hns (x20, 300 frames each).
      Also installs the EBOOT, the ME + HOME prx and the soak ROMs.
  S2  the same, reverse order (hns first: a fresh-boot H&S session).
  U   30 returns through the real UI: the browser's state shelf (LEFT, X on
      slot 1) picks each game, the in-game menu's own Quit to game list ends
      it (inproc_via_menu); emits `ui_return fade_done ms=` per return.
  Fr Fe Fc Fh   fresh boots of red / emerald / crystal / hns: the identity
      references (aw2's is S1's session 0, hns's also S2's session 0).
  X   (3000 only, follow-up chain `XZ`) jit_small = 1: the SMALL JIT tier
      leaves a real PSP-3000 room to hold H&S resident (32 blocks), which the
      LARGE tier does not; 60 returns hns>aw2>red>emerald>crystal, + the EBOOT.
  Z   restores the rig's pre-soak EBOOT, ME prx, CONFIG.INI and harness ini
      from the backup; staged by the final cycle, which sends STOP (park).
"""
import argparse
import os
import shutil

GAMES = [  # (soak name, rom, sav or None, st0)
    ("s_aw2.gba", "aw2.gba", "aw2.sav", "aw2.st0"),
    ("s_red.gb", "red.gb", None, "red.gb.st0"),
    ("s_emerald.gba", "emerald.gba", "emerald_3000.sav", "emerald_3000.st0"),
    ("s_crystal.gbc", "crystal.gbc", None, "crystal.gbc.st0"),
    ("s_hns.gba", "hns.gba", "hns.sav", "heart_soul_heavy.st0"),
]
ORDER_S1 = ["s_aw2.gba", "s_red.gb", "s_emerald.gba", "s_crystal.gbc", "s_hns.gba"]
ORDER_S2 = list(reversed(ORDER_S1))

COMMON = """heartbeat_s = 5
stall_watch_s = 15
log_input = 0
shash = 1
vhash = 1
me_mode = 1
handoff = 1
handoff_window_s = 90
handoff_max_runs = 100000
handoff_total_s = 900
handoff_park_s = 0
"""

CONFIG = """group = GPSP07
theme = 0
ui_shell = 1
console = 0
menu_button = home
me_mode = 1
me_boot = 1
show_fps = 0
"""

# The UI arm's browser script.  g_bframe (the browser loop's clock) runs on
# across visits and stops while a game runs, so visit k occupies frames
# [k*L+1, (k+1)*L]: move at +40 (after the 16-frame crossfade, which eats
# input), open the shelf at +70, load slot 1 at +L.  The GBA list on the card
# is aw2, msc (the link rig's), s_aw2, s_emerald, s_hns.  CONFIG.INI names no
# last_rom (PPSSPP upper-cases names, the card may not, so a name could match
# on one and not the other): the first visit opens on row 0 and steps DOWN
# twice to s_aw2; after that the browser reopens on the game just played and
# the moves D D U U ping-pong over the three soak games, never reaching the
# rig's own.
UI_L = 110
UI_CYCLES = 30


def ui_script(cycles):
    moves = ["DOWN", "DOWN", "UP", "UP"]
    lines = ["# inproc soak UI arm: shelf load + menu quit, L=%d" % UI_L]
    for k in range(cycles + 1):
        base = k * UI_L
        if k:
            lines.append("%d press %s" % (base + 40, moves[(k - 1) % 4]))
        else:
            lines.append("20 press DOWN")
            lines.append("40 press DOWN")
        lines.append("%d press LEFT" % (base + 70))
        lines.append("%d press CROSS" % (base + UI_L))
    return "\n".join(lines) + "\n"


def wr(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as fh:
        fh.write(text)


def cp(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def stage_fixture_resets(d, fx):
    """Saves and states every arm starts from (small; ROMs only in S1)."""
    for soak, _rom, sav, st0 in GAMES:
        # GBA sidecars replace the extension (aw2.st0), GB/GBC append it
        # (red.gb.st0) -- psp_state_slot1_suffix.
        gba = soak.endswith(".gba")
        stem = soak[:-4] if gba else soak
        if sav:
            cp(os.path.join(fx, sav),
               os.path.join(d, "roms", "soak", stem + ".sav"))
        cp(os.path.join(fx, st0), os.path.join(d, "roms", "soak", stem + ".st0"))


def arm(d, role, model, name, desc, keys, fx, extra=None):
    ini = "# inproc soak (claude/inproc-soak), PSP-%s, arm %s: %s\n" % (
        model, name, desc)
    ini += "# run_id and arm are appended by hw_loop.\n" + keys + COMMON
    wr(os.path.join(d, role + "-.gpsp-harness.ini"), ini)
    wr(os.path.join(d, "CONFIG.INI"), CONFIG)
    stage_fixture_resets(d, fx)
    for k, v in (extra or {}).items():
        wr(os.path.join(d, k), v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--fx", required=True)
    ap.add_argument("--builds", required=True)
    ap.add_argument("--backup1000", required=True)
    ap.add_argument("--backup3000", required=True)
    ap.add_argument("--only", default="",
                    help="comma list of arm dirs to (re)write, leaving the rest "
                         "alone (a running hw_loop reads them)")
    a = ap.parse_args()
    only = set(x for x in a.only.split(",") if x)
    for model, role, build, backup in (("1000", "join", "harness", a.backup1000),
                                       ("3000", "host", "harness64", a.backup3000)):
        root = os.path.join(a.out, model)
        if only:
            if model == "3000" and "X" in only:
                d = os.path.join(root, "X")
                if os.path.isdir(d):
                    shutil.rmtree(d)
                order = ["s_hns.gba", "s_aw2.gba", "s_red.gb", "s_emerald.gba",
                         "s_crystal.gbc"]
                arm(d, role, model, "X", "jit_small=1 (H&S resident), 60 "
                    "returns hns>aw2>red>emerald>crystal, harness quit, states",
                    "inproc_roms = %s\njit_small = 1\ninproc_cycles = 60\n"
                    "inproc_frames = 300\nautoexit_frames = 301\nload_state = 1\n"
                    % "|".join("soak/" + g for g in order), a.fx)
                bd = os.path.join(a.builds, build)
                for f in ("EBOOT.PBP", "gbadhoc_me.prx", "gbadhoc_home.prx"):
                    cp(os.path.join(bd, f), os.path.join(d, f))
                print("3000: X")
            continue
        if os.path.isdir(root):
            shutil.rmtree(root)
        rl = lambda o: "inproc_roms = %s\n" % "|".join("soak/" + g for g in o)
        soak = ("inproc_cycles = 100\ninproc_frames = 300\nautoexit_frames = 301\n"
                "load_state = 1\n")
        # S1: + the install
        d = os.path.join(root, "S1")
        arm(d, role, model, "S1", "100 returns aw2>red>emerald>crystal>hns, "
            "harness quit, states", rl(ORDER_S1) + soak, a.fx)
        bd = os.path.join(a.builds, build)
        for f in ("EBOOT.PBP", "gbadhoc_me.prx", "gbadhoc_home.prx"):
            cp(os.path.join(bd, f), os.path.join(d, f))
        for soakname, rom, _s, _t in GAMES:
            cp(os.path.join(a.fx, rom), os.path.join(d, "roms", "soak", soakname))
        arm(os.path.join(root, "S2"), role, model, "S2",
            "100 returns hns>crystal>emerald>red>aw2, harness quit, states",
            rl(ORDER_S2) + soak, a.fx)
        arm(os.path.join(root, "U"), role, model, "U",
            "%d returns via the browser state shelf + the menu's Quit"
            % UI_CYCLES,
            "browser = 1\nui_browser_script = bscript.txt\nui_theme_black = 0\n"
            "inproc_via_menu = 1\ninproc_cycles = %d\ninproc_frames = 300\n"
            "autoexit_frames = 301\n" % UI_CYCLES, a.fx,
            extra={"bscript.txt": ui_script(UI_CYCLES)})
        for tag, g in (("Fr", "s_red.gb"), ("Fe", "s_emerald.gba"),
                       ("Fc", "s_crystal.gbc"), ("Fh", "s_hns.gba")):
            arm(os.path.join(root, tag), role, model, tag, "fresh boot of " + g,
                "inproc_roms = soak/%s\ninproc_cycles = 0\nautoexit_frames = 301\n"
                "load_state = 1\n" % g, a.fx)
        # Z: the rig as it was
        z = os.path.join(root, "Z")
        app = os.path.join(backup, "files", "PSP", "GAME", "GBADHOC-GBLINK")
        for f in ("EBOOT.PBP", "gbadhoc_me.prx", "CONFIG.INI"):
            cp(os.path.join(app, f), os.path.join(z, f))
        with open(os.path.join(app, ".gpsp-harness.ini"), encoding="utf-8") as fh:
            lines = fh.read().splitlines()
        # drop the trailing run_id/arm hw_loop appended; it appends Z's own
        keep = [l for l in lines if not l.startswith(("run_id =", "arm ="))]
        while keep and not keep[-1].strip():
            keep.pop()
        wr(os.path.join(z, role + "-.gpsp-harness.ini"), "\n".join(keep) + "\n")
        # golden: the rig's own top-level saves (+ .bak), restored every cycle
        g = os.path.join(root, "golden")
        os.makedirs(g, exist_ok=True)
        for f in sorted(os.listdir(os.path.join(app, "roms"))):
            if f.endswith((".sav", ".sav.bak")):
                cp(os.path.join(app, "roms", f), os.path.join(g, role + "-" + f))
        print("%s: %s" % (model, ", ".join(sorted(os.listdir(root)))))


if __name__ == "__main__":
    main()
