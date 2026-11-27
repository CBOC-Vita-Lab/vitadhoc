#!/usr/bin/env python3
# Makes one small test cartridge per console of the app (Game Boy, NES, WonderSwan) that answers the A button ONE FRAME LATER, the way most
# games do: in vertical blank it puts the sprite position its LAST blank's pad read produced on screen, then reads the pad again (the game
# logic of a real game would run in between). One sprite jumps from x = 20 to x = 100..200 while A is held. ahead_latency.sh counts the
# refreshes between the press and the first changed picture: with "Quick response" (run-ahead by one frame) one fewer than without.
#   ahead_latency_gen.py OUTDIR      writes OUTDIR/latency.gb, latency.nes and, when nasm is installed, latency.ws
#   (the Game Boy one needs RGBDS: rgbasm, rgblink, rgbfix on PATH or in /opt/rgbds/bin)
# Written from scratch (no code from any game or test suite); the 6502 part has its own few-instruction assembler below.
# Copyright CBOC-Vita. MIT licence for this script.
import os, shutil, subprocess, sys, tempfile

GB_ASM = r'''
SECTION "entry", ROM0[$100]
	nop
	jp start
	ds $150 - $104
SECTION "main", ROM0[$150]
start:
	di
	ld sp, $FFFE
.vb0:	ldh a, [$44]
	cp 144
	jr c, .vb0
	xor a
	ldh [$40], a              ; LCD off: VRAM and OAM are free
	ld hl, $8000              ; tile 0 empty, tile 1 solid
	ld b, 16
	xor a
.t0:	ld [hl+], a
	dec b
	jr nz, .t0
	ld b, 16
	ld a, $FF
.t1:	ld [hl+], a
	dec b
	jr nz, .t1
	ld hl, $FE00              ; OAM cleared, then sprite 0 at x = 20
	ld b, 160
	xor a
.o0:	ld [hl+], a
	dec b
	jr nz, .o0
	ld a, 88
	ld [$FE00], a
	ld a, 28
	ld [$FE01], a
	ld a, 1
	ld [$FE02], a
	ld a, $E4
	ldh [$47], a
	ldh [$48], a
	ld a, 28
	ld [$C000], a
	ld a, $93                 ; LCD on, background on, sprites on, tiles at $8000
	ldh [$40], a
main:
.w1:	ldh a, [$44]
	cp 144
	jr nz, .w1                ; vertical blank starts
	ld a, [$C000]             ; the x the PREVIOUS blank's read produced goes into the sprite
	ld [$FE01], a
	ld a, $10                 ; the button keys
	ldh [$00], a
	ldh a, [$00]
	ldh a, [$00]
	ldh a, [$00]
	and 1                     ; A is bit 0, low when held
	ld a, 28
	jr nz, .st
	ld a, 108
.st:	ld [$C000], a             ; the answer waits in RAM for the next blank
.w2:	ldh a, [$44]
	cp 144
	jr z, .w2
	jr main
'''

class Asm6502:
    def __init__(s): s.b = bytearray(); s.lab = {}; s.fix = []
    def L(s, n): s.lab[n] = len(s.b)
    def e(s, *v): s.b += bytes(v)
    def abs_(s, op, t):
        s.b.append(op)
        if isinstance(t, str): s.fix.append((len(s.b), t, 'a')); s.b += b'\0\0'
        else: s.b += bytes([t & 255, t >> 8])
    def rel(s, op, t): s.b.append(op); s.fix.append((len(s.b), t, 'r')); s.b.append(0)
    def done(s, org):
        for at, n, k in s.fix:
            a = org + s.lab[n]
            if k == 'a': s.b[at] = a & 255; s.b[at + 1] = a >> 8
            else:
                d = s.lab[n] - (at + 1)
                assert -128 <= d <= 127; s.b[at] = d & 255
        return bytes(s.b)

