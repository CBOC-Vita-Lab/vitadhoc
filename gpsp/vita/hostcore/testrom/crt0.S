@ crt0.S -- startup for hostcore/testrom (a GBA cartridge image).
@ ARM code: the header, stacks, .data/.bss, the interrupt handler, then
@ main() in Thumb.
    .section .init, "ax"
    .arm
    .global _start
_start:
    b       rom_start
    .space  156, 0                  @ logo area (the open BIOS does not check it)
    .ascii  "GBADHOCTEST\0"         @ title, 12 bytes
    .ascii  "AGDE"                  @ game code
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

@ Called by the BIOS in ARM state on the IRQ stack.  Acknowledge what fired
@ (in IF and in the BIOS's own copy, which IntrWait watches) and count
@ vertical blanks.
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
    tst     r1, #1                  @ vblank
    ldrne   r2, =g_frames
    ldrne   r3, [r2]
    addne   r3, r3, #1
    strne   r3, [r2]
    bx      lr
    .pool
