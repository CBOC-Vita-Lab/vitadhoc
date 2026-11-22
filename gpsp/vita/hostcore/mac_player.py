#!/usr/bin/env python3
"""mac_player.py -- the aarch64 (or any) twin as the SECOND PLAYER against a real Vita.

The twin has no screen watcher and no keyboard: it is driven by a scripted pad (see
plat_host.c, THE SCRIPT).  This tool writes a data root and that script for one of the
four ways a second player takes part, starts the twin in real time, and leaves a log and
pictures behind.  Run it in the Linux VM on the Mac (see docs/TESTING.md, "Second player
on a Mac"); it needs only python3 (and Pillow, which link_tests.py imports).

    mac_player.py --exe ./gbadhoc_host-aarch64 --rom game.gba --join ABCDE
        Online: the Vita is hosting (Play together > Online) and shows the 5 character code.
    mac_player.py ... --join-lan --ip 192.168.1.50
        Same Wi-Fi: the Vita is hosting a Link Cable / Game Boy / Wireless Adapter room.
    mac_player.py ... --host-online
        The twin hosts Online and prints the code (the Vita joins it with the code).
    mac_player.py ... --host-lan --ip 192.168.1.50
        The twin hosts a Same Wi-Fi room (the Vita joins it from its Join list).

    --rom       the game; it must be the SAME game the Vita plays (Link Cable carries the
                host's cartridge across, a Game Boy / NES / WonderSwan link needs the file)
    --root DIR  the data root (default ./mac-root): roms, saves, log/gbadhoc.log, pictures
    --ip, --mask  the VM's LAN address for the Same Wi-Fi modes (the twin's stand-in for the
                console's Wi-Fi; the VM's network must be BRIDGED to the Wi-Fi, same subnet)
    --play SEC  after the link forms, press a pattern for SEC seconds (default 60), then
                disconnect and quit.  --pattern "cross,right,cross,left" is the pad's turn.
    (The version string link rooms compare is fixed when the twin is BUILT: HOST_VERSION=.)
    --qemu      run under qemu-aarch64 (testing on an x86 machine)
Everything the twin logs goes to ROOT/log/gbadhoc.log; the pictures to ROOT/shots/.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import link_tests as L          # noqa: E402  (the script helpers: open_play, keys, join_first ...)
import online_tests as O        # noqa: E402  (the code pad: join_script)

PRESS, GAP = L.PRESS, L.GAP


def pattern_script(names, seconds):
    """`seconds` of the pad's turn: each name pressed in turn, every half second."""
    out, n = "", int(seconds * 2)
    for i in range(n):
        out += "+%d:%s:%d\n" % (30 - PRESS if i else 30, names[i % len(names)], PRESS)
    return out


def build_script(a):
    names = [x.strip() for x in a.pattern.split(",") if x.strip()]
    play = pattern_script(names, a.play)
    # Disconnect from the game's menu (Play together, second row), then quit
    end = ("+60:start+select:%d\n" % L.CHORD + L.keys("down", "down", "cross") +
           L.keys("down", "cross", first=L.AFTER_SHOT) + "+%d:quit\n" % L.dur(3))
    if a.join:
        # the game's menu, Join a game, the code on the pad, the card, X to join
        return O.join_script(a.join.upper(), "", card_shot="card.png") + play + end
    if a.join_lan:
        return L.join_first(L.EARLY, wait=a.wait, shot="join_list.png") + play + end
    if a.host_online or a.host_lan:
        return L.host(L.EARLY, "host.png") + play + end
    sys.exit("choose one of --join CODE, --join-lan, --host-online, --host-lan")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--root", default="mac-root")
    ap.add_argument("--join", metavar="CODE")
    ap.add_argument("--join-lan", action="store_true")
    ap.add_argument("--host-online", action="store_true")
    ap.add_argument("--host-lan", action="store_true")
    ap.add_argument("--ip", default="")
    ap.add_argument("--mask", default="255.255.255.0")
    ap.add_argument("--nick", default="Mac")
    ap.add_argument("--wait", type=float, default=4.0, help="seconds the Join list gets to fill")
    ap.add_argument("--play", type=float, default=60)
    ap.add_argument("--pattern", default="cross,right,cross,left")
    ap.add_argument("--bios", default="", help="gba_bios.bin (must be the same file as on the Vita, if it has one)")
    ap.add_argument("--qemu", action="store_true")
    ap.add_argument("--extra", default="", help="more twin arguments")
    a = ap.parse_args()
    if (a.join_lan or a.host_lan) and not a.ip:
        sys.exit("--ip: this machine's address on the Wi-Fi's subnet is needed for Same Wi-Fi rooms")
    root = os.path.abspath(a.root)
    shutil.rmtree(os.path.join(root, "log"), ignore_errors=True)
    os.makedirs(os.path.join(root, "roms"), exist_ok=True)
    os.makedirs(os.path.join(root, "shots"), exist_ok=True)
    rom = os.path.basename(a.rom)
    shutil.copy(a.rom, os.path.join(root, "roms", rom))
    if a.bios:
        shutil.copy(a.bios, os.path.join(root, "gba_bios.bin"))
    open(os.path.join(root, "autoboot.txt"), "w").write(rom + "\n")
    online = a.join or a.host_online
    cfg = {"nick": a.nick, "quality_gba": 1, "quality_gb": 1, "quality_gbc": 1}
    if online:
        cfg["net_conn"] = 3            # Settings > Play together: Online
    with open(os.path.join(root, "config.ini"), "w") as f:
        f.write("".join("%s = %s\n" % kv for kv in cfg.items()))
    with open(os.path.join(root, "nethost.ini"), "w") as f:
        f.write("wifi = 1\nssid = MacVM\nip = %s\nmask = %s\n" % (a.ip or "10.0.2.15", a.mask))
    script = os.path.join(root, "script.txt")
    open(script, "w").write(build_script(a))
    cmd = (["qemu-aarch64"] if a.qemu else []) + [os.path.abspath(a.exe), "--root", root, "--realtime",
                                                  "--shot-dir", os.path.join(root, "shots"),
                                                  "--input-file", script] + a.extra.split()
    print("starting:", " ".join(cmd), flush=True)
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, env=dict(os.environ, TZ="UTC"))
    log = os.path.join(root, "log", "gbadhoc.log")
    seen, said = 0, False
    try:
        while p.poll() is None:
            time.sleep(0.5)
            try:
                txt = open(log, errors="replace").read()
            except OSError:
                continue
            m = re.search(r"online_room code=([A-Z0-9]{5})", txt)
            if a.host_online and m and not said:
                print("ROOM CODE: " + m.group(1) + "  (type it on the Vita: Join a game)", flush=True)
                said = True
            for line in txt.splitlines()[seen:]:
                if re.search(r"online_stage|link_action|peer_connected|join_refused|gblink_create|link_ended|link_net_up", line):
                    print("  " + line.strip()[:160], flush=True)
            seen = len(txt.splitlines())
    except KeyboardInterrupt:
        p.terminate()
    rc = p.wait()
    txt = open(log, errors="replace").read() if os.path.exists(log) else ""
    ok = bool(re.search(r"peer_connected|link_started|gblink_create", txt))
    print("twin exit %d; link %s; log %s" % (rc, "FORMED" if ok else "NOT formed", log))
    sys.exit(0 if ok and rc == 0 else 1)


if __name__ == "__main__":
    main()
