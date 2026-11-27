/* video_vita_fonts.S -- Inter and Outfit (both SIL OFL 1.1, gpsp/assets/fonts)
 * embedded in the binary, so the Vita UI has no font file to ship or lose.
 * video_vita_text.c rasterises Inter's two faces with stb_truetype at
 * start-up; vstyle.c makes its six sizes (Outfit for titles) on first use.
 * Assemble with -I<gpsp>/assets/fonts (vita/ui.mk and the host Makefile do).
 */
#ifdef __APPLE__
/* Mach-O: constants live in __TEXT,__const and C symbols carry an underscore */
#define FONT_RODATA .section __TEXT,__const
#define FSYM(n) _##n
#else
#define FONT_RODATA .section .rodata
#define FSYM(n) n
#endif
   FONT_RODATA
   .balign 4
   .global FSYM(vv_font_regular)
   .global FSYM(vv_font_regular_end)
FSYM(vv_font_regular):
   .incbin "Inter-Regular.ttf"
FSYM(vv_font_regular_end):
   .balign 4
   .global FSYM(vv_font_semibold)
   .global FSYM(vv_font_semibold_end)
FSYM(vv_font_semibold):
   .incbin "Inter-SemiBold.ttf"
FSYM(vv_font_semibold_end):
   .balign 4
   .global FSYM(vv_font_display_md)
FSYM(vv_font_display_md):
   .incbin "Outfit-Medium.ttf"
   .balign 4
   .global FSYM(vv_font_display_sb)
FSYM(vv_font_display_sb):
   .incbin "Outfit-SemiBold.ttf"
   .byte 0
#if defined(__linux__) && defined(__ELF__)
   .section .note.GNU-stack,"",%progbits
#endif
