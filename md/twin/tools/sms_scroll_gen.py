#!/usr/bin/env python3
# Makes a Master System test cartridge whose picture is known exactly: a world of 256 columns of tiles that scrolls sideways under a
# script (right, left, right, further left, stop, right), streaming one column per 8 pixels into the 32 column name table the way games do.
# usage: sms_scroll_gen.py OUT.sms|OUT.gg [--noblank] [--gg]      (--noblank: the first 8 columns are not masked; --gg: a Game Gear cartridge,
# 12 bit palette, the LCD shows the middle 160 x 144 of the same picture)
# The same file holds the truth functions wide_truth.py uses. Written from scratch (no code from any game or tool).
import sys, random

SCRIPT = [(700, 1), (350, -1), (500, 1), (1100, -1), (60, 0), (400, 1)]
CAM, CNT, DX, SPT, TC, WC = 0xC000, 0xC002, 0xC004, 0xC006, 0xC008, 0xC009

def tile_of(wc, row): return (wc + (wc >> 3) + row * 3) & 63
def attr_of(wc, row): return ((wc >> 4) & 1) * 2 + ((row >> 2) & 1) * 8      # high byte: bit 1 = flip X, bit 3 = palette 2
def tiles():
    r = random.Random(1234)
    return bytes(r.randrange(256) for _ in range(64 * 32))
def palette(): return bytes(((i * 7 + 3) & 63) for i in range(32))
def cam_at(n):
    """camera position after n vertical blanks"""
    cam = 0; i = 0
    total = sum(c for c, _ in SCRIPT)
    n2 = n
    while n2 > 0:
        c, d = SCRIPT[i % len(SCRIPT)]
        step = min(c, n2)
        cam += d * step; n2 -= step; i += 1
    return cam
def colour_index(wx, y):
    """(palette bit << 4 | colour) of the world pixel wx on line y"""
    wc = (wx >> 3) & 255; row = y >> 3
    t = tile_of(wc, row); a = attr_of(wc, row)
    x = wx & 7; fy = y & 7
    T = tiles()
    b = T[t * 32 + fy * 4: t * 32 + fy * 4 + 4]
    bit = x if (a & 2) else 7 - x
    ci = ((b[0] >> bit) & 1) | (((b[1] >> bit) & 1) << 1) | (((b[2] >> bit) & 1) << 2) | (((b[3] >> bit) & 1) << 3)
    return (((a >> 3) & 1) << 4) | ci

class Asm:
    def __init__(self): self.b = bytearray(); self.lab = {}; self.fix = []
    def L(self, n): self.lab[n] = len(self.b)
    def e(self, *bs): self.b += bytes(bs)
    def a16(self, op, lab): self.b += bytes(op); self.fix.append((len(self.b), lab, 'a')); self.b += b'\0\0'
    def r8(self, op, lab): self.b += bytes([op]); self.fix.append((len(self.b), lab, 'r')); self.b += b'\0'
    def w16(self, op, v): self.b += bytes(op) + bytes([v & 255, v >> 8])
    def done(self):
        for pos, lab, k in self.fix:
            t = self.lab[lab]
            if k == 'a': self.b[pos] = t & 255; self.b[pos + 1] = t >> 8
            else:
                d = t - (pos + 1); assert -128 <= d <= 127, (lab, d); self.b[pos] = d & 255
        return self.b

