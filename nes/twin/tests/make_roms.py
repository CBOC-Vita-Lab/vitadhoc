#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Writes the synthetic test cartridges of the NES twin (hand-written 6502, no commercial code).

  make_roms.py OUTDIR

  nrom.nes          iNES 1.0, NROM-128, 8 KiB battery RAM. Shows a pattern, 12 sprites on one row and a sprite the
                    d-pad moves; reads both pads (24 bits each: Four Score data too) every frame; sets the
                    background colour from the pads; plays a 440 Hz square wave; counts boots at $6000 and
                    presses of A at $6003; keeps the pad bytes at $6001/$6002/$6010..$6015 and $2002 (sprite
                    overflow) at $6020; the NMI count at $6030.
  nrom_nes2.nes     the same cartridge with a NES 2.0 header (must look identical)
  nrom_trainer.nes  the same with a 512 byte (zero) trainer in front of the PRG ROM
  nrom_diskdude.nes the same with "DiskDude!" junk in header bytes 7..15 (an old copier's mark)
  nrom_pal.nes      NES 2.0 header, timing PAL;  nrom_dendy.nes timing Dendy
  mmc1.nes          MMC1, 64 KiB PRG: switches three banks, stores their marker bytes at $6040..$6042
  mmc3.nes          MMC3, 128 KiB PRG: bank markers at $6040.., one scanline IRQ per frame counted at $6010
  gun.nes           left half of the screen white, right half black; counts polls of $4017 with light at $10/$12
                    and sets $11 when the Zapper trigger is seen (test with --opt zapper=1 --zapper ...)
  gun_hdr.nes, paddle_hdr.nes   the same with a NES 2.0 header naming the Zapper / Arkanoid as the expansion device
  paddle.nes        reads the Arkanoid paddle byte into $10 and the button into $11 each frame (--opt paddle=1)
  unif.unf          the NROM-128 cartridge as a UNIF file
  fds_bios/disksys.rom, nrom.fds, nrom_hdr.fds   a test "BIOS" and a disk image (with and without the
                    FDS header); the BIOS shows the drive status ($4032) as the background colour
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nesasm import assemble

ZP = """
P1A = $10
P1B = $11
P1C = $12
P2A = $13
P2B = $14
P2C = $15
PREVA = $16
"""


def chr_rom():
    t = bytearray(8192)
    # tile 1: colour 1 solid; tile 2: colour 2 solid; tile 3: a checker of colours 1/2/3
    for r in range(8):
        t[16 + r] = 0xFF
        t[32 + 8 + r] = 0xFF
        t[48 + r] = 0xAA if r % 2 == 0 else 0x55
        t[48 + 8 + r] = 0xCC if r % 2 == 0 else 0x33
    t[0x1000:0x1040] = t[0:0x40]
    return bytes(t)


PALETTE = """
pal: .byte $0f,$16,$2a,$12, $0f,$06,$19,$21, $0f,$0c,$1a,$28, $0f,$02,$22,$30
     .byte $0f,$30,$27,$11, $0f,$16,$26,$36, $0f,$0c,$1c,$2c, $0f,$09,$19,$29
"""

NROM_SRC = ZP + """
.org $C000
reset:
  sei
  cld
  ldx #$40
  stx $4017
  ldx #$ff
  txs
  inx
  stx $2000
  stx $2001
  stx $4010
  bit $2002
w1: bit $2002
  bpl w1
  lda #0
  tax
clr: sta $00,x
  sta $0100,x
  sta $0300,x
  sta $0400,x
  sta $0500,x
  sta $0600,x
  sta $0700,x
  lda #$fe
  sta $0200,x
  lda #0
  inx
  bne clr
  inc $6000
w2: bit $2002
  bpl w2
  lda #$3f
  sta $2006
  lda #0
  sta $2006
  ldx #0
pl: lda pal,x
  sta $2007
  inx
  cpx #32
  bne pl
  lda #$20
  sta $2006
  lda #0
  sta $2006
  ldy #4
no: ldx #0
ni: txa
  and #3
  sta $2007
  inx
  bne ni
  dey
  bne no
  lda #$80
  sta $0200
  lda #3
  sta $0201
  lda #0
  sta $0202
  lda #$80
  sta $0203
  ldx #0
  ldy #4
sl: lda #$60
  sta $0200,y
  lda #1
  sta $0201,y
  lda #0
  sta $0202,y
  txa
  asl a
  asl a
  asl a
  asl a
  clc
  adc #$10
  sta $0203,y
  iny
  iny
  iny
  iny
  inx
  cpx #12
  bne sl
  lda #1
  sta $4015
  lda #$bf
  sta $4000
  lda #$fd
  sta $4002
  lda #0
  sta $4003
  lda #$88
  sta $2000
  lda #$1e
  sta $2001
forever: jmp forever

nmi:
  pha
  txa
  pha
  tya
  pha
  lda $2002
  sta $6020
  lda #0
  sta $2003
  lda #2
  sta $4014
  lda #1
  sta $4016
  lda #0
  sta $4016
  ldy #8
r1: lda $4016
  lsr a
  rol P1A
  lda $4017
  lsr a
  rol P2A
  dey
  bne r1
  ldy #8
r2: lda $4016
  lsr a
  rol P1B
  lda $4017
  lsr a
  rol P2B
  dey
  bne r2
  ldy #8
r3: lda $4016
  lsr a
  rol P1C
  lda $4017
  lsr a
  rol P2C
  dey
  bne r3
  lda P1A
  sta $6001
  lda P2A
  sta $6002
  lda P1A
  sta $6010
  lda P1B
  sta $6011
  lda P1C
  sta $6012
  lda P2A
  sta $6013
  lda P2B
  sta $6014
  lda P2C
  sta $6015
  lda #$3f
  sta $2006
  lda #0
  sta $2006
  lda P1A
  and #$3f
  sta $2007
  lda P2A
  and #$3f
  sta $2007
  lda P1A
  and #1
  beq nr
  inc $0203
nr: lda P1A
  and #2
  beq nl
  dec $0203
nl: lda P1A
  and #4
  beq nd
  inc $0200
nd: lda P1A
  and #8
  beq nu
  dec $0200
nu: lda P1A
  and #$80
  beq arel
  lda PREVA
  bne adone
  inc $6003
  lda #1
  sta PREVA
  jmp adone
arel: lda #0
  sta PREVA
adone: inc $6030
  lda #$88
  sta $2000
  lda #0
  sta $2005
  sta $2005
  pla
  tay
  pla
  tax
  pla
  rti
irq: rti
""" + PALETTE + """
.org $FFFA
.word nmi
.word reset
.word irq
"""


def ines(prg, chr_, mapper=0, battery=True, mirror=1, nes2=False, trainer=None, tail=b"", region=None,
         junk=False, prg_ram_nv=None):
    """prg/chr_ are bytes. Returns the file."""
    h = bytearray(16)
    h[0:4] = b"NES\x1a"
    h[4] = (len(prg) // 16384) & 0xFF
    h[5] = (len(chr_) // 8192) & 0xFF
    h[6] = (mapper & 15) << 4 | (mirror & 1) | (2 if battery else 0) | (4 if trainer else 0)
    h[7] = (mapper & 0xF0)
    if nes2:
        h[7] |= 8
        h[8] = (mapper >> 8) & 15
        h[9] = 0
        # byte 10: PRG-RAM / NVRAM sizes (shift counts; 7 = 8 KiB)
        if battery:
            h[10] = (7 << 4)
        if region is not None:
            h[12] = region
    if junk:
        h[7:16] = b"DiskDude!"
    return bytes(h) + (trainer or b"") + prg + chr_ + tail


def nrom_prg():
    off, code, _ = assemble(NROM_SRC)
    assert off == 0xC000 and len(code) == 0x4000, (hex(off), len(code))
    return code


MMC1_SRC = ZP + """
.org $C000
wctl: sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
  rts
wprg: sta $e000
  lsr a
  sta $e000
  lsr a
  sta $e000
  lsr a
  sta $e000
  lsr a
  sta $e000
  rts
reset:
  sei
  cld
  ldx #$ff
  txs
  lda #$80
  sta $8000
  lda #$0e
  jsr wctl
  ldx #0
  stx $2000
  stx $2001
w1: bit $2002
  bpl w1
  ldx #0
bk: txa
  jsr wprg
  lda $8000
  sta $6040,x
  inx
  cpx #3
  bne bk
  inc $6000
w2: bit $2002
  bpl w2
  lda #$3f
  sta $2006
  lda #0
  sta $2006
  lda $6040
  cmp #$b0
  bne bad
  lda $6041
  cmp #$b1
  bne bad
  lda $6042
  cmp #$b2
  bne bad
  lda #$2a
  jmp setc
bad: lda #$16
setc: sta $2007
  lda #$20
  sta $2006
  lda #0
  sta $2006
  ldy #4
no: ldx #0
ni: txa
  and #3
  sta $2007
  inx
  bne ni
  dey
  bne no
  lda #$80
  sta $2000
  lda #$0a
  sta $2001
forever: jmp forever
nmi:
  pha
  inc $6030
  lda $6030
  and #1
  jsr wprg
  lda $8000
  sta $6044
  lda #0
  sta $2005
  sta $2005
  pla
  rti
irq: rti
.org $FFFA
.word nmi
.word reset
.word irq
"""

MMC3_SRC = ZP + """
.org $E000
reset:
  sei
  cld
  ldx #$ff
  txs
  inx
  stx $2000
  stx $2001
  stx $4015
w1: bit $2002
  bpl w1
  lda #1
  sta $a000
  lda #$80
  sta $a001
  lda #0
  sta $8000
  lda #0
  sta $8001
  lda #1
  sta $8000
  lda #2
  sta $8001
  lda #2
  sta $8000
  lda #4
  sta $8001
  lda #3
  sta $8000
  lda #5
  sta $8001
  lda #4
  sta $8000
  lda #6
  sta $8001
  lda #5
  sta $8000
  lda #7
  sta $8001
  ldx #0
bk: lda #6
  sta $8000
  stx $8001
  lda $8000
  sta $6040,x
  inx
  cpx #5
  bne bk
  inc $6000
w2: bit $2002
  bpl w2
  lda #$3f
  sta $2006
  lda #0
  sta $2006
  ldx #0
pl: lda pal,x
  sta $2007
  inx
  cpx #32
  bne pl
  lda #$20
  sta $2006
  lda #0
  sta $2006
  ldy #4
no: ldx #0
ni: txa
  and #3
  sta $2007
  inx
  bne ni
  dey
  bne no
  lda #$88
  sta $2000
  lda #$1e
  sta $2001
  cli
forever: jmp forever
nmi:
  pha
  inc $6030
  lda #79
  sta $c000
  sta $c001
  sta $e001
  lda #$1e
  sta $2001
  lda #$88
  sta $2000
  lda #0
  sta $2005
  sta $2005
  pla
  rti
irq:
  pha
  sta $e000
  inc $6010
  lda #$3e
  sta $2001
  pla
  rti
""" + PALETTE + """
.org $FFFA
.word nmi
.word reset
.word irq
"""

# A test "BIOS" for the Famicom Disk System: shows the drive's status bit 0 ($4032: 1 = no disk in the
# drive) as the background colour and the pad as another. 8 KiB at $E000.
FDS_BIOS_SRC = ZP + """
.org $E000
reset:
  sei
  cld
  ldx #$ff
  txs
  inx
  stx $2000
  stx $2001
w1: bit $2002
  bpl w1
w2: bit $2002
  bpl w2
  lda #$80
  sta $2000
  lda #$0a
  sta $2001
loop: lda $4032
  and #1
  tax
  lda cols,x
  sta $00
  jmp loop
nmi:
  pha
  lda #$3f
  sta $2006
  lda #0
  sta $2006
  lda $00
  sta $2007
  lda #0
  sta $2005
  sta $2005
  pla
  rti
irq: rti
cols: .byte $2a,$16
.org $FFFA
.word nmi
.word reset
.word irq
"""


GUN_SRC = """
.org $C000
reset:
  sei
  cld
  ldx #$ff
  txs
  inx
  stx $2000
  stx $2001
w1: bit $2002
  bpl w1
  lda #0
  tax
clr: sta $00,x
  inx
  bne clr
w2: bit $2002
  bpl w2
  lda #$3f
  sta $2006
  lda #0
  sta $2006
  lda #$0f
  sta $2007
  lda #$30
  sta $2007
  lda #$0f
  sta $2007
  sta $2007
  lda #$20
  sta $2006
  lda #0
  sta $2006
  ldy #30
row: ldx #0
col: txa
  and #$10
  eor #$10
  lsr a
  lsr a
  lsr a
  lsr a
  sta $2007
  inx
  cpx #32
  bne col
  dey
  bne row
  lda #0
  sta $2005
  sta $2005
  lda #$0a
  sta $2001
loop: lda $4017
  and #$08
  bne nol
  inc $10
  bne nol
  inc $12
nol: lda $4017
  and #$10
  beq not
  lda #1
  sta $11
not: jmp loop
nmi: rti
irq: rti
.org $FFFA
.word nmi
.word reset
.word irq
"""

PADDLE_SRC = """
.org $C000
reset:
  sei
  cld
  ldx #$ff
  txs
  inx
  stx $2000
  stx $2001
frame: bit $2002
  bpl frame
  lda #1
  sta $4016
  lda #0
  sta $4016
  ldx #8
  lda #0
  sta $10
rd: lda $4017
  and #$10
  cmp #$10
  rol $10
  dex
  bne rd
  lda $4017
  and #$08
  sta $11
  jmp frame
nmi: rti
irq: rti
.org $FFFA
.word nmi
.word reset
.word irq
"""


def make_fds_side():
    d = bytearray(65500)
    d[0] = 0x01
    d[1:15] = b"*NINTENDO-HVC*"
    d[15] = 0x01       # maker
    d[16:20] = b"TEST"
    return bytes(d)


def unif(prg, chr_, battery=True, mirror=1):
    def chunk(tag, data):
        return tag + struct.pack("<I", len(data)) + data
    body = b""
    body += chunk(b"MAPR", b"NES-NROM-128\x00")
    body += chunk(b"PRG0", prg)
    body += chunk(b"CHR0", chr_)
    body += chunk(b"MIRR", bytes([mirror]))
    if battery:
        body += chunk(b"BATR", b"\x01")
    return b"UNIF" + struct.pack("<I", 7) + b"\x00" * 24 + body


def main(out):
    os.makedirs(out, exist_ok=True)
    prg = nrom_prg()
    ch = chr_rom()

    def w(name, data):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)

    w("nrom.nes", ines(prg, ch))
    w("nrom_nes2.nes", ines(prg, ch, nes2=True))
    w("nrom_trainer.nes", ines(prg, ch, trainer=b"\x00" * 512))
    w("nrom_diskdude.nes", ines(prg, ch, junk=True))
    w("nrom_pal.nes", ines(prg, ch, nes2=True, region=1))
    w("nrom_dendy.nes", ines(prg, ch, nes2=True, region=3))
    w("nrom_nobatt.nes", ines(prg, ch, battery=False))

    # MMC1: four 16 KiB banks; bank n (0..2) starts with the marker byte 0xB0+n; bank 3 holds the code
    off, code, _ = assemble(MMC1_SRC)
    assert off == 0xC000 and len(code) == 0x4000
    banks = []
    for n in range(3):
        b = bytearray(0x4000)
        b[0] = 0xB0 + n
        banks.append(bytes(b))
    w("mmc1.nes", ines(b"".join(banks) + code, ch, mapper=1))

    # MMC3: sixteen 8 KiB banks; bank n starts with 0xC0+n; the last holds the code, bank 14 is fixed at $C000
    off, code, _ = assemble(MMC3_SRC)
    assert off == 0xE000 and len(code) == 0x2000, (hex(off), len(code))
    banks = []
    for n in range(15):
        b = bytearray(0x2000)
        b[0] = 0xC0 + n
        banks.append(bytes(b))
    w("mmc3.nes", ines(b"".join(banks) + code, ch, mapper=4))

    for name, src in (("gun.nes", GUN_SRC), ("paddle.nes", PADDLE_SRC)):
        off, code, _ = assemble(src)
        assert off == 0xC000 and len(code) == 0x4000, (name, hex(off), len(code))
        w(name, ines(code, ch, battery=False))
        # the same with a NES 2.0 header that names the default expansion device (byte 15): 8 = Zapper, 0x0F = Arkanoid
        h = bytearray(ines(code, ch, battery=False, nes2=True))
        h[15] = 0x08 if name == "gun.nes" else 0x0F
        w(name.replace(".nes", "_hdr.nes"), bytes(h))

    w("unif.unf", unif(prg, ch))

    off, bios, _ = assemble(FDS_BIOS_SRC)
    assert off == 0xE000 and len(bios) == 0x2000
    os.makedirs(os.path.join(out, "fds_bios"), exist_ok=True)
    w("fds_bios/disksys.rom", bios)
    side = make_fds_side()
    w("nrom.fds", side)
    w("nrom_hdr.fds", b"FDS\x1a\x01" + b"\x00" * 11 + side)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make_roms.py OUTDIR")
    main(sys.argv[1])
