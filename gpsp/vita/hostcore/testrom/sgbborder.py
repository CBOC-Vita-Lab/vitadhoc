#!/usr/bin/env python3
"""sgbborder.py -- hostcore/testrom: a Game Boy cartridge that sends a Super
Game Boy border, for app_tests.py's sgb test (GPL-2.0-or-later, like the
rest of gpsp/vita).  No assembler is needed: the program is a few dozen
SM83 instructions, encoded here.

    python3 sgbborder.py OUT.gb [--silent]

It does what an SGB game does at power-on (Pan Docs, "SGB Command 13h -
CHR_TRN" and "14h - PCT_TRN"): with the LCD off it puts the border's tiles
in VRAM, mapped in order on the screen, turns the LCD on and sends CHR_TRN
for tiles 0-127 and again for 128-255; then the map and its colours, and
PCT_TRN.  Each packet is sent on P1 bit by bit: a reset pulse (P14 and P15
low), 128 bits LSB first (P15 low = 1, P14 low = 0, each followed by both
high) and a 0 stop bit.  Then it waits forever.  --silent: the same
cartridge sending nothing (the border must then come from the frontend's
copy of it).

border_pixel(x, y) gives the colour (BGR555) the frontend must show at
(x, y) of the 256x224 picture."""
import sys

CHR = bytes((i * 37 + 11) & 0xFF for i in range(4096))


def pct_map():
    m = bytearray()
    for e in range(1024):
        m.append((e * 7) & 0xFF)
        m.append((4 + ((e >> 5) & 3)) << 2)
    return bytes(m)


def pal_value(p, k):
    return ((p * 5 + k) & 31) | (((k * 2) & 31) << 5) | (((31 - k) & 31) << 10)


def pct_data():
    d = bytearray(pct_map())
    for p in range(4, 8):
        for k in range(16):
            v = pal_value(p, k)
            d += bytes([v & 0xFF, v >> 8])
    return bytes(d) + bytes(4096 - len(d))


