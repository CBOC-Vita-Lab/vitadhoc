; gbsave.asm -- hostcore/testrom: a Game Boy cartridge with battery RAM
; (MBC1+RAM+BATTERY, 8 KiB), for the frontend's GB save path.
;
; Every boot reads the first byte of cartridge RAM, adds one, and writes 64
; bytes counting up from that.  Fresh cartridge RAM reads FF, so the first
; run leaves saves/gbsave.gb.sav starting 00 01 02 ...; run it again on the
; same data root and the file must start 01 02 03 ...: the first run's save
; was written, loaded back by the second, and written again.
;
; Built with rgbds (rgbasm/rgblink/rgbfix); see the Makefile.

SECTION "entry", ROM0[$100]
    nop
    jp   start
    ds   $150 - @, 0            ; header, filled in by rgbfix

SECTION "main", ROM0[$150]
start:
    di
    ld   a, $0A
    ld   [$0000], a             ; MBC1: enable cartridge RAM
    ld   hl, $A000
    ld   a, [hl]
    inc  a
    ld   b, 64
.fill:
    ld   [hl+], a
    inc  a
    dec  b
    jr   nz, .fill
    xor  a
    ld   [$0000], a             ; ...and protect it again
.idle:
    halt
    nop
    jr   .idle
