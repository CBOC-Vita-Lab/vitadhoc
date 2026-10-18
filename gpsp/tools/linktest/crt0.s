@ crt0.s -- cartridge header, start-up, and the routines of the link test ROM
@ that must be assembly: the interrupt entry, the three self-modifying /
@ RAM-executed routines, and the image of the EWRAM routine.
@
@ No Nintendo logo is included (it is not ours to ship).  A real GBA BIOS
@ would refuse the cartridge; the open-source BIOS gpSP bundles does not
@ look at it, and neither do gl_host / the emulator's direct boot.

        .section .header, "ax", %progbits
        .arm
        .global _start
_start:
        b       rom_start                       @ 00
        .space  156                             @ 04 logo (absent, see above)
        .ascii  "GBALINKTEST\0"                 @ A0 title
        .ascii  "ZLKT"                          @ AC game code
        .ascii  "00"                            @ B0 maker
        .byte   0x96                            @ B2 fixed value
        .byte   0, 0                            @ B3 unit, device
        .space  7                               @ B5 reserved
        .byte   0                               @ BC version
        .byte   0                               @ BD checksum (Makefile fills)
        .byte   0, 0                            @ BE reserved

rom_start:                                      @ C0
        mov     r0, #0xD3                       @ supervisor, IRQs off
        msr     cpsr_c, r0
        ldr     sp, =0x03007FE0
        mov     r0, #0xD2                       @ IRQ mode
        msr     cpsr_c, r0
        ldr     sp, =0x03007FA0
        mov     r0, #0x1F                       @ system mode, IRQs on
        msr     cpsr_c, r0
        ldr     sp, =0x03007D00                 @ leaves 0x2A0 for the IRQ stack

        ldr     r0, =__iwram_lma                @ IWRAM code + data out of ROM
        ldr     r1, =__iwram_start
        ldr     r2, =__iwram_end
1:      cmp     r1, r2
        ldrlo   r3, [r0], #4
        strlo   r3, [r1], #4
        blo     1b

        ldr     r1, =__bss_start
        ldr     r2, =__bss_end
        mov     r3, #0
2:      cmp     r1, r2
        strlo   r3, [r1], #4
        blo     2b

        ldr     r0, =irq_entry                  @ the BIOS jumps through here
        ldr     r1, =0x03007FFC
        str     r0, [r1]

        ldr     r0, =main
        mov     lr, pc
        bx      r0
3:      b       3b
        .pool

@ ---------------------------------------------------------------------------
@ IWRAM, ARM state
@ ---------------------------------------------------------------------------
        .section .iwram, "ax", %progbits
        .arm
        .align  2

@ The BIOS has saved r0-r3, r12, lr and calls us in IRQ mode.
        .global irq_entry
irq_entry:
.ifdef FLAGPROBE
        @ make FLAGPROBE=1 (linktest_flagprobe.gba): fold the NZCV flags the
        @ handler is entered with into results.pad[0].  Nothing before this
        @ instruction sets flags (the BIOS vector code does not), so this is
        @ the interrupted program's flags as the emulator delivers them --
        @ which a real program never looks at, and which is exactly why an
        @ emulator can get them wrong unnoticed (README, "flag probe").
        mrs     r2, cpsr
        mov     r3, #0x02000000
        ldr     r1, [r3, #0x70]
        add     r1, r1, r1, lsl #2
        add     r1, r1, r2, lsr #28
        str     r1, [r3, #0x70]
.endif
        mov     r3, #0x04000000
        add     r3, r3, #0x200
        ldr     r1, [r3]                        @ IE | IF << 16
        and     r0, r1, r1, lsr #16             @ enabled and pending
        strh    r0, [r3, #2]                    @ acknowledge
        ldr     r2, =0x03007FF8                 @ the BIOS's IntrWait flags
        ldrh    r1, [r2]
        orr     r1, r1, r0
        strh    r1, [r2]
        stmfd   sp!, {r4, lr}
        bl      irq_main                        @ iwram.c
        ldmfd   sp!, {r4, lr}
        bx      lr
        .pool

@ u32 mix_iw(u32 x) -- the checksum mixer.  main.c rewrites the instruction
@ at mix_iw_patch every frame (ROM code storing into translated IWRAM code)
@ and the serial interrupt handler calls it on every transfer.
        .global mix_iw, mix_iw_patch
mix_iw:
        eor     r0, r0, r0, lsl #5
mix_iw_patch:
        add     r0, r0, #0x55                   @ PATCHED: op and immediate
        eor     r0, r0, r0, lsr #7
        bx      lr

@ u32 selfmod_iw(u32 imm) -- rewrites one of its own instructions and then
@ runs it (IWRAM code storing into the block it is executing).  Returns
@ (imm & 0xFF) + 1000.
        .global selfmod_iw
selfmod_iw:
        ldr     r2, =0xE3A01000                 @ mov r1, #0
        and     r0, r0, #0xFF
        orr     r2, r2, r0
        adr     r3, 4f
        str     r2, [r3]
        nop                                     @ already in a real CPU's
        nop                                     @ pipeline when the store lands
4:      mov     r1, #0                          @ PATCHED: mov r1, #imm
        add     r0, r1, #1000
        bx      lr
        .pool

@ u32 swi_iw(u32 num, u32 den) -- BIOS calls made from IWRAM: Div (which the
@ dynarec replaces by a call to a helper in its ROM cache) and Sqrt (a real
@ SWI: a direct branch from RAM-cache code to the BIOS SWI block in the ROM
@ cache).  Returns quotient + remainder + sqrt(num).
        .global swi_iw
swi_iw:
        stmfd   sp!, {r4, r5, lr}
        mov     r5, r0
        swi     0x060000                        @ Div: r0 = quot, r1 = rem
        add     r4, r0, r1
        mov     r0, r5
        swi     0x080000                        @ Sqrt
        add     r0, r0, r4
        ldmfd   sp!, {r4, r5, lr}
        bx      lr

@ ---------------------------------------------------------------------------
@ The EWRAM routine, Thumb, position independent.  main.c copies this image
@ to EWCODE (by DMA3 and by CPU), patches the immediate at ew_img_patch in
@ the copy, and calls the copy.
@   u32 ew_func(u32 x, u32 den): t = x + imm; return t/den + ((t%den) << 8) + t
@ ---------------------------------------------------------------------------
        .section .ewcode, "ax", %progbits
        .thumb
        .align  2
        .global ew_img, ew_img_patch, ew_img_end
ew_img:
        push    {r4, lr}
        mov     r4, r0
ew_img_patch:
        mov     r2, #17                         @ PATCHED: immediate
        add     r4, r4, r2
        mov     r0, r4
        swi     6                               @ Div from EWRAM, Thumb
        lsl     r1, r1, #8
        add     r0, r0, r1
        add     r0, r0, r4
        pop     {r4}
        pop     {r1}
        bx      r1
        .align  2
ew_img_end:
