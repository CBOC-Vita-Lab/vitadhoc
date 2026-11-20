#!/usr/bin/env python3
# A Mega Drive test cartridge whose picture is known exactly, and the truth check of the wide view on it.
#   md_scroll_gen.py make OUT.md
#   md_scroll_gen.py check MD_TWIN ROM OUTDIR [--frames N] [extra md_twin args]
# The cartridge: H40, plane A (64 x 32 cells of known tiles, opaque) scrolls sideways one pixel a frame, 1024 frames one way and 1024 back,
# plane B at half that speed; per 2-cell vertical scroll is on:
# the five columns at each edge stand still and the ten in the middle bob up and down; every 512 frames the second column from the left
# bobs too for 512 frames (then the left edge's columns differ: no strip may be drawn there). The window plane takes the top 16 lines:
# empty at both ends, a bar of tiles in its middle; plane B (other tiles, second palette) shows through it.
# The check: every margin pixel the wide view shows at full weight as real picture must be the pixel of plane A's world (below the window)
# or of plane B's (beside the window lines). Prints MDTRUTH ... and exits 1 when a pixel is wrong or nothing was checked.
# Written from scratch (68000 code assembled by hand below); no code from any game or tool.
import sys, os, struct, random, subprocess, glob

def a_cell(col, row): return (1 + ((col * 7 + row * 13 + (col >> 2)) % 63)) | (0x0800 if (col >> 3) & 1 else 0)
def b_cell(col, row): return (1 + ((col * 11 + row * 5) % 63)) | 0x2000
def tiles():
    r = random.Random(4321)
    t = bytearray(64 * 32)
    for i in range(32, 64 * 32):                      # tile 0 stays empty; no pixel of the others is colour 0
        t[i] = (r.randrange(1, 16) << 4) | r.randrange(1, 16)
    return bytes(t)
def cram():
    c = []
    for i in range(32):
        r_, g_, b_ = (i * 3 + 1) % 8, (i * 5 + 2) % 8, (i * 7 + 3) % 8
        if i >= 16: r_, g_, b_ = (r_ + 3) % 8, (g_ + 5) % 8, (b_ + 1) % 8
        c.append((b_ << 9) | (g_ << 5) | (r_ << 1))
    c[0] = 0
    return c

def build():
    W = lambda *ws: b''.join(struct.pack('>H', w & 0xFFFF) for w in ws)
    L = lambda v: struct.pack('>I', v)
    regs = [0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x04, 0x81, 0x3F, 0x00, 0x02, 0x01, 0x00, 0x02]
    code = bytearray()
    code += W(0x46FC, 0x2700)
    code += W(0x41F9) + L(0xC00000) + W(0x43F9) + L(0xC00004)
    code += W(0x45F9) + L(0)                       # lea data,a2 (patched)
    lea_pos = len(code) - 4
    code += W(0x7000 | (len(regs) - 1), 0x329A, 0x51C8, 0xFFFC)
    b1 = len(code)
    code += W(0x301A, 0x6700)                      # move.w (a2)+,d0 ; beq.w main (patched)
    beq_pos = len(code); code += W(0)
    code += W(0x229A, 0x5340, 0x309A, 0x51C8, 0xFFFC)
    code += W(0x6000, (b1 - (len(code) + 2)) & 0xFFFF)
    main0 = len(code)
    code[beq_pos:beq_pos + 2] = W(main0 - beq_pos)
    code += W(0x7E00, 0x7C00)                      # moveq #0,d7 (frame) ; moveq #0,d6 (camera)
    main = len(code)
    code += W(0x3011, 0x0240, 0x0008, 0x66F8)      # w1: move.w (a1),d0 ; andi.w #8,d0 ; bne.s w1
    code += W(0x3011, 0x0240, 0x0008, 0x67F8)      # w2: ... beq.s w2
    code += W(0x5247)                              # addq.w #1,d7
    code += W(0x3007, 0x0240, 0x0400, 0x6704, 0x5346, 0x6002, 0x5246)   # frame & 0x400 ? camera-- : camera++
    code += W(0x22BC) + L(0x7C000003)              # hscroll table
    code += W(0x3006, 0x4440, 0x3080)              # plane A: -camera
    code += W(0x3006, 0xE240, 0x4440, 0x3080)      # plane B: -(camera >> 1)
    code += W(0x22BC) + L(0x40000010)              # vsram
    code += W(0x3007, 0xE248, 0x0240, 0x003F)      # d0 = (frame >> 1) & 63
    code += W(0x3407, 0x0242, 0x0200, 0x6702, 0x3400)   # d2 = (frame & 0x200) ? d0 : 0
    code += W(0x30BC, 0x0000, 0x30BC, 0x0000)      # column 0
    code += W(0x3082, 0x30BC, 0x0000)              # column 1
    code += W(0x7202, 0x30BC, 0x0000, 0x30BC, 0x0000, 0x51C9, 0xFFF6)      # columns 2..4
    code += W(0x7209, 0x3080, 0x30BC, 0x0000, 0x51C9, 0xFFF8)              # columns 5..14
    code += W(0x7204, 0x30BC, 0x0000, 0x30BC, 0x0000, 0x51C9, 0xFFF6)      # columns 15..19
    code += W(0x6000, (main - (len(code) + 2)) & 0xFFFF)
    data_at = 0x200 + len(code)
    code[lea_pos:lea_pos + 4] = L(data_at)
    data = b''.join(W(0x8000 | (i << 8) | v) for i, v in enumerate(regs))
    def block(cmd, words): return W(len(words)) + L(cmd) + W(*words)
    data += block(0xC0000000, cram())
    T = tiles(); data += block(0x40000000, list(struct.unpack('>%dH' % (len(T) // 2), T)))
    data += block(0x40000003, [a_cell(c, r) for r in range(32) for c in range(64)])
    data += block(0x60000003, [b_cell(c, r) for r in range(32) for c in range(64)])
    data += block(0x70000003, [(5 if 10 <= c < 30 else 0) for r in range(2) for c in range(64)])
    data += W(0)
    rom = bytearray(0x200)
    rom[0:8] = L(0x00FFFE00) + L(0x200)
    hdr = b'SEGA MEGA DRIVE ' + b'(C)TEST 2026.NOV' + b'WIDE VIEW TRUTH TEST'.ljust(48) + b'WIDE VIEW TRUTH TEST'.ljust(48) + b'GM 00000000-00' + b'\0\0' + \
          b'J'.ljust(16) + L(0) + L(0xFFFF) + L(0xFF0000) + L(0xFFFFFF) + b' ' * 12 + b' ' * 12 + b' ' * 40 + b'JUE'.ljust(16)
    rom[0x100:0x100 + len(hdr)] = hdr
    rom += code + data
    rom += bytes(0x10000 - len(rom))
    return bytes(rom)

def cam_at(n):
    cam = 0
    for d7 in range(1, n + 1): cam += -1 if d7 & 0x400 else 1
    return cam

def planes():
    T = tiles()
    def plane(cell):
        P = [[0] * 512 for _ in range(256)]
        for row in range(32):
            for col in range(64):
                w = cell(col, row); t = w & 0x7FF; pal = (w >> 13) & 3
                for fy in range(8):
                    yy = row * 8 + fy; sy = 7 - fy if w & 0x1000 else fy
                    b = T[t * 32 + sy * 4: t * 32 + sy * 4 + 4]
                    for x in range(8):
                        sx = 7 - x if w & 0x0800 else x
                        ci = (b[sx >> 1] >> (0 if sx & 1 else 4)) & 15
                        P[yy][col * 8 + x] = (pal << 4) | ci
        return P
    return plane(a_cell), plane(b_cell)

def load(fn):
    d = open(fn, 'rb').read()
    ow, oh, M, f = struct.unpack_from('4i', d, 0); o = 16
    real = [struct.unpack_from('2i', d, o + 8 * y) for y in range(oh)]; o += 8 * oh
    pix = struct.unpack_from('%dH' % (ow * oh), d, o); o += 2 * ow * oh
    return ow, oh, M, f, real, pix, d[o:o + ow * oh]

def check(argv):
    twin, rom, out = argv[:3]; rest = argv[3:]
    frames, extra = 4300, []
    i = 0
    while i < len(rest):
        if rest[i] == '--frames': frames = int(rest[i + 1]); i += 2
        else: extra.append(rest[i]); i += 1
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(out, 'mt_*.wd')): os.remove(f)
    o = subprocess.run([twin, rom, '--frames', str(frames), '--pad', 'none', '--wide', '51', '--wide-dump', os.path.join(out, 'mt') + ':60:37'] + extra, stdout=subprocess.PIPE, universal_newlines=True).stdout
    wl = [l for l in o.splitlines() if l.startswith('WIDE ')]
    A, B = planes()
    n_fr = checked = wrong = unloc = left_real_bad = 0
    side_px = [0, 0]; win_px = 0
    for fn in sorted(glob.glob(os.path.join(out, 'mt_*.wd'))):
        ow, oh, M, f, real, pix, prov = load(fn)
        found = None
        for n in range(max(0, f - 12), f + 3):
            mp = {}; ok = True
            cam = cam_at(n)
            for y in range(16, oh, 9):
                row = A[y]; base = y * ow + M
                for x in list(range(0, 16)) + list(range(272, 320)):
                    idx = row[(cam + x) & 511]; p = pix[base + x]
                    if mp.setdefault(idx, p) != p: ok = False; break
                if not ok: break
            if ok:
                for y in range(16, oh):
                    row = A[y]; base = y * ow + M
                    for x in list(range(0, 16)) + list(range(272, 320)):
                        idx = row[(cam + x) & 511]; p = pix[base + x]
                        if mp.setdefault(idx, p) != p: ok = False; break
                    if not ok: break
            if ok: found = (n, cam, mp); break
        if not found: unloc += 1; continue
        n, cam, mp = found
        n_fr += 1
        # plane B's colours: from the window lines of the centre, the columns the window leaves empty
        for y in range(0, 16):
            for x in list(range(0, 80)) + list(range(240, 320)):
                mp.setdefault(B[y][((cam >> 1) + x) & 511], pix[y * ow + M + x])
        split = (n & 0x200) != 0 and ((n >> 1) & 63) != 0     # the second column bobs: the left edge's columns differ
        for y in range(oh):
            base = y * ow
            if split and y >= 16 and real[y][0] > 0: left_real_bad += 1
            for x in list(range(0, M)) + list(range(M + 320, ow)):
                if (prov[base + x] & 15) != 4: continue
                if y >= 16: idx = A[y][(cam + x - M) & 511]
                else: idx = B[y][((cam >> 1) + x - M) & 511]; win_px += 1
                checked += 1; side_px[0 if x < M else 1] += 1
                if mp.get(idx) != pix[base + x]: wrong += 1
    print('MDTRUTH %s frames=%d checked=%d (left %d, right %d, beside the window lines %d) wrong=%d strips_beside_split_columns=%d unlocated=%d' %
          (os.path.basename(rom), n_fr, checked, side_px[0], side_px[1], win_px, wrong, left_real_bad, unloc))
    for l in wl: print('  ' + l[l.find('real='):])
    return 1 if (wrong or left_real_bad or not checked) else 0

if __name__ == '__main__':
    if sys.argv[1] == 'make': open(sys.argv[2], 'wb').write(build())
    else: sys.exit(check(sys.argv[2:]))
