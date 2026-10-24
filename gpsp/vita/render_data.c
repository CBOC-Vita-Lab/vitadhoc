/* render_data.c -- the memory the render thread's own copy of the GBA's
 * picture renderer reads (render_thread.h).
 *
 * gpsp/video.cc built a second time with ME_PRX_BUILD (vita/corebuild.mk,
 * CORE_R) refers to these by name, as the PSP's Media Engine build does
 * (psp/me/me_render_data.c); every global of that object, these included,
 * is then renamed gbar_<name>, so they are not the emulator's.  Sizes are
 * the core's (gba_memory.h, cpu.cc, main.h). */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

u16 io_registers[512] __attribute__((aligned(64)));
u16 oam_ram[512] __attribute__((aligned(64)));
u16 palette_ram_converted[512] __attribute__((aligned(64)));
u8  vram[1024 * 96] __attribute__((aligned(64)));
/* video.cc in ME_PRX_BUILD reads VRAM and keeps its sprite lists through
 * these pointers (ME_WORK_INDIRECT, GBAdhoc 4.x: the PSP module's work area
 * comes from its host).  Here they are simply this file's own memory: the
 * VRAM copy above and a sprite-list buffer of the size video.cc uses
 * (5 x 160 x 128 bytes); zeroed .bss, as the module's lists start. */
static u8 me_obj_lists[5 * 160 * 128] __attribute__((aligned(64)));
u8 *me_work_vram = vram;
u8 *me_work_vram_obj = vram + 0x10000;
u8 *me_work_obj = me_obj_lists;
u32 reg[64];
int sprite_limit;
u32 skip_next_frame;
