#!/usr/bin/env bash
# pair_rank.sh SET name=psp.nm [name=psp.nm ...] -- rank PSP builds (layouts)
# by simulated I-cache misses on the link pairs AND the single-player
# fixtures, for both translation-cache tiers (docs/GBA-LINK-MIRROR.md 25).
#
#   LARGE (3000/Go): instance A's cache on the heap at the pinned offset
#                    0x1500 mod 8 KiB, B's static as linked
#   SMALL (1000):    A's cache in its static arrays; in a session A is
#                    reloaded at 0x1000 and B at 0x0000 (GBA_SESS_PIN_*_SMALL)
#
# Needs the profile runs (pair_sim.sh ... -) named below under $LS_HOME/runs,
# the twins dr_pair_sched / dr_host_ship, and the fixtures in $LS_HOME/fx.
# Two simulations at a time: eight at once ran WSL out of memory.
# Output: $LS_HOME/sim/SET/RANK.txt -- misses per frame and x 215.3 ns.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
H=${LS_HOME:-$HOME/linkspeed/drprof}
SET=$1; shift
ELFS=(); for e in "$@"; do ELFS+=(--elf "$e@9635500"); done
FP="$HERE/psp_frame_path.txt"
lay() {  # fixture prof twin solo?
  local fx=$1 prof=$2 twin=$3 solo=$4 o="$SET/$1"
  rm -f "$H/sim/$o/pair.lay"
  if [ "$solo" = 1 ]; then
    python3 "$HERE/pair_layouts.py" --home "$H" --twin "$twin" --prof "$prof" --out "$o" --frame-path "$FP" --solo --tag _L "${ELFS[@]}" >/dev/null 2>&1
    python3 "$HERE/pair_layouts.py" --home "$H" --twin "$twin" --prof "$prof" --out "$o" --frame-path "$FP" --solo --tier small --tag _S --append "${ELFS[@]}" >/dev/null 2>&1
  else
    python3 "$HERE/pair_layouts.py" --home "$H" --twin "$twin" --prof "$prof" --out "$o" --frame-path "$FP" --tag _L "${ELFS[@]}" >/dev/null 2>&1
    python3 "$HERE/pair_layouts.py" --home "$H" --twin "$twin" --prof "$prof" --out "$o" --frame-path "$FP" --tier small --pin-a 0x1000 --pin-b 0x0000 --tag _S --append "${ELFS[@]}" >/dev/null 2>&1
  fi
}
cp "$H/fx/aw2.sav" "$H/fx/aw2_s0.sav"; cp "$H/fx/aw2.sav" "$H/fx/aw2_s1.sav"
lay sf pf_sf sched 0;  lay aw2 pf_aw2 sched 0
for f in em ubr ubd hns d2 nfs; do lay $f ps_$f ship 1; done
P="$HERE/pair_sim.sh"; L="/w/sim/$SET"
( bash "$P" rk_${SET}_sf sched $L/sf/pair.lay --fast3 --headless1q >/dev/null 2>&1 &
  ROM=aw2.gba HS=aw2_host.txt JS=aw2_join.txt STATE=aw2.st0 FRAMES=8000 SKIP=6000 bash "$P" rk_${SET}_aw2 sched $L/aw2/pair.lay --save0 aw2_s0.sav --save1 aw2_s1.sav --fast3 --headless1q >/dev/null 2>&1 & wait )
( SOLO=1 ROM=emerald.gba STATE=emerald_3000.st0 SAV=emerald_3000.sav HS=emerald_battle.txt FRAMES=2100 SKIP=300 bash "$P" rk_${SET}_em ship $L/em/pair.lay >/dev/null 2>&1 &
  SOLO=1 ROM=unbound.gba STATE=unbound_rival_high.st0 SAV=unbound.sav HS=battle.txt FRAMES=3100 SKIP=300 bash "$P" rk_${SET}_ubr ship $L/ubr/pair.lay >/dev/null 2>&1 & wait )
( SOLO=1 ROM=unbound.gba STATE=unbound_double_high.st0 SAV=unbound.sav HS=battle.txt FRAMES=3100 SKIP=300 bash "$P" rk_${SET}_ubd ship $L/ubd/pair.lay >/dev/null 2>&1 &
  SOLO=1 ROM=hns.gba STATE=heart_soul_heavy.st0 SAV=hns.sav HS=battle.txt FRAMES=2000 SKIP=300 bash "$P" rk_${SET}_hns ship $L/hns/pair.lay >/dev/null 2>&1 & wait )
( SOLO=1 ROM=d2.gba STATE=- HS=d2_drive.txt FRAMES=4300 SKIP=1500 bash "$P" rk_${SET}_d2 ship $L/d2/pair.lay >/dev/null 2>&1 &
  SOLO=1 ROM=nfsu2.gba STATE=- HS=nfsu2_race.txt FRAMES=5200 SKIP=2500 bash "$P" rk_${SET}_nfs ship $L/nfs/pair.lay >/dev/null 2>&1 & wait )
{
  printf '%-10s' layout; for f in sf aw2 em ubr ubd hns d2 nfs; do printf ' %9s' $f; done; echo '   (I-cache misses per frame; ms = x 215.3 ns)'
  for n in $(awk '/^layout/{print $2}' "$H/runs/rk_${SET}_sf/sim.txt"); do
    printf '%-10s' "$n"
    for f in sf aw2 em ubr ubd hns d2 nfs; do
      awk -v n="$n" '/^frames/{F=$2} $1=="layout" && $2==n {printf " %6.0f/%.1f", $6/F, $6/F*215.3e-6}' "$H/runs/rk_${SET}_$f/sim.txt"
    done; echo
  done
} | tee "$H/sim/$SET/RANK.txt"
