; rwtest.asm -- tests/rewind: a Game Boy cartridge whose picture says which
; frame it is, with battery RAM (MBC1+RAM+BATTERY, 8 KiB).  Built twice from
; this one source: rwtest.gb (DMG) and rwtest.gbc (the CGB flag set), so the
; rewind tests have a moving, checkable picture on both consoles.
;
; Every vertical blank:
;   row 2   the 16-bit frame counter, most significant bit first (a solid
;           tile = 1)
;   row 4   the first byte of cartridge RAM (FF on a fresh cartridge)
;   row 6   how many times A was pressed
;   row 8   a block that moves one column a frame
; A pressed: the press count goes up and 64 bytes of cartridge RAM are
; written, counting up from count * 16.
;
; rgbds (rgbasm / rgblink / rgbfix); see the Makefile.  GPL-2.0-or-later,
; like the rest of gpsp/vita.

FRAME_LO EQU $C000
FRAME_HI EQU $C001
NPRESS   EQU $C002
KEYSWAS EQU $C003

SECTION "vblank", ROM0[$40]
    jp   vblank

SECTION "entry", ROM0[$100]
    nop
    jp   start
    ds   $150 - @, 0            ; header, filled in by rgbfix

SECTION "main", ROM0[$150]
start:
    di
    ld   sp, $FFFE
.wait:                          ; the LCD may only be switched off in vblank
    ldh  a, [$44]
    cp   144
    jr   c, .wait
    xor  a
    ldh  [$40], a

    ld   hl, $8000              ; tile 0 blank, tile 1 solid
    ld   b, 16
.t0:
    ld   [hl+], a
    dec  b
    jr   nz, .t0
    ld   b, 16
    ld   a, $FF
.t1:
    ld   [hl+], a
    dec  b
    jr   nz, .t1

    ld   hl, $9800              ; the background map, all blank
    ld   bc, $0400
.clr:
    xor  a
    ld   [hl+], a
    dec  bc
    ld   a, b
    or   c
    jr   nz, .clr

    ld   a, $E4                 ; DMG palette: 0 white ... 3 black
    ldh  [$47], a
    ld   a, $80                 ; CGB background palette 0, auto-increment
    ldh  [$68], a               ; (nothing is at these addresses on a DMG)
    ld   a, $FF
    ldh  [$69], a
    ld   a, $7F
    ldh  [$69], a               ; colour 0: white
    ld   b, 6
    xor  a
.pal:
    ldh  [$69], a               ; colours 1-3: black
    dec  b
    jr   nz, .pal

    xor  a
    ld   [FRAME_LO], a
    ld   [FRAME_HI], a
    ld   [NPRESS], a
    ld   [KEYSWAS], a
    ldh  [$42], a
    ldh  [$43], a
    ld   a, $91                 ; LCD on, background on, tiles at $8000
    ldh  [$40], a
    xor  a
    ldh  [$0F], a
    ld   a, 1
    ldh  [$FF], a               ; the vblank interrupt
    ei
.idle:
    halt
    nop
    jr   .idle

; a = value, hl = where: eight tiles, most significant bit first
bits8:
    ld   b, 8
.bit:
    rlca
    ld   c, a
    ld   a, 0
    adc  a, 0
    ld   [hl+], a
    ld   a, c
    dec  b
    jr   nz, .bit
    ret

vblank:
    push af
    push bc
    push de
    push hl

    ld   hl, FRAME_LO
    inc  [hl]
    jr   nz, .nocarry
    inc  hl
    inc  [hl]
.nocarry:
    ld   hl, $9842              ; row 2, column 2
    ld   a, [FRAME_HI]
    call bits8
    ld   a, [FRAME_LO]
    call bits8

    ld   a, $10                 ; the action buttons
    ldh  [$00], a
    ldh  a, [$00]
    ldh  a, [$00]
    cpl
    and  $0F
    ld   b, a
    ld   a, $30
    ldh  [$00], a
    ld   a, [KEYSWAS]
    ld   c, a
    ld   a, b
    ld   [KEYSWAS], a
    bit  0, b                   ; A down now...
    jr   z, .nopress
    bit  0, c                   ; ...and not a frame ago
    jr   nz, .nopress
    ld   hl, NPRESS
    inc  [hl]
    ld   a, [hl]
    swap a
    and  $F0
    ld   d, a
    ld   a, $0A
    ld   [$0000], a             ; MBC1: enable cartridge RAM
    ld   hl, $A000
    ld   b, 64
    ld   a, d
.fill:
    ld   [hl+], a
    inc  a
    dec  b
    jr   nz, .fill
    xor  a
    ld   [$0000], a
.nopress:

    ld   a, $0A
    ld   [$0000], a
    ld   a, [$A000]
    ld   d, a
    xor  a
    ld   [$0000], a
    ld   hl, $9882              ; row 4
    ld   a, d
    call bits8
    ld   hl, $98C2              ; row 6
    ld   a, [NPRESS]
    call bits8

    ld   hl, $9902              ; row 8: the moving block
    ld   b, 16
    xor  a
.clr2:
    ld   [hl+], a
    dec  b
    jr   nz, .clr2
    ld   a, [FRAME_LO]
    and  $0F
    add  a, $02
    ld   l, a
    ld   h, $99
    ld   a, 1
    ld   [hl], a

    pop  hl
    pop  de
    pop  bc
    pop  af
    reti
