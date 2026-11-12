#!/usr/bin/env python3
"""Synthetic WonderSwan test ROMs, assembled here from hand-written V30MZ (8086 subset) machine code.
No commercial code, nothing downloaded. Usage: python3 -I make_roms.py OUTDIR

Every ROM: 64 KiB, boots through the footer far jump, sets up a tile map with diagonal bands that scroll one pixel per frame
(BG X scroll = frame counter), shows the key state read from the key matrix (port 0xB5) as 4+4+4 cells on row 17
(black cell = pressed), plays a square wave on channel 1 (about 272 Hz), and on every boot increments a counter in
cartridge SRAM / EEPROM (so a save made by run N is seen by run N+1).

  h_sram.ws     mono, footer horizontal, 8 KiB SRAM, scans the X group + buttons
  v_sram.ws     mono, footer VERTICAL flag, 8 KiB SRAM, scans the Y group + buttons
  switch.ws     mono, footer horizontal, scans X + buttons for 64 frames, then Y + buttons for 64 frames, and so on
  icon.ws       mono, footer horizontal, scans ALL groups every frame, LCD icon port 0x15: horizontal icon for 128 frames, then
                vertical icon for 128 frames, and so on  (the way Klonoa: Moonlight Museum signals it)
  h_eeprom.ws   mono, 2 KiB cartridge EEPROM instead of SRAM
  h_nosave.ws   mono, no save memory
  color.ws      WonderSwan Color (footer: colour only), colour video mode, red/green/blue bands, SRAM
"""
import os, sys

AX, CX, DX, BX, SP, BP, SI, DI = range(8)
AL, CL, DL, BL, AH, CH, DH, BH = range(8)

class Asm:
    def __init__(self):
        self.b = bytearray(); self.labels = {}; self.fix = []
    def emit(self, *x):
        for v in x:
            if isinstance(v, (bytes, bytearray)): self.b += v
            else: self.b.append(v & 0xFF)
    def w(self, v): self.emit(v & 0xFF, (v >> 8) & 0xFF)
    def label(self, n): self.labels[n] = len(self.b)
    # data movement
    def mov16(self, r, v): self.emit(0xB8 + r); self.w(v)
    def mov8(self, r, v): self.emit(0xB0 + r, v)
    def out_i(self, port, v): self.mov8(AL, v); self.emit(0xE6, port)
    def out_al(self, port): self.emit(0xE6, port)
    def in_al(self, port): self.emit(0xE4, port)
    def mov_ds_ax(self): self.emit(0x8E, 0xD8)
    def mov_es_ax(self): self.emit(0x8E, 0xC0)
    def mov_ss_ax(self): self.emit(0x8E, 0xD0)
    def st8(self, addr): self.emit(0xA2); self.w(addr)         # mov [addr], al
    def ld8(self, addr): self.emit(0xA0); self.w(addr)         # mov al, [addr]
    def st_byte_imm(self, addr, v): self.emit(0xC6, 0x06); self.w(addr); self.emit(v)
    def inc_word(self, addr): self.emit(0xFF, 0x06); self.w(addr)
    def and_al(self, v): self.emit(0x24, v)
    def cmp_al(self, v): self.emit(0x3C, v)
    def xor_ax(self): self.emit(0x33, 0xC0)
    def nops(self, n): self.emit(*([0x90] * n))
    def test_byte(self, addr, v): self.emit(0xF6, 0x06); self.w(addr); self.emit(v)
    # control flow (near jumps only; conditional = inverted short skip over a near jmp)
    def jmp(self, n): self.emit(0xE9); self.fix.append((len(self.b), n)); self.w(0)
    def jne(self, n): self.emit(0x74, 0x03); self.jmp(n)
    def je(self, n): self.emit(0x75, 0x03); self.jmp(n)
    def jz(self, n): self.je(n)
    def jnz(self, n): self.jne(n)
    def call(self, n): self.emit(0xE8); self.fix.append((len(self.b), n)); self.w(0)
    def ret(self): self.emit(0xC3)
    def resolve(self):
        for pos, n in self.fix:
            rel = self.labels[n] - (pos + 2)
            self.b[pos] = rel & 0xFF; self.b[pos + 1] = (rel >> 8) & 0xFF

