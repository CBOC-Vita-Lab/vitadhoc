#!/usr/bin/env bash
# run_tests.sh -- boot every test ROM on both host twins and compare: the
# CORES' test (the x86 dynarec against the ARM dynarec the Vita uses).  The
# app's own test -- the game list, the menu, states, fast forward... -- is
# app_tests.py.
#
#   ./run_tests.sh [ROMDIR] [FRAMES] [SHOT_FRAME]
#   ARMDIR=armhf-vitasim ./run_tests.sh [ROMDIR]    (after make ARCH=armhf VITASIM=1)
#
# For each *.gba / *.gb / *.gbc in ROMDIR (none when it is omitted), and the
# port's own testrom/gbadhoc-test.gba when it has been built (make -C
# testrom): run build/x86/gbadhoc_host (x86 dynarec) and
# build/armhf/gbadhoc_host under qemu-arm for FRAMES refreshes, write the
# composed picture at SHOT_FRAME to shots/<rom>.<arch>.png, and compare the
# two pictures, the two sound streams and the two sets of save files.  Each
# ROM gets its own scratch data root with an autoboot.txt naming it, so the
# game starts without the game list and saves do not leak between runs.
#
# The pictures are compared with the lowest green bit masked: the x86 stub
# copies bit 15 of a GBA palette entry into green's sixth bit and the ARM
# stub clears it (x86/x86_stub.S ext_store_palette16b vs arm/arm_stub.S
# ext_store_palette_u16), so "white" is 0xFFFF on one and 0xFFDF on the
# other.  Upstream behaviour, below what a GBA's 5-bit green can express.
# (The state previews, .thumb and .shot, carry the same bit and are left
# out of the save comparison for that reason.)
set -u
ROMDIR=${1:-}
FRAMES=${2:-600}
SHOT=${3:-300}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$HERE/shots"
QEMU=${QEMU:-qemu-arm -L /usr/arm-linux-gnueabihf}
# ARMDIR=armhf-vitasim runs the VITASIM build (the Vita's own translation-
# cache path over a simulated kernel) as the ARM side.
ARMDIR=${ARMDIR:-armhf}
OWN="$HERE/testrom/gbadhoc-test.gba"
# What the port's own test ROM is driven with: buttons, a battery write (A),
# SELECT+L = quick save, SELECT+R = quick load (the default bindings).
OWN_INPUT="100:A:5,150:UP+RIGHT:60,300:A:5,400:SELECT+L:3,500:A:5,560:SELECT+R:3,580:START+B:40"
fail=0

run_one() {   # arch root [extra args...]
  local arch=$1 root=$2; shift 2
  local run="" dir=$arch
  [ "$arch" = armhf ] && { run="$QEMU"; dir=$ARMDIR; }
  $run "$HERE/build/$dir/gbadhoc_host" --root "$root" "$@" > "$root/out.txt" 2>&1
  echo $? > "$root/rc"
}