def build(blank=True, gg=False):
    A = Asm(); e = A.e
    e(0xF3); e(0xED, 0x56); A.w16([0x31], 0xDFF0)
    A.a16([0x21], 'regtab'); e(0x06, 22); e(0x0E, 0xBF); e(0xED, 0xB3)
    e(0x3E, 0x00); e(0xD3, 0xBF); e(0x3E, 0xC0); e(0xD3, 0xBF)
    A.a16([0x21], 'pal'); e(0x06, 64 if gg else 32); e(0x0E, 0xBE); e(0xED, 0xB3)
    e(0xAF); e(0xD3, 0xBF); e(0x3E, 0x40); e(0xD3, 0xBF)
    A.a16([0x21], 'tiles'); A.w16([0x01], 2048)
    A.L('tl'); e(0x7E); e(0xD3, 0xBE); e(0x23); e(0x0B); e(0x78); e(0xB1); A.r8(0x20, 'tl')
    e(0x3E, 0x00); e(0xD3, 0xBF); e(0x3E, 0x7F); e(0xD3, 0xBF); e(0x3E, 0xD0); e(0xD3, 0xBE)
    A.w16([0x21], 0); A.w16([0x22], CAM)
    e(0x1E, 0)
    A.L('ic'); e(0x7B); A.w16([0x32], TC); A.w16([0x32], WC); e(0xD5); A.a16([0xCD], 'wcol'); e(0xD1)
    e(0x1C); e(0x7B); e(0xFE, 32); A.r8(0x20, 'ic')
    A.a16([0x21], 'script'); A.a16([0xCD], 'loadphase')
    A.L('main'); e(0xDB, 0xBF); e(0xE6, 0x80); A.r8(0x28, 'main')
    A.w16([0x2A], CNT); e(0x7C); e(0xB5); A.r8(0x20, 'nolp')
    A.a16([0xCD], 'nextphase')
    A.L('nolp'); A.w16([0x2A], CNT); e(0x2B); A.w16([0x22], CNT)
    A.w16([0x3A], DX); e(0xB7); A.r8(0x28, 'setscroll')
    A.w16([0x2A], CAM); e(0xFE, 1); A.r8(0x20, 'neg1'); e(0x23); A.r8(0x18, 'st')
    A.L('neg1'); e(0x2B)
    A.L('st'); A.w16([0x22], CAM)
    A.w16([0x3A], DX); e(0xFE, 1); A.r8(0x20, 'left')
    e(0x7D); e(0xE6, 7); e(0xFE, 1); A.r8(0x20, 'setscroll')
    A.a16([0xCD], 'cam3'); e(0x57); e(0xE6, 31); A.w16([0x32], TC); e(0x7A); e(0xC6, 32); A.w16([0x32], WC); A.a16([0xCD], 'wcol'); A.r8(0x18, 'setscroll')
    A.L('left'); e(0x7D); e(0xE6, 7); A.r8(0x20, 'setscroll')
    A.a16([0xCD], 'cam3'); e(0x57); e(0xE6, 31); A.w16([0x32], TC); e(0x7A); A.w16([0x32], WC); A.a16([0xCD], 'wcol')
    A.L('setscroll'); A.w16([0x3A], CAM); e(0xED, 0x44); e(0xD3, 0xBF); e(0x3E, 0x88); e(0xD3, 0xBF)
    A.a16([0xC3], 'main')
    A.L('cam3'); A.w16([0x2A], CAM)
    for _ in range(3): e(0xCB, 0x3C); e(0xCB, 0x1D)
    e(0x7D); e(0xC9)
    A.L('wcol'); e(0x06, 0)
    A.L('wr'); e(0x26, 0); e(0x68)
    for _ in range(6): e(0x29)
    A.w16([0x3A], TC); e(0x87); e(0x5F); e(0x16, 0x38); e(0x19)
    e(0x7D); e(0xD3, 0xBF); e(0x7C); e(0xF6, 0x40); e(0xD3, 0xBF)
    A.w16([0x3A], WC); e(0x4F); e(0xCB, 0x3F); e(0xCB, 0x3F); e(0xCB, 0x3F); e(0x81); e(0x57); e(0x78); e(0x87); e(0x80); e(0x82); e(0xE6, 0x3F); e(0xD3, 0xBE)
    e(0x79); e(0x0F); e(0x0F); e(0x0F); e(0x0F); e(0xE6, 1); e(0x87); e(0x57); e(0x78); e(0x0F); e(0x0F); e(0xE6, 1); e(0x87); e(0x87); e(0x87); e(0xB2); e(0xD3, 0xBE)
    e(0x04); e(0x78); e(0xFE, 24); A.r8(0x20, 'wr')
    e(0xC9)
    A.L('nextphase'); A.w16([0x2A], SPT)
    A.L('loadphase'); e(0x5E); e(0x23); e(0x56); e(0x23); e(0x7A); e(0xB3); A.r8(0x20, 'lpok')
    A.a16([0x21], 'script'); A.r8(0x18, 'loadphase')
    A.L('lpok'); A.w16([0xED, 0x53], CNT); e(0x7E); A.w16([0x32], DX); e(0x23); A.w16([0x22], SPT); e(0xC9)
    A.L('regtab')
    regs = [0x24 if blank else 0x04, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF]
    for i, v in enumerate(regs): e(v, 0x80 | i)
    A.L('pal'); A.b += (b''.join(bytes([((i * 7 + 3) & 15) | ((((i * 5 + 1) & 15)) << 4), (i * 11 + 2) & 15]) for i in range(32)) if gg else palette())
    A.L('script')
    for c, d in SCRIPT: e(c & 255, c >> 8, d & 255)
    e(0, 0, 0)
    A.L('tiles'); A.b += tiles()
    rom = bytearray(A.done())
    assert len(rom) < 0x7FF0
    rom += bytes(0x8000 - len(rom))
    rom[0x7FF0:0x7FF8] = b'TMR SEGA'
    rom[0x7FFF] = 0x6C if gg else 0x4C
    return bytes(rom)

if __name__ == '__main__':
    open(sys.argv[1], 'wb').write(build('--noblank' not in sys.argv, '--gg' in sys.argv))
