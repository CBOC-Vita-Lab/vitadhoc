#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""A small 65816 assembler (Python DSL) for the synthetic SNES test cartridges.

    a = Asm(0x8000)            # LoROM bank 0, origin $8000
    a.label("reset"); a.sei(); a.clc(); a.xce(); a.rep(0x30)
    a.lda("#", 0x1234); a.sta("abs", 0x2116); a.bne("reset")

Modes: "#" immediate (width follows the tracked M/X flags: rep/sep update them), "dp", "dpx", "abs", "absx", "absy",
"long", "longx". Branches take a label. Only the instructions the tests use are in the table.
"""
import struct

OPS = {
    "lda": {"#": 0xA9, "dp": 0xA5, "dpx": 0xB5, "abs": 0xAD, "absx": 0xBD, "absy": 0xB9, "long": 0xAF, "longx": 0xBF},
    "sta": {"dp": 0x85, "dpx": 0x95, "abs": 0x8D, "absx": 0x9D, "absy": 0x99, "long": 0x8F, "longx": 0x9F},
    "ldx": {"#": 0xA2, "dp": 0xA6, "abs": 0xAE, "absy": 0xBE},
    "ldy": {"#": 0xA0, "dp": 0xA4, "abs": 0xAC, "absx": 0xBC},
    "stx": {"dp": 0x86, "abs": 0x8E}, "sty": {"dp": 0x84, "abs": 0x8C},
    "stz": {"dp": 0x64, "dpx": 0x74, "abs": 0x9C, "absx": 0x9E},
    "adc": {"#": 0x69, "dp": 0x65, "dpx": 0x75, "abs": 0x6D, "absx": 0x7D, "long": 0x6F, "longx": 0x7F},
    "sbc": {"#": 0xE9, "dp": 0xE5, "abs": 0xED, "absx": 0xFD, "long": 0xEF, "longx": 0xFF},
    "cmp": {"#": 0xC9, "dp": 0xC5, "abs": 0xCD, "absx": 0xDD, "long": 0xCF, "longx": 0xDF},
    "cpx": {"#": 0xE0, "dp": 0xE4, "abs": 0xEC}, "cpy": {"#": 0xC0, "dp": 0xC4, "abs": 0xCC},
    "and": {"#": 0x29, "dp": 0x25, "abs": 0x2D, "long": 0x2F, "longx": 0x3F},
    "ora": {"#": 0x09, "dp": 0x05, "abs": 0x0D, "long": 0x0F, "longx": 0x1F},
    "eor": {"#": 0x49, "dp": 0x45, "abs": 0x4D},
    "inc": {"dp": 0xE6, "abs": 0xEE}, "dec": {"dp": 0xC6, "abs": 0xCE},
    "jmp": {"abs": 0x4C}, "jsr": {"abs": 0x20}, "jsl": {"long": 0x22}, "jml": {"long": 0x5C},
}
IMPL = {
    "inca": 0x1A, "deca": 0x3A, "inx": 0xE8, "iny": 0xC8, "dex": 0xCA, "dey": 0x88, "asla": 0x0A, "lsra": 0x4A, "rola": 0x2A, "rora": 0x6A,
    "tax": 0xAA, "tay": 0xA8, "txa": 0x8A, "tya": 0x98, "txs": 0x9A, "tsx": 0xBA, "tcd": 0x5B, "tcs": 0x1B, "tdc": 0x7B, "txy": 0x9B, "tyx": 0xBB,
    "pha": 0x48, "pla": 0x68, "phx": 0xDA, "plx": 0xFA, "phy": 0x5A, "ply": 0x7A, "phb": 0x8B, "plb": 0xAB, "phd": 0x0B, "pld": 0x2B, "phk": 0x4B,
    "clc": 0x18, "sec": 0x38, "sei": 0x78, "cli": 0x58, "xce": 0xFB, "xba": 0xEB, "nop": 0xEA, "wai": 0xCB, "rts": 0x60, "rtl": 0x6B, "rti": 0x40,
}
BRANCH = {"bra": 0x80, "beq": 0xF0, "bne": 0xD0, "bcc": 0x90, "bcs": 0xB0, "bmi": 0x30, "bpl": 0x10, "bvc": 0x50, "bvs": 0x70}


class Asm:
    def __init__(self, origin=0x8000):
        self.org = origin
        self.buf = bytearray()
        self.labels = {}
        self.fix = []          # (offset in buf, label, kind, pc_after)
        self.m16 = False
        self.x16 = False

    @property
    def pc(self):
        return self.org + len(self.buf)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def byte(self, *vals):
        for v in vals:
            self.buf.append(v & 0xFF)

    def word(self, *vals):
        for v in vals:
            self.buf += struct.pack("<H", v & 0xFFFF)

    def rep(self, mask):
        self.byte(0xC2, mask)
        if mask & 0x20: self.m16 = True
        if mask & 0x10: self.x16 = True

    def sep(self, mask):
        self.byte(0xE2, mask)
        if mask & 0x20: self.m16 = False
        if mask & 0x10: self.x16 = False

    def _op(self, name, mode, arg):
        tab = OPS[name]
        assert mode in tab, (name, mode)
        self.byte(tab[mode])
        if mode == "#":
            wide = self.x16 if name in ("ldx", "ldy", "cpx", "cpy") else self.m16
            if isinstance(arg, str):
                self.fix.append((len(self.buf), arg, "abs16", 0)); self.word(0)
            elif wide: self.word(arg)
            else: self.byte(arg)
        elif mode in ("dp", "dpx"):
            self.byte(arg)
        elif mode in ("abs", "absx", "absy"):
            if isinstance(arg, str):
                self.fix.append((len(self.buf), arg, "abs16", 0)); self.word(0)
            else:
                self.word(arg)
        else:
            if isinstance(arg, str):
                self.fix.append((len(self.buf), arg, "abs24", 0)); self.byte(0, 0, 0)
            else:
                self.byte(arg, arg >> 8, arg >> 16)

    def __getattr__(self, name):
        name = name.rstrip('_')
        if name in OPS:
            return lambda mode, arg: self._op(name, mode, arg)
        if name in IMPL:
            return lambda: self.byte(IMPL[name])
        if name in BRANCH:
            def br(label):
                self.byte(BRANCH[name]); self.fix.append((len(self.buf), label, "rel8", self.pc + 1)); self.byte(0)
            return br
        raise AttributeError(name)

    def link(self):
        for off, label, kind, after in self.fix:
            t = self.labels[label]
            if kind == "abs16": self.buf[off:off + 2] = struct.pack("<H", t & 0xFFFF)
            elif kind == "abs24": self.buf[off:off + 3] = struct.pack("<I", t & 0xFFFFFF)[:3]
            elif kind == "rel8":
                d = t - after
                assert -128 <= d <= 127, "branch to %s out of range (%d)" % (label, d)
                self.buf[off] = d & 0xFF
        return bytes(self.buf)


def make_lorom(banks, title, nmi_addr, reset_addr, sram_kb=0):
    """banks: dict bank -> bytes (loaded at $8000 of that bank). Returns the ROM image (padded to a power of two >= 64 KB)."""
    nb = max(banks) + 1
    size = 1
    while size < nb * 0x8000: size <<= 1
    size = max(size, 0x10000)
    rom = bytearray(size)
    for b, data in banks.items():
        rom[b * 0x8000:b * 0x8000 + len(data)] = data
    h = 0x7FC0
    t = title.encode("ascii")[:21].ljust(21, b" ")
    rom[h:h + 21] = t
    rom[h + 0x15] = 0x20                       # LoROM, slow
    rom[h + 0x16] = 0x00 if not sram_kb else 0x02
    k = 0
    while (1 << k) < size // 1024: k += 1
    rom[h + 0x17] = k                          # ROM size
    rom[h + 0x18] = 0 if not sram_kb else 0x03
    rom[h + 0x19] = 0x01; rom[h + 0x1A] = 0x33
    vec = {0x7FEA: nmi_addr, 0x7FFA: nmi_addr, 0x7FFC: reset_addr, 0x7FEE: nmi_addr, 0x7FFE: nmi_addr}
    for off, v in vec.items():
        rom[off] = v & 0xFF; rom[off + 1] = v >> 8
    s = sum(rom) & 0xFFFF
    rom[h + 0x1E], rom[h + 0x1F] = s & 0xFF, s >> 8
    c = s ^ 0xFFFF
    rom[h + 0x1C], rom[h + 0x1D] = c & 0xFF, c >> 8
    # a second pass so the checksum includes itself correctly
    s = sum(rom) & 0xFFFF
    rom[h + 0x1E], rom[h + 0x1F] = s & 0xFF, s >> 8
    c = s ^ 0xFFFF
    rom[h + 0x1C], rom[h + 0x1D] = c & 0xFF, c >> 8
    return bytes(rom)
