#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Synthetic scrolling cartridges for the wide view tests (hand-written 6502, generated art, no commercial code).

Every ROM comes with a ".meta" file and a ".truth" file: the TRUE full level picture (what a screen of any width would
show), so the twin can count the wrong pixels in the margins of the composed wide picture.

  scroll_vert.nes   vertical mirroring, horizontal scroller that streams one new column at the seam like commercial games
                    (2 columns ahead, attributes a frame after the tiles, left 8 px masked off), sprite-0 status bar split
                    at the top, sky rows uniform, ground + platforms + walls + pits not periodic
  scroll_vert_nomask.nes   the same with the left mask off
  single_screen.nes MMC1 one-screen mirroring, no scrolling: a playfield of platforms, sprites, one scene change
  scroll_4way.nes   horizontal mirroring, left 8 px masked; right along the top corridor, down/up the shafts (no left scroll:
                    with a 32 column ring that needs a column the hardware cannot hold)
  scroll_mmc3.nes   MMC3, vertical mirroring, scrolling level with an IRQ status bar at the bottom that switches the CHR
                    banks and the scroll, plus a cosmetic CHR bank animation of the playfield
  scroll_chrram.nes CHR-RAM game (no CHR ROM): tiles are uploaded from PRG, and a second tile set replaces the first
                    half way (screen blank while uploading)

