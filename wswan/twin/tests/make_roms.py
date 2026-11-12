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
  (build_orient_table: a ROM driven by a per frame table, for the orientation property test)
  sram_32k.ws .. eeprom_1k.ws  the other save types of the footer (save_type test)
  (build_rtc_rom: copies the RTC clock into SRAM at boot: the rtc test)
  dconan_footer.ws  64 KiB image with the footer of the Detective Conan patch (a load-time bounds test)
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

def build(name, color=False, vertical_flag=False, save="sram", scan="x", icon=False, cart_id=0x70, checksum=None, save_code=None, sram_bank=0):
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
        a.out_i(0xC1, sram_bank)                        # SRAM bank (port C1; the power-on value is 0x3F, so a game selects it)
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
    foot[11] = save_code if save_code is not None else {"sram": 0x01, "eeprom": 0x20, "none": 0x00}[save]   # footer save type byte
    foot[12] = 0x04 | (1 if vertical_flag else 0)
    foot[13] = 0x00
    s = sum(rom[:65536 - 2]) & 0xFFFF                            # checksum (not checked by the core)
    s = (s + sum(foot[:14])) & 0xFFFF
    if checksum is not None: s = checksum     # a footer checksum chosen by the test (the core does not check it)
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
    # 64 KiB image whose footer matches the Detective Conan patch (cart 0x27, checksum 0x8DE1): the core writes
    # reset-vector bytes at 0xFFFE8..0xFFFEC, outside a 64 KiB image unless the host guards it
    "dconan_footer.ws": dict(scan="x", cart_id=0x27, checksum=0x8DE1),
    # every save type the footer can declare (byte 11): SRAM 8 KiB .. 512 KiB, EEPROM 128 B / 1 KiB / 2 KiB, none
    "sram_32k.ws":   dict(scan="x", save_code=0x02, cart_id=0x78),
    "sram_128k.ws":  dict(scan="x", save_code=0x03, cart_id=0x79),
    "sram_256k.ws":  dict(scan="x", save_code=0x04, cart_id=0x7A),
    "sram_512k.ws":  dict(scan="x", save_code=0x05, cart_id=0x7B),
    "eeprom_128.ws": dict(scan="x", save="eeprom", save_code=0x10, cart_id=0x7C),
    "eeprom_1k.ws":  dict(scan="x", save="eeprom", save_code=0x50, cart_id=0x7D),
    # 512 KiB SRAM, the boot counter written in bank 5 (port C1): it lands at file offset 0x50000
    "sram_512k_b5.ws": dict(scan="x", save_code=0x05, sram_bank=5, cart_id=0x7F),
}


TABLE_OFF = 0x8000   # the per frame table lives in the top bank: segment F000, offset 0x8000 (2 bytes per frame)

