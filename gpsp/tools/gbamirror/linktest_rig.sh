#!/bin/bash
# linktest_rig.sh -- everything a dynarec backend must show before it carries
# the link cable, on the homebrew test ROM (tools/linktest) and optionally on
# public CPU test ROMs.  One build per call; the same script runs the x86
# build, the ARM build under qemu-arm, and the ARM far-cache builds.
#
#   linktest_rig.sh OUTDIR FRAMES [options] -- GL_HOST_COMMAND...
#
#   options:  --jobs N        runs at a time (default 2)
#             --cpu-roms DIR  also run the yield test on DIR/*.gba, which
#                             never touch the cable: --solo against
#                             --yield-every C must be identical
#             --quick         skip the lookahead and yield sweeps
#             --flagprobe     also run the yield and lookahead comparisons on
#                             linktest_flagprobe.gba, whose interrupt entry
#                             records the flags it is entered with
#
#   e.g.  ./linktest_rig.sh /tmp/lt-x86 20000 -- ./gl_host
#         ./linktest_rig.sh /tmp/lt-arm 20000 -- \
#               qemu-arm -L /usr/arm-linux-gnueabihf ./gl_host_arm
#         GL_JIT=split ./linktest_rig.sh /tmp/lt-split 20000 -- \
#               qemu-arm -L /usr/arm-linux-gnueabihf ./gl_host_armfar
#
# What it runs (docs/GBA-LINK-MIRROR.md sections 2 and 4, on our own ROM):
#   pair       both machines, FRAMES frames, the two input scripts; the ROM's
#              own verdict (tools/linktest/check.py): every phase passes on
#              both, transfers counted on both ports, bus overflow 0
#   rerun      the same again                         -> hashes IDENTICAL
#   swap       --swap (slot 0 on instance B)          -> IDENTICAL
#   la*        --lookahead 2464 / 5000 / 10000        -> IDENTICAL to default
#   keys       slot 1 with a different input script   -> must DIFFER, and pass
#   yield      slot 0 alone through the link loop, never stopped (yref: the
#              reference tools/gbamirror/yieldtest.sh uses), against stopped
#              every 37 / 300 / 1000 / 5000 cycles -> IDENTICAL
#   solo       --solo: the same machine stepped by retro_run with no link
#              code at all.  Reported, not required: with the cable attached
#              the SIO registers read differently, and linktest looks at them
#   cpu        (--cpu-roms) yield and solo again on ROMs that never use the
#              cable; there --solo must be IDENTICAL too, apart from the
#              io= field (entering link mode rewrites the read-only cable
#              bits of SIOCNT and RCNT)
#   fp-*       (--flagprobe) yield and lookahead on the flag-probe ROM
# A comparison that fails says how many frame lines differ and how many of
# those differ in the register hash (r=) alone.
# Exit status 0 only if every line of OUTDIR/summary.txt says PASS.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LT=$HERE/../linktest
ROM=$LT/linktest.gba

OUT=$1; FRAMES=$2; shift 2
JOBS=2; CPUDIR=; QUICK=0; FLAGPROBE=0
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  case "$1" in
    --jobs) JOBS=$2; shift 2 ;;
    --cpu-roms) CPUDIR=$2; shift 2 ;;
    --quick) QUICK=1; shift ;;
    --flagprobe) FLAGPROBE=1; shift ;;
    *) echo "bad option $1" >&2; exit 2 ;;
  esac
