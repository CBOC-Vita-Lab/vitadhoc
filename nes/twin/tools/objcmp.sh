#!/bin/bash
# Copyright CBOC-Vita. MIT licence. nescore.o built for x86_64 and for armhf, both linked with tests/obj_hash.c; the printed
# frame hashes, audio hash, RAM hash and state size must be identical (armhf runs under qemu-arm).
# usage: objcmp.sh FRAMES ROM...   (expects ./obj_hash_x86 and ./obj_hash_arm beside it)
# objcmp.sh FRAMES ROM... : nescore.o x86_64 vs armhf (qemu-arm), outputs must be identical
cd "$(dirname "$0")"
F=$1; shift; same=0; diff=0
for r in "$@"; do
  ./obj_hash_x86 "$r" $F > /tmp/oh_x86.$$ 2>&1
  qemu-arm -cpu max ./obj_hash_arm "$r" $F > /tmp/oh_arm.$$ 2>&1
  if cmp -s /tmp/oh_x86.$$ /tmp/oh_arm.$$ && [ -s /tmp/oh_x86.$$ ]; then same=$((same+1)); echo "SAME $r $(grep -c . /tmp/oh_x86.$$) lines"; else diff=$((diff+1)); echo "DIFF $r"; fi
done
rm -f /tmp/oh_x86.$$ /tmp/oh_arm.$$
echo "same=$same diff=$diff"
