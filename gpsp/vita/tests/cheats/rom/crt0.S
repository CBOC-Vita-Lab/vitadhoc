@ crt0.S -- startup of the cheat test cartridge (tests/cheats/rom).
@ The header, stacks, .data/.bss, the interrupt handler, then main() in
@ Thumb.  As hostcore/testrom/crt0.S, with its own title and game code:
@ the database tests look the cartridge up by both.
    .section .init, "ax"
    .arm
    .global _start
_start:
    b       rom_start
    .space  156, 0                  @ logo area (the open BIOS does not check it)
    .ascii  "CHEAT TEST\0\0"        @ title, 12 bytes
    .ascii  "ACHE"                  @ game code
    .ascii  "01"                    @ maker
    .byte   0x96, 0, 0              @ fixed value, unit code, device type
    .space  7, 0
    .byte   0, 0                    @ version, header checksum (unchecked)
    .space  2, 0

rom_start:
    mov     r0, #0xD2               @ IRQ mode, interrupts masked
    msr     cpsr_c, r0
    ldr     sp, =0x03007FA0
    mov     r0, #0x1F               @ system mode, interrupts open
    msr     cpsr_c, r0
    ldr     sp, =0x03007F00

    ldr     r0, =__data_lma
    ldr     r1, =__data_start
    ldr     r2, =__data_end
1:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     1b
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    mov     r3, #0
2:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     2b

    ldr     r0, =irq_handler        @ the BIOS jumps through 0x03007FFC
    ldr     r1, =0x03007FFC
    str     r0, [r1]
    ldr     r0, =main
    bx      r0                      @ main is Thumb

irq_handler:
    mov     r0, #0x04000000
    add     r0, r0, #0x200
    ldr     r1, [r0]                @ IE | IF << 16
    and     r1, r1, r1, lsr #16
    strh    r1, [r0, #2]            @ IF: write 1 to clear
    ldr     r2, =0x03007FF8
    ldrh    r3, [r2]
    orr     r3, r3, r1
    strh    r3, [r2]
    bx      lr
    .pool

@ ---- what the ROM-patch codes aim at: fixed addresses (gba.ld) -----------
    .section .patch, "ax"
    .thumb
    .global rom_const
    .thumb_func
rom_const:                          @ 0x08002000
    movs    r0, #5                  @ 0x2005   <- patched to movs r0, #9
    bx      lr                      @ 0x4770
    .global rom_lit
    .thumb_func
rom_lit:                            @ 0x08002004
    ldr     r0, rom_lit_pool        @ the translator reads the pool word
    bx      lr                      @ when it translates this
    .align  2
rom_lit_pool:                       @ 0x08002008
    .word   0x11223344

@ The routine main() copies to internal RAM (0x03004000) and calls there.
    .global ram_template
ram_template:
    movs    r0, #7                  @ 0x2007   <- a RAM write makes it #8
    bx      lr
