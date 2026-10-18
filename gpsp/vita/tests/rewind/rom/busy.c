/* busy.c -- tests/rewind: a GBA program that behaves, to a rewind buffer,
 * like a game in the middle of the action (GPL-2.0-or-later, like the rest
 * of gpsp/vita).
 *
 * A rewind buffer stores what CHANGED between two moments, so what matters
 * about a game is how much of its memory it rewrites per frame and where.
 * hostcore/testrom's cartridge repaints a few boxes: a quiet game.  This one
 * does what an action game's engine does every frame:
 *
 *   - NOISE bytes of work RAM (EWRAM) rewritten in 64-byte pieces at
 *     places that wander over 128 KiB: decompression buffers, the heap
 *   - 128 "objects" of 16 bytes in IWRAM, each stepped (position, velocity,
 *     animation): 2 KiB touched, most of it changing
 *   - all 128 sprite entries of OAM rewritten from them
 *   - one column of a 32x32 tile map and one tile's pixels rewritten
 *   - a scroll register and two palette entries changed
 *
 * NOISE is set at build time: 2048 for "busy" (rwbusy.gba), 16384 for
 * "heavy" (rwheavy.gba), a game that would be remarkable.  The frame
 * counter is on screen in binary (mode 3 would be simpler to draw, but a
 * bitmap mode is not what such a game runs in: this is mode 0 with tiles),
 * and A writes the battery like the other cartridge, so one ROM serves the
 * measurements and the checks.
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#ifndef NOISE
#define NOISE 2048
#endif

#define REG16(a) (*(volatile u16 *)(0x04000000u + (a)))
#define PAL      ((volatile u16 *)0x05000000u)
#define VRAM16   ((volatile u16 *)0x06000000u)
#define OAM      ((volatile u16 *)0x07000000u)
#define SRAM     ((volatile u8 *)0x0E000000u)
#define EWRAM    ((volatile u8 *)0x02000000u)
#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))
/* background 0: tiles at 0x06000000, its map at 0x06008000 (screen block 16) */
#define MAP      (VRAM16 + 0x8000 / 2)

volatile u32 g_frames;                       /* counted by crt0's handler */
const char backup_id[] = "SRAM_V113";

typedef struct { short x, y, vx, vy; u16 tile, anim; u32 seed; } obj;
static obj objs[128];
static u32 rng = 0x2545F491u;

static u32 rnd(void)
{
   rng ^= rng << 13;
   rng ^= rng >> 17;
   rng ^= rng << 5;
   return rng;
}

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");
}

static void tile_fill(int t, u32 nibble)
{
   int i;
   u16 v = (u16)(nibble * 0x1111u);
   for (i = 0; i < 16; i++)
      VRAM16[t * 16 + i] = v;
}

int main(void)
{
   u32 presses = 0, last_keys = 0;
   int i;

   REG16(0x00) = 0x1100;                     /* mode 0, BG0, sprites */
   REG16(0x08) = 0x1000;                     /* BG0: map at block 16 */
   PAL[0] = RGB(2, 2, 6);
   PAL[1] = RGB(31, 31, 31);
   PAL[2] = RGB(6, 6, 6);
   PAL[3] = RGB(31, 8, 8);
   PAL[256 + 1] = RGB(8, 31, 8);
   tile_fill(0, 0);
   tile_fill(1, 1);
   tile_fill(2, 2);
   tile_fill(3, 3);
   for (i = 0; i < 16; i++)                  /* sprite tile 1 */
      VRAM16[0x10000 / 2 + 16 + i] = 0x1111;
   for (i = 0; i < 32 * 32; i++)
      MAP[i] = 0;
   for (i = 0; i < 128; i++)
   {
      objs[i].x = (short)(rnd() % 240);
      objs[i].y = (short)(rnd() % 160);
      objs[i].vx = (short)(rnd() % 5) - 2;
      objs[i].vy = (short)(rnd() % 5) - 2;
      objs[i].seed = rnd();
   }
   REG16(0x04) = 0x0008;                     /* vblank IRQ */
   REG16(0x200) = 0x0001;
   REG16(0x208) = 1;

   for (;;)
   {
      u32 f, keys, n;

      vblank_wait();
      f = g_frames;
      keys = ~REG16(0x130) & 0x3FF;

      /* the objects, and the sprites drawn from them */
      for (i = 0; i < 128; i++)
      {
         obj *o = &objs[i];
         o->x = (short)(o->x + o->vx);
         o->y = (short)(o->y + o->vy);
         if (o->x < 0 || o->x > 232) o->vx = (short)-o->vx;
         if (o->y < 0 || o->y > 152) o->vy = (short)-o->vy;
         o->anim++;
         o->seed = o->seed * 1664525u + 1013904223u;
         OAM[i * 4 + 0] = (u16)(o->y & 0xFF);
         OAM[i * 4 + 1] = (u16)(o->x & 0x1FF);
         OAM[i * 4 + 2] = 1;
      }

      /* work RAM: NOISE bytes in 64-byte pieces, wandering over 128 KiB */
      for (n = 0; n < NOISE / 64; n++)
      {
         u32 at = (rnd() & 0x1FFC0u), v = rnd();
         volatile u32 *p = (volatile u32 *)(0x02000000u + at);
         for (i = 0; i < 16; i++)
         {
            p[i] = v;
            v = v * 1103515245u + 12345u;
         }
      }

      /* the map: rows 12.. scroll past, one column rewritten a frame */
      for (i = 12; i < 32; i++)
         MAP[i * 32 + (f & 31)] = (u16)(2 + ((f + (u32)i) & 1));
      REG16(0x10) = (u16)(f & 0xFF);         /* BG0 horizontal scroll... */
      /* ...which must not move the top rows: there is no window for that
       * in a test this small, so the counter is drawn at the scrolled
       * position instead (it then stands still on screen) */
      PAL[2] = RGB(6 + (f & 7), 6, 6);
      VRAM16[3 * 16 + (f & 15)] = (u16)(0x3333u ^ (f & 0x0F0Fu));

      /* the frame counter in binary, rows 2-3; the battery's first byte in
       * rows 5-6; the press count in rows 8-9 */
      for (i = 0; i < 16; i++)
      {
         u16 t = (u16)((f >> (15 - i)) & 1);
         u32 col = ((f >> 3) + 2 + (u32)i) & 31;
         MAP[2 * 32 + col] = t;
         MAP[2 * 32 + ((col + 16) & 31)] = 0;   /* the cell that scrolled off */
      }
      if ((keys & 1) && !(last_keys & 1))
      {
         presses++;
         for (i = 0; i < 64; i++)
            SRAM[i] = (u8)(i * 7 + presses * 16);
      }
      for (i = 0; i < 8; i++)
      {
         u32 col = ((f >> 3) + 2 + (u32)i) & 31;
         MAP[5 * 32 + col] = (u16)((SRAM[1] >> (7 - i)) & 1);
         MAP[5 * 32 + ((col + 8) & 31)] = 0;
         MAP[8 * 32 + col] = (u16)((presses >> (7 - i)) & 1);
         MAP[8 * 32 + ((col + 8) & 31)] = 0;
      }
      last_keys = keys;
   }
   return 0;
}
