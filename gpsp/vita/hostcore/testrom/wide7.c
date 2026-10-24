/* wide7.c -- hostcore/testrom: a "Mode 7" floor for the wide picture's tests
 * (GPL-2.0-or-later, like the rest of gpsp/vita).
 *
 * What a racing game does to the screen, in a hundred lines: video mode 1,
 * background 2 rotated and scaled anew on every line (a table copied into
 * the registers by a DMA at each horizontal blank) so a flat map becomes a
 * floor to the horizon; a tile background above it for the sky, kept apart
 * by window 0; sprites at fixed places on the screen.
 *
 * gpsp/wide.c shows such a game past the console's 240 pixels.  The test
 * needs to know what a console WOULD show there, so the cartridge can show
 * it: while L is held everything on the screen is drawn 21 pixels further
 * right (the 21 columns left of the picture come into it), while R is held
 * 21 pixels further left.  The margins of a wide picture must be those
 * columns, pixel for pixel (app_tests.py, wide).
 *
 * A held for a mosaic of 3x3 (21 and 240 are multiples of 3, so the moved
 * picture has the same blocks), B for one of 4x4 (it does not: there the
 * renderer's own check is the test).
 *
 * The camera moves by the frame counter alone: frame N looks the same in
 * every run.
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed short   s16;
typedef signed int     s32;

#define REG16(a) (*(volatile u16 *)(0x04000000u + (a)))
#define REG32(a) (*(volatile u32 *)(0x04000000u + (a)))
#define VRAM8    ((volatile u8 *)0x06000000u)
#define VRAM16   ((volatile u16 *)0x06000000u)
#define PAL_BG   ((volatile u16 *)0x05000000u)
#define PAL_OBJ  ((volatile u16 *)0x05000200u)
#define OAM      ((volatile u16 *)0x07000000u)
#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

#define SHIFT_PX 21            /* gpsp/wide.c's WIDE_MARGIN_PX */
#define HORIZON  64
#define FOCAL    128
#define CAM_H    48

volatile u32 g_frames;         /* counted by crt0's handler */

static const s16 sin_lut[256] = {
      0,    6,   13,   19,   25,   31,   38,   44,   50,   56,   62,   68,
     74,   80,   86,   92,   98,  104,  109,  115,  121,  126,  132,  137,
    142,  147,  152,  157,  162,  167,  172,  177,  181,  185,  190,  194,
    198,  202,  206,  209,  213,  216,  220,  223,  226,  229,  231,  234,
    237,  239,  241,  243,  245,  247,  248,  250,  251,  252,  253,  254,
    255,  255,  256,  256,  256,  256,  256,  255,  255,  254,  253,  252,
    251,  250,  248,  247,  245,  243,  241,  239,  237,  234,  231,  229,
    226,  223,  220,  216,  213,  209,  206,  202,  198,  194,  190,  185,
    181,  177,  172,  167,  162,  157,  152,  147,  142,  137,  132,  126,
    121,  115,  109,  104,   98,   92,   86,   80,   74,   68,   62,   56,
     50,   44,   38,   31,   25,   19,   13,    6,    0,   -6,  -13,  -19,
    -25,  -31,  -38,  -44,  -50,  -56,  -62,  -68,  -74,  -80,  -86,  -92,
    -98, -104, -109, -115, -121, -126, -132, -137, -142, -147, -152, -157,
   -162, -167, -172, -177, -181, -185, -190, -194, -198, -202, -206, -209,
   -213, -216, -220, -223, -226, -229, -231, -234, -237, -239, -241, -243,
   -245, -247, -248, -250, -251, -252, -253, -254, -255, -255, -256, -256,
   -256, -256, -256, -255, -255, -254, -253, -252, -251, -250, -248, -247,
   -245, -243, -241, -239, -237, -234, -231, -229, -226, -223, -220, -216,
   -213, -209, -206, -202, -198, -194, -190, -185, -181, -177, -172, -167,
   -162, -157, -152, -147, -142, -137, -132, -126, -121, -115, -109, -104,
    -98,  -92,  -86,  -80,  -74,  -68,  -62,  -56,  -50,  -44,  -38,  -31,
    -25,  -19,  -13,   -6,
};
#define SIN(a) (sin_lut[(a) & 255])
#define COS(a) (sin_lut[((a) + 64) & 255])

/* One horizontal blank's worth of registers: BG2PA PB PC PD, BG2X, BG2Y. */
typedef struct { s16 pa, pb, pc, pd; s32 x, y; } affine_line;
static affine_line g_lines[161];

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");
}

