/* biossound.c -- hostcore/testrom: a cartridge that leaves its PCM mixing to
 * the BIOS sound driver (SWI 1Ah SoundDriverInit, 1Bh SoundDriverMode, 1Ch
 * SoundDriverMain, 1Dh SoundDriverVSync), the way a game that keeps its
 * sequencer in the cartridge does.  It starts one looping sine voice by
 * writing the driver's SoundArea itself (app_tests.py, t_biossound), then
 * calls the driver once per frame.  With a driver that answers those calls
 * with a bare return the sound hardware is never switched on and the output
 * is silent; with the open BIOS's sound driver it is a steady tone.
 * GPL-2.0-or-later, like the rest of gpsp/vita. */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed char    s8;
typedef signed int     s32;

volatile u32 g_frames;                      /* crt0.S counts vertical blanks here */

/* The wave: header (type, status with the loop bit, rate * 1024, loop start,
 * size) then 64 signed 8-bit samples of a sine of amplitude 100. */
typedef struct { u16 type, status; u32 freq, loop_start, size; s8 data[65]; } Wave;
static const Wave wave = { 0, 0x4000, 13379 * 1024, 0, 64, {
    0,  10,  20,  29,  38,  47,  55,  63,  71,  77,  83,  89,  92,  96,  98,  99,
  100,  99,  98,  96,  92,  89,  83,  77,  71,  63,  55,  47,  38,  29,  20,  10,
    0, -10, -20, -29, -38, -47, -55, -63, -71, -77, -83, -89, -92, -96, -98, -99,
 -100, -99, -98, -96, -92, -89, -83, -77, -71, -63, -55, -47, -38, -29, -20, -10, 0 } };

/* SoundArea (0xFB0 bytes); the voice fields used here are those of channel 0
 * at 0x50: status, type, right, left, attack, decay, sustain, release ...
 * count 0x68, frequency 0x70, wave 0x74. */
static u32 area[0xFB0 / 4] __attribute__((aligned(4)));

#define SWI(n, a) __asm__ volatile("mov r0, %0\n swi " #n : : "r"(a) : "r0", "r1", "r2", "r3", "memory")
#define SWI0(n)   __asm__ volatile("swi " #n : : : "r0", "r1", "r2", "r3", "memory")

int main(void)
{
   volatile u8 *ch = (volatile u8 *)area + 0x50;
   *(volatile u16 *)0x04000004 = 0x0008;     /* vblank IRQ on */
   *(volatile u16 *)0x04000200 = 0x0001;
   *(volatile u16 *)0x04000208 = 1;
   SWI(0x1A, area);                           /* SoundDriverInit */
   SWI(0x1B, 0x0094F800u);                    /* 8 voices, volume 15, 13379 Hz */
   ch[1] = 0;                                 /* type: variable rate */
   ch[2] = 255; ch[3] = 255;                  /* right, left volume */
   ch[4] = 255; ch[5] = 0; ch[6] = 255; ch[7] = 0;   /* attack .. release */
   *(u32 *)(ch + 0x20) = 13379;               /* output rate: one input sample per output */
   *(const Wave **)(ch + 0x24) = &wave;
   ch[0] = 0x80;                              /* start */
   for (;;)
   {
      __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");   /* VBlankIntrWait */
      SWI0(0x1D);                             /* SoundDriverVSync */
      SWI0(0x1C);                             /* SoundDriverMain */
   }
}
