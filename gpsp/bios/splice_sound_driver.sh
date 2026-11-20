#!/usr/bin/env bash
# splice_sound_driver.sh -- build bios/source/sound_driver.c and splice it into
# bios/open_gba_bios.bin (see bios/README.md).
#
# The rest of the image was built long ago with devkitARM (and patched by
# patch_unaligned_decomp.py) and is kept byte for byte: rebuilding it with
# another compiler would move every SWI for no gain.  The driver is compiled on
# its own (ARMv4T, ARM state, freestanding, no library) and linked at 0x2880
# (sound_driver.ld), after the decompression stubs, into the zero tail of the
# image.  Rebuild the image from scratch with:
#
#   python3 -I patch_unaligned_decomp.py ORIGINAL.bin open_gba_bios.bin
#   ./splice_sound_driver.sh
#
#   CROSS   cross-compiler prefix (default arm-linux-gnueabihf-; arm-none-eabi- works too)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
P=${CROSS:-arm-linux-gnueabihf-}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cd "$W"
cp "$HERE/source/sound_driver.c" "$HERE/sound_driver.ld" .
"${P}gcc" --version | head -1
"${P}gcc" -march=armv4t -marm -mfloat-abi=soft -O2 -ffreestanding -fno-builtin \
  -fno-pic -fno-pie -fno-stack-protector -fno-tree-loop-distribute-patterns \
  -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror \
  -c sound_driver.c -o sd.o
"${P}ld" -nostdlib -static -T sound_driver.ld -o sd.elf sd.o
"${P}nm" sd.elf > sd.nm
# no initialised or zero-initialised data: it would be RAM the BIOS does not own
"${P}objdump" -h sd.elf | awk '$2 ~ /^\.(data|bss)$/ && $3 != "00000000" {print; bad=1} END {exit bad}'
if "${P}nm" -u sd.o | grep -q .; then "${P}nm" -u sd.o; echo "undefined symbols"; exit 1; fi
"${P}objcopy" -O binary -j .text sd.elf sd.bin
"${P}objdump" -d sd.elf > sd.dis
python3 -I "$HERE/splice_sound_driver.py" "$HERE/open_gba_bios.bin" "$W/sd.bin" "$W/sd.nm"
if [ -n "${SD_KEEP:-}" ]; then cp "$W"/sd.dis "$W"/sd.nm "$SD_KEEP"/; fi
