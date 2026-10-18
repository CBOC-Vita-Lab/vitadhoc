; gbcheat.asm -- the Game Boy cheat test cartridge (MBC1, 64 KiB, four
; banks), for tests/cheats/cheat_e2e.c.  Built with rgbds.
;
;   C000  lives     5, minus one every eight frames down to 0; also the
;                   background palette (FF47), so it shows on the screen
;   C001  gg        the operand of "ld a, $11" at ROM 0x0300
;   C002  bank2     ROM 0x4000 of bank 2 (0x22)
;   C003  bank3     ROM 0x4000 of bank 3 (0x44)
;   C004  frames
;   C005  wram1     Game Boy Color: work RAM bank 1's D000 (the game itself
;                   keeps bank 2 selected and writes 0x5A to ITS D000)
;   C006  hram      a copy of FF90 (0x77 unless a code says otherwise)
;   C007  model     what the boot ROM left in A (0x11: Game Boy Color)
;   C008  wram2     Game Boy Color: work RAM bank 2's D000 (0x5A)

SECTION "entry", ROM0[$100]
    nop
    jp   start
    ds   $150 - @, 0            ; header, filled in by rgbfix

SECTION "main", ROM0[$150]
start:
    di
    ld   sp, $DFF0
    ld   b, a                   ; A = $11 on a Game Boy Color
    xor  a
    ldh  [$FF0F], a
    ldh  [$FFFF], a
.wait_vblank:
    ldh  a, [$FF44]
    cp   144
    jr   c, .wait_vblank
    xor  a
    ldh  [$FF40], a             ; LCD off

    ; tile 0: every pixel colour 3
    ld   hl, $8000
    ld   c, 16
    ld   a, $FF
.tile:
    ld   [hl+], a
    dec  c
    jr   nz, .tile
    ; the map is all tile 0 (clear both maps)
    ld   hl, $9800
    ld   de, $0800
.map:
    xor  a
    ld   [hl+], a
    dec  de
    ld   a, d
    or   e
    jr   nz, .map

    ld   a, 5
    ld   [$C000], a
    xor  a
    ld   [$C004], a
    ld   a, $77
    ldh  [$FF90], a
    ld   a, b
    ld   [$C007], a             ; $11 = Game Boy Color

    cp   $11
    jr   nz, .no_cgb
    ld   a, 1
    ldh  [$FF70], a
    xor  a
    ld   [$D000], a             ; bank 1: 0 until a code writes it
    ld   a, 2
    ldh  [$FF70], a
    ld   a, $5A
    ld   [$D000], a             ; bank 2, the one the game lives in
.no_cgb:

    ld   a, $91
    ldh  [$FF40], a             ; LCD on, BG on, tiles at 8000

frame:
.wait_line:                     ; one pass per frame: wait for line 144
    ldh  a, [$FF44]
    cp   144
    jr   nz, .wait_line

    ld   hl, $C004
    inc  [hl]
    ld   a, [hl]
    and  7
    jr   nz, .no_dec
    ld   a, [$C000]
    or   a
    jr   z, .no_dec
    dec  a
    ld   [$C000], a
.no_dec:
    ld   a, [$C000]
    ldh  [$FF47], a             ; lives -> the palette

    call gg_target
    ld   [$C001], a

    ld   a, 2
    ld   [$2000], a             ; MBC1: ROM bank 2
    ld   a, [$4000]
    ld   [$C002], a
    ld   a, 3
    ld   [$2000], a
    ld   a, [$4000]
    ld   [$C003], a
    ld   a, 1
    ld   [$2000], a

    ld   a, [$C007]
    cp   $11
    jr   nz, .no_bank
    ld   a, 1
    ldh  [$FF70], a
    ld   a, [$D000]
    ld   [$C005], a
    ld   a, 2
    ldh  [$FF70], a
    ld   a, [$D000]
    ld   [$C008], a
.no_bank:
    ldh  a, [$FF90]
    ld   [$C006], a

.leave_line:                    ; ...and for it to pass
    ldh  a, [$FF44]
    cp   144
    jr   z, .leave_line
    jr   frame

SECTION "gg", ROM0[$2FF]
gg_target:
    ld   a, $11                 ; opcode at 0x02FF, operand at 0x0300
    ret

SECTION "bank1", ROMX[$4000], BANK[1]
    db   $33
SECTION "bank2", ROMX[$4000], BANK[2]
    db   $22
SECTION "bank3", ROMX[$4000], BANK[3]
    db   $44
