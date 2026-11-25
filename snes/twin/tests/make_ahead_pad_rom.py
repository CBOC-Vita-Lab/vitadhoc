#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Writes a small LoROM for Quick response's wrong guesses (ahead_check.sh). While any button is held, every vertical blank
counts up and writes the count into the character data of tile 0 (the whole background and sprite 0 show it) and into sprite 0's
position; with no button held it writes nothing. On the frame a button is let go the look-ahead frame (run with the buttons of the
frame before) has written video RAM and sprite memory that the real frame does not write again, so the picture after it is right
only if the way back also brings the caches that follow the machine in line (tile cache, the picture thread's video RAM and
sprite tables).
    make_ahead_pad_rom.py OUT.sfc"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm65816 import Asm, make_lorom

a = Asm(0x8000)
a.label("reset")
a.sei(); a.clc(); a.xce()
a.rep(0x10); a.sep(0x20)
a.ldx("#", 0x1FFF); a.txs()
a.lda("#", 0x80); a.sta("abs", 0x2100)
a.stz("abs", 0x2105)                           # mode 0
a.lda("#", 0x04); a.sta("abs", 0x2107)        # BG1 map at word $0400 (all tile 0), characters at 0
a.stz("abs", 0x210B)
a.stz("abs", 0x2101)                           # sprites 8x8, characters at 0
a.stz("abs", 0x2121)                           # colours 0, 1 of the background
a.stz("abs", 0x2122); a.stz("abs", 0x2122)
a.lda("#", 0xFF); a.sta("abs", 0x2122); a.lda("#", 0x7F); a.sta("abs", 0x2122)
a.lda("#", 0x81); a.sta("abs", 0x2121)        # colour 1 of the sprites
a.lda("#", 0x1F); a.sta("abs", 0x2122); a.stz("abs", 0x2122)
a.stz("abs", 0x2102); a.stz("abs", 0x2103)    # every sprite below the picture
a.ldx("#", 128)
a.label("oam")
a.stz("abs", 0x2104); a.lda("#", 0xF0); a.sta("abs", 0x2104); a.stz("abs", 0x2104); a.stz("abs", 0x2104)
a.dex(); a.bne("oam")
a.stz("dp", 0x12)
a.lda("#", 0x11); a.sta("abs", 0x212C)        # BG1 and sprites
a.lda("#", 0x0F); a.sta("abs", 0x2100)
a.lda("#", 0x81); a.sta("abs", 0x4200)
a.label("idle")
a.wai(); a.bra("idle")

a.label("nmi")
a.sep(0x20)
a.lda("abs", 0x4210)
a.ldx("#", 0x0100)                             # past the automatic pad read, still in the vertical blank
a.label("wait")
a.dex(); a.bne("wait")
a.lda("abs", 0x4218); a.ora("abs", 0x4219)
a.beq("done")
a.inc("dp", 0x12)
a.stz("abs", 0x2115)                           # video RAM: the eight low bytes of tile 0's first plane
a.stz("abs", 0x2116); a.stz("abs", 0x2117)
a.ldx("#", 8)
a.label("vram")
a.lda("dp", 0x12); a.sta("abs", 0x2118)
a.dex(); a.bne("vram")
a.stz("abs", 0x2102); a.stz("abs", 0x2103)    # sprite 0: x = the count, y = 100, tile 0
a.lda("dp", 0x12); a.sta("abs", 0x2104)
a.lda("#", 100); a.sta("abs", 0x2104)
a.stz("abs", 0x2104); a.lda("#", 0x30); a.sta("abs", 0x2104)   # in front of the background
a.label("done")
a.rti()
code = a.link()
open(sys.argv[1], "wb").write(make_lorom({0: code}, "AHEAD PAD TEST", a.labels["nmi"], a.labels["reset"]))
