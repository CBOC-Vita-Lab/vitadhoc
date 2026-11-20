#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Writes a tiny LoROM that never stands still, for the rewind / fast forward / RetroAchievements tests of the player.
Reset: $7E0010 := $05, once.  Then, forever: $7E0011 := the operand of an LDA # in ROM (at $00800D, value $05), INC $7E0012 (a counter
that moves every pass, so the machine's state differs from one frame to the next), the backdrop colour from ($0010, $0011).
The patch tests change the byte at $00800D (file offset 0x0D); the RetroAchievements tests poke $7E0010 (RetroAchievements address
$000010) to make an achievement true."""
import sys
rom = bytearray(0x8000)
code = bytes([
    0x78, 0x18, 0xFB,                    # $8000 SEI CLC XCE
    0xA9, 0x0F, 0x8D, 0x00, 0x21,        # $8003 LDA #$0F STA $2100   screen on, full brightness
    0xA9, 0x05, 0x85, 0x10,              # $8008 LDA #$05 STA $10     (once)
    # loop at $800C:
    0xA9, 0x05, 0x85, 0x11,              # $800C LDA #$05 STA $11     (the operand $05 is at $800D)
    0xE6, 0x12,                          # INC $12                     the moving part
    0xA9, 0x00, 0x8D, 0x21, 0x21,        # LDA #0 STA $2121     CGADD = 0
    0xA5, 0x10, 0x8D, 0x22, 0x21,        # LDA $10 STA $2122    low byte
    0xA5, 0x11, 0x8D, 0x22, 0x21,        # LDA $11 STA $2122    high byte
    0x4C, 0x0C, 0x80,                    # JMP $800C
])
rom[0:len(code)] = code
h = 0x7FC0
rom[h:h + 21] = b"SPEED TEST ROM       "
rom[h + 0x15] = 0x20; rom[h + 0x16] = 0x00; rom[h + 0x17] = 0x05
rom[h + 0x18] = 0x00; rom[h + 0x19] = 0x01; rom[h + 0x1A] = 0x33
rom[0x7FFC], rom[0x7FFD] = 0, 0x80
s = sum(rom) & 0xFFFF
rom[h + 0x1E], rom[h + 0x1F] = s & 0xFF, s >> 8
c = s ^ 0xFFFF
rom[h + 0x1C], rom[h + 0x1D] = c & 0xFF, c >> 8
open(sys.argv[1], "wb").write(rom)