RAM at $0300: TX, FX, TY, FY, frame counter, tile set, scene.
"""
import os, random, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nesasm import assemble

BD = 0x21
PAL_BG = [[BD, 0x30, 0x10, 0x00], [BD, 0x19, 0x29, 0x07], [BD, 0x17, 0x27, 0x37], [BD, 0x0F, 0x30, 0x10]]
PAL_SP = [[BD, 0x16, 0x27, 0x30], [BD, 0x30, 0x12, 0x0F], [BD, 0x16, 0x26, 0x36], [BD, 0x09, 0x19, 0x29]]

# tile numbers
T_BLANK = 0
T_CLOUD = 1            # 1..8 (4x2)
T_GT = 16              # ground top variants 16..23
T_GF = 24              # ground fill variants 24..31
T_BR = 32              # bricks 32..39
T_WALL = 40            # wall 40..47
T_GLYPH = 64           # HUD glyphs 64..95
T_HUDB = 96            # HUD background (solid colour 1: black in palette 3)
T_HUDD = 97            # HUD divider (solid colour 3)


def tile_bytes(px):
    lo = bytearray(8)
    hi = bytearray(8)
    for y in range(8):
        for x in range(8):
            v = px[y][x]
            if v & 1:
                lo[y] |= 0x80 >> x
            if v & 2:
                hi[y] |= 0x80 >> x
    return bytes(lo) + bytes(hi)


def solid(v):
    return [[v] * 8 for _ in range(8)]


def make_bg_tiles(seed):
    """256 BG tiles (list of 8x8 index arrays). Art is generated; seed picks the variant (tile set 0 or 1)."""
    rng = random.Random(seed)
    t = [solid(0) for _ in range(256)]
    # clouds: 4x2 tiles from a 32x16 canvas of three circles
    cv = [[0] * 32 for _ in range(16)]
    for cx, cy, r in ((8, 9, 6), (16, 7, 7), (24, 9, 6)):
        for y in range(16):
            for x in range(32):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r and y <= 13:
                    cv[y][x] = 1
    for x in range(32):
        for y in range(15):
            if cv[y][x] == 1 and (y == 13 or cv[y + 1][x] == 0):
                cv[y][x] = 2
    for ty in range(2):
        for tx in range(4):
            t[T_CLOUD + ty * 4 + tx] = [row[tx * 8:tx * 8 + 8] for row in cv[ty * 8:ty * 8 + 8]]
    for k in range(8):
        g = [[3] * 8 for _ in range(8)]
        for x in range(8):
            g[0][x] = 2 if rng.random() < 0.7 else 1
            g[1][x] = 1
            g[2][x] = 1 if rng.random() < 0.5 else 3
        for y in range(3, 8):
            for x in range(8):
                if rng.random() < 0.18:
                    g[y][x] = rng.choice((1, 2))
        t[T_GT + k] = g
        f = [[3] * 8 for _ in range(8)]
        for y in range(8):
            for x in range(8):
                if rng.random() < 0.2:
                    f[y][x] = rng.choice((1, 2))
        t[T_GF + k] = f
        b = [[2] * 8 for _ in range(8)]
        for x in range(8):
            b[0][x] = 3
            b[4][x] = 3
        for y in range(8):
            b[y][0 if y < 4 else 4] = 3
        for y in range(8):
            for x in range(8):
                if rng.random() < 0.06:
                    b[y][x] = 1
        t[T_BR + k] = b
    for k in range(8):
        w = [[1] * 8 for _ in range(8)]
        for y in range(8):
            w[y][0] = 3
            w[y][7] = 2
            if y in (0, 7):
                w[y] = [3] * 8
        for y in range(8):
            for x in range(1, 7):
                if rng.random() < 0.1:
                    w[y][x] = rng.choice((2, 3))
        t[T_WALL + k] = w
    for k in range(32):
        g = solid(1)
        for y in range(1, 7):
            for x in range(1, 7):
                if rng.random() < 0.45:
                    g[y][x] = 2
        t[T_GLYPH + k] = g
    t[T_HUDB] = solid(1)
    t[T_HUDD] = solid(3)
    return t


def make_sp_tiles():
    t = [solid(0) for _ in range(256)]
    t[0] = solid(1)
    pl = [[0] * 8 for _ in range(8)]
    for y in range(8):
        for x in range(8):
            if 1 <= x <= 6 and y >= 1:
                pl[y][x] = 1 if (x + y) % 3 else 2
    t[1] = pl
    t[2] = [row[::-1] for row in pl]
    t[3] = [[3 if (x + y) % 2 == 0 else 1 for x in range(8)] for y in range(8)]
    return t


def chr_bytes(bg_tiles, sp_tiles):
    return b"".join(tile_bytes(t) for t in bg_tiles) + b"".join(tile_bytes(t) for t in sp_tiles)


# ---------------------------------------------------------------------------------------------------------------------
# levels

class Level:
    """A metatile grid (2x2 tiles, one palette each). mrows x mcols. Tiles T[row][col] and palettes P[mrow][mcol]."""

    def __init__(self, mrows, mcols):
        self.mr, self.mc = mrows, mcols
        self.T = [[T_BLANK] * (mcols * 2) for _ in range(mrows * 2)]
        self.P = [[0] * mcols for _ in range(mrows)]

    def put(self, mr, mc, tiles, pal):
        if not (0 <= mr < self.mr and 0 <= mc < self.mc):
            return
        self.T[mr * 2][mc * 2], self.T[mr * 2][mc * 2 + 1], self.T[mr * 2 + 1][mc * 2], self.T[mr * 2 + 1][mc * 2 + 1] = tiles
        self.P[mr][mc] = pal


def gen_runner(rng, mcols, top, bottom, hud_top_rows=2):
    """horizontal level: rows hud_top_rows.. bottom (inclusive) are the playfield; `top` row is the first non-uniform
    sky row (rows above stay empty sky)."""
    lv = Level(bottom + 1 + (1 if False else 0), mcols)
    ground = bottom - 2          # surface row
    surf = [ground] * mcols
    pit = [False] * mcols
    i = 6
    while i < mcols - 6:
        kind = rng.random()
        n = rng.randint(4, 10)
        if kind < 0.28:
            w = rng.randint(2, 3)
            for k in range(w):
                pit[i + k] = True
            i += w
        elif kind < 0.55:
            h = rng.randint(1, 2)
            w = rng.randint(2, 5)
            for k in range(w):
                if i + k < mcols - 4:
                    surf[i + k] = ground - h
            i += w
        i += n
    for c in range(mcols):
        if pit[c]:
            continue
        for r in range(surf[c], bottom + 1):
            top_row = r == surf[c]
            if top_row:
                tl = [T_GT + rng.randrange(8), T_GT + rng.randrange(8), T_GF + rng.randrange(8), T_GF + rng.randrange(8)]
            else:
                tl = [T_GF + rng.randrange(8) for _ in range(4)]
            lv.put(r, c, tl, 1)
    # platforms
    c = 8
    while c < mcols - 8:
        c += rng.randint(5, 13)
        w = rng.randint(2, 5)
        r = rng.choice((ground - 3, ground - 4))
        for k in range(w):
            if not (0 <= c + k < mcols):
                continue
            lv.put(r, c + k, [T_BR + rng.randrange(8) for _ in range(4)], 2)
    # walls
    c = 12
    while c < mcols - 10:
        c += rng.randint(9, 22)
        if pit[c % mcols] or c >= mcols - 6:
            continue
        h = rng.randint(2, 3)
        for k in range(h):
            r = surf[c] - 1 - k
            lv.put(r, c, [T_WALL + rng.randrange(8) for _ in range(4)], 2)
            if c + 1 < mcols and not pit[c + 1]:
                lv.put(r, c + 1, [T_WALL + rng.randrange(8) for _ in range(4)], 2)
    # clouds (rows top .. top+3, not the first two rows below the HUD)
    c = 3
    while c < mcols - 3:
        c += rng.randint(4, 11)
        r = rng.randint(top, top + 3)
        lv.put(r, c, [T_CLOUD, T_CLOUD + 1, T_CLOUD + 4, T_CLOUD + 5], 0)
        lv.put(r, c + 1, [T_CLOUD + 2, T_CLOUD + 3, T_CLOUD + 6, T_CLOUD + 7], 0)
    return lv


def gen_2d(rng, mrows, mcols):
    """vertical shaft world for the 4-way game: sky, a ground strip at the bottom, platforms everywhere, walls."""
    lv = Level(mrows, mcols)
    for r in range(mrows - 2, mrows):
        for c in range(mcols):
            tl = [T_GT + rng.randrange(8), T_GT + rng.randrange(8), T_GF + rng.randrange(8), T_GF + rng.randrange(8)] if r == mrows - 2 \
                else [T_GF + rng.randrange(8) for _ in range(4)]
            lv.put(r, c, tl, 1)
    for r in range(3, mrows - 3, 3):
        c = rng.randint(0, 4)
        while c < mcols - 2:
            w = rng.randint(2, 5)
            for k in range(w):
                lv.put(r, c + k, [T_BR + rng.randrange(8) for _ in range(4)], 2)
            c += w + rng.randint(2, 6)
    for k in range(mrows):                      # side walls
        for c in (0, mcols - 1):
            if k % 5 != 0:
                lv.put(k, c, [T_WALL + rng.randrange(8) for _ in range(4)], 2)
    for r in range(1, mrows - 4, 4):
        c = rng.randint(1, mcols - 3)
        lv.put(r, c, [T_CLOUD, T_CLOUD + 1, T_CLOUD + 4, T_CLOUD + 5], 0)
        lv.put(r, c + 1, [T_CLOUD + 2, T_CLOUD + 3, T_CLOUD + 6, T_CLOUD + 7], 0)
    return lv


# ---------------------------------------------------------------------------------------------------------------------
# PPU write blocks

class Blocks:
    """Appends data blocks at $8000 and remembers their addresses. A block: runs of [ctl hi lo data...] with ctl =
    (inc32 << 7) | count (1..127), then $FF, then a 2 byte link (address of the next block, run a frame later; 0 = none)."""

    def __init__(self, base=0x8000):
        self.base = base
        self.data = bytearray()

    def add(self, runs, link=0):
        """runs: list of (addr, bytes, inc32). Returns the address."""
        addr = self.base + len(self.data)
        for a, b, inc in runs:
            b = bytes(b)
            while b:
                chunk, b = b[:127], b[127:]
                self.data += bytes([(0x80 if inc else 0) | len(chunk), (a >> 8) & 0xFF, a & 0xFF]) + chunk
                a += len(chunk) * (32 if inc else 1)
        self.data += bytes([0xFF, link & 0xFF, link >> 8])
        return addr


def nt_addr(page, row, col):
    return 0x2000 + page * 0x400 + row * 32 + col


def attr_addr(page, g, ac):
    return 0x23C0 + page * 0x400 + g * 8 + ac


def attr_value(P, g, ac, hudpal=None):
    """attribute byte of page-local row group g, attribute column ac from the metatile palettes P[mrow][mcol]"""
    def pal(mr, mc):
        if hudpal is not None and hudpal(mr):
            return 3
        if 0 <= mr < len(P) and 0 <= mc < len(P[0]):
            return P[mr][mc]
        return 0
    v = 0
    for q, (dr, dc) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
        v |= pal(2 * g + dr, 2 * ac + dc) << (2 * q)
    return v


# ---------------------------------------------------------------------------------------------------------------------
# 6502 engine

ENGINE_HEAD = """
TX = $00
FX = $01
TY = $02
FY = $03
TYR = $04
TYP = $05
PAD = $06
CNT = $07
BLK = $08
QH = $0A
QT = $0B
SXV = $0C
SYV = $0D
NTV = $0E
FRM = $0F
TMP = $10
TSET = $13
SCENE = $14
PADP = $15
SRCL = $16
SRCH = $17
NMION = $18
"""

PALETTE_SRC = """
pal: .byte %s
"""


def fmt_bytes(b):
    return ",".join("$%02x" % x for x in b)


def pal_bytes():
    out = []
    for p in PAL_BG:
        out += p
    for p in PAL_SP:
        out += p
    return out


def engine_source(cfg, tables):
    """cfg keys: hud ('top'|'bottom'|None), mask, txmax, tymax, left, vert (C style lanes), mapper ('nrom'|'mmc1'|'mmc3'),
    chrram, swap_tx, nstatic, init (address), hud_irq_line, hud_banks."""
    A = []
    a = A.append
    a(ENGINE_HEAD)
    for k, v in tables.items():
        a("%s = $%04x" % (k, v))
    a("TXMAX = %d" % cfg["txmax"])
    a("TYMAX = %d" % cfg.get("tymax", 0))
    base2000 = 0x88
    maskv = 0x1C if cfg["mask"] else 0x1E
    a(".org $E000")
    # --- the block interpreter
    a("""
