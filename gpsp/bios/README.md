
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


VitAdhoc 2.0 change: the sound driver. The image answered the BIOS sound-driver
calls (SWI 19h-1Eh, 28h, 29h) with a bare return, so games that keep their
sequencer in the cartridge and leave the PCM mixing to the BIOS (X-Men: Reign
of Apocalypse) never switched the sound hardware on and ran silent.
`source/sound_driver.c` implements those calls (direct sound only: the music
player, 20h-24h, and the sequencer jump table stay empty returns). It comes
from GBAdhoc 5.0.0-beta.6 (upstream commit abcd4ed, GPL-2.0-or-later, from the
public interface description, checked black-box against the console BIOS).
`splice_sound_driver.sh` compiles it alone (ARMv4T, freestanding), links it at
0x2880 (`sound_driver.ld`, after the decompression stubs) and points the eight
table words at it. The whole image is reproducible: ORIGINAL.bin (the 1.0 image)
through `patch_unaligned_decomp.py`, then `splice_sound_driver.sh`.
