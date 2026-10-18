/* topdown.c -- hostcore/testrom: a world seen from above, for the wide
 * picture's automatic mode (GPL-2.0-or-later, like the rest of gpsp/vita).
 *
 * The other kind of game with a camera (scroll.c is the side-scroller): an
 * adventure or role-playing game's map, which scrolls in all four
 * directions.
 *   - background 1 is the GROUND and background 2 what stands on it (tree
 *     tops, roofs): two layers of one world, always scrolled alike.  Each
 *     is a 256 x 256 pixel ring; the world is endless (a function of the
 *     world tile), and every frame the tiles on screen are written into
 *     the rings -- nothing past the edges;
 *   - background 0 is a TEXT PANEL over the bottom three rows.  It does not
 *     scroll with the world; START slides it away and back (its own
 *     vertical scroll: a panel that moves, but not as the camera does);
 *   - the D-pad moves the camera one pixel a frame (two with B); A jumps to
 *     another part of the world (a scene change).
 *
 * gpsp/wide.c's automatic mode has to work out, from watching that, what
 * lies beside the screen -- and when the picture moves up or down there is
 * nothing to watch: only its second machine, stepping aside and going
 * ahead, can show it.  So that a test knows the answer, the cartridge can
 * show it: while L is held the camera stands 21 pixels further left, while
 * R is held 21 further right, and the panel is put away (app_tests.py,
 * wide_top).
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
#define MAP_PANEL  (0xF800 / 2)
#define MAP_GROUND (0xF000 / 2)
#define MAP_OVER   (0xE800 / 2)

volatile u32 g_frames;         /* counted by crt0's handler */

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");
}

static u32 hash(u32 x, u32 y)
{
   u32 h = x * 0x9E3779B1u ^ (y * 0x85EBCA6Bu + 0x7F4A7C15u);
   h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
   return h;
}

/* The ground at world tile (wx, wy): paths every 16 tiles each way, ponds,
 * and grass of four kinds in between -- no two screens of it alike. */
static u16 ground(u32 wx, u32 wy)
{
   u32 h = hash(wx, wy) & 0xFF, big = hash(wx >> 3, wy >> 3) & 0xFF;
   if ((wx & 15) == 3 || (wy & 15) == 9)
      return (u16)(5 + (h & 1));             /* a path                   */
   if (big < 28)
      return (u16)(7 | (1 << 12));           /* water                    */
   return (u16)(1 + (h & 3));                /* grass                    */
}

/* What stands on it: a tree top here and there, never on a path. */
static u16 over(u32 wx, u32 wy)
{
   u32 h = hash(wx + 7777u, wy + 31u) & 0xFF;
   if ((wx & 15) == 3 || (wy & 15) == 9)
      return 0;
   return (u16)(h < 26 ? (8 + (h & 1)) | (2 << 12) : 0);
}

static void tile(int n, u32 fill, u32 other, int kind)
{
   int y;
   for (y = 0; y < 8; y++)
   {
      u32 a = fill * 0x1111u, b = fill * 0x1111u;
      if (kind == 1 && (y == 2 || y == 5)) a = (a & 0xF0FFu) | (other << 8);
      if (kind == 2 && (y == 1 || y == 6)) b = (b & 0xFF0Fu) | (other << 4);
      if (kind == 3 && y == 3) a = b = (fill * 0x1010u) | (other * 0x0101u);
      if (kind == 4 && (y & 1)) a = b = (fill * 0x1100u) | (other * 0x0011u);
      if (kind == 5 && (y == 0 || y == 7)) a = b = other * 0x1111u;
      if (kind == 6 && y >= 1 && y < 7)                      /* a round top */
         a = (other * 0x1110u) | 0, b = (other * 0x0111u);
      if (kind == 6 && (y < 1 || y >= 7)) a = b = 0;
      VRAM16[n * 16 + y * 2] = (u16)a;
      VRAM16[n * 16 + y * 2 + 1] = (u16)b;
   }
}