getb: ldy #0
  lda (BLK),y
  inc BLK
  bne gb1
  inc BLK+1
gb1: rts
runblk:
rb1: jsr getb
  cmp #$ff
  beq rbdone
  tax
  and #$7f
  sta CNT
  txa
  bmi rbi
  lda #$08
  bne rbs
rbi: lda #$0c
rbs: ora NMION
  sta $2000
  jsr getb
  sta $2006
  jsr getb
  sta $2006
rbd: jsr getb
  sta $2007
  dec CNT
  bne rbd
  jmp rb1
rbdone: jsr getb
  sta SRCL
  jsr getb
  sta SRCH
  rts
pushq: cpy #0
  beq pqx
  ldx QT
  sta $0400,x
  tya
  sta $0408,x
  inx
  txa
  and #7
  sta QT
pqx: rts
""")
    # --- reset
    a("""
reset: sei
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
""")
    if cfg["mapper"] == "mmc1":
        a("""
  lda #$80
  sta $8000
  lda #%d
  sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
""" % cfg["mmc1ctl"])
    if cfg["mapper"] == "mmc3":
        a("""
  lda #0
  sta $a000
  lda #$80
  sta $a001
  lda #6
  sta $8000
  lda #0
  sta $8001
  lda #7
  sta $8000
  lda #1
  sta $8001
""")
        for reg, val in zip(range(6), cfg["pf_banks"]):
            a("  lda #%d\n  sta $8000\n  lda #%d\n  sta $8001" % (reg, val))
    a("""
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
""")
    if cfg["chrram"]:
        a("""
  lda #0
  sta $2000
  sta $2006
  sta $2006
  lda #<CHRSET0
  sta BLK
  lda #>CHRSET0
  sta BLK+1
  ldx #32
  jsr chrup
""")
    # clear the nametables
    a("""
  lda #$20
  sta $2006
  lda #0
  sta $2006
  ldx #16
  tay
cn: sta $2007
  iny
  bne cn
  dex
  bne cn
  lda #<INITBLK
  sta BLK
  lda #>INITBLK
  sta BLK+1
  jsr runblk
  jsr initspr
  lda #$88
  sta NTV
  lda #$80
  sta NMION
""")
    if cfg["mapper"] == "mmc3":
        a("  cli")
    a("""
  lda #$88
  sta $2000
  lda #%d
  sta $2001
forever: jmp forever
""" % maskv)
    # chr upload: BLK = source, X = number of 256 byte pages
    a("""
chrup: ldy #0
cu1: lda (BLK),y
  sta $2007
  iny
  bne cu1
  inc BLK+1
  dex
  bne cu1
  rts
""")
    a("""
initspr:
  lda #%d
  sta $0200
  lda #0
  sta $0201
  lda #$20
  sta $0202
  lda #%d
  sta $0203
  lda #160
  sta $0204
  lda #1
  sta $0205
  lda #0
  sta $0206
  lda #124
  sta $0207
  lda #160
  sta $0208
  lda #2
  sta $0209
  lda #0
  sta $020a
  lda #132
  sta $020b
  lda #96
  sta $020c
  lda #3
  sta $020d
  lda #1
  sta $020e
  lda #0
  sta $020f
  rts
""" % (cfg.get("s0y", 0xF0), cfg.get("s0x", 64)))
    # --- NMI
    a("""
nmi: pha
  txa
  pha
  tya
  pha
  lda #0
  sta $2003
  lda #2
  sta $4014
  inc FRM
