#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Writes a tiny LoROM for the cheat tests.
Reset: $7E0010 := $05, once (only a code can change it afterwards).  Then, forever:
  $7E0011 := the operand of an LDA # in ROM (at $00800D, value $05), every pass;
  CGRAM entry 0 := ($0010, $0011) (the backdrop colour), battery RAM $700000 := $0010, $700001 := $0011.
A raw/PAR code on $7E0010 changes the picture and the save for good; a Game Genie patch of the ROM byte at $00800D
changes what the game writes into $0011, and when the patch is switched off the old operand is back."""
import sys
rom = bytearray(0x8000)
code = bytes([
    0x78, 0x18, 0xFB,                    # $8000 SEI CLC XCE
    0xA9, 0x0F, 0x8D, 0x00, 0x21,        # $8003 LDA #$0F STA $2100   screen on, full brightness
    0xA9, 0x05, 0x85, 0x10,              # $8008 LDA #$05 STA $10     (once)
    # loop at $800C:
    0xA9, 0x05, 0x85, 0x11,              # $800C LDA #$05 STA $11     (the operand $05 is at $800D)
    0xA9, 0x00, 0x8D, 0x21, 0x21,        # LDA #0 STA $2121     CGADD = 0
    0xA5, 0x10, 0x8D, 0x22, 0x21,        # LDA $10 STA $2122    low byte
    0xA5, 0x11, 0x8D, 0x22, 0x21,        # LDA $11 STA $2122    high byte
    0xA5, 0x10, 0x8F, 0x00, 0x00, 0x70,  # LDA $10 STA $700000
    0xA5, 0x11, 0x8F, 0x01, 0x00, 0x70,  # LDA $11 STA $700001
    0x4C, 0x0C, 0x80,                    # JMP $800C
])
rom[0:len(code)] = code
h = 0x7FC0
rom[h:h + 21] = b"CHEAT TEST ROM       "
rom[h + 0x15] = 0x20; rom[h + 0x16] = 0x02; rom[h + 0x17] = 0x05
rom[h + 0x18] = 0x01; rom[h + 0x19] = 0x01; rom[h + 0x1A] = 0x33
rom[0x7FFC], rom[0x7FFD] = 0, 0x80
s = sum(rom) & 0xFFFF
rom[h + 0x1E], rom[h + 0x1F] = s & 0xFF, s >> 8
c = s ^ 0xFFFF
rom[h + 0x1C], rom[h + 0x1D] = c & 0xFF, c >> 8
open(sys.argv[1], "wb").write(rom)
