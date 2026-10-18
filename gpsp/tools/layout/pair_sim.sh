#!/usr/bin/env bash
# pair_sim.sh OUT VARIANT LAYOUTS|- [host args...] -- the GBA link PAIR
# (tools/drprof `make pair`) through lpsim_plugin: the Allegrex I-cache under
# PSP layouts, both core instances (docs/LAYOUT-PINNING.md, GBA-LINK-MIRROR 24).
#   OUT      run dir under $LS_HOME/runs (sim.txt, counts.txt)
#   VARIANT  $LS_HOME/bin/dr_pair_VARIANT (SOLO=1: dr_host_VARIANT)
#   LAYOUTS  layouts file as the container sees it (/w = $LS_HOME), or - for
#            a counts-only profile run (pair_layouts.py needs its counts.txt)
# Environment: LS_HOME (default ~/linkspeed/drprof), ROM/HS/JS/STATE (SF
# defaults: sf.gba h.txt j.txt sf_p0.st0), FRAMES (5200), SKIP (3000: the
# fight only).  Ranges: instance A's ROM+RAM caches, B's ROM cache, B's RAM
# cache (three jit= arguments; a layout line gives a PSP base for each).
# SOLO=1: one machine, the single-player fixtures:
#   SOLO=1 ROM=.. STATE=..|- SAV=..|- HS=script FRAMES=.. SKIP=.. pair_sim.sh OUT VARIANT LAYOUTS
set -euo pipefail
OUT=$1; V=$2; LAY=$3; shift 3
HERE="$(cd "$(dirname "$0")" && pwd)"
H=${LS_HOME:-$HOME/linkspeed/drprof}
ROM=${ROM:-sf.gba}; HS=${HS:-h.txt}; JS=${JS:-j.txt}; ST=${STATE:-sf_p0.st0}
FR=${FRAMES:-5200}; SKIP=${SKIP:-3000}; SOLO=${SOLO:-0}; SAV=${SAV:--}
mkdir -p "$H/runs/$OUT" "$H/plug"
cmp -s "$HERE/lpsim_plugin.c" "$H/plug/lpsim_plugin.c" || cp "$HERE/lpsim_plugin.c" "$H/plug/lpsim_plugin.c"
BIN=/w/bin/dr_pair_$V
[ "$SOLO" = 1 ] && BIN=/w/bin/dr_host_$V
cat > "$H/runs/$OUT/go.sh" <<EOS
set -e
O=/w/runs/$OUT
if [ ! /w/plug/lpsim_plugin.so -nt /w/plug/lpsim_plugin.c ]; then
  gcc -O2 -shared -fPIC -Wall -I/opt/qemu/include \$(pkg-config --cflags glib-2.0) \\
    -o /w/plug/lpsim_plugin.so /w/plug/lpsim_plugin.c \$(pkg-config --libs glib-2.0)
fi
a() { awk -v n=\$1 '\$NF==n{print \$1}' $BIN.nm; }
MARK=\$(a drprof_host_mark)
AR=\$(a rom_translation_cache); AM=\$(a ram_translation_cache)
J="jit=\$AR:\$(printf %x \$((0x\$AM + 0x80000)))"
if [ $SOLO != 1 ]; then
  BR=\$(a gbab_rom_translation_cache); BM=\$(a gbab_ram_translation_cache)
  J="\$J,jit=\$BR:\$(printf %x \$((0x\$BR + 0xa00000))),jit=\$BM:\$(printf %x \$((0x\$BM + 0x80000)))"
fi
echo "\$J mark=\$MARK" > \$O/run.info
L='$LAY'; if [ "\$L" = - ]; then echo '' > /tmp/empty.lay; L=/tmp/empty.lay; fi
P="-plugin /w/plug/lpsim_plugin.so,out=\$O/sim.txt,counts=\$O/counts.txt,misses=\$O/misses.txt,\$J,mark=\$MARK,skip=$SKIP,layouts=\$L"
if [ $SOLO = 1 ]; then
  SV=''; if [ '$SAV' != - ]; then cp $SAV /tmp/run.sav; SV='--save /tmp/run.sav'; fi
  STA=''; [ '$ST' != - ] && STA='--state $ST'
  /opt/qemu/bin/qemu-mipsel -cpu 74Kf \$P $BIN --rom $ROM --bios-dir . \$SV \$STA \\
     --script $HS --frames $FR --log \$O/host.log $* > \$O/run.log 2>&1 || true
else
  /opt/qemu/bin/qemu-mipsel -cpu 74Kf \$P $BIN --rom $ROM --bios-dir . --ap0 $HS --ap1 $JS \\
     --state0 $ST --state1 $ST --frames $FR $* > \$O/run.log 2>&1 || true
fi
tail -n 1 \$O/run.log
EOS
docker run --rm -v "$H":/w -w /w/fx drprof-qemu bash /w/runs/$OUT/go.sh