""")
    if cfg.get("scenes"):
        a("""
  lda PAD
  and #$10
  beq nsc
  lda PADP
  and #$10
  bne nsc
  jsr scenechg
nsc:
""")
    # queue pop
    a("""
  lda QH
  cmp QT
  beq nq
  tax
  lda $0400,x
  sta BLK
  lda $0408,x
  sta BLK+1
  inx
  txa
  and #7
  sta QH
  jsr runblk
  lda SRCL
  ora SRCH
  beq nq
  lda SRCL
  ldy SRCH
  jsr pushq
nq:
""")
    if cfg.get("swap_tx"):
        a("""
  lda TSET
  bne nsw
  lda TX
  cmp #%d
  bcc nsw
  jsr tileswap
nsw:
""" % cfg["swap_tx"])
    # the shadow sprites: an enemy that drifts through the left edge
    a("""
  lda FRM
  lsr a
  sta $0213
  lda #200
  sta $0210
  lda #3
  sta $0211
  lda #1
  sta $0212
""")
    hud = cfg["hud"]
    if hud == "top":
        a("""
  lda #$88
  sta $2000
  lda #0
  sta $2005
  lda SYV
  sta $2005
s0a: bit $2002
  bvs s0a
s0b: bit $2002
  bvc s0b
  lda NTV
  sta $2000
  lda SXV
  sta $2005
  lda SYV
  sta $2005
""")
    elif hud == "bottom":
        a("""
  lda NTV
  sta $2000
  lda SXV
  sta $2005
  lda SYV
  sta $2005
  lda #0
  sta $8000
  lda FRM
  and #16
  beq an0
  lda #%d
  jmp an1
an0: lda #%d
an1: sta $8001
  lda #1
  sta $8000
  lda #%d
  sta $8001
  lda #%d
  sta $c000
  sta $c001
  sta $e001
""" % (cfg["pf_alt"], cfg["pf_banks"][0], cfg["pf_banks"][1], cfg["irq_line"]))
    else:
        a("""
  lda NTV
  sta $2000
  lda SXV
  sta $2005
  lda SYV
  sta $2005
""")
    a("""
  lda #%d
  sta $2001
""" % maskv)
    # logic
    a("""
  lda PAD
  sta PADP
  lda #1
  sta $4016
  lda #0
  sta $4016
  ldx #8
rd: lda $4016
  lsr a
  rol PAD
  dex
  bne rd
""")
    if cfg.get("scroll", True):
        a("  jsr move")
    a("""
  lda TX
  sta $0300
  lda FX
  sta $0301
  lda TY
  sta $0302
  lda FY
  sta $0303
  lda FRM
  sta $0304
  lda TSET
  sta $0305
  lda SCENE
  sta $0306
  pla
  tay
  pla
  tax
  pla
  rti
""")
    if cfg["mapper"] == "mmc3":
        a("""
irq: pha
  txa
  pha
  sta $e000
  lda #0
  sta $8000
  lda #%d
  sta $8001
  lda #1
  sta $8000
  lda #%d
  sta $8001
  lda #$88
  sta $2000
  lda #0
  sta $2005
  sta $2005
  pla
  tax
  pla
  rti
""" % (cfg["hud_banks"][0], cfg["hud_banks"][1]))
    else:
        a("irq: rti")
    if cfg.get("scroll", True):
        # --- movement and scroll registers
        a("""
move:
  lda PAD
  and #1
  beq mr_no
  jsr stepr
  lda PAD
  and #$80
  beq mr_no
  jsr stepr
mr_no:
""")
        if cfg["left"]:
            a("""
  lda PAD
  and #2
  beq ml_no
  jsr stepl
  lda PAD
  and #$80
  beq ml_no
  jsr stepl
ml_no:
""")
        if cfg["vert"]:
            a("""
  lda PAD
  and #4
  beq md_no
  jsr stepd
  lda PAD
  and #$80
  beq md_no
  jsr stepd
md_no:
  lda PAD
  and #8
  beq mu_no
  jsr stepu
  lda PAD
  and #$80
  beq mu_no
  jsr stepu
mu_no:
""")
        a("""
  lda FX
  sta TMP
  lda TX
  asl a
  asl a
  asl a
  ora TMP
  sta SXV
  lda TX
  lsr a
  lsr a
  lsr a
  lsr a
  lsr a
  and #1
  ora #$88
  sta NTV
""")
        if cfg["vert"]:
            a("""
  lda TYR
  asl a
  asl a
  asl a
  ora FY
  sta SYV
  lda TYP
  beq nyp
  lda NTV
  ora #2
  sta NTV
nyp:
""")
        a("  rts")
        a("""
stepr: lda TX
  cmp #TXMAX
  bcs sr_x
""")
        if cfg["vert"]:
            a("  lda TY\n  bne sr_x")
        a("""
  inc FX
  lda FX
  cmp #8
  bcc sr_x
  lda #0
  sta FX
  inc TX
  ldx TX
  lda RTH,x
  tay
  lda RTL,x
  jsr pushq
sr_x: rts
""")
        if cfg["left"]:
            a("""
stepl: lda FX
  bne sl_dec
  lda TX
  beq sl_x
  dec TX
  lda #7
  sta FX
  ldx TX
  lda LTH,x
  tay
  lda LTL,x
  jsr pushq
  rts
sl_dec: dec FX
sl_x: rts
""")
        if cfg["vert"]:
            # lanes: vertical movement only at TX = 0 (set 0) or TX = TXMAX (set 1)
            a("""
vlane: lda TX
  beq vl0
  cmp #TXMAX
  bne vlno
  lda #1
  clc
  rts
vl0: lda #0
  clc
  rts
vlno: sec
  rts
stepd: lda FX
  bne sd_x
  lda TY
  cmp #TYMAX
  bcs sd_x
  jsr vlane
  bcs sd_x
  sta TMP
  inc FY
  lda FY
  cmp #8
  bcc sd_x
  lda #0
  sta FY
  inc TY
  inc TYR
  lda TYR
  cmp #30
  bcc sd_nw
  lda #0
  sta TYR
  lda TYP
  eor #1
  sta TYP