def build_orient_table(entries, vertical_flag=False, cart_id=0x80):
    """A ROM whose key-group writes and LCD icon come from a table, so a test can drive the orientation detector with any
    sequence. entries[k] = (pattern, icon): pattern bit 0 = write the Y group (0x10) to port B5 in that frame, bit 1 = the
    X group (0x20); icon = the byte written to port 0x15 (bits 1 and 2 are the icons). Frame k of the table is used in the
    frame after the k-th boot frame: run_frame f (f >= 1) evidence = entries[f - 1]. Frames past the table read junk: run
    no more frames than the table has."""
    assert len(entries) * 2 + TABLE_OFF < 0xFFF0
    a = Asm()
    a.emit(0xFA, 0xFC)                                  # cli, cld
    a.xor_ax(); a.mov_ds_ax(); a.mov_ss_ax(); a.mov16(SP, 0x2000)
    a.mov16(AX, 0xF000); a.mov_es_ax()                   # ES = the top bank, for the table reads
    a.st_byte_imm(0x100, 0); a.st_byte_imm(0x101, 0)     # frame index (word)
    a.out_i(0x00, 0x00)
    a.label("frame")
    a.label("w1"); a.in_al(0x02); a.cmp_al(144); a.jne("w1")
    a.label("w2"); a.in_al(0x02); a.cmp_al(144); a.je("w2")
    a.label("w3"); a.in_al(0x02); a.cmp_al(60); a.jne("w3")   # the writes happen mid frame, away from the frame edges
    a.emit(0x8B, 0x1E); a.w(0x0100)                     # mov bx, [0x100]
    a.emit(0x01, 0xDB)                                  # add bx, bx  (2 bytes per entry)
    a.emit(0x26, 0x8A, 0x87); a.w(TABLE_OFF)            # mov al, es:[bx+TABLE]        pattern
    a.emit(0x26, 0x8A, 0x97); a.w(TABLE_OFF + 1)        # mov dl, es:[bx+TABLE+1]      icon
    a.st8(0x102)                                        # keep the pattern
    a.emit(0xA8, 0x01); a.je("skipy")                   # test al,1 ; jz skipy
    a.mov8(AL, 0x10); a.out_al(0xB5)                    # Y group selected: counted as Y evidence
    a.label("skipy")
    a.ld8(0x102); a.emit(0xA8, 0x02); a.je("skipx")     # mov al,[pattern] ; test al,2 ; jz skipx
    a.mov8(AL, 0x20); a.out_al(0xB5)                    # X group
    a.label("skipx")
    a.emit(0x8A, 0xC2)                                  # mov al, dl
    a.out_al(0x15)                                      # LCD icon register
    a.inc_word(0x100)
    a.jmp("frame")
    a.resolve()
    code = bytes(a.b)
    assert len(code) < TABLE_OFF
    rom = bytearray([0xFF] * 65536)
    rom[0:len(code)] = code
    for k, (pat, icon) in enumerate(entries):
        rom[TABLE_OFF + 2 * k] = pat & 0xFF
        rom[TABLE_OFF + 2 * k + 1] = icon & 0xFF
    foot = bytearray(16)
    foot[0:5] = bytes([0xEA, 0x00, 0x00, 0x00, 0xF0])
    foot[6] = 0x01; foot[8] = cart_id; foot[11] = 0x00; foot[12] = 0x04 | (1 if vertical_flag else 0)
    s = (sum(rom[:65536 - 2]) + sum(foot[:14])) & 0xFFFF
    foot[14] = s & 0xFF; foot[15] = s >> 8
    rom[65536 - 16:] = foot
    return bytes(rom)


def build_rtc_rom(cart_id=0x7E):
    """Copies the cartridge RTC's seven bytes (command 0x15: year, month, day, weekday, hour, minute, second, all BCD)
    into the first bytes of cartridge SRAM, then a marker 0xA5 at byte 7, and spins. The save file therefore shows the
    clock the core seeded at load: with a fixed host time it must be the same on every run and every machine."""
    a = Asm()
    a.emit(0x90)                                        # nop (interrupts stay disabled)
    a.xor_ax(); a.mov_ds_ax(); a.mov_ss_ax(); a.mov16(SP, 0x2000)
    a.mov16(AX, 0x1000); a.mov_es_ax()                  # ES = SRAM segment (offset 0 = first SRAM byte)
    a.out_i(0xCA, 0x15)                                 # RTC command: read the time
    for i in range(7):
        a.in_al(0xCB)
        a.emit(0x26, 0xA2); a.w(i)                      # mov [es:i], al
    a.emit(0x26, 0xC6, 0x06); a.w(7); a.emit(0xA5)     # mov byte [es:7], 0xA5
    a.label("spin"); a.jmp("spin")
    a.resolve()
    code = bytes(a.b)
    rom = bytearray([0xFF] * 65536)
    rom[0:len(code)] = code
    foot = bytearray(16)
    foot[0:5] = bytes([0xEA, 0x00, 0x00, 0x00, 0xF0])
    foot[6] = 0x01; foot[8] = cart_id; foot[11] = 0x01; foot[12] = 0x04
    s = (sum(rom[:65536 - 2]) + sum(foot[:14])) & 0xFFFF
    foot[14] = s & 0xFF; foot[15] = s >> 8
    rom[65536 - 16:] = foot
    return bytes(rom)

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for n, kw in ROMS.items():
        with open(os.path.join(out, n), "wb") as f:
            f.write(build(n, **kw))
    print("wrote", len(ROMS), "ROMs to", out)