static void tile8(int n, u8 a, u8 b, int kind)
{
   int x, y;
   for (y = 0; y < 8; y++)
      for (x = 0; x < 8; x++)
      {
         u8 c = a;
         if (kind == 1 && ((x ^ y) & 4)) c = b;         /* small checks  */
         if (kind == 2 && (x == 0 || y == 0)) c = b;    /* a grid line   */
         if (kind == 3 && x >= 3 && x <= 4) c = b;      /* a stripe      */
         /* 8 bits a pixel, written a halfword at a time (video memory
          * takes no byte writes) */
         if (x & 1)
            VRAM16[(n * 64 + y * 8 + x) / 2] =
               (u16)((VRAM16[(n * 64 + y * 8 + x) / 2] & 0x00FF) | (c << 8));
         else
            VRAM16[(n * 64 + y * 8 + x) / 2] = c;
      }
}

static void map8(int tx, int ty, u8 t)
{
   volatile u16 *p = &VRAM16[(0x8000 + ty * 64 + tx) / 2];
   if (tx & 1) *p = (u16)((*p & 0x00FF) | (t << 8));
   else        *p = (u16)((*p & 0xFF00) | t);
}

static void sprite(int n, int x, int y, int tile, int pal)
{
   OAM[n * 4 + 0] = (u16)(y & 0xFF);                    /* square, 4bpp   */
   OAM[n * 4 + 1] = (u16)((x & 0x1FF) | 0x4000);        /* 16x16          */
   OAM[n * 4 + 2] = (u16)(tile | (pal << 12));
}