sd_nw: ldx TY
  lda TMP
  bne sd1
  lda D0H,x
  tay
  lda D0L,x
  jmp sd_p
sd1: lda D1H,x
  tay
  lda D1L,x
sd_p: jsr pushq
sd_x: rts
stepu: lda FX
  bne su_x
  jsr vlane
  bcs su_x
  sta TMP
  lda FY
  beq su_cr
  dec FY
  rts
su_cr: lda TY
  beq su_x
  dec TY
  lda #7
  sta FY
  dec TYR
  bpl su_nw
  lda #29
  sta TYR
  lda TYP
  eor #1
  sta TYP
su_nw: ldx TY
  lda TMP
  bne su1
  lda U0H,x
  tay
  lda U0L,x
  jmp su_p
su1: lda U1H,x
  tay
  lda U1L,x
su_p: jsr pushq
su_x: rts
""")
    if cfg.get("swap_tx"):
        a("""
tileswap:
  lda #0
  sta $2001
  sta $2000
  lda #1
  sta TSET
  lda #0
  sta $2006
  sta $2006
  lda #<CHRSET1
  sta BLK
  lda #>CHRSET1
  sta BLK+1
  ldx #16
  jsr chrup
  rts
""")
    if cfg.get("scenes"):
        a("""
scenechg:
  lda #0
  sta NMION
  sta $2000
  sta $2001
  lda SCENE
  eor #1
  sta SCENE
  lda #$20
  sta $2006
  lda #0
  sta $2006
  ldx #4
  lda #0
  tay
sc1: sta $2007
  iny
  bne sc1
  dex
  bne sc1
  lda SCENE
  beq sc_a
  lda #<SCENEB
  sta BLK
  lda #>SCENEB
  sta BLK+1
  jmp sc_g
sc_a: lda #<INITBLK
  sta BLK
  lda #>INITBLK
  sta BLK+1
sc_g: jsr runblk
  lda #$80
  sta NMION
  rts
""")
    a(PALETTE_SRC % fmt_bytes(pal_bytes()))
    a("""
