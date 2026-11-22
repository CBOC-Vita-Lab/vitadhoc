#!/bin/sh
# macho-group.sh -- hide or rename the globals of a group of Mach-O objects.
#
# macOS has no GNU ld -r -T / objcopy -w --keep-global-symbol, which the ELF
# builds (gpsp/vita/corebuild.mk, nescore.mk, wswancore.mk) use to
#   - keep a whole core's globals private (FCEUmm, Beetle, TGB Dual), and
#   - make the second instance of a core (instance B: gbab_*, gbcoreb_*).
# Here every object of the group is copied with llvm-objcopy --redefine-syms,
# one map for the whole group, so a definition and every reference to it in
# the group are renamed together.  What the group only references (libc, the
# other cores' API) is not in the map and is left alone.
#
#   macho-group.sh NM OBJCOPY US OUTDIR PREFIX KEEP_RE API_FROM API_TO OBJ...
#
#   NM, OBJCOPY  llvm-nm and llvm-objcopy (GNU-style options)
#   US           "_" on macOS (C symbols carry an underscore), "" elsewhere
#   OUTDIR       receives n.o (renamed copies, numbered) and .syms (the map)
#   PREFIX       what every defined global gets in front of its name
#   KEEP_RE      extended regex (names without US): globals that are NOT
#                prefixed ("" = none are kept)
#   API_FROM/TO  a kept name that starts with API_FROM has it replaced by
#                API_TO (instance B's API: gbcore_ -> gbcoreb_); "" = kept as is
#
# The numbered copies are for `ar` or the link line; a group is rebuilt whole.

set -eu
if [ $# -lt 9 ]; then
  echo "usage: macho-group.sh NM OBJCOPY US OUTDIR PREFIX KEEP_RE API_FROM API_TO OBJ..." >&2
  exit 2
fi
NM=$1; OBJCOPY=$2; US=$3; OUT=$4; PFX=$5; KEEP=$6; AF=$7; AT=$8
shift 8

rm -rf "$OUT"
mkdir -p "$OUT"

# One nm over the whole group: a file with many objects prints "name:" lines
# between the blocks (fewer than three fields: skipped).
"$NM" -g --defined-only "$@" | awk -v us="$US" -v pfx="$PFX" -v keep="$KEEP" \
    -v af="$AF" -v at="$AT" '
  NF == 3 && !seen[$3]++ {
    n = $3
    has = (us != "" && substr(n, 1, length(us)) == us)
    base = has ? substr(n, length(us) + 1) : n
    if (keep != "" && base ~ keep) {
      if (af == "") next
      new = at substr(base, length(af) + 1)
    } else
      new = pfx base
    print n " " (has ? us : "") new
  }' > "$OUT/.syms"

i=0
for o in "$@"; do
  i=$((i + 1))
  "$OBJCOPY" --redefine-syms="$OUT/.syms" "$o" "$OUT/$i.o"
done
echo "macho-group: $i objects, $(wc -l < "$OUT/.syms" | tr -d ' ') names -> $OUT"
