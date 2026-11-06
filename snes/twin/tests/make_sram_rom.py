#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Writes a tiny LoROM that adds 1 to the first byte of its battery RAM at every
boot and then loops. It is our own test cartridge for the .srm rules."""
import sys
rom = bytearray(0x8000)
code = bytes([0x78, 0x18, 0xFB,              # sei, clc, xce
              0xAF, 0, 0, 0x70, 0x1A,        # lda $700000 ; inc a
              0x8F, 0, 0, 0x70,              # sta $700000
              0x4C, 0x0C, 0x80])             # jmp $800C (loop)
rom[0:len(code)] = code
h = 0x7FC0
rom[h:h + 21] = b"SRAM TEST ROM        "
rom[h + 0x15] = 0x20   # LoROM
rom[h + 0x16] = 0x02   # ROM + RAM + battery
rom[h + 0x17] = 0x05   # 32 KB
rom[h + 0x18] = 0x01   # 2 KB battery RAM
rom[h + 0x19] = 0x01
rom[h + 0x1A] = 0x33
rom[0x7FFC], rom[0x7FFD] = 0, 0x80
rom[h + 0x1C:h + 0x20] = bytes([0, 0, 0xFF, 0xFF])
s = sum(rom) & 0xFFFF
rom[h + 0x1E], rom[h + 0x1F] = s & 0xFF, s >> 8
c = s ^ 0xFFFF
rom[h + 0x1C], rom[h + 0x1D] = c & 0xFF, c >> 8
open(sys.argv[1], "wb").write(rom)