.org $FFFA
.word nmi
.word reset
.word irq
""")
    return "\n".join(A)


def table_bytes(entries, n=256):
    lo = bytearray(n)
    hi = bytearray(n)
    for i, a in entries.items():
        lo[i] = a & 0xFF
        hi[i] = a >> 8
    return bytes(lo), bytes(hi)


def hud_runs(page_other=True):
    """status bar rows 0..3 in both pages: black background with glyph runs on page 0, divider on row 3"""
    runs = []
    for page in (0, 1):
        for row in range(3):
            runs.append((nt_addr(page, row, 0), bytes([T_HUDB] * 32), False))
        runs.append((nt_addr(page, 3, 0), bytes([T_HUDD] * 32), False))
    rng = random.Random(5)
    runs.append((nt_addr(0, 1, 4), bytes([T_GLYPH + rng.randrange(32) for _ in range(8)]), False))
    runs.append((nt_addr(0, 2, 18), bytes([T_GLYPH + rng.randrange(32) for _ in range(8)]), False))
    for page in (0, 1):
        for g in range(1):
            runs.append((attr_addr(page, 0, 0), bytes([0xFF] * 8), False))
    return runs


def finish_rom(name, outdir, cfg, blocks_bytes, tables, tabblob, chr_data, meta, truth, mapper, mirror, prg_kb=32,
               chrram=False):
    """assemble the engine, lay out PRG, write .nes/.meta/.truth"""
    src = engine_source(cfg, tables)
    off, code, syms = assemble(src)
    assert off == 0xE000, hex(off)
    prg = bytearray(0x8000)
    assert len(blocks_bytes) < 0x6000, "blocks too large: %d" % len(blocks_bytes)
    prg[0:len(blocks_bytes)] = blocks_bytes
    assert len(code) <= 0x2000
    prg[0x6000:0x6000 + len(code)] = code
    # tables at $F000.. (inside the code window above the code): placed by the caller at the addresses in `tables`
    for addr, blob in tabblob:
        assert addr >= 0xF000 and addr + len(blob) <= 0xFFFA, (hex(addr), len(blob))
        prg[addr - 0x8000:addr - 0x8000 + len(blob)] = blob
    rom = ines_file(bytes(prg), chr_data, mapper, mirror, prg_kb, chrram)
    open(os.path.join(outdir, name + ".nes"), "wb").write(rom)
    open(os.path.join(outdir, name + ".meta"), "w").write(meta)
    open(os.path.join(outdir, name + ".truth"), "wb").write(truth)


def ines_file(prg, chr_data, mapper, mirror, prg_kb, chrram):
    if prg_kb == 64:
        # MMC3: data banks first (0..1), fixed banks last: move the upper 16 KiB to the end
        full = bytearray(0x10000)
        full[0:0x4000] = prg[0:0x4000]
        full[0xC000:0x10000] = prg[0x4000:0x8000]
        prg = bytes(full)
    h = bytearray(16)
    h[0:4] = b"NES\x1a"
    h[4] = len(prg) // 16384
    h[5] = 0 if chrram else len(chr_data) // 8192
    h[6] = ((mapper & 15) << 4) | (mirror & 1)
    h[7] = mapper & 0xF0
    return bytes(h) + prg + (b"" if chrram else chr_data)


# ---------------------------------------------------------------------------------------------------------------------
# truth images

def render_world(lv, tiles):
    """palette index image of the whole level: list of rows of bytes (NES colour 0..63), width mc*16, height mr*16"""
    W, H = lv.mc * 16, lv.mr * 16
    img = [[0] * W for _ in range(H)]
    for tr in range(lv.mr * 2):
        for tc in range(lv.mc * 2):
            tile = tiles[lv.T[tr][tc]]
            pal = lv.P[tr // 2][tc // 2]
            for y in range(8):
                for x in range(8):
                    v = tile[y][x]
                    img[tr * 8 + y][tc * 8 + x] = PAL_BG[pal][v] if v else BD
    return img


def truth_blob(imgs, W, H, known_rows=None, hud=None):
    """.truth: u32 W, u32 H, u32 nimages, then per image W*H bytes: bit7 known, bits0..5 colour"""
    out = struct.pack("<III", W, H, len(imgs))
    for img in imgs:
        for y in range(H):
            row = bytearray(W)
            if y < len(img):
                for x in range(W):
                    v = img[y][x]
                    row[x] = (v | 0x80) if v >= 0 else 0
            out += bytes(row)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# builders

def meta_text(**kw):
    return "".join("%s=%s\n" % (k, v) for k, v in kw.items())


def col_runs(lv, c, r0, r1, nt_pages=2):
    """tiles of world column c, tile rows r0..r1, into the ring of a vertically mirrored (64 wide) layout"""
    page = (c // 32) % nt_pages
    return [(nt_addr(page, r0, c % 32), bytes(lv.T[r][c] for r in range(r0, r1 + 1)), True)]


def attr_col_runs(lv, ac, g0, g1, hudrows=None):
    page = (ac // 8) % 2
    return [(attr_addr(page, g, ac % 8), bytes([attr_value(lv.P, g, ac)]), False) for g in range(g0, g1 + 1)]


def build_runner(outdir, name, mapper, seed, lw, mrows, hud, mask, lead_r, lead_l, chrram=False, swap=False, irq=False):
    """A, D, E: horizontal scrollers with vertical mirroring."""
    rng = random.Random(seed)
    bottom = mrows - 1 if hud != "bottom" else 11
    lv = gen_runner(rng, lw, top=4 if hud != "bottom" else 2, bottom=bottom)
    rows_lo = 4 if hud == "top" else 0
    rows_hi = 29 if hud != "bottom" else 23
    g_lo = 1 if hud == "top" else 0
    g_hi = 7 if hud != "bottom" else 5
    blocks = Blocks()
    ncol = lw * 2

    def colblock(c, link_attr):
        runs = col_runs(lv, c, rows_lo, rows_hi)
        link = 0
        if link_attr:
            link = blocks.add(attr_col_runs(lv, c // 4, g_lo, g_hi), 0)
        return blocks.add(runs, link)

    # init block: HUD, columns [0, 32 + lead_r), attributes
    init = []
    if hud == "top":
        init += hud_runs()
    if hud == "bottom":
        for page in (0, 1):
            init.append((nt_addr(page, 24, 0), bytes([T_HUDD] * 32), False))
            for row in range(25, 30):
                init.append((nt_addr(page, row, 0), bytes([T_HUDB] * 32), False))
            init.append((attr_addr(page, 6, 0), bytes([0xFF] * 8), False))
            init.append((attr_addr(page, 7, 0), bytes([0xFF] * 8), False))
        rng2 = random.Random(5)
        init.append((nt_addr(0, 26, 4), bytes([T_GLYPH + rng2.randrange(32) for _ in range(8)]), False))
        init.append((nt_addr(0, 27, 18), bytes([T_GLYPH + rng2.randrange(32) for _ in range(8)]), False))
    for c in range(0, 32 + lead_r):
        init += col_runs(lv, c, rows_lo, rows_hi)
    for ac in range(0, (32 + lead_r) // 4 + 1):
        init += attr_col_runs(lv, ac, g_lo, g_hi)
    initblk = blocks.add(init, 0)
    txmax = ncol - 32
    R, L = {}, {}
    for tx in range(1, txmax + 1):
        c = tx + 32 + lead_r - 1
        if c < ncol:
            R[tx] = colblock(c, c % 4 == 0)
    for tx in range(0, txmax):
        c = tx - lead_l
        if c >= 0:
            L[tx] = colblock(c, c % 4 == 3)
    tables = {"INITBLK": initblk}
    tabblob = []
    base = 0xF000
    for nm, ent in (("RT", R), ("LT", L)):
        lo, hi = table_bytes(ent)
        tables[nm + "L"] = base
        tabblob.append((base, lo))
        tables[nm + "H"] = base + 256
        tabblob.append((base + 256, hi))
        base += 512
    cfg = dict(hud=hud, mask=mask, txmax=txmax, left=True, vert=False, mapper="nrom", chrram=chrram)
    if hud == "top":
        cfg.update(s0y=27, s0x=64)
    bg = make_bg_tiles(1)
    sp = make_sp_tiles()
    tiles_list = [bg]
    chr_data = chr_bytes(bg, sp)
    if chrram:
        bg1 = make_bg_tiles(2)
        set0 = chr_bytes(bg, sp)
        set1 = b"".join(tile_bytes(t) for t in bg1)
        a0 = 0x8000 + len(blocks.data)
        tables["CHRSET0"] = a0
        blocks.data += set0
        a1 = 0x8000 + len(blocks.data)
        tables["CHRSET1"] = a1
        blocks.data += set1
        tiles_list = [bg, bg1]
        cfg["swap_tx"] = swap if swap else 0
    if mapper == 4:
        mm = dict(mapper="mmc3", pf_banks=(0, 2, 16, 17, 18, 19), pf_alt=4, hud_banks=(8, 2), irq_line=irq)
        cfg.update(mapper="mmc3", pf_banks=(0, 2, 16, 17, 18, 19), pf_alt=4, hud_banks=(8, 2), irq_line=irq)
        # CHR: 128 banks of 1 KiB. PF set at banks 0..3, alt at 4,5 (tile 120 differs), HUD set at 8,9, sprites at 16..19
        rngj = random.Random(77)
        chrb = bytearray(128 * 1024)
        pf = [list(t) for t in bg]
        for k in range(64, 98):                                  # junk where the HUD tiles are in the other set
            pf[k] = [[rngj.randrange(4) for _ in range(8)] for _ in range(8)]
        pfb = b"".join(tile_bytes(t) for t in pf)
        chrb[0:4096] = pfb
        alt = bytearray(pfb[0:2048])
        alt[120 * 16:121 * 16] = bytes([0xAA] * 16)
        chrb[4096:4096 + 2048] = alt
        hudset = b"".join(tile_bytes(t) for t in bg[0:128])
        chrb[8 * 1024:8 * 1024 + 2048] = hudset
        spb = b"".join(tile_bytes(t) for t in sp)
        chrb[16 * 1024:16 * 1024 + 4096] = spb
        chr_data = bytes(chrb)
    imgs = [render_world(lv, t) for t in tiles_list]
    W, H = lw * 16, mrows * 16 if hud != "bottom" else 240
    if hud == "bottom":
        # playfield 192 lines + 48 HUD lines
        for img in imgs:
            for y in range(192, 240):
                img.append([0x10 if y < 200 else 0x0F] * W)
    for img in imgs:
        while len(img) < H:
            img.append([BD] * W)
    hudspec = ""
    if hud == "top":
        for img in imgs:
            for y in range(32):
                img[y] = [0x0F if y < 24 else 0x10] * W
        hudspec = "0:23:0F;24:31:10"
        hudlines = "0-28"
    elif hud == "bottom":
        hudspec = "192:199:10;200:239:0F"
        hudlines = "192-239"
    else:
        hudlines = ""
    meta = meta_text(name=name, ram_tx="0300", ram_fx="0301", ram_ty="0302", ram_fy="0303", ram_tset="0305", ram_scene="0306",
                     world_w=W, world_h=H, nimages=len(imgs), select="tset" if chrram else "none", hud_rows=hudlines,
                     hud_colours=hudspec, txmax=txmax, tymax=0, mode="scroll", mask=1 if mask else 0)
    truth = truth_blob(imgs, W, H)
    finish_rom(name, outdir, cfg, bytes(blocks.data), tables, tabblob, chr_data, meta, truth, mapper, 1,
               prg_kb=64 if mapper == 4 else 32, chrram=chrram)


def build_4way(outdir, name, seed):
    rng = random.Random(seed)
    mcols, mrows = 41, 45            # 82 x 90 tiles
    lv = gen_2d(rng, mrows, mcols)
    ncol, nrow = mcols * 2, mrows * 2
    # a 32 column ring cannot change an attribute byte for the entering columns without also changing the same bytes for the
    # columns still leaving on the other side: games with this layout keep palettes constant along each attribute row band
    for vp in range(mrows // 15):
        for g in range(8):
            rows = [vp * 15 + lr for lr in (2 * g, 2 * g + 1) if lr < 15]
            cnt = {}
            for mr in rows:
                for mc in range(mcols):
                    cnt[lv.P[mr][mc]] = cnt.get(lv.P[mr][mc], 0) + 1
            best = max(sorted(cnt), key=lambda k: cnt[k])
            for mr in rows:
                for mc in range(mcols):
                    lv.P[mr][mc] = best
    txmax, tymax = 48, 60
    blocks = Blocks()

    def ring_addr(r, c):
        rr = r % 60
        return nt_addr(0, rr, c % 32) if rr < 30 else 0x2800 + (rr - 30) * 32 + (c % 32)

    def page_of(r):
        return 0x2000 if (r % 60) < 30 else 0x2800

    def attr_a(r, ac):
        rr = r % 60
        return (0x23C0 if rr < 30 else 0x2BC0) + ((rr % 30) // 4) * 8 + (ac % 8)

    def attr_val(r, ac):
        vp, rp = r // 30, r % 30
        g = rp // 4
        v = 0
        for q, (dr, dc) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
            lr = 2 * g + dr
            mr = vp * 15 + lr
            # a column beyond the level (the 32 column ring's attribute slot of column 80 is shared with columns 48..51) takes the
            # palette of the last column: palettes are constant along a band, so the shared byte is right for both
            mc = min(2 * ac + dc, mcols - 1)
            p = lv.P[mr][mc] if lr < 15 and mr < mrows else 0
            v |= p << (2 * q)
        return v

    def col_window(c, r0, r1):
        """write column c (tile rows r0..r1) into its ring slot: runs split at the ring page boundary"""
        runs = []
        r = r0
        while r <= r1:
            rr = r % 60
            end = min(r1, r + ((30 - rr % 30) - 1))
            runs.append((ring_addr(r, c), bytes(lv.T[k][c] for k in range(r, end + 1)), True))
            r = end + 1
        return runs

    def row_runs(r, c0):
        """write row r for the 32 columns c0..c0+31 into their ring slots (split at the slot wrap)"""
        s0 = c0 % 32
        n1 = 32 - s0
        runs = [(ring_addr(r, c0), bytes(lv.T[r][c0:c0 + n1]), False)]
        if s0:
            runs.append((ring_addr(r, c0 + n1), bytes(lv.T[r][c0 + n1:c0 + 32]), False))
        return runs

    def row_attr(r, c0):
        # the attribute bytes of every attribute column the window touches
        out = []
        seen = set()
        for c in range(c0, c0 + 32):
            ac = c // 4
            if ac in seen:
                continue
            seen.add(ac)
            out.append((attr_a(r, ac), bytes([attr_val(r, ac)]), False))
        return out

    # init: rows 0..30, columns 0..31, attributes of page A and group 0 of page B
    init = []
    # slot 0 holds column 32 from the start (column 0 is under the left mask at tx = 0 and never shown): the sliver of
    # column 32 at the right edge while fx > 0 needs it
    for r in range(0, 31):
        init += row_runs(r, 1)
    for r in range(0, 31, 4):
        init += row_attr(r, 1)
    init += row_attr(30, 1)
    initblk = blocks.add(init, 0)
    R = {}
    for tx in range(1, txmax + 1):
        c = tx + 32
        link = 0
        if c % 4 == 0:
            ac = c // 4
            ar = [(attr_a(r, ac), bytes([attr_val(r, ac)]), False) for r in range(0, 31, 4)] + [(attr_a(30, ac), bytes([attr_val(30, ac)]), False)]
            link = blocks.add(ar, 0)
        R[tx] = blocks.add(col_window(c, 0, 30), link)
    D0, D1, U0, U1 = {}, {}, {}, {}
    for ty in range(1, tymax + 1):
        for tab, c0 in ((D0, 1), (D1, txmax + 1)):
            r = ty + 29
            link = blocks.add(row_attr(r, c0), 0) if (r % 30) % 4 == 0 else 0
            tab[ty] = blocks.add(row_runs(r, c0), link)
    for ty in range(0, tymax):
        for tab, c0 in ((U0, 1), (U1, txmax + 1)):
            r = ty
            link = blocks.add(row_attr(r, c0), 0) if (r % 30) % 4 == 3 else 0
            tab[ty] = blocks.add(row_runs(r, c0), link)
    tables = {"INITBLK": initblk}
    tabblob = []
    base = 0xF000
    for nm, ent in (("RT", R), ("D0", D0), ("D1", D1), ("U0", U0), ("U1", U1)):
        lo, hi = table_bytes(ent, 128)
        tables[nm + "L"] = base
        tabblob.append((base, lo))
        tables[nm + "H"] = base + 128
        tabblob.append((base + 128, hi))
        base += 256
    cfg = dict(hud=None, mask=True, txmax=txmax, tymax=tymax, left=False, vert=True, mapper="nrom", chrram=False)
    bg = make_bg_tiles(1)
    sp = make_sp_tiles()
    img = render_world(lv, bg)
    W, H = mcols * 16, mrows * 16
    meta = meta_text(name=name, ram_tx="0300", ram_fx="0301", ram_ty="0302", ram_fy="0303", ram_tset="0305", ram_scene="0306",
                     world_w=W, world_h=H, nimages=1, select="none", hud_rows="", hud_colours="", txmax=txmax, tymax=tymax,
                     mode="scroll", mask=1)
    finish_rom(name, outdir, cfg, bytes(blocks.data), tables, tabblob, chr_bytes(bg, sp), meta, truth_blob([img], W, H), 0, 0)


def build_single(outdir, name, seed):
    rng = random.Random(seed)

    def scene(k):
        lv = Level(15, 16)
        # ground
        for c in range(16):
            lv.put(13, c, [T_GT + rng.randrange(8), T_GT + rng.randrange(8), T_GF + rng.randrange(8), T_GF + rng.randrange(8)], 1)
            lv.put(14, c, [T_GF + rng.randrange(8) for _ in range(4)], 1)
        # platforms
        for r, c0, w in ((10, 2 + k, 4), (8, 8, 5), (6, 1 + 2 * k, 3), (11, 11 - k, 3)):
            for j in range(w):
                lv.put(r, c0 + j, [T_BR + rng.randrange(8) for _ in range(4)], 2)
        for r, c in ((2, 3 + 3 * k), (4, 10)):
            lv.put(r, c, [T_CLOUD, T_CLOUD + 1, T_CLOUD + 4, T_CLOUD + 5], 0)
            lv.put(r, c + 1, [T_CLOUD + 2, T_CLOUD + 3, T_CLOUD + 6, T_CLOUD + 7], 0)
        return lv

    scenes = [scene(0), scene(1)]
    blocks = Blocks()

    def full(lv):
        runs = []
        for r in range(30):
            runs.append((nt_addr(0, r, 0), bytes(lv.T[r]), False))
        for g in range(8):
            runs.append((attr_addr(0, g, 0), bytes(attr_value(lv.P, g, ac) for ac in range(8)), False))
        return runs

    initblk = blocks.add(full(scenes[0]), 0)
    sceneb = blocks.add(full(scenes[1]), 0)
    tables = {"INITBLK": initblk, "SCENEB": sceneb}
    cfg = dict(hud=None, mask=False, txmax=0, left=False, vert=False, mapper="mmc1", mmc1ctl=12, chrram=False, scroll=False,
               scenes=True)
    bg = make_bg_tiles(1)
    sp = make_sp_tiles()
    W = 256 + 192
    imgs = []
    for lv in scenes:
        img = render_world(lv, bg)
        out = []
        for y in range(240):
            row = [-1] * W
            for x in range(256):
                row[96 + x] = img[y][x]
            # a text row that is uniform backdrop all the way across: the margins continue it
            tr = y // 8
            if all(lv.T[tr][c] == T_BLANK for c in range(32)) and all(lv.T[(y // 16) * 2 + 1 - (tr % 2)][c] == T_BLANK for c in range(32)):
                for x in list(range(96)) + list(range(96 + 256, W)):
                    row[x] = BD
            out.append(row)
        imgs.append(out)
    meta = meta_text(name=name, ram_tx="0300", ram_fx="0301", ram_ty="0302", ram_fy="0303", ram_tset="0305", ram_scene="0306",
                     world_w=W, world_h=240, nimages=2, select="scene", hud_rows="", hud_colours="", txmax=0, tymax=0,
                     mode="static", origin_x=96, mask=0)
    finish_rom(name, outdir, cfg, bytes(blocks.data), tables, [], chr_bytes(bg, sp), meta, truth_blob(imgs, W, 240), 1, 0)


def write_all(out):
    os.makedirs(out, exist_ok=True)
    build_runner(out, "scroll_vert", 0, 11, 96, 15, "top", True, 2, 2)
    build_runner(out, "scroll_vert_nomask", 0, 12, 96, 15, "top", False, 2, 2)
    build_runner(out, "scroll_mmc3", 4, 13, 64, 15, "bottom", False, 1, 1, irq=190)
    build_runner(out, "scroll_chrram", 0, 14, 64, 15, None, False, 2, 2, chrram=True, swap=64)
    build_4way(out, "scroll_4way", 15)
    build_single(out, "single_screen", 16)


if __name__ == "__main__":
    write_all(sys.argv[1])
