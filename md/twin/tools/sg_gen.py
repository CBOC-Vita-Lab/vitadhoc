#!/usr/bin/env python3
# Makes an SG-1000 test cartridge (no Master System mode, no header: the TMS9918 modes only): Graphics I (32 x 24 tiles, a 32 byte colour
# table), one 8x8 sprite that moves one pixel per frame (and a slower counter in RAM, the byte the RetroAchievements test watches), and button 1 (bit 4 of port $DC, low when held) turning the sprite white.
# The first SG-1000 and (with --gg) Game Gear cartridge the player tests (player_selftest.sh) run when no public homebrew .sg / .gg is given
# (GATE_SG_ROM / GATE_GG_ROM); --gg makes the Game Gear's sms_scroll_gen cartridge with a ".gg" name instead, so the library, the extension
# and the system's header ($7FFF = $6C) take the real Game Gear path, not an SMS ROM with the console id forced.
# usage: sg_gen.py OUT.sg            |    sg_gen.py OUT.gg --gg
# Written from scratch (no code from any game or tool); the assembler helper is the one of sms_scroll_gen.py.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sms_scroll_gen import Asm, build as build_scroll

SPR_X = 0xC000

def pattern(t):
    """8 bytes of tile t: a diagonal and a box that differ for every tile"""
    return bytes(((0x80 >> ((r + t) & 7)) | (0xFF if (r in (0, 7) and t & 1) else 0) | ((t * 37 + r * 11) & 0x18)) & 255 for r in range(8))

def build():
    A = Asm(); e = A.e
    e(0xF3); e(0xED, 0x56); A.w16([0x31], 0xDFF0)                          # di; im 1; ld sp
    A.a16([0x21], 'regtab'); e(0x06, 16); e(0x0E, 0xBF); e(0xED, 0xB3)       # the 8 registers: Graphics I, display off
    # patterns at $0000 (256 tiles)
    e(0xAF); e(0xD3, 0xBF); e(0x3E, 0x40); e(0xD3, 0xBF)
    A.a16([0x21], 'pat'); A.w16([0x01], 2048)
    A.L('lp'); e(0x7E); e(0xD3, 0xBE); e(0x23); e(0x0B); e(0x78); e(0xB1); A.r8(0x20, 'lp')
    # names at $1400 (768 bytes)
    e(0x3E, 0x00); e(0xD3, 0xBF); e(0x3E, 0x54); e(0xD3, 0xBF)
    e(0x21, 0, 0); e(0x01, 0, 3)
    A.L('ln'); e(0x7D); e(0xAC); e(0xD3, 0xBE); e(0x23); e(0x0B); e(0x78); e(0xB1); A.r8(0x20, 'ln')
    # colours at $2000 (32 bytes)
    e(0x3E, 0x00); e(0xD3, 0xBF); e(0x3E, 0x60); e(0xD3, 0xBF)
    A.a16([0x21], 'col'); e(0x06, 32); e(0x0E, 0xBE); e(0xED, 0xB3)
    # sprite pattern at $1800: a solid 8 x 8 box
    e(0x3E, 0x00); e(0xD3, 0xBF); e(0x3E, 0x58); e(0xD3, 0xBF)
    e(0x06, 8); e(0x3E, 0xFF)
    A.L('ls'); e(0xD3, 0xBE); e(0x10, 0xFD)
    # sprite 0 at $3B00 (y, x, pattern 0, colour), sprite 1 ends the list
    e(0x3E, 0x00); e(0xD3, 0xBF); e(0x3E, 0x7B); e(0xD3, 0xBF)
    e(0x3E, 79); e(0xD3, 0xBE); e(0x3E, 0); e(0xD3, 0xBE); e(0x3E, 0); e(0xD3, 0xBE); e(0x3E, 10); e(0xD3, 0xBE)
    e(0x3E, 0xD0); e(0xD3, 0xBE)
    e(0xAF); A.w16([0x32], SPR_X)
    e(0x3E, 0xC0); e(0xD3, 0xBF); e(0x3E, 0x81); e(0xD3, 0xBF)             # register 1: 16K, display on
    A.L('main'); e(0xDB, 0xBF); e(0xE6, 0x80); A.r8(0x28, 'main')            # wait for the frame flag
    A.w16([0x3A], SPR_X); e(0x3C); A.w16([0x32], SPR_X)                      # x + 1
    e(0xCB, 0x3F); e(0xCB, 0x3F); e(0xCB, 0x3F); A.w16([0x32], SPR_X + 1)                      # x / 8 in the next byte: a counter that changes on one frame in eight
    A.w16([0x3A], SPR_X)
    e(0x4F)                                                                  # ld c,a
    e(0x3E, 0x01); e(0xD3, 0xBF); e(0x3E, 0x7B); e(0xD3, 0xBF)               # $3B01: the x byte
    e(0x79); e(0xD3, 0xBE); e(0x3E, 0); e(0xD3, 0xBE)                        # x, pattern 0
    e(0xDB, 0xDC); e(0xE6, 0x10); e(0x3E, 15); A.r8(0x28, 'held')            # button 1 held: white
    e(0x3E, 10)
    A.L('held'); e(0xD3, 0xBE)                                               # the colour byte
    A.a16([0xC3], 'main')
    A.L('regtab')
    for i, v in enumerate([0x00, 0x80, 0x05, 0x80, 0x00, 0x76, 0x03, 0x04]): e(v, 0x80 | i)
    A.L('col'); A.b += bytes(((i * 5 + 2) & 15) << 4 | ((i * 3 + 7) & 15) for i in range(32))
    A.L('pat'); A.b += b''.join(pattern(t) for t in range(256))
    rom = bytearray(A.done())
    assert len(rom) < 0x8000
    rom += bytes(0x8000 - len(rom))
    return bytes(rom)

if __name__ == '__main__':
    open(sys.argv[1], 'wb').write(build_scroll(True, True) if '--gg' in sys.argv else build())