done
shift
GL=("$@")
[ ${#GL[@]} -gt 0 ] || { echo "no gl_host command" >&2; exit 2; }
mkdir -p "$OUT" "$OUT/nobios"
# An empty BIOS directory: the core falls back to the open-source BIOS it
# embeds (bios/open_gba_bios.bin).
BIOS=$OUT/nobios
PEEK="--peek 0x02000000:28 --peek 0x02000080:16 --peek 0x020000c0:16"

run() {   # name, then gl_host arguments; stdout -> OUT/name.out
  local name=$1; shift
  nice -n 10 "${GL[@]}" --rom "$ROM" --bios-dir "$BIOS" "$@" \
      > "$OUT/$name.out" 2> "$OUT/$name.err"
  echo $? > "$OUT/$name.rc"
}
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done; }

PAIR=(--frames "$FRAMES" --script0 "$LT/keys0.txt" --script1 "$LT/keys1.txt" $PEEK)
SOLO=(--frames "$FRAMES" --script0 "$LT/keys0.txt" $PEEK)

throttle; run pair   "${PAIR[@]}" --hash "$OUT/pair.hash" &
throttle; run rerun  "${PAIR[@]}" --hash "$OUT/rerun.hash" &
throttle; run swap   "${PAIR[@]}" --swap --hash "$OUT/swap.hash" &
throttle; run keys   --frames "$FRAMES" --script0 "$LT/keys0.txt" \
                     --script1 "$LT/keys1b.txt" $PEEK --hash "$OUT/keys.hash" &
if [ $QUICK = 0 ]; then
  for la in 2464 5000 10000; do
    throttle; run la$la "${PAIR[@]}" --lookahead $la --hash "$OUT/la$la.hash" &
  done
  throttle; run yref "${SOLO[@]}" --yield-every 1000000000 --hash "$OUT/yref.hash" &
  throttle; run solo "${SOLO[@]}" --solo --hash "$OUT/solo.hash" &
  for c in 37 300 1000 5000; do
    throttle; run y$c "${SOLO[@]}" --yield-every $c --hash "$OUT/y$c.hash" &
  done
fi
if [ $FLAGPROBE = 1 ]; then
  FP=(--rom "$LT/linktest_flagprobe.gba" --frames "$FRAMES" --script0 "$LT/keys0.txt")
  # (a later --rom wins in gl_host's argument loop)
  throttle; run fp-yref "${FP[@]}" --yield-every 1000000000 --hash "$OUT/fp-yref.hash" &
  for c in 37 300 5000; do
    throttle; run fp-y$c "${FP[@]}" --yield-every $c --hash "$OUT/fp-y$c.hash" &
  done
  throttle; run fp-pair "${FP[@]}" --script1 "$LT/keys1.txt" $PEEK --hash "$OUT/fp-pair.hash" &
  throttle; run fp-la2464 "${FP[@]}" --script1 "$LT/keys1.txt" --lookahead 2464 --hash "$OUT/fp-la2464.hash" &
fi
CPUROMS=()
if [ -n "$CPUDIR" ]; then
  for r in "$CPUDIR"/*.gba; do
    n=$(basename "$r" .gba)
    CPUROMS+=("$n")
    cpu() {   # name, extra args
      local name=$1; shift
      nice -n 10 "${GL[@]}" --rom "$r" --bios-dir "$BIOS" --frames 600 \
          --screen-hash "$@" > "$OUT/$name.out" 2> "$OUT/$name.err"
      echo $? > "$OUT/$name.rc"
    }
    throttle; cpu cpu-$n-solo --solo --hash "$OUT/cpu-$n-solo.hash" &
    throttle; cpu cpu-$n-yref --yield-every 1000000000 --hash "$OUT/cpu-$n-yref.hash" &
    for c in 37 300 1000 5000; do
      throttle; cpu cpu-$n-y$c --yield-every $c --hash "$OUT/cpu-$n-y$c.hash" &
    done
  done
fi
wait

S=$OUT/summary.txt
: > "$S"
line() { printf '%-5s %-14s %s\n' "$1" "$2" "$3" | tee -a "$S"; }
ran() { [ "$(cat "$OUT/$1.rc" 2>/dev/null)" = 0 ]; }
verdict() {   # name, check.py options
  local name=$1; shift
  if ! ran "$name"; then line FAIL "$name" "gl_host exit $(cat "$OUT/$name.rc" 2>/dev/null)"; return; fi
  if python3 "$LT/check.py" "$@" < "$OUT/$name.out" > "$OUT/$name.check"; then
    line PASS "$name" "$(grep -c '^ok' "$OUT/$name.check") checks; $(grep '^slot 0' "$OUT/$name.out" | sed 's/.*\(mp=.*\)/slot0 \1/') $(grep '^slot 1' "$OUT/$name.out" | sed 's/.*\(mp=.*\)/slot1 \1/') $(grep -o 'overflow=[0-9]*' "$OUT/$name.out") $(grep -o 'wall=.*' "$OUT/$name.out")"
  else
    line FAIL "$name" "$(grep '^FAIL' "$OUT/$name.check" | head -3 | tr '\n' ';')"
  fi
}
differ() {   # two hash files -> "N of M frame lines differ, K in r= only"
  python3 - "$1" "$2" <<'PY'
import sys
a = open(sys.argv[1]).read().split("\n")
b = open(sys.argv[2]).read().split("\n")
n = ronly = 0
first = ""
for x, y in zip(a, b):
    if x == y:
        continue
    n += 1
    if not first:
        first = x.split(" r=")[0]
    fx = x.split()
    fy = y.split()
    d = [i for i in range(min(len(fx), len(fy))) if fx[i] != fy[i]]
    if len(fx) == len(fy) and d and all(fx[i].startswith("r=") for i in d):
        ronly += 1
n += abs(len(a) - len(b))
print("%d of %d frame lines differ (%d only in r=), first at %s" %
      (n, max(len(a), len(b)) - 1, ronly, first))
PY
}
same() {   # name, reference, [sed expression applied to both first]
  if ! ran "$1" || ! ran "$2"; then line FAIL "$1" "did not run"; return; fi
  local a="$OUT/$1.hash" b="$OUT/$2.hash" note=""
  if [ -n "${3:-}" ]; then
    sed "$3" "$a" > "$OUT/$1.cmp"; sed "$3" "$b" > "$OUT/$1.ref"
    a="$OUT/$1.cmp"; b="$OUT/$1.ref"; note=" apart from io="
  fi
  if cmp -s "$a" "$b" && [ -s "$a" ]; then
    line PASS "$1" "IDENTICAL to $2$note ($(wc -l < "$a") frame lines)"
  else
    line FAIL "$1" "differs from $2: $(differ "$a" "$b")"
  fi
}

verdict pair --frames "$FRAMES"
same rerun pair
same swap pair
if ! ran keys; then line FAIL keys "did not run"
elif cmp -s "$OUT/keys.hash" "$OUT/pair.hash"; then line FAIL keys "other buttons, same hashes"
else verdict keys --frames "$FRAMES"; fi
if [ $QUICK = 0 ]; then
  for la in 2464 5000 10000; do same la$la pair; done
  verdict yref --solo
  for c in 37 300 1000 5000; do same y$c yref; done
  if cmp -s "$OUT/solo.hash" "$OUT/yref.hash"; then line INFO solo "IDENTICAL to yref"
  else line INFO solo "differs from yref (expected: the cable changes what the SIO registers read): $(differ "$OUT/solo.hash" "$OUT/yref.hash")"; fi
fi
if [ $FLAGPROBE = 1 ]; then
  for c in 37 300 5000; do same fp-y$c fp-yref; done
  verdict fp-pair --frames "$FRAMES"
  same fp-la2464 fp-pair
fi
for n in "${CPUROMS[@]}"; do
  if ran cpu-$n-solo; then
    line PASS cpu-$n "$(grep '^screen s=0' "$OUT/cpu-$n-solo.out")"
  else
    line FAIL cpu-$n "gl_host exit $(cat "$OUT/cpu-$n-solo.rc")"
  fi
  same cpu-$n-yref cpu-$n-solo 's/ io=[0-9a-f]*//'
  for c in 37 300 1000 5000; do same cpu-$n-y$c cpu-$n-yref; done
done
grep 'far veneers' "$OUT/pair.out" | sed 's/^/      /' | tee -a "$S"
grep '^jit:' "$OUT/pair.out" | sed 's/^/      /' | tee -a "$S"
! grep -q '^FAIL' "$S"
