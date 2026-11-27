#!/usr/bin/env python3
# Makes a Master System test cartridge that answers the pad one frame later, the way most games do: in vertical blank it uploads the sprite
# position its LAST blank's pad read produced and reads the pad again (the game logic would run in between), so the picture shows a press one frame later. md_twin measures how many frames pass between a
# press and the first changed picture: 1 plain, 0 with "Quick response" (run-ahead). One 8x8 sprite jumps from x=20 to x=100 while button 1 is held.
# usage: sms_latency_gen.py OUT.sms
# Written from scratch (no code from any game or tool); the assembler helper is the one of sms_scroll_gen.py.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sms_scroll_gen import Asm

def build():
    A = Asm(); e = A.e
    e(0xF3); e(0xED, 0x56); A.w16([0x31], 0xDFF0)                      # di; im 1; ld sp
    A.a16([0x21], 'regtab'); e(0x06, 22); e(0x0E, 0xBF); e(0xED, 0xB3)   # the 11 registers: display off, sprites at 0x3F00 table, patterns at 0
    e(0xAF); e(0xD3, 0xBF); e(0x3E, 0x40); e(0xD3, 0xBF)               # VRAM write address 0 (tile 0 = the sprite's pattern)
    A.a16([0x21], 'tile'); e(0x06, 32); e(0x0E, 0xBE); e(0xED, 0xB3)
    e(0x3E, 0x00); e(0xD3, 0xBF); e(0x3E, 0x7F); e(0xD3, 0xBF)         # VRAM write address 0x3F00: sprite Y table
    e(0x3E, 79); e(0xD3, 0xBE); e(0x3E, 0xD0); e(0xD3, 0xBE)           # sprite 0 at y = 80, sprite 1 ends the list
    e(0x3E, 0x80); e(0xD3, 0xBF); e(0x3E, 0x7F); e(0xD3, 0xBF)         # 0x3F80: x / tile pairs
    e(0x3E, 20); e(0xD3, 0xBE); e(0x3E, 0); e(0xD3, 0xBE)
    e(0x3E, 0x10); e(0xD3, 0xBF); e(0x3E, 0xC0); e(0xD3, 0xBF)         # CRAM address 16: the sprite palette
    A.a16([0x21], 'pal'); e(0x06, 16); e(0x0E, 0xBE); e(0xED, 0xB3)
    e(0x3E, 0x64); e(0xD3, 0xBF); e(0x3E, 0x81); e(0xD3, 0xBF)         # register 1 = 0x64: display on
    A.L('main'); e(0xDB, 0xBF); e(0xE6, 0x80); A.r8(0x28, 'main')       # wait for the frame interrupt flag (vertical blank)
    e(0x3E, 0x80); e(0xD3, 0xBF); e(0x3E, 0x7F); e(0xD3, 0xBF)         # first the x the PREVIOUS blank's read produced goes into the sprite table
    A.w16([0x3A], 0xC000); e(0xD3, 0xBE)                                 # (the game logic of a real game runs between the read and the upload)
    e(0xDB, 0xDC); e(0xE6, 0x10); e(0x3E, 100); A.r8(0x28, 'pressed')  # then the pad is read: button 1 is bit 4, low when held
    e(0x3E, 20)
    A.L('pressed'); A.w16([0x32], 0xC000)                                # the answer waits in RAM for the next blank
    A.a16([0xC3], 'main')
    A.L('regtab')
    for i, v in enumerate([0x04, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF]): e(v, 0x80 | i)
    A.L('tile'); A.b += bytes([0xFF] * 32)                              # all four planes set: colour 15 of the sprite palette
    A.L('pal'); A.b += bytes([0x00] * 15 + [0x3F])                      # the last entry white
    rom = bytearray(A.done())
    assert len(rom) < 0x7FF0
    rom += bytes(0x8000 - len(rom))
    rom[0x7FF0:0x7FF8] = b'TMR SEGA'
    rom[0x7FFF] = 0x4C
    return bytes(rom)

if __name__ == '__main__':
    open(sys.argv[1], 'wb').write(build())
