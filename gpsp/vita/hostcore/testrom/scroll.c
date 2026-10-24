/* scroll.c -- hostcore/testrom: a side-scroller for the wide picture's
 * automatic mode (GPL-2.0-or-later, like the rest of gpsp/vita).
 *
 * What most GBA games with a camera do, in its barest form:
 *   - background 1 is the LEVEL: a 256 pixel ring of tiles.  The level
 *     itself is endless (a function of the world column); every frame the
 *     columns on screen, and one beyond the edge the camera is moving to,
 *     are written into the ring.  The rest of the ring keeps whatever was
 *     there -- on a console that is never seen;
 *   - background 2 is a far SKY that scrolls at half speed and repeats: its
 *     map is written once;
 *   - background 0 is a STATUS BAR that never scrolls;
 *   - LEFT / RIGHT move the camera two pixels a frame; A jumps it to
 *     another part of the level (a scene change).
 *
 * gpsp/wide.c's automatic mode has to work out, from watching that, what
 * lies beside the screen.  So that a test knows the answer, the cartridge
 * can show it: while L is held the camera stands 21 pixels further left
 * (the 21 columns left of the picture come into it), while R is held 21
 * further right, and the status bar is put away (app_tests.py, wide_auto).
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed int     s32;

#define REG16(a) (*(volatile u16 *)(0x04000000u + (a)))
#define VRAM16   ((volatile u16 *)0x06000000u)
#define PAL_BG   ((volatile u16 *)0x05000000u)
#define PAL_OBJ  ((volatile u16 *)0x05000200u)
#define OAM      ((volatile u16 *)0x07000000u)
#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

#define SHIFT_PX 21            /* gpsp/wide.c's WIDE_MARGIN_PX */
#define MAP_HUD   (0xF800 / 2)
#define MAP_LEVEL (0xF000 / 2)
#define MAP_SKY   (0xE800 / 2)

volatile u32 g_frames;         /* counted by crt0's handler */

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");
}

/* The level: what tile world column wx has in row wy (0..20).  Ground with
 * a top edge, pillars, floating blocks, and a numbered post every 16
 * columns -- no two screens of it alike. */
static u16 level(u32 wx, u32 wy)
{
   u32 h = (wx * 2654435761u) >> 24;         /* 0..255, by column        */
   u32 top = 14 + ((wx / 5) * 40503u >> 13 & 3);   /* the ground's height */
   if (wy >= top)
      return (u16)(wy == top ? 2 : 1 + ((wx ^ wy) & 1) * 2);
   if ((wx & 15) == 0 && wy >= top - 4)
      return (u16)(4 | (1 << 12));           /* a post                   */
   if ((wx & 15) == 0 && wy == top - 5)
      return (u16)((5 + ((wx >> 4) & 3)) | (1 << 12));   /* its sign     */
   if (h < 40 && wy == 6 + (h & 3))
      return 9;                              /* a floating block         */
   if (h > 235 && wy > top - 3 - (h & 1))
      return (u16)(4 | (2 << 12));           /* a pillar                 */
   return 0;
}

static void tile(int n, u32 fill, u32 other, int kind)
{
   int y;
   for (y = 0; y < 8; y++)
   {
      u32 a = fill * 0x1111u, b = fill * 0x1111u;
      if (kind == 1 && y == 0) a = b = other * 0x1111u;            /* top */
      if (kind == 2 && (y & 3) == 3) a = b = other * 0x1111u;      /* bars */
      if (kind == 3) { a = (a & 0xFFF0u) | other; b = (b & 0x0FFFu) | (other << 12); }
      if (kind >= 4 && y >= 2 && y < 6)                            /* a mark */
         a = (a & 0xFF00u) | ((kind - 3) & 1 ? other * 0x11u : 0),
         b = (b & 0x00FFu) | ((kind - 3) & 2 ? other * 0x1100u : 0);
      VRAM16[n * 16 + y * 2] = (u16)a;
      VRAM16[n * 16 + y * 2 + 1] = (u16)b;
   }
}

