/* objrand.c -- a test cartridge for the hardware's sprite priority rules,
 * checked pixel by pixel (app_tests.py t_objrand).  Four regions of the
 * picture (120 x 80 pixels each) hold 16 random sprites each, 8x8 or 16x16,
 * four tile patterns with transparent holes, every priority 0..3, in front of
 * three backgrounds of priority 2, 1 and 0 that have holes of their own.  The
 * test rebuilds the same sprites with the same generator and works out every
 * pixel from the rules (among sprites a later one takes a pixel only with a
 * strictly better priority; a transparent pixel of such a sprite gives the
 * priority to the sprite already there; a sprite beats a background of the
 * same priority), then compares it with the twin's picture.  Every third
 * sprite is an affine one with the identity matrix.  All data is made here. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

#define REG(a) (*(vu16 *)(0x04000000 + (a)))
volatile u32 g_frames;              /* crt0.S counts vertical blanks here */
#define OAM ((vu16 *)0x07000000)

static u32 seed = 12345;
static u32 rnd(void)
{
   seed = seed * 1103515245u + 12345u;
   return (seed >> 16) & 0x7FFF;
}

/* tile n of pattern p: pixel (x, y) is opaque unless (x + 2 y + n + p) % 3 == 0 */
static u32 sprite_row(int p, int n, int y)
{
   u32 v = 0;
   for (int x = 0; x < 8; x++)
      if (((x + 2 * y + n + p) % 3) != 0)
         v |= 1u << (4 * x);
   return v;
}

/* backgrounds: layer k's tile: opaque unless (x * (k + 2) + y * 3 + k) % 4 == 0 */
static u32 bg_row(int k, int y)
{
   u32 v = 0;
   for (int x = 0; x < 8; x++)
      if (((x * (k + 2) + y * 3 + k) % 4) != 0)
         v |= 1u << (4 * x);
   return v;
}

int main(void)
{
   int i, q, p, n, y;
   vu32 *t;
   vu16 *m;

   REG(0x000) = 0x0080;
   ((vu16 *)0x05000000)[0] = 0x0000;                    /* backdrop black */
   for (i = 0; i < 3; i++)                              /* BG banks 0..2 */
      ((vu16 *)0x05000000)[16 * i + 1] = i == 0 ? 0x7C00 : i == 1 ? 0x03E0 : 0x001F;
   for (i = 0; i < 16; i++)                             /* OBJ banks */
      ((vu16 *)0x05000200)[16 * i + 1] =
         (u16)((((i & 3) * 8 + 4)) | ((((i >> 2) & 3) * 8 + 4) << 5) | (16 << 10));

   t = (vu32 *)0x06000000;                              /* BG tiles 1..3 */
   for (i = 0; i < 3; i++)
      for (y = 0; y < 8; y++)
         t[8 * (i + 1) + y] = bg_row(i, y);
   for (i = 0; i < 3; i++) {                            /* BG i: map block 8+i */
      m = (vu16 *)(0x06004000 + 0x800 * i);
      for (n = 0; n < 32 * 32; n++)
         m[n] = (u16)((i + 1) | (i << 12));
   }
   REG(0x008) = 0x0802 | 0;                             /* BG0 prio 2, block 8 */
   REG(0x00A) = 0x0901;                                 /* BG1 prio 1, block 9 */
   REG(0x00C) = 0x0A00;                                 /* BG2 prio 0, block 10 */

   t = (vu32 *)0x06010000;                              /* OBJ tiles, 1D */
   for (p = 0; p < 4; p++)
      for (n = 0; n < 4; n++)
         for (y = 0; y < 8; y++)
            t[8 * (4 * p + n) + y] = sprite_row(p, n, y);

   for (i = 0; i < 128; i++) {
      OAM[i * 4 + 0] = 0x0200;
      OAM[i * 4 + 1] = 0;
      OAM[i * 4 + 2] = 0;
   }
   OAM[0 * 4 + 3] = 0x0100;  OAM[1 * 4 + 3] = 0;       /* affine group 0: identity */
   OAM[2 * 4 + 3] = 0;       OAM[3 * 4 + 3] = 0x0100;

   for (q = 0; q < 4; q++)
      for (i = 0; i < 16; i++) {
         int idx = 4 + q * 16 + i;                      /* OAM 0..3 hold the matrix */
         int big = rnd() & 1;
         int x = (q & 1) * 120 + (int)(rnd() % 96);
         int yy = (q >> 1) * 80 + (int)(rnd() % 56);
         int prio = rnd() & 3;
         int pat = rnd() & 3;
         int aff = (rnd() % 3) == 0;
         OAM[idx * 4 + 0] = (u16)((aff ? 0x0100 : 0) | (yy & 0xFF));
         OAM[idx * 4 + 1] = (u16)((x & 0x1FF) | (big ? 0x4000 : 0));
         OAM[idx * 4 + 2] = (u16)((4 * pat) | (prio << 10) | (i << 12));
      }

   REG(0x000) = 0x1740;                                 /* mode 0, BG0..2, OBJ, 1D */
   for (;;)
      ;
}
