; gblocal.asm -- hostcore/testrom: a Game Boy that talks over its link cable,
; for the local link's tests (GPL-2.0-or-later, like the rest of gpsp/vita).
;
; Both machines run this cartridge.  Each waits with its serial port armed
; on the EXTERNAL clock (a slave).  The one whose A button is pressed
; becomes the master: from then on it sends one byte a frame on the
; internal clock, a counter, and the slave answers each byte with its
; complement in the next transfer.  Both sides check every byte they
; receive against what the protocol says it must be.
;
;   master sends   c(k) = k + 1                 k = 0, 1, 2, ...
;   slave  sends   $55 the first time, then  c(k-1) XOR $FF
;
; What it leaves to look at:
;   WRAM  C000 role (0 none, 1 master, 2 slave)
;         C001 good bytes (low), C002 (high)     C003 bad bytes
;         C004 last byte sent   C005 last byte received
;         C006 boot count (read from the battery at power on, plus one)
;         C007 buttons (the frame's: A B Select Start Right Left Up Down)
;   cartridge RAM (MBC1+RAM+BATTERY), written at power on and once more
;   when 64 good bytes have crossed:
;         A000 boot count  A001 role  A002/A003 good  A004 bad  A005 $A5
;   the screen: a row of 16 boxes, the good count in binary; under it two
;   boxes at the left for a master or at the right for a slave; a full row
;   of boxes if any byte was bad; and one box that follows the d-pad's
;   Right, so a picture shows whose buttons reach the machine.
;
; Built with rgbds (rgbasm/rgblink/rgbfix); see the Makefile.

rP1 EQU $FF00
rSB EQU $FF01
rSC EQU $FF02
rLCDC EQU $FF40
rLY EQU $FF44
rBGP EQU $FF47

wRole EQU $C000
wGoodL EQU $C001
wGoodH EQU $C002
wBad EQU $C003
wTx EQU $C004
wRx EQU $C005
wBoot EQU $C006
wKeys EQU $C007
wSaved EQU $C008
wAnswered EQU $C009

SECTION "entry", ROM0[$100]
    nop
    jp   start
    ds   $150 - @, 0            ; header, filled in by rgbfix

SECTION "main", ROM0[$150]
start:
    di
    ld   sp, $DFF0
    ; the picture: LCD off in the vertical blank, two tiles, an empty map
.wait_vb:
    ldh  a, [rLY]
    cp   145
    jr   nz, .wait_vb
    xor  a
    ldh  [rLCDC], a
    ld   hl, $8000              ; tile 0: colour 0
    ld   b, 16
.t0:
    ld   [hl+], a
    dec  b
    jr   nz, .t0
    ld   a, $FF                 ; tile 1: colour 3
    ld   b, 16
.t1:
    ld   [hl+], a
    dec  b
    jr   nz, .t1
    ld   hl, $9800              ; the 32 x 32 map: tile 0
    ld   bc, $0400
.map:
    xor  a
    ld   [hl+], a
    dec  bc
    ld   a, b
    or   c
    jr   nz, .map
    ld   a, $E4
    ldh  [rBGP], a
    ; the variables
    ld   hl, wRole
    ld   b, 16
    xor  a
.vars:
    ld   [hl+], a
    dec  b
    jr   nz, .vars
    ; the battery: its first byte counts the boots
    ld   a, $0A
    ld   [$0000], a
    ld   a, [$A000]
    inc  a
    ld   [$A000], a
    ld   [wBoot], a
    xor  a
    ld   [$A001], a
    ld   [$A005], a
    ld   [$0000], a
    ; a slave until told otherwise: armed, the first answer is $55
    ld   a, $55
    ldh  [rSB], a
    ld   a, $80
    ldh  [rSC], a
    ld   a, $91                 ; LCD on, tiles at 8000, background on
    ldh  [rLCDC], a

frame:
    ; the start of the vertical blank
.w1:
    ldh  a, [rLY]
    cp   144
    jr   nz, .w1
    call draw
    call keys
    ld   a, [wRole]
    cp   1
    jr   z, master
    ; ---- slave (or nobody yet) --------------------------------------------
    ld   a, [wKeys]
    bit  0, a                   ; A: this machine clocks from now on
    jr   nz, become_master
    ldh  a, [rSC]
    bit  7, a
    jr   nz, next               ; still armed: nothing arrived
    ; a byte arrived
    ldh  a, [rSB]
    ld   b, a                   ; b = received
    ld   a, [wRole]
    or   a
    jr   nz, .check
    ld   a, 2                   ; the first byte makes a slave of us
    ld   [wRole], a
    ld   a, b
    cp   1
    jr   z, .good
    jr   .bad
.check:
    ld   a, [wRx]
    inc  a
    cp   b
    jr   z, .good
.bad:
    call count_bad
    jr   .answer
.good:
    call count_good
.answer:
    ld   a, b
    ld   [wRx], a
    cpl
    ld   [wTx], a
    ldh  [rSB], a
    ld   a, $80
    ldh  [rSC], a
    jr   next

become_master:
    ld   a, 1
    ld   [wRole], a
    xor  a
    ld   [wTx], a
    ldh  [rSC], a               ; no longer armed
    jr   send

master:
    ldh  a, [rSC]
    bit  7, a
    jr   nz, next               ; the last byte is still on the wire
    ldh  a, [rSB]
    ld   b, a                   ; b = the slave's answer
    ld   a, [wAnswered]
    or   a
    jr   nz, .later
    inc  a
    ld   [wAnswered], a
    ld   a, $55                 ; the answer to the first byte
    jr   .cmp
.later:
    ld   a, [wTx]
    dec  a
    cpl                         ; the complement of the byte before
.cmp:
    cp   b
    jr   z, .good
    call count_bad
    jr   .done
.good:
    call count_good
.done:
    ld   a, b
    ld   [wRx], a
send:
    ld   a, [wTx]
    inc  a
    ld   [wTx], a
    ldh  [rSB], a
    ld   a, $81
    ldh  [rSC], a

next:
    call battery
    ; the end of this vertical blank's first line: one pass a frame
.w2:
    ldh  a, [rLY]
    cp   146
    jr   nz, .w2
    jp   frame

count_good:
    ld   hl, wGoodL
    inc  [hl]
    ret  nz
    inc  hl
    inc  [hl]
    ret

count_bad:
    ld   hl, wBad
    inc  [hl]
    ret  nz
    dec  [hl]                   ; stays at 255
    ret

; 64 good bytes: the verdict goes to the battery, once
battery:
    ld   a, [wSaved]
    or   a
    ret  nz
    ld   a, [wGoodH]
    or   a
    jr   nz, .write
    ld   a, [wGoodL]
    cp   64
    ret  c
.write:
    ld   a, 1
    ld   [wSaved], a
    ld   a, $0A
    ld   [$0000], a
    ld   a, [wRole]
    ld   [$A001], a
    ld   a, [wGoodL]
    ld   [$A002], a
    ld   a, [wGoodH]
    ld   [$A003], a
    ld   a, [wBad]
    ld   [$A004], a
    ld   a, $A5
    ld   [$A005], a
    xor  a
    ld   [$0000], a
    ret

; the buttons, as one byte: A B Select Start Right Left Up Down
keys:
    ld   a, $10                 ; the button row
    ldh  [rP1], a
    ldh  a, [rP1]
    ldh  a, [rP1]
    cpl
    and  $0F
    ld   b, a
    ld   a, $20                 ; the direction row
    ldh  [rP1], a
    ldh  a, [rP1]
    ldh  a, [rP1]
    cpl
    and  $0F
    swap a
    or   b
    ld   [wKeys], a
    ld   a, $30
    ldh  [rP1], a
    ret

; the picture (in the vertical blank)
draw:
    ; row 4: the good count, sixteen boxes, most significant first
    ld   hl, $9800 + 4 * 32 + 2
    ld   a, [wGoodH]
    call .byte
    ld   a, [wGoodL]
    call .byte
    ; row 7: master at the left, slave at the right
    ld   a, [wRole]
    ld   b, a
    ld   hl, $9800 + 7 * 32 + 2
    cp   1
    call .flag
    ld   [hl+], a
    ld   [hl+], a
    ld   a, b
    ld   hl, $9800 + 7 * 32 + 16
    cp   2
    call .flag
    ld   [hl+], a
    ld   [hl+], a
    ; row 10: any bad byte fills it
    ld   a, [wBad]
    or   a
    jr   z, .nobad
    ld   a, 1
.nobad:
    ld   hl, $9800 + 10 * 32 + 2
    ld   b, 16
.badrow:
    ld   [hl+], a
    dec  b
    jr   nz, .badrow
    ; row 13: Right on the d-pad
    ld   a, [wKeys]
    swap a
    and  1
    ld   [$9800 + 13 * 32 + 9], a
    ld   [$9800 + 13 * 32 + 10], a
    ret
.flag:
    ld   a, 0
    ret  nz
    inc  a
    ret
.byte:
    ld   b, 8
.bit:
    rlca
    ld   c, a
    and  1
    ld   [hl+], a
    ld   a, c
    dec  b
    jr   nz, .bit
    ret
