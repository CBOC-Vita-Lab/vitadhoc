#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Writes a synthetic cartridge that keeps a coprocessor busy forever, to measure
the worst-case cost of the chip bucket (no real chip workload is in the free test ROMs):
    make_chip_rom.py gsu OUT.sfc     Super FX running a NOP/branch loop at 21 MHz
    make_chip_rom.py sa1 OUT.sfc     SA-1 running a jump-to-self loop
The 65816 side just starts the chip and spins."""
import sys

def build(kind):
    rom = bytearray(0x10000 if kind == "gsu" else 0x20000)
    start = 0x8100
    if kind == "gsu":
        cpu = [0x78, 0x18, 0xFB, 0xE2, 0x30,                # sei clc xce sep #$30
               0xA9, 0x01, 0x8D, 0x39, 0x30,                # CLSR = 21 MHz
               0xA9, 0x80, 0x8D, 0x37, 0x30,                # CFGR: IRQ masked
               0x9C, 0x38, 0x30, 0x9C, 0x34, 0x30, 0x9C, 0x36, 0x30, 0x9C, 0x3C, 0x30,
               0xA9, 0x18, 0x8D, 0x3A, 0x30,                # SCMR: RON | RAN
               0xA9, start & 0xFF, 0x8D, 0x1E, 0x30,        # R15 low
               0xA9, start >> 8, 0x8D, 0x1F, 0x30]          # R15 high: GO
        chip_code = [0x01, 0x05, 0xFD, 0x01]                # L: nop; bra L; nop (delay slot)
        rtype, rsize, ram = 0x13, 0x06, 0x05
    else:
        cpu = [0x78, 0x18, 0xFB, 0xE2, 0x30,
               0xA9, start & 0xFF, 0x8D, 0x03, 0x22,        # SA-1 reset vector low
               0xA9, start >> 8, 0x8D, 0x04, 0x22,          # high
               0xA9, 0x00, 0x8D, 0x00, 0x22]                # CCNT: release SA-1 reset
        chip_code = [0x4C, start & 0xFF, start >> 8]        # jmp self
        rtype, rsize, ram = 0x34, 0x07, 0x03
    base = len(cpu)
    cpu += [0x4C, (0x8000 + base) & 0xFF, (0x8000 + base) >> 8]   # spin
    rom[0:len(cpu)] = bytes(cpu)
    off = start - 0x8000
    rom[off:off + len(chip_code)] = bytes(chip_code)
    h = 0x7FC0
    name = (b"CHIP LOAD " + kind.upper().encode()).ljust(21)
    rom[h:h + 21] = name
    rom[h + 0x15] = 0x23 if kind == "sa1" else 0x20
    rom[h + 0x16] = rtype
    rom[h + 0x17] = rsize
    rom[h + 0x18] = ram
    rom[h + 0x19] = 0x01
    rom[h + 0x1A] = 0x33
    rom[0x7FFC], rom[0x7FFD] = 0, 0x80
    # SA-1: its own vectors are read from the SNES-side registers; the CPU reset vector is above
    rom[h + 0x1C:h + 0x20] = bytes([0, 0, 0xFF, 0xFF])
    s = sum(rom) & 0xFFFF
    rom[h + 0x1E], rom[h + 0x1F] = s & 0xFF, s >> 8
    c = s ^ 0xFFFF
    rom[h + 0x1C], rom[h + 0x1D] = c & 0xFF, c >> 8
    return rom

open(sys.argv[2], "wb").write(build(sys.argv[1]))
