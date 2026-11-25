#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Writes a small LoROM that shows the pad one frame after it reads it (Quick response's latency test, ahead_check.sh).
Every vertical blank the backdrop colour is set from the buttons read in the vertical blank before, and then the buttons of this
one are read (after the automatic read is done). A press is therefore on screen one frame after the frame that read it: 1 frame
later than the press without Quick response, 0 with it.
    make_latency_rom.py OUT.sfc"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm65816 import Asm, make_lorom

a = Asm(0x8000)
a.label("reset")
a.sei(); a.clc(); a.xce()
a.rep(0x10); a.sep(0x20)
a.ldx("#", 0x1FFF); a.txs()
a.lda("#", 0x80); a.sta("abs", 0x2100)        # forced blank while the palette is set
a.stz("abs", 0x2121); a.stz("abs", 0x2122); a.stz("abs", 0x2122)
a.stz("dp", 0x10); a.stz("dp", 0x11)
a.stz("abs", 0x212C)                           # no layer: the picture is the backdrop
a.lda("#", 0x0F); a.sta("abs", 0x2100)
a.lda("#", 0x81); a.sta("abs", 0x4200)        # NMI and the automatic pad read
a.label("idle")
a.wai(); a.bra("idle")

a.label("nmi")
a.sep(0x20)
a.lda("abs", 0x4210)
a.stz("abs", 0x2121)                           # colour 0 from the buttons read one frame ago
a.lda("dp", 0x10); a.sta("abs", 0x2122)
a.lda("dp", 0x11); a.sta("abs", 0x2122)
a.ldx("#", 0x0600)                             # past the automatic read (it starts 2 lines after the NMI)
a.label("wait")
a.dex(); a.bne("wait")
a.lda("abs", 0x4218); a.ora("abs", 0x4219); a.sta("dp", 0x10)
a.lda("abs", 0x4219); a.and_("#", 0x7F); a.sta("dp", 0x11)
a.rti()
code = a.link()
open(sys.argv[1], "wb").write(make_lorom({0: code}, "LATENCY TEST", a.labels["nmi"], a.labels["reset"]))
