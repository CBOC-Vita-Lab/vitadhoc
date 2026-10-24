; gblink.asm -- hostcore/testrom: a Game Boy cartridge that uses the link
; cable, for the network link tests (hostcore/link_tests.py).
;
; Both consoles run the same cartridge.  Each waits as the cable's slave
; (external clock, SB = $A5).  The one whose player presses A becomes the
; MASTER: once a frame it sends the next byte of 1, 2, 3 ... 200 on its own
; clock and checks what comes back -- $A5 for the first, then the complement
; of the byte it sent before.  The other one notices its first completed
; transfer and is the SLAVE: it checks that the bytes arrive in order and
; answers each with its complement.  After the 200th byte each writes its
; verdict to cartridge RAM (the battery save) once, and stops:
;
;   A000  'G' 'L'
;   A002  role: 1 master, 2 slave
;   A003  bytes exchanged (200)
;   A004  checks passed (200)
;   A005  checks failed (0)
;   A006  the last byte received
;   A007  $5A
;
; The same eight values minus the letters live at C000 (work RAM) from the
; first exchange on, for a test that reads the machine's memory.
;
; The screen is one flat shade: white while nobody has pressed A, light grey
; on the master, dark grey on the slave, black as soon as a check fails.
;
; Built with rgbds; see the Makefile (make gb).

rROLE EQU $C000
rN EQU $C001
rOK EQU $C002
rBAD EQU $C003
rLAST EQU $C004
rDONE EQU $C005
COUNT EQU 200

SECTION "entry", ROM0[$100]
    nop
    jp   start
    ds   $150 - @, 0            ; header, filled in by rgbfix

SECTION "main", ROM0[$150]
start:
    di
    ld   sp, $DFF0
.wait_vbl:
    ldh  a, [$FF44]
    cp   144
    jr   c, .wait_vbl
    xor  a
    ldh  [$FF40], a             ; LCD off
    ld   hl, $8000              ; tile 0 and both maps: zero
.clear:
    xor  a
    ld   [hl+], a
    ld   a, h
    cp   $A0
    jr   nz, .clear
    xor  a
    ldh  [$FF47], a             ; colour 0 = white
    ld   a, $91
    ldh  [$FF40], a             ; LCD on, background on
    ld   hl, rROLE
    ld   b, 16
    xor  a
.zero:
    ld   [hl+], a
    dec  b
    jr   nz, .zero
    call arm_slave

; Nobody has a role yet: poll the cable and the A button.
idle:
    ldh  a, [$FF02]
    bit  7, a
    jr   z, become_slave        ; a transfer completed: someone clocked us
    ld   a, $10                 ; select the action buttons
    ldh  [$FF00], a
    ldh  a, [$FF00]
    ldh  a, [$FF00]
    bit  0, a                   ; A, 0 = pressed
    jr   nz, idle
    ld   a, 1
    ld   [rROLE], a
    ldh  [$FF47], a             ; light grey
    jr   master

become_slave:
    ld   a, 2
    ld   [rROLE], a
    ldh  [$FF47], a             ; dark grey
    jr   slave_got

; ---- the slave: answer every byte as soon as it has arrived ----------------
slave:
    ldh  a, [$FF02]
    bit  7, a
    jr   nz, slave
slave_got:
    ldh  a, [$FF01]
    ld   [rLAST], a
    ld   b, a
    ld   hl, rN
    inc  [hl]                   ; the byte we expect: 1, 2, 3 ...
    ld   a, [hl]
    cp   b
    jr   nz, .bad
    ld   hl, rOK
    inc  [hl]
    jr   .answer
.bad:
    call failed
.answer:
    ld   a, b
    cpl
    ldh  [$FF01], a             ; the complement of what came
    ld   a, $80
    ldh  [$FF02], a
    ld   a, [rN]
    cp   COUNT
    jr   nz, slave
    call save
    jr   halt_here

; ---- the master: one byte a frame ---------------------------------------------
master:
    ldh  a, [$FF44]             ; the next line 100
    cp   100
    jr   z, master
.line:
    ldh  a, [$FF44]
    cp   100
    jr   nz, .line
    ld   hl, rN
    inc  [hl]
    ld   a, [hl]
    ldh  [$FF01], a
    ld   a, $81                 ; start, internal clock
    ldh  [$FF02], a
.busy:
    ldh  a, [$FF02]
    bit  7, a
    jr   nz, .busy
    ldh  a, [$FF01]
    ld   [rLAST], a
    ld   b, a
    ld   a, [rN]
    dec  a
    jr   z, .first
    cpl                         ; the complement of the byte before
    jr   .compare
.first:
    ld   a, $A5
.compare:
    cp   b
    jr   nz, .bad
    ld   hl, rOK
    inc  [hl]
    jr   .next
.bad:
    call failed
.next:
    ld   a, [rN]
    cp   COUNT
    jr   nz, master
    call save

halt_here:
    ld   a, 1
    ld   [rDONE], a
.forever:
    halt
    nop
    jr   .forever

arm_slave:
    ld   a, $A5
    ldh  [$FF01], a
    ld   a, $80                 ; start, external clock
    ldh  [$FF02], a
    ret

failed:
    ld   hl, rBAD
    inc  [hl]
    ld   a, 3
    ldh  [$FF47], a             ; black
    ret

; The verdict, once, into the battery RAM.
save:
    ld   a, $0A
    ld   [$0000], a             ; MBC1: enable cartridge RAM
    ld   hl, $A000
    ld   a, "G"
    ld   [hl+], a
    ld   a, "L"
    ld   [hl+], a
    ld   de, rROLE
    ld   b, 5
.copy:
    ld   a, [de]
    inc  de
    ld   [hl+], a
    dec  b
    jr   nz, .copy
    ld   a, $5A
    ld   [hl], a
    xor  a
    ld   [$0000], a
    ret