def build(name, color=False, vertical_flag=False, save="sram", scan="x", icon=False, cart_id=0x70):
    a = Asm()
    a.emit(0xFA, 0xFC)                                  # cli, cld
    a.xor_ax(); a.mov_ds_ax(); a.mov_es_ax(); a.mov_ss_ax(); a.mov16(SP, 0x2000)
    a.out_i(0x00, 0x00)                                 # display off while setting up
    if color:
        a.out_i(0x60, 0x80)                             # colour video mode
    # shade pool and palette 0 (mono): pool index 0..3 = white, light, dark, black
    a.out_i(0x1C, 0x50); a.out_i(0x1D, 0xFA); a.out_i(0x1E, 0xFF); a.out_i(0x1F, 0xFF)
    a.out_i(0x20, 0x10); a.out_i(0x21, 0x32)
    # colour palette 0 at 0xFE00 (colour mode only matters): white, red, green, blue
    for i, c in enumerate((0x0FFF, 0x0F00, 0x00F0, 0x000F)):
        a.mov16(AX, c); a.emit(0xA3); a.w(0xFE00 + 2 * i)
    # tiles at 0x2000, 16 bytes each (2 bytes per row: plane 0, plane 1): tile n is a solid colour n
    for n in range(4):
        for row in range(8):
            a.st_byte_imm(0x2000 + n * 16 + row * 2, 0xFF if n & 1 else 0x00)
            a.st_byte_imm(0x2000 + n * 16 + row * 2 + 1, 0xFF if n & 2 else 0x00)
    # BG map at 0x0800 (32 x 32): diagonal bands, tile = ((x >> 1) + (y >> 1)) & 3  -> unrolled for the 28 x 18 visible cells
    for y in range(18):
        for x in range(28):
            a.st_byte_imm(0x0800 + (y * 32 + x) * 2, ((x >> 1) + (y >> 1)) & 3)
    a.out_i(0x07, 0x01)                                 # BG map base 0x0800
    # wave table: 8 x 0xFF then 8 x 0x00 at 0x0200 (channel 1's 16 bytes)
    for i in range(16): a.st_byte_imm(0x0200 + i, 0xFF if i < 8 else 0x00)
    a.out_i(0x8F, 0x08)
    a.out_i(0x80, 0xA0); a.out_i(0x81, 0x06)            # period 0x6A0
    a.out_i(0x88, 0xFF); a.out_i(0x91, 0x0F); a.out_i(0x90, 0x01)
    # save memory: increment the boot counter
    if save == "sram":
        a.mov16(AX, 0x1000); a.mov_es_ax(); a.emit(0x26, 0xFE, 0x06); a.w(0x0000)    # es: inc byte [0]
        a.xor_ax(); a.mov_es_ax()
    elif save == "eeprom":
        a.out_i(0xC6, 0x00); a.out_i(0xC7, 0x00); a.in_al(0xC4); a.emit(0xFE, 0xC0); a.out_al(0xC4)   # in al,C4; inc al; out C4,al
    a.out_i(0x00, 0x01)                                 # BG on
    a.st_byte_imm(0x100, 0); a.st_byte_imm(0x101, 0)

    a.label("frame")
    a.label("w1"); a.in_al(0x02); a.cmp_al(144); a.jne("w1")        # wait for the vertical blank line
    a.label("w2"); a.in_al(0x02); a.cmp_al(144); a.je("w2")
    a.inc_word(0x100)
    a.ld8(0x100); a.out_al(0x10)                        # BG X scroll = frame counter
    if icon:                                            # LCD icon by the frame counter bit 7
        a.test_byte(0x100, 0x80); a.jz("hicon")
        a.out_i(0x15, 0x02); a.jmp("icdone")
        a.label("hicon"); a.out_i(0x15, 0x04); a.label("icdone")
    # key scan: groups to read this frame
    def scan_group(sel, mem):
        a.out_i(0xB5, sel); a.nops(6); a.in_al(0xB5); a.and_al(0x0F); a.st8(mem)
    if scan == "x":
        scan_group(0x20, 0x110); a.st_byte_imm(0x111, 0); scan_group(0x40, 0x112)
    elif scan == "y":
        a.st_byte_imm(0x110, 0); scan_group(0x10, 0x111); scan_group(0x40, 0x112)
    elif scan == "all":
        scan_group(0x20, 0x110); scan_group(0x10, 0x111); scan_group(0x40, 0x112)
    elif scan == "alt":                                  # X for 64 frames, then Y for 64
        a.test_byte(0x100, 0x40); a.jnz("scy")
        scan_group(0x20, 0x110); a.st_byte_imm(0x111, 0); a.jmp("scd")
        a.label("scy"); a.st_byte_imm(0x110, 0); scan_group(0x10, 0x111)
        a.label("scd"); scan_group(0x40, 0x112)
    # show the three nibbles as 4 cells each on row 17 (columns 2..5, 8..11, 14..17): black when pressed
    for k, (mem, col0) in enumerate(((0x110, 2), (0x111, 8), (0x112, 14))):
        for bit in range(4):
            a.ld8(mem); a.and_al(1 << bit)
            a.emit(0x74, 0x04); a.mov8(AL, 3); a.emit(0xEB, 0x02); a.emit(0x32, 0xC0)
            a.st8(0x0800 + (17 * 32 + col0 + bit) * 2)
    a.jmp("frame")
    a.resolve()

    code = bytes(a.b)
    assert len(code) < 0xFF00, len(code)
    rom = bytearray([0xFF] * 65536)
    rom[0:len(code)] = code
    foot = bytearray(16)
    foot[0:5] = bytes([0xEA, 0x00, 0x00, 0x00, 0xF0])          # jmpf F000:0000
    foot[5] = 0x00
    foot[6] = 0x01                                               # developer id
    foot[7] = 0x01 if color else 0x00                           # minimum system: colour / mono
    foot[8] = cart_id                                            # cart id
    foot[9] = 0x00
    foot[10] = 0x00                                              # ROM size code
    foot[11] = {"sram": 0x01, "eeprom": 0x20, "none": 0x00}[save]
    foot[12] = 0x04 | (1 if vertical_flag else 0)
    foot[13] = 0x00
    s = sum(rom[:65536 - 2]) & 0xFFFF                            # checksum (not checked by the core)
    s = (s + sum(foot[:14])) & 0xFFFF
    foot[14] = s & 0xFF; foot[15] = s >> 8
    rom[65536 - 16:] = foot
    return bytes(rom)

ROMS = {
    "h_sram.ws":    dict(scan="x", cart_id=0x71),
    "v_sram.ws":    dict(scan="y", vertical_flag=True, cart_id=0x72),
    "switch.ws":    dict(scan="alt", cart_id=0x73),
    "icon.ws":      dict(scan="all", icon=True, cart_id=0x74),
    "h_eeprom.ws":  dict(scan="x", save="eeprom", cart_id=0x75),
    "h_nosave.ws":  dict(scan="x", save="none", cart_id=0x76),
    "color.ws":     dict(scan="x", color=True, cart_id=0x77),
}

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for n, kw in ROMS.items():
        with open(os.path.join(out, n), "wb") as f:
            f.write(build(n, **kw))
    print("wrote", len(ROMS), "ROMs to", out)
