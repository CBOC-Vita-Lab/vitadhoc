
This BIOS is an open source replacement for Nintendo's official BIOS.
It was written originally by Normmatt and the VBA/VBA-M team, and its source
code can be found at https://github.com/Nebuleon/ReGBA/tree/master/bios

It is distributed under the GPL2 license (see repo)


VitAdhoc 1.3 change: the decompression calls (LZ77UnComp Wram/Vram, HuffUnComp,
RLUnCompWram, Diff8bit/16bit UnFilter) read their header word from the source
address rounded down to a multiple of 4, as RLUnCompVram already did and as
VBA-M and mGBA do, and carry on from source + 4. Mortal Kombat: Deadly Alliance
passes a halfword-aligned source: the old code read a rotated, garbage size and
ran for tens of seconds. `source/softwareinterrupts.c` has the change.
`open_gba_bios.bin` was NOT rebuilt from it (no devkitARM here): it was patched
with `patch_unaligned_decomp.py`, which jumps from each header read to a
16-byte stub in the unused end of the image that does the same thing.
