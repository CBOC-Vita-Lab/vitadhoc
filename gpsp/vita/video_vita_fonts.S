/* video_vita_fonts.S -- Inter and Outfit (both SIL OFL 1.1, gpsp/assets/fonts)
 * embedded in the binary, so the Vita UI has no font file to ship or lose.
 * video_vita_text.c rasterises Inter's two faces with stb_truetype at
 * start-up; vstyle.c makes its six sizes (Outfit for titles) on first use.
 * Assemble with -I<gpsp>/assets/fonts (vita/ui.mk and the host Makefile do).
 */
   .section .rodata
   .balign 4
   .global vv_font_regular
   .global vv_font_regular_end
vv_font_regular:
   .incbin "Inter-Regular.ttf"
vv_font_regular_end:
   .balign 4
   .global vv_font_semibold
   .global vv_font_semibold_end
vv_font_semibold:
   .incbin "Inter-SemiBold.ttf"
vv_font_semibold_end:
   .balign 4
   .global vv_font_display_md
vv_font_display_md:
   .incbin "Outfit-Medium.ttf"
   .balign 4
   .global vv_font_display_sb
vv_font_display_sb:
   .incbin "Outfit-SemiBold.ttf"
   .byte 0
#if defined(__linux__) && defined(__ELF__)
   .section .note.GNU-stack,"",%progbits
#endif