def nes():
    A = Asm6502(); e = A.e
    A.L('reset'); e(0x78, 0xD8)                                   # sei; cld
    e(0xA2, 0x40); A.abs_(0x8E, 0x4017); e(0xA2, 0xFF, 0x9A, 0xE8) # ldx #$40; stx $4017; ldx #$FF; txs; inx
    A.abs_(0x8E, 0x2000); A.abs_(0x8E, 0x2001); A.abs_(0x8E, 0x4010)
    for n in ('w1', 'w2'):
        A.L(n); A.abs_(0x2C, 0x2002); A.rel(0x10, n)               # bit $2002; bpl: two vertical blanks
    e(0xA9, 0x3F); A.abs_(0x8D, 0x2006); e(0xA9, 0x00); A.abs_(0x8D, 0x2006); e(0xA9, 0x0F); A.abs_(0x8D, 0x2007)
    e(0xA9, 0x3F); A.abs_(0x8D, 0x2006); e(0xA9, 0x11); A.abs_(0x8D, 0x2006); e(0xA9, 0x30); A.abs_(0x8D, 0x2007)   # sprite colour 1 white
    e(0xA2, 0x00, 0xA9, 0xFF)                                      # all 64 sprites below the picture
    A.L('o'); A.abs_(0x9D, 0x0200); e(0xE8, 0xE8, 0xE8, 0xE8); A.rel(0xD0, 'o')
    e(0xA9, 100); A.abs_(0x8D, 0x0200); e(0xA9, 1); A.abs_(0x8D, 0x0201); e(0xA9, 0); A.abs_(0x8D, 0x0202)
    e(0xA9, 20); A.abs_(0x8D, 0x0203); e(0x85, 0x00)               # sprite 0 at x = 20; the zero page byte $00 holds the next x
    e(0xA9, 0x80); A.abs_(0x8D, 0x2000); e(0xA9, 0x1E); A.abs_(0x8D, 0x2001)
    A.L('spin'); A.abs_(0x4C, 'spin')
    A.L('nmi'); e(0xA5, 0x00); A.abs_(0x8D, 0x0203)                # the x the PREVIOUS blank's read produced goes into the sprite
    e(0xA9, 0x02); A.abs_(0x8D, 0x4014)                             # sprite DMA
    e(0xA9, 0x00); A.abs_(0x8D, 0x2005); A.abs_(0x8D, 0x2005)
    e(0xA9, 0x01); A.abs_(0x8D, 0x4016); e(0xA9, 0x00); A.abs_(0x8D, 0x4016); A.abs_(0xAD, 0x4016)   # then the pad is read: A is bit 0
    e(0x29, 0x01); A.rel(0xF0, 'no'); e(0xA9, 200); A.rel(0xD0, 'st')
    A.L('no'); e(0xA9, 20)
    A.L('st'); e(0x85, 0x00, 0x40)                                  # the answer waits in the zero page; rti
    code = A.done(0xC000)
    prg = bytearray(0x4000); prg[:len(code)] = code
    nmi, rst = 0xC000 + A.lab['nmi'], 0xC000 + A.lab['reset']
    prg[0x3FFA:0x4000] = bytes([nmi & 255, nmi >> 8, rst & 255, rst >> 8, 0, 0])
    chr_ = bytearray(0x2000); chr_[16:24] = b'\xFF' * 8             # tile 1: colour 1 everywhere
    return b'NES\x1a\x01\x01\x00\x00' + bytes(8) + bytes(prg) + bytes(chr_)

WS_ASM = r'''
	cpu 186
	bits 16
	org 0
start:
	cli
	cld
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x1000
	out 0x00, al              ; display off
	mov al, 0x20              ; the shades
	out 0x1C, al
	mov al, 0x64
	out 0x1D, al
	mov al, 0xA8
	out 0x1E, al
	mov al, 0xFC
	out 0x1F, al
	mov al, 0x00              ; sprite palette 8: colour 3 the darkest shade, the rest the lightest
	out 0x30, al
	mov al, 0x70
	out 0x31, al
	mov di, 0x2000            ; tile 0 empty, tile 1 solid (colour 3)
	mov cx, 8
	xor ax, ax
	rep stosw
	mov cx, 8
	mov ax, 0xFFFF
	rep stosw
	mov al, 0x07              ; sprite table at 0x0E00
	out 0x04, al
	mov di, 0x0E00            ; sprite 0: tile 1, palette 8, y 70, x 20
	mov word [di], 0x0001
	mov byte [di+2], 70
	mov byte [di+3], 20
	xor al, al
	out 0x05, al
	mov al, 1
	out 0x06, al
	mov byte [0x0100], 20
	mov al, 0x01
	out 0x14, al              ; LCD on
	mov al, 0x04
	out 0x00, al              ; sprites on
main:
.w1:	in al, 0x02
	cmp al, 144
	jne .w1                   ; vertical blank starts
	mov al, [0x0100]          ; the x the PREVIOUS blank's read produced goes into the sprite
	mov [0x0E03], al
	mov al, 0x40              ; the buttons
	out 0xB5, al
	nop
	nop
	nop
	nop
	in al, 0xB5
	mov ah, 20
	test al, 0x04             ; A
	jz .st
	mov ah, 100
.st:	mov [0x0100], ah          ; the answer waits in RAM for the next blank
.w2:	in al, 0x02
	cmp al, 144
	je .w2
	jmp main
'''

def ws(tmp):
    nasm = shutil.which('nasm')
    if not nasm: return None
    src = os.path.join(tmp, 'l.asm'); open(src, 'w').write(WS_ASM)
    out = os.path.join(tmp, 'l.bin')
    subprocess.run([nasm, '-f', 'bin', '-o', out, src], check=True)
    code = open(out, 'rb').read()
    # 64 KB cartridge at segment 0xF000: the reset entry is the last 16 bytes (jump far to F000:0000)
    rom = bytearray(0x10000); rom[:len(code)] = code
    rom[0xFFF0:0x10000] = bytes([0xEA, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x42, 0x00, 0x01, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00])
    return bytes(rom)

def gb(tmp):
    bindir = None
    for d in os.environ.get('PATH', '').split(os.pathsep) + ['/opt/rgbds/bin']:
        if os.path.exists(os.path.join(d, 'rgbasm')): bindir = d; break
    if not bindir: return None
    src = os.path.join(tmp, 'l.asm'); open(src, 'w').write(GB_ASM)
    o, out = os.path.join(tmp, 'l.o'), os.path.join(tmp, 'l.gb')
    subprocess.run([os.path.join(bindir, 'rgbasm'), '-o', o, src], check=True)
    subprocess.run([os.path.join(bindir, 'rgblink'), '-o', out, o], check=True)
    subprocess.run([os.path.join(bindir, 'rgbfix'), '-v', '-p', '0xFF', out], check=True)
    return open(out, 'rb').read()

if __name__ == '__main__':
    d = sys.argv[1]; os.makedirs(d, exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        for name, fn in (('latency.gb', lambda: gb(t)), ('latency.nes', nes), ('latency.ws', lambda: ws(t))):
            r = fn()
            if r is None: print('skip', name, '(no assembler)'); continue
            open(os.path.join(d, name), 'wb').write(r); print('wrote', name, len(r))