int main(void)
{
   s32 cam_x = 40000, cam_y = 20000;
   s32 ox0 = 0, ox1 = -1, oy0 = 0, oy1 = -1;   /* the tiles in the rings */
   int jumped = 0, panel = 0, panel_to = 0, started = 0;
   int x, y;

   PAL_BG[0] = RGB(6, 10, 6);
   PAL_BG[1] = RGB(8, 20, 8);   PAL_BG[2] = RGB(12, 24, 10);
   PAL_BG[3] = RGB(5, 15, 6);   PAL_BG[4] = RGB(24, 21, 12);
   PAL_BG[5] = RGB(28, 25, 16); PAL_BG[6] = RGB(18, 15, 8);
   PAL_BG[16 + 1] = RGB(8, 14, 28); PAL_BG[16 + 2] = RGB(16, 22, 31);
   PAL_BG[32 + 1] = RGB(3, 12, 4);  PAL_BG[32 + 2] = RGB(6, 18, 6);
   PAL_BG[48 + 1] = RGB(30, 30, 31); PAL_BG[48 + 2] = RGB(10, 10, 20);
   PAL_OBJ[1] = RGB(31, 6, 20); PAL_OBJ[2] = RGB(31, 31, 31);

   tile(1, 1, 2, 1);           /* grass, four kinds                        */
   tile(2, 1, 3, 2);
   tile(3, 2, 1, 3);
   tile(4, 1, 2, 4);
   tile(5, 4, 5, 1);           /* path                                     */
   tile(6, 5, 6, 2);
   tile(7, 1, 2, 3);           /* water (palette 1)                        */
   tile(8, 1, 2, 6);           /* tree tops (palette 2)                    */
   tile(9, 2, 1, 6);
   tile(10, 1, 2, 5);          /* the panel (palette 3)                    */

   for (y = 0; y < 32; y++)
      for (x = 0; x < 32; x++)
         VRAM16[MAP_PANEL + y * 32 + x] =
            (u16)(y >= 17 && y < 20 && x < 30 ? (10 | (3 << 12)) : 0);

   /* a sprite in the middle: the player */
   for (x = 0; x < 16; x++)
      VRAM16[0x10000 / 2 + x] = (u16)((x >= 2 && x < 14) ? 0x1221 : 0x1111);
   for (x = 0; x < 128; x++)
      OAM[x * 4] = 160;
   OAM[0] = 76; OAM[1] = 116; OAM[2] = 0;

   REG16(0x08) = (u16)(0 | (31 << 8));                   /* BG0 the panel  */
   REG16(0x0A) = (u16)(2 | (30 << 8));                   /* BG1 the ground */
   REG16(0x0C) = (u16)(1 | (29 << 8));                   /* BG2 over it    */
   REG16(0x04) = 0x0008;
   REG16(0x200) = 0x0001;
   REG16(0x208) = 1;

   for (;;)
   {
      u32 keys = (u32)~REG16(0x130);
      int shift = (keys & 0x200) ? -SHIFT_PX : (keys & 0x100) ? SHIFT_PX : 0;
      int step = (keys & 2) ? 2 : 1;
      s32 cx, tx, ty, tx0, tx1, ty0, ty1;

      if ((keys & 1) && !jumped) { cam_x += 8000; cam_y += 4000; jumped = 1; }
      if (!(keys & 1)) jumped = 0;
      if ((keys & 8) && !started) { panel_to = panel_to ? 0 : 24; started = 1; }
      if (!(keys & 8)) started = 0;
      if (panel < panel_to) panel += 2;
      if (panel > panel_to) panel -= 2;
      if (keys & 0x10) cam_x += step;
      if (keys & 0x20) cam_x -= step;
      if (keys & 0x80) cam_y += step;
      if (keys & 0x40) cam_y -= step;
      cx = cam_x + shift;

      /* the tiles on screen at the new place that were not on it at the
       * old one (31 x 21 of the rings' 32 x 32 at most, so none of them is
       * one still showing there): a row or a column as a rule, everything
       * after a jump */
      tx0 = cx >> 3; tx1 = (cx + 239) >> 3;
      ty0 = cam_y >> 3; ty1 = (cam_y + 159) >> 3;
      for (ty = ty0; ty <= ty1; ty++)
         for (tx = tx0; tx <= tx1; tx++)
         {
            u32 at = (u32)(ty & 31) * 32 + (u32)(tx & 31);
            if (tx >= ox0 && tx <= ox1 && ty >= oy0 && ty <= oy1)
               continue;
            VRAM16[MAP_GROUND + at] = ground((u32)tx, (u32)ty);
            VRAM16[MAP_OVER + at] = over((u32)tx, (u32)ty);
         }
      ox0 = tx0; ox1 = tx1; oy0 = ty0; oy1 = ty1;

      vblank_wait();
      REG16(0x14) = (u16)(cx & 0x1FF); REG16(0x16) = (u16)(cam_y & 0x1FF);
      REG16(0x18) = (u16)(cx & 0x1FF); REG16(0x1A) = (u16)(cam_y & 0x1FF);
      REG16(0x12) = (u16)((0 - panel) & 0x1FF);          /* the panel's own */
      REG16(0x00) = (u16)(0x0600 | 0x1000 | 0x40 | (shift ? 0 : 0x0100));
   }
}
