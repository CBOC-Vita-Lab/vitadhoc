/* audio.c -- hostcore/testrom: known sounds for tools/audio_measure.py
 * (GPL-2.0-or-later, like the rest of gpsp/vita).  Each phase lasts two
 * seconds (120 refreshes) after one silent second:
 *
 *   1  Direct Sound A, 100 %, a 1024 Hz sine of amplitude 100 (16384 Hz FIFO)
 *   2  Direct Sound A,  50 %, the same sine
 *   3  square 1, 440 Hz, full volume
 *   4  square 1, 3000 Hz (aliasing)
 *   5  Direct Sound A and B, 100 %, the same sine in both (the sum clips)
 *   6  Direct Sound A, 100 %, 1024 Hz sine fed at 32768 Hz
 *   7  Direct Sound A + square 1 (440 Hz) + noise together
 *   8  Direct Sound A 100 %, sine at 1024 Hz fed at 16384 Hz, amplitude 127
 * then silence.
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed char    s8;

#define REG16(a) (*(volatile u16 *)(0x04000000u + (a)))
#define REG32(a) (*(volatile u32 *)(0x04000000u + (a)))
#define EWRAM    ((s8 *)0x02000000u)

volatile u32 g_frames;
static const s8 sine16[16] = { 0, 38, 71, 92, 100, 92, 71, 38, 0, -38, -71, -92, -100, -92, -71, -38 };
static const s8 sine32[32] = { 0, 20, 38, 56, 71, 83, 92, 98, 100, 98, 92, 83, 71, 56, 38, 20, 0, -20, -38, -56, -71, -83, -92, -98, -100, -98, -92, -83, -71, -56, -38, -20 };

#define REGION 0x12000                       /* 73728 bytes: > 2 s at 32768 */

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");
}

static void dma_stop(void)
{
   REG16(0xC6) = 0;
   REG16(0xD2) = 0;
}

static void dma_a(u32 region, int fifo_b)
{
   if (!fifo_b)
   {
      REG32(0xBC) = 0x02000000u + region;
      REG32(0xC0) = 0x040000A0u;
      REG16(0xC6) = 0xB640;
   }
   else
   {
      REG32(0xC8) = 0x02000000u + region;
      REG32(0xCC) = 0x040000A4u;
      REG16(0xD2) = 0xB640;
   }
}

/* soundcnt_h: PSG 100 %; DS A volume (bit 2), B volume (bit 3), both to both
 * sides on timer 0, FIFOs reset */
#define H_A100   (0x0002 | 0x0004 | 0x0300 | 0x0800)
#define H_A50    (0x0002 | 0x0300 | 0x0800)
#define H_AB100  (H_A100 | 0x0008 | 0x3000 | 0x8000)

static void phase(int p)
{
   dma_stop();
   REG16(0x102) = 0;
   REG16(0x62) = 0;                          /* square 1 off */
   REG16(0x78) = 0;
   switch (p)
   {
   case 1: case 8:
      REG16(0x82) = H_A100; REG16(0x100) = 0xFC00; REG16(0x102) = 0x0080;
      dma_a(0, 0); break;
   case 2:
      REG16(0x82) = H_A50; REG16(0x100) = 0xFC00; REG16(0x102) = 0x0080;
      dma_a(0, 0); break;
   case 3:
      REG16(0x82) = 0x0002; REG16(0x62) = 0xF080; REG16(0x64) = 0x8000 | 1750;
      break;
   case 4:
      REG16(0x82) = 0x0002; REG16(0x62) = 0xF080; REG16(0x64) = 0x8000 | 2004;
      break;
   case 5:
      REG16(0x82) = H_AB100; REG16(0x100) = 0xFC00; REG16(0x102) = 0x0080;
      dma_a(0, 0); dma_a(0, 1); break;
   case 6:
      REG16(0x82) = H_A100; REG16(0x100) = 0xFE00; REG16(0x102) = 0x0080;
      dma_a(REGION, 0); break;
   case 7:
      REG16(0x82) = H_A100; REG16(0x100) = 0xFC00; REG16(0x102) = 0x0080;
      dma_a(0, 0);
      REG16(0x62) = 0xA080; REG16(0x64) = 0x8000 | 1750;
      REG16(0x78) = 0xA000; REG16(0x7C) = 0x8035;
      break;
   default:
      REG16(0x82) = 0x0002; break;
   }
}

int main(void)
{
   int i, cur = -1;
   REG16(0x04) = 0x0008;
   REG16(0x200) = 0x0001;
   REG16(0x208) = 1;
   REG16(0x84) = 0x0080;
   REG16(0x80) = 0x9977;                     /* square 1 + noise, both sides, vol 7 */
   REG16(0x88) = 0x0200;
   for (i = 0; i < REGION; i++)
      EWRAM[i] = sine16[i & 15];
   for (i = 0; i < REGION; i++)
      EWRAM[REGION + i] = sine32[i & 31];
   for (;;)
   {
      u32 f;
      int p;
      vblank_wait();
      f = g_frames;
      p = f < 60 ? 0 : (int)((f - 60) / 120) + 1;
      if (p > 9) p = 9;
      if (p != cur) { cur = p; phase(p); }
   }
}
