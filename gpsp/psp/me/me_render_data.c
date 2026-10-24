/* me_render_data.c — ME-local storage for the render inputs video.cc reads as
 * externs.  The renderer object (video.o) references these by name with C
 * linkage; the linker matches by name+size, so plain types suffice.  These are
 * the ME's working copy of the frame snapshot (host DMAs into them each frame).
 * Sizes mirror the core: cpu.cc / gba_memory.h.
 *
 * Only the SMALL ones live here.  The module is loaded into the kernel
 * partition, and what it keeps in .bss is what decides whether it loads at
 * all on a console whose kernel memory is partly taken.  The three large
 * buffers -- the VRAM mirror, the OBJ lists and the output frame, 278 KB --
 * are the host's to provide (me_mbox.h, "THE RENDERER'S WORK AREA"); the
 * module keeps only where they are for the render it is executing. */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

u16 io_registers[512];
u16 oam_ram[512];
u16 palette_ram_converted[512];
/* The work area's buffers, set by me_render_run() from the desc of every
 * render before anything reads them.  video.cc reaches `vram` and its OBJ
 * lists through these (ME_WORK_INDIRECT). */
u8 *me_work_vram;
u8 *me_work_vram_obj;      /* = me_work_vram + 0x10000, the OBJ tiles */
u8 *me_work_obj;
u32 reg[64];
int sprite_limit;
u32 skip_next_frame;
