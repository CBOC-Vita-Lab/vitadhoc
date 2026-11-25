#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Writes a small cartridge that keeps a coprocessor busy with real work every frame, for the proofs that need the chip's state
to matter (Quick response, ahead_check.sh). No free test ROM uses the SA-1 or the DSP-1.
    make_chip_work_rom.py sa1 OUT.sfc    the SA-1 counts, multiplies with its arithmetic unit (the pad mixed in) and writes the
                                         products all over BW-RAM; the SNES side shows a product as the backdrop colour every
                                         frame and hands the SA-1 the pad
    make_chip_work_rom.py dsp1 OUT.sfc   the SNES side drives the DSP-1 one step per frame (command, first operand, second operand,
                                         result), so the chip is in the middle of a command at most frame ends; the result is the
                                         backdrop colour"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm65816 import Asm, make_lorom


def finish(rom, map_mode, rom_type, ram_size):
    rom = bytearray(rom)
    h = 0x7FC0
    rom[h + 0x15] = map_mode
    rom[h + 0x16] = rom_type
    rom[h + 0x18] = ram_size
    for _ in range(2):
        rom[h + 0x1C:h + 0x20] = b"\xff\xff\x00\x00"
        s = sum(rom) & 0xFFFF
        rom[h + 0x1E], rom[h + 0x1F] = s & 0xFF, s >> 8
        c = s ^ 0xFFFF
        rom[h + 0x1C], rom[h + 0x1D] = c & 0xFF, c >> 8
    return bytes(rom)


def screen_on(a):
    a.stz("abs", 0x2121); a.stz("abs", 0x2122); a.stz("abs", 0x2122)
    a.stz("abs", 0x212C)
    a.lda("#", 0x0F); a.sta("abs", 0x2100)
    a.lda("#", 0x81); a.sta("abs", 0x4200)        # NMI and the automatic pad read


def sa1():
    a = Asm(0x8000)
    a.label("reset")
    a.sei(); a.clc(); a.xce()
    a.rep(0x10); a.sep(0x20)
    a.ldx("#", 0x1FFF); a.txs()
    a.lda("#", 0x80); a.sta("abs", 0x2100)
    a.lda("#", 0x20); a.sta("abs", 0x2200)        # SA-1 held in reset
    a.ldx("#", "sa1_reset"); a.stx("abs", 0x2203)
    a.lda("#", 0x80); a.sta("abs", 0x2226); a.sta("abs", 0x2227)
    a.lda("#", 0xFF); a.sta("abs", 0x2229); a.sta("abs", 0x222A)
    a.stz("abs", 0x2224); a.stz("abs", 0x2225)
    a.stz("abs", 0x2200)                           # the SA-1 runs
    screen_on(a)
    a.label("idle")
    a.wai(); a.bra("idle")

    a.label("nmi")
    a.sep(0x20)
    a.lda("abs", 0x4210)
    a.stz("abs", 0x2121)
    a.lda("abs", 0x3004); a.sta("abs", 0x2122)    # a product of the SA-1 (I-RAM)
    a.lda("abs", 0x3005); a.and_("#", 0x7F); a.sta("abs", 0x2122)
    a.ldx("#", 0x0600)
    a.label("wait")
    a.dex(); a.bne("wait")
    a.rep(0x20)
    a.lda("abs", 0x4218); a.sta("abs", 0x3002)    # the pad, for the SA-1
    a.lda("abs", 0x6010); a.sta("abs", 0x3006)    # BW-RAM through the SNES window
    a.sep(0x20)
    a.rti()

    # the SA-1's program
    a.label("sa1_reset")
    a.sei(); a.clc(); a.xce()
    a.rep(0x30)
    a.ldx("#", 0x07FF); a.txs()
    a.label("sa1_loop")
    a.lda("abs", 0x3000); a.inca(); a.sta("abs", 0x3000)
    a.sep(0x20); a.stz("abs", 0x2250); a.rep(0x20)             # arithmetic unit: multiply
    a.lda("abs", 0x3000); a.sta("abs", 0x2251)
    a.lda("abs", 0x3002); a.ora("#", 0x0101); a.sta("abs", 0x2253)
    a.nop(); a.nop(); a.nop(); a.nop(); a.nop()
    a.lda("abs", 0x3000); a.and_("#", 0x3FFE); a.tax()
    a.lda("abs", 0x2306); a.sta("longx", 0x400000)
    a.sta("abs", 0x3004)
    a.lda("abs", 0x2308); a.sta("longx", 0x404000)
    a.bra("sa1_loop")
    code = a.link()
    rom = make_lorom({0: code}, "SA1 WORK TEST", a.labels["nmi"], a.labels["reset"], sram_kb=32)
    return finish(rom, 0x23, 0x35, 0x05)


def dsp1():
    a = Asm(0x8000)
    a.label("reset")
    a.sei(); a.clc(); a.xce()
    a.rep(0x10); a.sep(0x20)
    a.ldx("#", 0x1FFF); a.txs()
    a.lda("#", 0x80); a.sta("abs", 0x2100)
    a.stz("dp", 0x10); a.stz("dp", 0x11); a.stz("dp", 0x12); a.stz("dp", 0x13)
    screen_on(a)
    a.label("idle")
    a.wai(); a.bra("idle")

    # one step a frame: 0 command 00 (16 x 16 multiply), 1 the first operand, 2 the second (the pad mixed in), 3 the result
    DR, SR = 0x308000, 0x30C000
    a.label("nmi")
    a.sep(0x20)
    a.lda("abs", 0x4210)
    a.inc("dp", 0x12)
    a.lda("dp", 0x10); a.and_("#", 0x03)
    a.bne("s1")
    a.lda("#", 0x00); a.sta("long", DR)            # command 00
    a.bra("done")
    a.label("s1")
    a.cmp("#", 0x01); a.bne("s2")
    a.lda("dp", 0x12); a.sta("long", DR); a.lda("dp", 0x13); a.sta("long", DR)
    a.bra("done")
    a.label("s2")
    a.cmp("#", 0x02); a.bne("s3")
    a.lda("abs", 0x4219); a.ora("#", 0x31); a.sta("long", DR); a.lda("dp", 0x12); a.sta("long", DR)
    a.bra("done")
    a.label("s3")
    a.lda("long", SR)
    a.lda("long", DR); a.sta("dp", 0x13)
    a.stz("abs", 0x2121); a.sta("abs", 0x2122)
    a.lda("long", DR); a.and_("#", 0x7F); a.sta("abs", 0x2122)
    a.label("done")
    a.inc("dp", 0x10)
    a.rti()
    code = a.link()
    rom = make_lorom({0: code}, "DSP1 WORK TEST", a.labels["nmi"], a.labels["reset"])
    return finish(rom, 0x20, 0x03, 0x00)


kind, out = sys.argv[1], sys.argv[2]
open(out, "wb").write({"sa1": sa1, "dsp1": dsp1}[kind]())