int main(void)
{
   int i, x, y;

   /* ---- colours */
   PAL_BG[0] = RGB(12, 20, 31);                         /* sky            */
   PAL_BG[1] = RGB(6, 22, 8);   PAL_BG[2] = RGB(4, 16, 6);    /* grass   */
   PAL_BG[3] = RGB(13, 13, 14); PAL_BG[4] = RGB(31, 31, 31);  /* road    */
   PAL_BG[5] = RGB(28, 6, 4);   PAL_BG[6] = RGB(31, 27, 4);
   PAL_BG[7] = RGB(4, 8, 24);
   PAL_BG[16 + 1] = RGB(31, 31, 31);                    /* cloud          */
   PAL_BG[16 + 2] = RGB(22, 26, 31);
   PAL_OBJ[1] = RGB(31, 4, 4);  PAL_OBJ[2] = RGB(31, 31, 31);
   PAL_OBJ[16 + 1] = RGB(4, 28, 6);  PAL_OBJ[16 + 2] = RGB(0, 0, 0);
   PAL_OBJ[32 + 1] = RGB(30, 4, 30); PAL_OBJ[32 + 2] = RGB(31, 31, 0);

   /* ---- the floor: tiles in character block 0, 8 bits a pixel */
   tile8(0, 1, 2, 1);          /* grass                                    */
   tile8(1, 2, 1, 2);          /* darker grass with a grid                 */
   tile8(2, 3, 3, 0);          /* road                                     */
   tile8(3, 3, 4, 3);          /* road with the middle stripe              */
   tile8(4, 5, 4, 1);          /* red and white kerb                       */
   tile8(5, 6, 7, 1);          /* a yellow and blue marker                 */
   /* ...and its map, 64x64 tiles (512x512 pixels), at 0x8000: a ring road
    * with kerbs, markers on a grid, two shades of grass in 4x4 blocks */
   for (y = 0; y < 64; y++)
      for (x = 0; x < 64; x++)
      {
         int dx = x - 32, dy = y - 32, d2 = dx * dx + dy * dy;
         u8 t = (u8)(((x >> 2) ^ (y >> 2)) & 1);
         if (d2 >= 18 * 18 && d2 < 24 * 24)
            t = (d2 >= 20 * 20 && d2 < 22 * 22 && ((x + y) & 1)) ? 3 : 2;
         else if ((d2 >= 17 * 17 && d2 < 18 * 18) || (d2 >= 24 * 24 && d2 < 25 * 25))
            t = 4;
         else if ((x & 7) == 0 && (y & 7) == 0)
            t = 5;
         map8(x, y, t);
      }

   /* ---- the sky: 4-bit tiles in character block 1, map at 0xA000 */
   for (i = 0; i < 16; i++)
      VRAM16[(0x4000 + 32 + i * 2) / 2] =               /* tile 1: a cloud */
         (i >= 4 && i < 12) ? 0x1111 : (i >= 2 && i < 14) ? 0x2112 : 0;
   for (y = 0; y < 32; y++)
      for (x = 0; x < 32; x++)
         VRAM16[(0xA000 + (y * 32 + x) * 2) / 2] =
            (u16)(((x * 7 + y * 13) % 11 == 0 || (x % 9 == 2 && y % 3 == 1))
                  ? (1 | (1 << 12)) : 0);

   /* ---- sprites: 16x16, a filled square with a frame (tile 0..3) */
   for (i = 0; i < 4 * 32 / 2; i++)
   {
      int px = (i % 2) * 4, row = (i / 2) % 8, t = i / 16;
      int top = (t < 2 && row == 0), bot = (t >= 2 && row == 7);
      u16 v = 0x1111;
      if (top || bot) v = 0x2222;
      if (!(t & 1) && px == 0) v = (u16)((v & 0xFFF0) | 2);   /* left edge  */
      if ((t & 1) && px == 4) v = (u16)((v & 0x0FFF) | 0x2000); /* right    */
      VRAM16[(0x10000 + i * 2) / 2] = v;
   }
   for (i = 0; i < 128; i++)
      sprite(i, 0, 160, 0, 0);                          /* off the screen */

   /* (both with the mosaic bit: it does nothing until a size is set) */
   REG16(0x08) = (u16)(1 | (1 << 2) | 0x40 | (20 << 8)); /* BG0: the sky   */
   REG16(0x0C) = (u16)(0 | (0 << 2) | 0x40 | 0x80 | (16 << 8) | 0x2000 | (2 << 14));
                                           /* BG2: 256 colours, wraps, 512 */
   REG16(0x40) = (u16)((0 << 8) | 240);                  /* WIN0H 0..240   */
   REG16(0x44) = (u16)((0 << 8) | HORIZON);              /* WIN0V 0..64    */
   REG16(0x48) = 0x0011;                                 /* in: BG0 + OBJ  */
   REG16(0x4A) = 0x0014;                                 /* out: BG2 + OBJ */
   REG16(0x00) = (u16)(1 | 0x40 | 0x0100 | 0x0400 | 0x1000 | 0x2000);

   REG16(0x04) = 0x0008;                                 /* vblank IRQ     */
   REG16(0x200) = 0x0001;
   REG16(0x208) = 1;

   for (;;)
   {
      u32 f = g_frames, keys = (u32)~REG16(0x130);
      int shift = (keys & 0x200) ? SHIFT_PX : (keys & 0x100) ? -SHIFT_PX : 0;
      int a = (int)(f / 2);                              /* the heading    */
      s32 cs = COS(a), sn = SIN(a);
      s32 camx = (256 << 8) + SIN(f) * 96, camy = (256 << 8) + COS(f) * 96;

      for (y = 0; y < 160; y++)
      {
         affine_line *l = &g_lines[y];
         s32 lam = y >= HORIZON ? ((CAM_H << 8) / (y - HORIZON + 8)) : 256;
         l->pa = (s16)((lam * cs) >> 8);
         l->pc = (s16)((lam * sn) >> 8);
         l->pb = 0;
         l->pd = 0;
         l->x = camx + ((lam * (-120 * cs + FOCAL * sn)) >> 8) - shift * l->pa;
         l->y = camy + ((lam * (-120 * sn - FOCAL * cs)) >> 8) - shift * l->pc;
      }

      vblank_wait();

      /* Line 0 by hand; lines 1.. by the DMA, one entry a horizontal blank. */
      REG32(0xB0 + 8) = 0;                               /* DMA0 off       */
      REG16(0x20) = (u16)g_lines[0].pa; REG16(0x22) = 0;
      REG16(0x24) = (u16)g_lines[0].pc; REG16(0x26) = 0;
      REG32(0x28) = (u32)g_lines[0].x;  REG32(0x2C) = (u32)g_lines[0].y;
      REG32(0xB0) = (u32)&g_lines[1];
      REG32(0xB4) = 0x04000020u;
      REG32(0xB8) = 8u | (0x0060u << 16)                 /* dest: reload   */
                  | (0x0200u << 16)                      /* repeat         */
                  | (0x2000u << 16)                      /* at hblank      */
                  | (0x8000u << 16);
      REG16(0x10) = (u16)((a * 3 - shift) & 0x1FF);      /* the sky turns  */
      REG16(0x12) = 0;
      REG16(0x4C) = (keys & 1) ? 0x0022 : (keys & 2) ? 0x0033 : 0;

      /* Sprites: the kart; one across each edge; one past each edge (in a
       * margin only); they bob so every frame differs. */
      {
         int b = (int)((f >> 3) & 7);
         sprite(0, 112 + shift, 120, 0, 0);
         sprite(1, -8 + shift, 80 + b, 0, 1);
         sprite(2, 232 + shift, 100 - b, 0, 2);
         sprite(3, 246 + shift, 30 + b, 0, 1);
         sprite(4, -20 + shift, 130, 0, 2);
         sprite(5, 100 + shift, 20, 0, 2);
      }
   }
}
