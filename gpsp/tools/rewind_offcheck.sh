#!/usr/bin/env bash
# rewind_offcheck.sh OLD_TREE NEW_TREE [WORK_DIR]
#
# "With REWIND undefined the build is the same as before": prove it.
# Builds the core (platform=unix) and the desktop frontend's objects from two
# source trees, both WITHOUT REWIND, and compares the .text, .data, .rodata and
# .bss of every object.  Debug info is left out on purpose: the branch moves
# line numbers, which is not code.
#
# OLD_TREE: the tree at the commit the rewind branch started from.  A git
# worktree cannot be read by git inside WSL, so make it with the Windows git:
#     git -C <worktree> archive <base-commit> | tar -x -C <dir>
# NEW_TREE: the branch.  Run under WSL/Linux; both trees are copied to
# WORK_DIR (default ~/rewind-offcheck) and built there.
#
# The desktop link itself fails without the GB-core stand-ins
# (tools/e2e/run_me_timing_sim.sh); the objects are what is compared.
set -e
OLD=${1:?usage: rewind_offcheck.sh OLD_TREE NEW_TREE [WORK_DIR]}
NEW=${2:?usage: rewind_offcheck.sh OLD_TREE NEW_TREE [WORK_DIR]}
R=${3:-~/rewind-offcheck}
rm -rf "$R"; mkdir -p "$R"
for t in old new; do
  src=$OLD; [ $t = new ] && src=$NEW
  rsync -a --exclude '.git' --exclude '*.o' --exclude 'obj' --exclude 'gpsp_sdl' \
    --exclude '*.a' "$src/" "$R/$t/"
  (cd "$R/$t" && make platform=unix -j"$(nproc)" > "$R/$t.core.log" 2>&1 || true
   cd sdl && make gpsp_sdl > "$R/$t.sdl.log" 2>&1 || true)
done
cd "$R"
bad=0; n=0
for o in $(cd new && find . -name '*.o' | sort); do
  [ -f "old/$o" ] || { echo "ONLY IN NEW: $o"; bad=1; continue; }
  n=$((n + 1))
  for sec in .text .data .rodata .bss; do
    a=$(objdump -s -j $sec "new/$o" 2>/dev/null | tail -n +3 | md5sum)
    b=$(objdump -s -j $sec "old/$o" 2>/dev/null | tail -n +3 | md5sum)
    sa=$(size -A "new/$o" | awk -v s=$sec '$1==s{print $2}')
    sb=$(size -A "old/$o" | awk -v s=$sec '$1==s{print $2}')
    if [ "$a" != "$b" ] || [ "$sa" != "$sb" ]; then
      echo "DIFFERS: $o $sec ($sb -> $sa bytes)"; bad=1
    fi
  done
done
for o in $(cd old && find . -name '*.o' | sort); do
  [ -f "new/$o" ] || { echo "ONLY IN OLD: $o"; bad=1; }
done
echo "objects compared: $n"
[ $n -gt 20 ] || { echo "too few objects: the builds did not run"; exit 2; }
if [ $bad = 0 ]; then echo "REWIND-off build: IDENTICAL code and data"; else
  echo "REWIND-off build: DIFFERENCES"; exit 1; fi
