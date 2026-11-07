#!/usr/bin/env python3
"""patch_unaligned_decomp.py -- how open_gba_bios.bin differs from the
ReGBA/VBA open BIOS it came from (VitAdhoc 1.3).

The decompression calls read their 4-byte header with `ldr rX, [src], #4`.
When a game hands them a source that is only halfword aligned (Mortal Kombat:
Deadly Alliance does, for run-length data), the CPU rotates the word it reads
and the "size" is garbage: the call then runs for tens of seconds and fills
memory with junk.  Real BIOS code, VBA-M and mGBA read the header from the
word-aligned address below the source (RLUnCompVram in this BIOS already did)
and still continue from source + 4.

Each of the seven header reads below is replaced by a jump to a 16-byte stub
in the unused tail of the image:

    bic  rX, rY, #3        @ aligned-down address
    ldr  rX, [rX]          @ the header word
    add  rY, rY, #4        @ source + 4, as before
    b    <instruction after the patched one>

No flags change, no other register is used, and for an aligned source the
result is identical to the old code.

    patch_unaligned_decomp.py ORIGINAL.bin PATCHED.bin
"""
import struct
import sys

STUBS = 0x2800                      # 7 stubs x 16 bytes, in the zero tail (0x2439..)
# (address, original word, rX, rY): LZ77 Wram, LZ77 body shared with Vram,
# Huffman, RL Wram, Diff8 Wram, Diff8 Vram, Diff16
SITES = [
    (0x0A44, 0xE4902004, 2, 0),
    (0x0B60, 0xE4905004, 5, 0),
    (0x0CEC, 0xE4934004, 4, 3),
    (0x0F18, 0xE4902004, 2, 0),
    (0x10C4, 0xE492C004, 12, 2),
    (0x1124, 0xE4936004, 6, 3),
    (0x11B0, 0xE493C004, 12, 3),
]


def branch(src, dst):
    off = (dst - (src + 8)) // 4
    return 0xEA000000 | (off & 0xFFFFFF)


def main(argv):
    img = bytearray(open(argv[1], "rb").read())
    assert len(img) == 0x4000, "not a 16 KB BIOS image"
    assert not any(img[STUBS:STUBS + 16 * len(SITES)]), "stub area is not free"
    for n, (addr, old, rx, ry) in enumerate(SITES):
        assert struct.unpack_from("<I", img, addr)[0] == old, hex(addr)
        stub = STUBS + 16 * n
        words = [0xE3C00003 | (rx << 12) | (ry << 16),      # bic rX, rY, #3
                 0xE5900000 | (rx << 12) | (rx << 16),      # ldr rX, [rX]
                 0xE2800004 | (ry << 12) | (ry << 16),      # add rY, rY, #4
                 branch(stub + 12, addr + 4)]
        struct.pack_into("<4I", img, stub, *words)
        struct.pack_into("<I", img, addr, branch(addr, stub))
    open(argv[2], "wb").write(img)


if __name__ == "__main__":
    main(sys.argv)
