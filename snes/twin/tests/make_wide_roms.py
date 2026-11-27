#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Synthetic SNES cartridges with a known full-level truth, for the wide view proofs (wide_truth.py checks the output).

  make_wide_roms.py OUTDIR      writes wide_scroll64.sfc, wide_scroll32.sfc (and the mode 7 / window ROMs, see below) plus
                                <name>.json (level geometry, palettes, tile graphics, camera table for the truth model)

scroll64 / scroll32: mode 1 horizontal scroller. BG1 = level (64 or 32 tile wide map, columns streamed by DMA in the vblank at
the seam), BG2 = sky (32 wide, scrolls at half speed), BG3 = HUD (fixed, priority), plus three sprites (x=-5, x=250, x=-40).
The camera is a table (one entry per frame), the ROM only looks it up.

alias_a / alias_b / alias_c / destr / destr_far (the learned map's safety proofs, see WIDE-INTEGRATION.md): the same 64 wide
scroller with several levels and a schedule (an event table, one word per frame): a level switch with forced blank (new level
data, tile graphics, camera restarts) or a change of the level data without blank (the ring is streamed again from the new
data). alias_a: level B shares the scroll coordinates with A but no tiles; alias_b: B = A with 15 % of the cells replaced;
alias_c: B has A's tilemap words but other graphics; destr: cells near the camera change at frame 300 (a block broken, a
switch: they are written into the ring at once); destr_far: cells far beyond the ring change at frame 1300 (RAM only, the
game streams them when the camera arrives: the case no picture based map can know, kept as a measured residual).
"""
import json, os, random, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm65816 import Asm, make_lorom

NFRAMES = 3200
LL = 6      # columns kept left of the screen
LR = 6      # and right


def rgb555(r, g, b): return (b << 10) | (g << 5) | r


def pack4(px):  # 8x8 values 0..15 -> 32 bytes
    out = bytearray(32)
    for y in range(8):
        for p in range(4):
            v = 0
            for x in range(8):
                v |= ((px[y * 8 + x] >> p) & 1) << (7 - x)
            out[(p >> 1) * 16 + y * 2 + (p & 1)] = v
    return bytes(out)


def pack2(px):
    out = bytearray(16)
    for y in range(8):
        for p in range(2):
            v = 0
            for x in range(8):
                v |= ((px[y * 8 + x] >> p) & 1) << (7 - x)
            out[y * 2 + p] = v
    return bytes(out)


# sched: (frame, level, blank, reset camera). blank: forced blank for the frame + the level's graphics are loaded
MULTI = {
    "alias_a": [(0, 0, 0, 0), (1000, 1, 1, 1), (2000, 0, 1, 1)],
    "alias_b": [(0, 0, 0, 0), (1000, 1, 1, 1), (2000, 0, 1, 1)],
    "alias_c": [(0, 0, 0, 0), (1000, 1, 1, 1), (2000, 0, 1, 1)],
    "destr": [(0, 0, 0, 0), (300, 1, 0, 0)],
    "destr_far": [(0, 0, 0, 0), (1300, 1, 0, 0)],
}
SPEED = 2


def camera_multi(sched, nframes):
    """camera table: forward at SPEED x the pattern, back from the end, again; restarts at a flagged level switch"""
    ev = {f: (lv, bl, rc) for (f, lv, bl, rc) in sched}
    pat = [0, 1, 2, 3, 1, 0, 2, 1, -1, 1, 2, 0, 3, 1, 1, 2]
    maxc = (255 - 32 - 6 - 1) * 8
    cam = []; c = 0; f0 = 0; dr = 1
    for f in range(nframes):
        if f in ev and ev[f][2]: c = 0; f0 = f; dr = 1
        d = pat[((f - f0) // 16) % len(pat)] * SPEED
        c += d * dr
        if c >= maxc: c = maxc; dr = -1
        if c <= 0: c = 0; dr = 1
        cam.append(c)
    return cam


def camera(kind, wide):
    """cam[f] in pixels. monotone with pauses and small back steps; 32-wide: multiples of 8."""
    cam = []
    c = 0
    rnd = random.Random(7)
    f = 0
    maxc = (255 - 32 - 6 - 1) * 8
    pat = [0, 1, 2, 3, 1, 0, 2, 1, -1, 1, 2, 0, 3, 1, 1, 2]
    while f < NFRAMES:
        d = pat[(f // 16) % len(pat)]
        c += d
        if kind == "s32":
            # multiples of 8: step 8 px every so often
            pass
        c = max(0, min(maxc, c))
        cam.append(c)
        f += 1
    if kind == "s32":
        cam = [(v // 8) * 8 for v in cam]
    return cam


def build(kind):
    rnd = random.Random(1234 if kind == "s64" else 4321 if kind == "s64e" else 99)
    win = kind.startswith("win")
    wk_ = kind[-1]
    multi = kind in MULTI
    N = 64 if kind in ("s64", "s64e", "win64_a", "win64_b") or multi else 32
    W = 32
    LL = 6 if N == 64 else 0
    LR = 6 if N == 64 else 0
    if kind == "s64e":          # worst case: columns are streamed exactly when they become visible (no lead at all beyond the
        LL, LR = 0, 1           # 33rd partial column), the rest of the ring holds stale columns of the previous lap

    # palettes (CGRAM 512 B): avoid exact magenta
    def col():
        while True:
            r, g, b = rnd.randrange(32), rnd.randrange(32), rnd.randrange(32)
            if not (r == 31 and g == 0 and b == 31): return (r, g, b)
    cg = [(0, 0, 0)] * 256
    cg[0] = (3, 3, 12)
    for i in range(1, 16): cg[32 + i] = col()
    for i in range(1, 16): cg[48 + i] = col()
    for i in range(1, 4): cg[28 + i] = col()
    cg[128 + 15] = (31, 0, 31)
    cgram = b"".join(struct.pack("<H", rgb555(*c)) for c in cg)
    # BG1 tiles: 256, tile 0 transparent, others 1..15
    t1 = [[0] * 64]
    for t in range(1, 256): t1.append([rnd.randrange(1, 16) for _ in range(64)])
    # BG2 tiles: 64 opaque
    t2 = [[rnd.randrange(1, 16) for _ in range(64)] for _ in range(64)]
    # BG3 2bpp tiles: 8
    t3 = [[0] * 64] + [[rnd.randrange(0, 4) for _ in range(64)] for _ in range(7)]
    # level: 256 columns x 32 rows of tile indices (0 = empty)
    level = []
    for c in range(256):
        col_ = []
        for r in range(32):
            if r >= 24 and r < 28: t = 1 + (c * 7 + r * 13 + (c >> 3)) % 255
            elif 6 <= r < 24 and ((c * 5 + r * 3 + (c >> 2)) % 5) in (0, 1): t = 1 + (c * 11 + r * 17 + (c >> 2)) % 255
            else: t = 0
            col_.append(t)
        level.append(col_)
    sky = [[(x + y * 5) % 64 for x in range(32)] for y in range(32)]
    hud = [[0] * 32 for _ in range(32)]
    for y in range(3):
        for x in range(32): hud[y][x] = 1 + (x + y * 3) % 7
    # levels: (level cells, tile graphics); level 0 is the one above
    levels = [(level, t1)]
    sched = MULTI.get(kind, [])
    part = None
    if multi:
        cam = camera_multi(sched, NFRAMES)
        r2 = random.Random(31337)
        def newtile(old): return 1 + r2.randrange(255)
        if kind == "alias_a":                  # no tile in common: other formula, other tiles
            lvB = [[(0 if (r < 6 or r >= 28 or ((c * 7 + r * 5 + (c >> 3)) % 5) not in (0, 1)) and not (24 <= r < 28) else 1 + (c * 13 + r * 29 + (c >> 1)) % 255) for r in range(32)] for c in range(256)]
            for c in range(256):               # make sure no cell equals A's (empty cells too: a ground row everywhere)
                for r in range(32):
                    if lvB[c][r] == level[c][r]: lvB[c][r] = 1 + (lvB[c][r] + 17) % 255
            levels.append((lvB, t1))
        elif kind == "alias_b":                # 85 % of the cells equal A's
            lvB = [list(col_) for col_ in level]
            cells = [(c, r) for c in range(256) for r in range(32)]
            r2.shuffle(cells)
            for (c, r) in cells[:len(cells) * 15 // 100]:
                lvB[c][r] = 1 + (lvB[c][r] + 1 + r2.randrange(254)) % 255
            levels.append((lvB, t1))
        elif kind == "alias_c":                # the same words, other graphics
            t1B = [[0] * 64] + [[1 + r2.randrange(15) for _ in range(64)] for _ in range(1, 256)]
            levels.append(([list(col_) for col_ in level], t1B))
        else:                                  # destr / destr_far: cells change at the event frame
            fe = sched[1][0]
            cc = cam[fe] >> 3
            c0, c1 = (cc + 6, cc + 22) if kind == "destr" else (cc + 100, cc + 150)
            lvB = [list(col_) for col_ in level]
            for c in range(c0, c1 + 1):
                for r in range(4, 28):
                    lvB[c][r] = 1 + (lvB[c][r] * 3 + 7 + r) % 255 if (r + c) % 3 else 0
            levels.append((lvB, t1))
            part = (c0, c1)
            print("  %s: cells change in columns %d..%d at frame %d (camera column %d)" % (kind, c0, c1, fe, cc))
    else:
        cam = camera("s32" if N == 32 else kind, N == 32)
    # ---- ROM data
    bank1 = bytearray(0x8000)
    lvbanks = []
    for k, (lv_, tt_) in enumerate(levels):
        bk = bank1 if k == 0 else bytearray(0x8000)
        for c in range(256):
            for r in range(32):
                v = lv_[c][r] | (2 << 10)
                bk[c * 64 + r * 2: c * 64 + r * 2 + 2] = struct.pack("<H", v)
        for t in range(256): bk[0x4000 + t * 32: 0x4000 + t * 32 + 32] = pack4(tt_[t])
        lvbanks.append(bk)
    evw = [0] * NFRAMES
    for (f, lv, bl, rc) in sched:
        if f: evw[f] = (lv + 1) | (0x80 if bl else 0)
    bank2 = bytearray(0x8000)
    o = {}
    def put(name, off, data):
        bank2[off:off + len(data)] = data; o[name] = off
    put("cg", 0x0000, cgram)
    put("t2", 0x0200, b"".join(pack4(t) for t in t2))
    put("m2", 0x0A00, b"".join(struct.pack("<H", sky[y][x] | (3 << 10)) for y in range(32) for x in range(32)))
    put("t3", 0x1200, b"".join(pack2(t) for t in t3))
    put("m3", 0x1280, b"".join(struct.pack("<H", (hud[y][x] | (7 << 10) | 0x2000) if hud[y][x] else 0) for y in range(32) for x in range(32)))
    put("tobj", 0x1A80, b"\xff" * 32)
    oam = bytearray(544)
    for i in range(128): oam[i * 4:i * 4 + 4] = bytes([0, 240, 0, 0x30])
    sprites = [(-5, 100), (250, 140), (-40, 60)]
    for i, (x, y) in enumerate(sprites):
        oam[i * 4:i * 4 + 4] = bytes([x & 255, y, 0, 0x30])
    hi = bytearray(32)
    for i, (x, y) in enumerate(sprites):
        if x < 0: hi[i >> 2] |= 1 << ((i & 3) * 2)
    oam[512:544] = hi
    put("oam", 0x1AA0, bytes(oam))
    put("cam", 0x2000, b"".join(struct.pack("<H", c) for c in cam))
    if multi: put("ev", 0x4000, b"".join(struct.pack("<H", v) for v in evw))
    a = Asm(0x8000)
    # ---- code
    R = a
    R.label("reset")
    R.sei(); R.clc(); R.xce(); R.rep(0x38)
    R.ldx("#", 0x1FFF); R.txs(); R.lda("#", 0); R.tcd()
    R.sep(0x20)
    R.lda("#", 0x8F); R.sta("abs", 0x2100)
    R.stz("abs", 0x4200)
    def w8(reg, v): R.lda("#", v); R.sta("abs", reg)
    w8(0x2101, 0x03); w8(0x2102, 0); w8(0x2103, 0)
    w8(0x2105, 0x09); w8(0x2106, 0)
    w8(0x2107, 0x01 if N == 64 else 0x00); w8(0x2108, 0x08); w8(0x2109, 0x0C)
    w8(0x210B, 0x42); w8(0x210C, 0x07)
    for reg in (0x210D, 0x210F, 0x2111): w8(reg, 0); w8(reg, 0)
    for reg in (0x210E, 0x2110, 0x2112): w8(reg, 0xFF); w8(reg, 0xFF)
    for reg in range(0x2123, 0x212C): w8(reg, 0)
    w8(0x212C, 0x01 if win else 0x17); w8(0x212D, 0); w8(0x212E, 0x01 if win else 0); w8(0x212F, 0)
    w8(0x2130, 0); w8(0x2131, 0); w8(0x2133, 0)
    if win:
        if wk_ == "a":
            w8(0x2126, 0); w8(0x2127, 127); w8(0x2128, 200); w8(0x2129, 255); w8(0x2123, 0x0A); w8(0x2125, 0x20); w8(0x2130, 0x20)
        else:
            w8(0x2126, 8); w8(0x2127, 247); w8(0x2123, 0x02)
        w8(0x212A, 0); w8(0x2131, 0x21)
        w8(0x2132, 0x20 | 12); w8(0x2132, 0x40 | 6); w8(0x2132, 0x80 | 3)
    def dma(mode, breg, srcaddr, bank, size):
        w8(0x4300, mode); w8(0x4301, breg)
        R.rep(0x20); R.lda("#", srcaddr); R.sta("abs", 0x4302); R.sep(0x20)
        w8(0x4304, bank)
        R.rep(0x20); R.lda("#", size); R.sta("abs", 0x4305); R.sep(0x20)
        w8(0x420B, 1)
    def vaddr(v):
        R.rep(0x20); R.lda("#", v); R.sta("abs", 0x2116); R.sep(0x20)
    w8(0x2121, 0); dma(0x00, 0x22, 0x8000 + o["cg"], 2, 512)
    w8(0x2115, 0x80)
    vaddr(0x2000); dma(0x01, 0x18, 0xC000, 1, 8192)
    vaddr(0x4000); dma(0x01, 0x18, 0x8000 + o["t2"], 2, 2048)
    vaddr(0x0800); dma(0x01, 0x18, 0x8000 + o["m2"], 2, 2048)
    vaddr(0x7000); dma(0x01, 0x18, 0x8000 + o["t3"], 2, 128)
    vaddr(0x0C00); dma(0x01, 0x18, 0x8000 + o["m3"], 2, 2048)
    vaddr(0x6000); dma(0x01, 0x18, 0x8000 + o["tobj"], 2, 32)
    w8(0x2102, 0); w8(0x2103, 0); dma(0x00, 0x04, 0x8000 + o["oam"], 2, 544)
    # tags = 0xFFFF
    R.rep(0x30)
    R.lda("#", 0xFFFF); R.ldx("#", 0)
    R.label("tg"); R.sta("absx", 0x0400); R.inx(); R.inx(); R.cpx("#", 128); R.bne("tg")
    R.lda("#", 0); R.sta("dp", 0x02)       # frame
    R.lda("#", 1); R.sta("dp", 0x0E)       # bank of the level data
    R.lda("#", 0); R.sta("dp", 0x16)       # level
    R.jsr("abs", "update")                       # frame 0 state into VRAM during forced blank
    R.sep(0x20)
    w8(0x4200, 0x81); w8(0x2100, 0x0F)
    R.rep(0x30)
    R.label("main"); R.wai(); R.bra("main")
    # ---- update(): cam lookup, history, stream, scroll   (called with A,X 16 bit)
    R.label("update")
    R.rep(0x30)
    R.lda("dp", 0x02); R.asla(); R.tax()
    R.lda("longx", 0x020000 + 0x8000 + o["cam"]); R.sta("dp", 0x00)
    if multi:
        R.lda("longx", 0x020000 + 0x8000 + o["ev"]); R.sta("dp", 0x10)
        R.beq("noev")
        R.and_("#", 0x7F); R.deca(); R.sta("dp", 0x16)
        R.asla(); R.inca(); R.sta("dp", 0x0E)
        if kind == "destr_far":                # RAM only: nothing is written into the ring now, later streams use the new data
            pass
        elif part:                             # the changed columns are written into the ring at once (a block broken, a switch)
            R.lda("#", 0xFFFF); R.ldx("#", (part[0] & 63) * 2)
            R.label("rt"); R.sta("absx", 0x0400); R.inx(); R.inx(); R.cpx("#", ((part[1] & 63) + 1) * 2); R.bne("rt")
        else:
            R.lda("#", 0xFFFF); R.ldx("#", 0)
            R.label("rt"); R.sta("absx", 0x0400); R.inx(); R.inx(); R.cpx("#", 128); R.bne("rt")
        R.lda("dp", 0x10); R.and_("#", 0x80); R.beq("noev")
        R.sep(0x20)
        R.lda("#", 0x8F); R.sta("abs", 0x2100)
        R.lda("#", 0x80); R.sta("abs", 0x2115)
        R.rep(0x20); R.lda("#", 0x2000); R.sta("abs", 0x2116); R.sep(0x20)
        R.lda("#", 0x01); R.sta("abs", 0x4300); R.lda("#", 0x18); R.sta("abs", 0x4301)
        R.rep(0x20); R.lda("#", 0xC000); R.sta("abs", 0x4302); R.sep(0x20)
        R.lda("dp", 0x0E); R.sta("abs", 0x4304)
        R.rep(0x20); R.lda("#", 0x2000); R.sta("abs", 0x4305); R.sep(0x20)
        R.lda("#", 1); R.sta("abs", 0x420B)
        R.rep(0x30)
        R.label("noev")
    R.lda("dp", 0x02); R.and_("#", 0x03FF); R.asla(); R.tax()
    R.lda("dp", 0x00); R.sta("absx", 0x1000)
    if multi:
        R.lda("dp", 0x16)
        for _ in range(12): R.asla()
        R.ora("dp", 0x00); R.sta("absx", 0x1000)
        R.lda("dp", 0x00)
    R.lsra(); R.lsra(); R.lsra(); R.sta("dp", 0x04)          # cam_col
    R.sec(); R.sbc("#", LL); R.bpl("c0ok"); R.lda("#", 0)
    R.label("c0ok"); R.sta("dp", 0x06)                      # c = max(0, cam_col - LL)
    R.lda("dp", 0x04); R.clc(); R.adc("#", W + LR - 1); R.sta("dp", 0x0C)   # end
    R.label("sl")
    R.lda("dp", 0x06); R.and_("#", N - 1); R.asla(); R.tax()
    R.lda("dp", 0x06); R.cmp("absx", 0x0400); R.beq("snx")
    R.sta("absx", 0x0400)
    R.jsr("abs", "stream")
    R.label("snx")
    R.inc("dp", 0x06); R.lda("dp", 0x06); R.cmp("dp", 0x0C); R.bcc("sl"); R.beq("sl")
    # scroll registers
    R.sep(0x20)
    R.lda("dp", 0x00); R.sta("abs", 0x210D); R.lda("dp", 0x01); R.and_("#", 3); R.sta("abs", 0x210D)
    R.rep(0x20); R.lda("dp", 0x00); R.lsra(); R.and_("#", 0x00FF); R.sep(0x20)
    R.sta("abs", 0x210F); R.stz("abs", 0x210F)
    R.rep(0x30)
    R.inc("dp", 0x02)
    R.rts()
    # ---- stream(): column dp[06] into the BG1 map ring (VRAM), DMA with VMAIN=$81
    R.label("stream")
    R.rep(0x30)
    R.lda("dp", 0x06); R.asla(); R.asla(); R.asla(); R.asla(); R.asla(); R.asla()   # *64
    R.clc(); R.adc("#", 0x8000); R.sta("abs", 0x4302)
    R.lda("dp", 0x06); R.and_("#", 0x1F); R.sta("dp", 0x0A)
    if N == 64:
        R.lda("dp", 0x06); R.and_("#", 0x20); R.asla(); R.asla(); R.asla(); R.asla(); R.asla()
        R.ora("dp", 0x0A)
    else:
        R.lda("dp", 0x0A)
    R.sta("abs", 0x2116)
    R.lda("#", 64); R.sta("abs", 0x4305)
    R.sep(0x20)
    R.lda("#", 0x81); R.sta("abs", 0x2115)
    R.lda("#", 0x01); R.sta("abs", 0x4300)
    R.lda("#", 0x18); R.sta("abs", 0x4301)
    R.lda("dp", 0x0E); R.sta("abs", 0x4304)
    R.lda("#", 1); R.sta("abs", 0x420B)
    R.rep(0x30)
    R.rts()
    # ---- NMI
    R.label("nmi")
    R.rep(0x30)
    R.sep(0x20); R.lda("abs", 0x4210)
    if multi: R.lda("#", 0x0F); R.sta("abs", 0x2100)
    R.rep(0x30)
    R.jsr("abs", "update")
    R.rti()
    code = R.link()
    banks = {0: code, 1: bytes(bank1), 2: bytes(bank2)}
    for k in range(1, len(levels)): banks[2 * k + 1] = bytes(lvbanks[k])
    rom = make_lorom(banks, "WIDE SYNTH " + kind.upper(), R.labels["nmi"], R.labels["reset"])
    meta = dict(kind=kind, win=kind if win else "", N=N, W=W, LL=LL, LR=LR, level=level, t1=t1, t2=t2, t3=t3, sky=sky, hud=hud,
                cg=[list(c) for c in cg], cam=cam, sprites=sprites)
    if multi: meta["levels"] = [dict(level=lv_, t1=tt_) for (lv_, tt_) in levels]; meta["sched"] = sched
    return rom, meta


M7 = dict(H=96, V=200, CX=128, CY=128)


def m7_matrix(y):
    """per line (A, B, C, D) as signed 16 bit (8.8): a perspective floor (zoom changes with the line), with a slight shear"""
    s = int(round(256 * (0.5 + 6.0 * (1 - y / 224.0))))
    sh = int(round(24 * (y - 112) / 112.0))
    return (s, sh, -sh, s)


def build_m7(kind="m7"):
    rnd = random.Random(555)
    cg = [(0, 0, 0)] * 256
    for i in range(1, 256):
        while True:
            c = (rnd.randrange(32), rnd.randrange(32), rnd.randrange(32))
            if c != (31, 0, 31): break
        cg[i] = c
    cgram = b"".join(struct.pack("<H", rgb555(*c)) for c in cg)
    tmap = [[(x * 3 + y * 5 + (x >> 2) * 7) % 256 for x in range(128)] for y in range(128)]
    chars = [[rnd.randrange(1, 256) for _ in range(64)] for _ in range(256)]
    bank1 = bytearray(0x8000)
    for y in range(128):
        for x in range(128): bank1[y * 128 + x] = tmap[y][x]
    for t in range(256):
        for p in range(64): bank1[0x4000 + t * 64 + p] = chars[t][p]
    mats = [m7_matrix(y) for y in range(224)]
    t1 = bytearray(); t2 = bytearray()
    for (a, b, c, d) in mats:
        t1 += bytes([1]) + struct.pack("<hh", a, b)
        t2 += bytes([1]) + struct.pack("<hh", c, d)
    t1 += b"\0"; t2 += b"\0"
    bank2 = bytearray(0x8000)
    bank2[0:512] = cgram
    bank2[0x200:0x200 + len(t1)] = t1
    bank2[0x800:0x800 + len(t2)] = t2
    R = Asm(0x8000)
    R.label("reset")
    R.sei(); R.clc(); R.xce(); R.rep(0x38)
    R.ldx("#", 0x1FFF); R.txs(); R.lda("#", 0); R.tcd()
    R.sep(0x20)
    def w8(reg, v): R.lda("#", v); R.sta("abs", reg)
    def w16(reg, v): w8(reg, v & 255); w8(reg, (v >> 8) & 255)
    w8(0x2100, 0x8F); w8(0x4200, 0); w8(0x420C, 0)
    w8(0x2105, 0x07); w8(0x211A, 0x00)
    for reg in range(0x2123, 0x212C): w8(reg, 0)
    w8(0x212C, 0x01); w8(0x212D, 0); w8(0x212E, 0); w8(0x212F, 0)
    w8(0x2130, 0); w8(0x2131, 0); w8(0x2133, 0)
    w16(0x210D, M7["H"]); w16(0x210E, M7["V"]); w16(0x211F, M7["CX"]); w16(0x2120, M7["CY"])
    w16(0x211B, 0x100); w16(0x211C, 0); w16(0x211D, 0); w16(0x211E, 0x100)
    def dma(mode, breg, srcaddr, bank, size):
        w8(0x4300, mode); w8(0x4301, breg)
        R.rep(0x20); R.lda("#", srcaddr); R.sta("abs", 0x4302); R.sep(0x20)
        w8(0x4304, bank)
        R.rep(0x20); R.lda("#", size); R.sta("abs", 0x4305); R.sep(0x20)
        w8(0x420B, 1)
    w8(0x2121, 0); dma(0x00, 0x22, 0x8000, 2, 512)
    w8(0x2115, 0x00)
    R.rep(0x20); R.lda("#", 0); R.sta("abs", 0x2116); R.sep(0x20)
    dma(0x00, 0x18, 0x8000, 1, 16384)          # tile map bytes to the low byte
    w8(0x2115, 0x80)
    R.rep(0x20); R.lda("#", 0); R.sta("abs", 0x2116); R.sep(0x20)
    dma(0x00, 0x19, 0xC000, 1, 16384)          # characters to the high byte
    # HDMA: ch1 -> $211B,$211C (two writes each), ch2 -> $211D,$211E
    w8(0x4310, 0x03); w8(0x4311, 0x1B); w8(0x4314, 2)
    R.rep(0x20); R.lda("#", 0x8200); R.sta("abs", 0x4312); R.sep(0x20)
    w8(0x4320, 0x03); w8(0x4321, 0x1D); w8(0x4324, 2)
    R.rep(0x20); R.lda("#", 0x8800); R.sta("abs", 0x4322); R.sep(0x20)
    w8(0x420C, 0x06)
    w8(0x4200, 0x81); w8(0x2100, 0x0F)
    R.label("main"); R.wai(); R.bra("main")
    R.label("nmi")
    R.sep(0x20); R.lda("abs", 0x4210); R.rti()
    code = R.link()
    rom = make_lorom({0: code, 1: bytes(bank1), 2: bytes(bank2)}, "WIDE SYNTH M7", R.labels["nmi"], R.labels["reset"])
    return rom, dict(kind="m7", m7=M7, cg=[list(c) for c in cg], tmap=tmap, chars=chars, mats=mats)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for kind, name in (("s64", "wide_scroll64"), ("s64e", "wide_scroll64e"), ("s32", "wide_scroll32"), ("win_a", "wide_win_a"), ("win_b", "wide_win_b"), ("win64_a", "wide_win64_a"), ("win64_b", "wide_win64_b"),
                       ("alias_a", "wide_alias_a"), ("alias_b", "wide_alias_b"), ("alias_c", "wide_alias_c"), ("destr", "wide_destr"), ("destr_far", "wide_destr_far")):
        rom, meta = build(kind)
        open(os.path.join(out, name + ".sfc"), "wb").write(rom)
        json.dump(meta, open(os.path.join(out, name + ".json"), "w"))
        print("wrote", name, len(rom))
    rom, meta = build_m7()
    open(os.path.join(out, "wide_mode7.sfc"), "wb").write(rom)
    json.dump(meta, open(os.path.join(out, "wide_mode7.json"), "w"))
    print("wrote wide_mode7", len(rom))


if __name__ == "__main__":
    main()