def border_pixel(x, y):
    m = pct_map()
    e = (y // 8) * 32 + x // 8
    tile, attr = m[2 * e], m[2 * e + 1]
    pal = (attr >> 2) & 7
    if pal < 4:
        pal += 4
    chars = CHR + CHR            # CHR_TRN 0 and 1 send the same 128 tiles
    t = tile * 32
    r = y % 8
    bit = 7 - (x % 8)
    a, b = chars[t + r * 2], chars[t + r * 2 + 1]
    c, d = chars[t + 16 + r * 2], chars[t + 16 + r * 2 + 1]
    color = ((a >> bit) & 1) | (((b >> bit) & 1) << 1) | \
        (((c >> bit) & 1) << 2) | (((d >> bit) & 1) << 3)
    idx = pal * 16 + color
    if idx in (64, 80, 96, 112):
        return 0x7FFF            # the game's colour 0 (SGB's power-on white)
    return pal_value(pal, color)


class Asm:
    def __init__(self, org):
        self.b = bytearray()
        self.org = org
        self.labels, self.fix = {}, []

    def here(self):
        return self.org + len(self.b)

    def label(self, name):
        self.labels[name] = self.here()

    def emit(self, *xs):
        self.b += bytes(xs)

    def jr(self, op, name):          # JR / JR NZ / JR Z to a label
        self.emit(op, 0)
        self.fix.append(("rel", len(self.b) - 1, name))

    def call(self, name):
        self.emit(0xCD, 0, 0)
        self.fix.append(("abs", len(self.b) - 2, name))

    def done(self):
        for kind, at, name in self.fix:
            t = self.labels[name]
            if kind == "rel":
                off = t - (self.org + at + 1)
                assert -128 <= off <= 127, name
                self.b[at] = off & 0xFF
            else:
                self.b[at], self.b[at + 1] = t & 0xFF, t >> 8
        return bytes(self.b)


def program(silent):
    a = Asm(0x150)
    a.emit(0xF3, 0x31, 0xFE, 0xFF)                     # DI ; LD SP,$FFFE
    if silent:
        a.label("forever")
        a.jr(0x18, "forever")
        a.label("wait_vbl")
        a.emit(0xC9)
        return a.done()
    a.call("lcd_off")
    a.emit(0x21, 0x00, 0x10, 0x11, 0x00, 0x80, 0x01, 0x00, 0x10)  # CHR -> 8000
    a.call("copy")
    a.emit(0x21, 0x00, 0x20, 0x11, 0x00, 0x98, 0x01, 0x00, 0x04)  # map -> 9800
    a.call("copy")
    a.call("lcd_on")
    a.emit(0x21, 0x00, 0x30)                           # CHR_TRN 0
    a.call("send")
    a.emit(0x21, 0x10, 0x30)                           # CHR_TRN 1
    a.call("send")
    a.call("lcd_off")
    a.emit(0x21, 0x00, 0x40, 0x11, 0x00, 0x80, 0x01, 0x00, 0x10)  # PCT -> 8000
    a.call("copy")
    a.call("lcd_on")
    a.emit(0x21, 0x20, 0x30)                           # PCT_TRN
    a.call("send")
    a.label("forever")
    a.jr(0x18, "forever")

    a.label("wait_vbl")                                 # until LY = 144
    a.emit(0xF0, 0x44, 0xFE, 0x90)
    a.jr(0x20, "wait_vbl")
    a.emit(0xC9)
    a.label("lcd_off")
    a.call("wait_vbl")
    a.emit(0xAF, 0xE0, 0x40, 0xC9)                     # XOR A ; LDH (40),A
    a.label("lcd_on")
    a.emit(0x3E, 0x91, 0xE0, 0x40)                     # LCD on, tiles at 8000
    a.emit(0x06, 0x04)                                 # LD B,4: four frames
    a.label("frames")
    a.call("wait_vbl")
    a.label("not_vbl")
    a.emit(0xF0, 0x44, 0xFE, 0x90)
    a.jr(0x28, "not_vbl")
    a.emit(0x05)                                       # DEC B
    a.jr(0x20, "frames")
    a.emit(0xC9)
    a.label("copy")                                     # HL -> DE, BC bytes
    a.emit(0x2A, 0x12, 0x13, 0x0B, 0x78, 0xB1)         # LD A,(HL+) ; LD (DE),A ; INC DE ; DEC BC ; LD A,B ; OR C
    a.jr(0x20, "copy")
    a.emit(0xC9)
    a.label("send")                                     # one packet at HL
    a.emit(0xAF, 0xE0, 0x00, 0x3E, 0x30, 0xE0, 0x00)   # reset: 00, 30
    a.emit(0x06, 0x10)                                 # LD B,16 bytes
    a.label("byte")
    a.emit(0x2A, 0x5F, 0x0E, 0x08)                     # LD A,(HL+) ; LD E,A ; LD C,8
    a.label("bit")
    a.emit(0x7B, 0xE6, 0x01, 0x3E, 0x20)               # LD A,E ; AND 1 ; LD A,$20 (flags kept)
    a.jr(0x28, "zero")
    a.emit(0x3E, 0x10)                                 # a 1: P15 low
    a.label("zero")
    a.emit(0xE0, 0x00, 0x3E, 0x30, 0xE0, 0x00)         # LDH (00),A ; both high
    a.emit(0xCB, 0x3B, 0x0D)                           # SRL E ; DEC C
    a.jr(0x20, "bit")
    a.emit(0x05)                                       # DEC B
    a.jr(0x20, "byte")
    a.emit(0x3E, 0x20, 0xE0, 0x00, 0x3E, 0x30, 0xE0, 0x00)  # stop bit 0
    a.emit(0x06, 0x04)                                 # four frames' rest
    a.label("rest")
    a.call("wait_vbl")
    a.label("rest2")
    a.emit(0xF0, 0x44, 0xFE, 0x90)
    a.jr(0x28, "rest2")
    a.emit(0x05)
    a.jr(0x20, "rest")
    a.emit(0xC9)
    return a.done()


def rom(silent=False):
    logo = bytes([0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
                  0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
                  0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
                  0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E])
    r = bytearray(0x8000)
    r[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])   # NOP ; JP $0150
    r[0x104:0x134] = logo
    r[0x134:0x13F] = b"SGB BORDER\0"
    r[0x146] = 0x03                                      # SGB functions
    r[0x147] = 0x00
    r[0x14B] = 0x33                                      # new licensee: SGB
    p = program(silent)
    r[0x150:0x150 + len(p)] = p
    assert 0x150 + len(p) < 0x1000
    r[0x1000:0x2000] = CHR
    r[0x2000:0x2400] = bytes((row * 20 + col) & 0xFF if col < 20 else 0
                             for row in range(32) for col in range(32))
    r[0x3000] = (0x13 << 3) | 1                          # CHR_TRN, tiles 0-127
    r[0x3010] = (0x13 << 3) | 1
    r[0x3011] = 1                                        # ...128-255
    r[0x3020] = (0x14 << 3) | 1                          # PCT_TRN
    r[0x4000:0x5000] = pct_data()
    c = 0
    for i in range(0x134, 0x14D):
        c = (c - r[i] - 1) & 0xFF
    r[0x14D] = c
    s = sum(r[i] for i in range(len(r)) if i not in (0x14E, 0x14F))
    r[0x14E], r[0x14F] = (s >> 8) & 0xFF, s & 0xFF
    return bytes(r)


if __name__ == "__main__":
    open(sys.argv[1], "wb").write(rom("--silent" in sys.argv))
