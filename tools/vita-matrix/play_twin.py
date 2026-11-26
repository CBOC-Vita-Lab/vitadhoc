#!/usr/bin/env python3
"""play.py GAME ROMFILE "script" [--from N] [--save N] [--every F] [--frames F]
Twin play session in /root/vad/play/GAME/root. Starts from checkpoint N (a copy of a
quick save), runs the script (plat_host syntax, frames relative to start), takes a
screenshot every F frames, quick-saves at the end (SELECT+L) and keeps it as checkpoint --save."""
import argparse, glob, os, shutil, subprocess, sys, zlib
X = "os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../gpsp/vita/hostcore/build/x86/gbadhoc_host")"
ap = argparse.ArgumentParser()
ap.add_argument("game"); ap.add_argument("rom"); ap.add_argument("script")
ap.add_argument("--from", dest="frm", type=int, default=0)
ap.add_argument("--save", type=int, default=0)
ap.add_argument("--every", type=int, default=600)
ap.add_argument("--frames", type=int, default=0)
a = ap.parse_args()
base = os.path.join(os.environ.get("PLAY_DIR", "play"), a.game)
root = base + "/root"
os.makedirs(root + "/roms", exist_ok=True)
rom = os.path.basename(a.rom)
if not os.path.exists(root + "/roms/" + rom):
    shutil.copy(a.rom, root + "/roms/" + rom)
stem = rom.rsplit(".", 1)[0]
bk = "%02x" % (zlib.crc32(stem.encode()) >> 24)
ext = "gb" if rom.lower().endswith(".gb") else ("gbc" if rom.lower().endswith(".gbc") else "gba")
sdir = "%s/states/%s/%s" % (root, ext, bk)
os.makedirs(sdir, exist_ok=True)
open(root + "/config.ini", "w").write("hints = 0\nui_home = 0\nwide_gba = 0\nquality_gba = 1\nstick_rewind = 1\nbind_screenshot = L+R+SELECT\n")
ab = rom + "\n"
if a.frm:
    shutil.copy("%s/cp%02d.st0" % (base, a.frm), "%s/%s.st0" % (sdir, stem))
    ab += "slot=1\n"
open(root + "/autoboot.txt", "w").write(ab)
shots = "%s/shots" % base
shutil.rmtree(shots, ignore_errors=True); os.makedirs(shots)
# the script: absolute frames (from the run's start; the state loads at ~frame 10)
ents = [l.strip() for l in a.script.replace(",", "\n").splitlines() if l.strip()]
end_f = a.frames or 3600
ents += ["%d:shot=s%05d.png" % (f, f) for f in range(60, end_f + 1, a.every)]
ents.sort(key=lambda l: int(l.split(":", 1)[0]))
script = "\n".join(ents) + "\n%d:select+l:3\n%d:quit\n" % (end_f + 5, end_f + 60)
open(base + "/script.txt", "w").write(script)
r = subprocess.run([X, "--root", root, "--shot-dir", shots, "--input-file", base + "/script.txt"],
                   capture_output=True, text=True, timeout=1800)
log = open(root + "/log/gbadhoc.log").read() if os.path.exists(root + "/log/gbadhoc.log") else ""
ok = "state_save slot=1" in log.split("EVT autoboot")[-1]
if a.save and ok:
    shutil.copy("%s/%s.st0" % (sdir, stem), "%s/cp%02d.st0" % (base, a.save))
print("rc", r.returncode, "saved" if ok else "NOT saved", "shots:", len(glob.glob(shots + "/*.png")))
print(r.stderr[-400:])