printf '%-16s %-10s %-10s %-9s %-9s %-9s %s\n' rom x86 armhf picture sound save note
ROMS=("$OWN")
[ -n "$ROMDIR" ] && ROMS=("$ROMDIR"/*.gba "$ROMDIR"/*.gb "$ROMDIR"/*.gbc "$OWN")
for rom in "${ROMS[@]}"; do
  [ -f "$rom" ] || continue
  name=$(basename "$rom")
  input=(); [ "$rom" = "$OWN" ] && input=(--input "$OWN_INPUT")
  for arch in x86 armhf; do
    root="$WORK/$name.$arch"
    mkdir -p "$root/roms"
    cp "$rom" "$root/roms/"
    echo "$name" > "$root/autoboot.txt"
    run_one $arch "$root" --frames "$FRAMES" --shot "$SHOT:$root/shot.png" \
        --wav "$root/audio.wav" "${input[@]}"
  done
  python3 -W ignore - "$WORK/$name" "$HERE/shots/${name%.*}" <<'PY'
import glob, os, sys, zlib
from PIL import Image
base, out = sys.argv[1], sys.argv[2]
res = {}
for arch in ("x86", "armhf"):
    root = f"{base}.{arch}"
    rc = open(f"{root}/rc").read().strip()
    try:
        im = Image.open(f"{root}/shot.png").convert("RGB")
        im.save(f"{out}.{arch}.png", optimize=True)
    except Exception:
        im = None
    wav = open(f"{root}/audio.wav", "rb").read() if rc == "0" else b""
    # battery saves and states sit beside the ROM, as on the PSP
    sav = b"".join(open(f, "rb").read()
                   for f in sorted(glob.glob(f"{root}/roms/*.sav") +
                                   glob.glob(f"{root}/roms/*.st[0-4]")))
    res[arch] = (rc, im, zlib.crc32(wav[44:]), len(wav), zlib.crc32(sav), len(sav))
def masked(im):
    return bytes(b & m for px in im.getdata() for b, m in zip(px, (0xFF, 0xFB, 0xFF)))
a, b = res["x86"], res["armhf"]
pic = "missing"
if a[1] and b[1]:
    pic = "same" if masked(a[1]) == masked(b[1]) else "DIFFER"
snd = "same" if a[2:4] == b[2:4] else "DIFFER"
sav = ("same" if a[5] else "none") if a[4:6] == b[4:6] else "DIFFER"
blank = a[1] and len(set(a[1].getdata())) == 1
name = os.path.basename(out)
print(f"{name:<16} rc={a[0]:<7} rc={b[0]:<7} {pic:<9} {snd:<9} {sav:<9} "
      f"{'blank picture' if blank else ''}")
sys.exit(0 if a[0] == b[0] == "0" and pic == "same" and snd == "same"
         and sav != "DIFFER" else 1)
PY
  [ $? = 0 ] || fail=1
done

# The link pair on one console (vita.ini link_bench = 1): instance B beside
# instance A from the first state load on, the player's buttons on both.
# Every frame of both machines is hashed; the trace must be the same on the
# two CPU backends, and the two machines must agree with each other on
# everything but the I/O registers (each end of the cable has its own id).
if [ -f "$OWN" ]; then
  for arch in x86 armhf; do
    root="$WORK/linkbench.$arch"
    mkdir -p "$root/roms"
    cp "$OWN" "$root/roms/"
    echo "$(basename "$OWN")" > "$root/autoboot.txt"
    echo "link_bench = 1" > "$root/vita.ini"
    run_one $arch "$root" --frames 1300 \
        --input "100:SELECT+L:3,110:SELECT+R:3,300:A:5,500:UP+B:100,900:A:4"
  done
  python3 - "$WORK/linkbench" <<'PY'
import re, sys
base = sys.argv[1]
traces = {}
for arch in ("x86", "armhf"):
    try:
        traces[arch] = open(f"{base}.{arch}/log/linkhash.txt").read()
    except OSError:
        traces[arch] = ""
rows = {}
for line in traces["armhf"].splitlines():
    m = re.match(r"s=(\d) f=(\d+) (.*)", line)
    rows.setdefault(int(m.group(2)), {})[int(m.group(1))] = dict(
        kv.split("=") for kv in m.group(3).split())
both = [r for r in rows.values() if len(r) == 2]
agree = sum(1 for r in both
            if all(r[0][k] == r[1][k] for k in r[0] if k != "io"))
same = bool(traces["x86"]) and traces["x86"] == traces["armhf"]
print(f"link bench: {len(both)} frames with both machines; A and B agree "
      f"(all but io) on {agree}; x86 and armhf traces "
      f"{'IDENTICAL' if same else 'DIFFER'} "
      f"({len(traces['armhf'].splitlines())} lines)")
sys.exit(0 if same and both and agree == len(both) else 1)
PY
  [ $? = 0 ] || fail=1
fi
exit $fail