int main(void)
{
   s32 cam = 100000, jumped = 0;
   int x, y;

   PAL_BG[0] = RGB(10, 16, 28);                         /* far sky        */
   PAL_BG[1] = RGB(12, 8, 4);  PAL_BG[2] = RGB(6, 22, 6);    /* earth, grass */
   PAL_BG[3] = RGB(16, 11, 6); PAL_BG[4] = RGB(22, 22, 24);
   PAL_BG[5] = RGB(31, 31, 31); PAL_BG[6] = RGB(31, 20, 4);
   PAL_BG[16 + 4] = RGB(26, 6, 6); PAL_BG[16 + 5] = RGB(31, 31, 10);
   PAL_BG[16 + 6] = RGB(4, 4, 6);
   PAL_BG[32 + 4] = RGB(8, 12, 26); PAL_BG[32 + 5] = RGB(20, 26, 31);
   PAL_BG[48 + 1] = RGB(20, 24, 31); PAL_BG[48 + 2] = RGB(14, 20, 30);
   PAL_BG[64 + 1] = RGB(2, 2, 4); PAL_BG[64 + 2] = RGB(31, 31, 31);
   PAL_OBJ[1] = RGB(31, 6, 20); PAL_OBJ[2] = RGB(31, 31, 31);

   tile(1, 1, 3, 3);           /* earth                                    */
   tile(2, 1, 2, 1);           /* earth with grass on top                  */
   tile(3, 3, 1, 3);           /* lighter earth                            */
   tile(4, 4, 5, 2);           /* a post / pillar (palette 1 / 2)          */
   tile(5, 5, 6, 4); tile(6, 5, 6, 5); tile(7, 5, 6, 6); tile(8, 5, 6, 7);
   tile(9, 6, 5, 1);           /* a block                                  */
   tile(10, 1, 2, 2);          /* cloud (palette 3)                        */
   tile(11, 1, 2, 3);          /* the status bar (palette 4)               */

   /* the sky, once: it repeats every 256 pixels */
   for (y = 0; y < 32; y++)
      for (x = 0; x < 32; x++)
         VRAM16[MAP_SKY + y * 32 + x] =
            (u16)((y < 12 && ((x * 5 + y * 11) % 9) < 2) ? (10 | (3 << 12)) : 0);
   /* the status bar: the top row of the screen, and nothing else */
   for (x = 0; x < 30; x++)
      VRAM16[MAP_HUD + x] = (u16)(11 | (4 << 12));

   /* a sprite in the middle: the player */
   for (x = 0; x < 16; x++)
      VRAM16[0x10000 / 2 + x] = (u16)((x >= 2 && x < 14) ? 0x1221 : 0x1111);
   for (x = 0; x < 128; x++)
      OAM[x * 4] = 160;
   OAM[0] = 104; OAM[1] = 116; OAM[2] = 0;

   REG16(0x08) = (u16)(0 | (31 << 8));                   /* BG0 the bar    */
   REG16(0x0A) = (u16)(1 | (30 << 8));                   /* BG1 the level  */
   REG16(0x0C) = (u16)(2 | (29 << 8));                   /* BG2 the sky    */
   REG16(0x04) = 0x0008;
   REG16(0x200) = 0x0001;
   REG16(0x208) = 1;

   for (;;)
   {
      u32 keys = (u32)~REG16(0x130);
      int shift = (keys & 0x200) ? -SHIFT_PX : (keys & 0x100) ? SHIFT_PX : 0;
      int dir = (keys & 0x10) ? 1 : (keys & 0x20) ? -1 : 0;
      s32 c, wx;

      if ((keys & 1) && !jumped) { cam += 8000; jumped = 1; }
      if (!(keys & 1)) jumped = 0;
      cam += dir * 2;
      c = cam + shift;

      /* the columns on screen, and one past the edge ahead -- when the
       * ring has room for it: 31 columns show at most positions, and a
       * 32nd written then would be the one still on screen at the other
       * edge until the scroll registers follow at vertical blank */
      {
         s32 first = c >> 3, last = (c + 239) >> 3;
         if (last - first < 30)
         {
            if (dir > 0) last++;
            if (dir < 0) first--;
         }
         for (wx = first; wx <= last; wx++)
            for (y = 0; y < 21; y++)
               VRAM16[MAP_LEVEL + y * 32 + (wx & 31)] = level((u32)wx, (u32)y);
      }

      vblank_wait();
      REG16(0x14) = (u16)(c & 0x1FF);  REG16(0x16) = 0;      /* the level  */
      /* (the sky moves at half the camera's speed; "21 pixels further
       * left" is the same place on the same screen, so it shifts whole) */
      REG16(0x18) = (u16)(((cam >> 1) + shift) & 0x1FF); REG16(0x1A) = 0;
      REG16(0x00) = (u16)(0x0600 | 0x1000 | 0x40 | (shift ? 0 : 0x0100));
   }
}
