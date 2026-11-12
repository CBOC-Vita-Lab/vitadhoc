/* objprio.c -- a test cartridge for the hardware's per-pixel OBJ priority
 * (video.cc OBJ_EXACT, app_tests.py t_objprio).  A BG of priority 1 covers the
 * whole screen in blue; the sprites are drawn on scanlines 20..27:
 *
 *  case 1  A: OAM 0, priority 2, red, 8x8 at x=20
 *          B: OAM 1, priority 0, green on its right half only, 8x8 at x=24
 *          B is TRANSPARENT over A's right half (x 24..27).  The sprite engine
 *          gives A's pixel there B's better priority, so it is drawn in front
 *          of the BG (red); x 20..23 is A alone, behind the BG (blue); B's
 *          opaque half x 28..31 is green.  Painted by priority alone, x 24..27
 *          comes out blue.
 *  case 2  C: OAM 2, priority 0, red, at x=60;  D: OAM 3, priority 2, green,
 *          at x=64.  The earlier sprite has the better priority, nothing to
 *          fix: red over x 60..67, D hidden behind the BG.
 *  case 3  E: OAM 4, priority 2, red, at x=100;  F: OAM 5, an AFFINE sprite
 *          (identity matrix), priority 0, completely transparent, same place:
 *          its area is E's, so E is red in front of the BG on x 100..107.
 *  case 4  a sprite pair in the wrong order that does not overlap (x=150 red
 *          prio 2, x=170 green prio 0) draws as before.
 * No battery RAM; the test reads the picture. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

#define REG(a) (*(vu16 *)(0x04000000 + (a)))
volatile u32 g_frames;              /* crt0.S counts vertical blanks here */

#define OAM ((vu16 *)0x07000000)
static void spr(int n, u16 a0, u16 a1, u16 a2) {
   OAM[n * 4 + 0] = a0;
   OAM[n * 4 + 1] = a1;
   OAM[n * 4 + 2] = a2;
}

int main(void)
{
   int i;
   vu32 *t;
   vu16 *m;

   REG(0x000) = 0x0080;                       /* forced blank while setting up */
   ((vu16 *)0x05000000)[0] = 0x0000;          /* backdrop black */
   ((vu16 *)0x05000000)[1] = 0x7C00;          /* BG colour 1: blue */
   ((vu16 *)0x05000200)[1] = 0x001F;          /* OBJ colour 1: red */
   ((vu16 *)0x05000200)[2] = 0x03E0;          /* OBJ colour 2: green */

   t = (vu32 *)(0x06000000 + 32);             /* BG tile 1: solid colour 1 */
   for (i = 0; i < 8; i++) t[i] = 0x11111111;
   m = (vu16 *)0x06004000;                    /* screen block 8 */
   for (i = 0; i < 32 * 32; i++) m[i] = 1;
   REG(0x008) = 0x0801;                       /* BG0: priority 1, block 8 */

   t = (vu32 *)0x06010000;                    /* OBJ tiles (1D, 4bpp) */
   for (i = 0; i < 8; i++) t[i] = 0x11111111;                /* 0: red */
   for (i = 0; i < 8; i++) t[16 + i] = 0x22220000;           /* 2: green right */
   for (i = 0; i < 8; i++) t[24 + i] = 0;                    /* 3: empty */

   for (i = 0; i < 128; i++) spr(i, 0x0200, 0, 0);           /* all off */
   /* (the affine group 0 lives in the attr3 of OAM 0..3) */
   OAM[0 * 4 + 3] = 0x0100;                   /* pa */
   OAM[1 * 4 + 3] = 0x0000;                   /* pb */
   OAM[2 * 4 + 3] = 0x0000;                   /* pc */
   OAM[3 * 4 + 3] = 0x0100;                   /* pd */

   spr(0, 20, 20, 0 | (2 << 10));             /* A */
   spr(1, 20, 24, 2 | (0 << 10));             /* B */
   spr(2, 20, 60, 0 | (0 << 10));             /* C */
   spr(3, 20, 64, 2 | (2 << 10));             /* D: green right half on tile 2 */
   spr(4, 20, 100, 0 | (2 << 10));            /* E */
   spr(5, 0x0100 | 20, 100, 3 | (0 << 10));   /* F: affine, group 0, tile 3 */
   spr(6, 20, 150, 0 | (2 << 10));            /* case 4 */
   spr(7, 20, 170, 2 | (0 << 10));

   REG(0x000) = 0x1140;                       /* mode 0, BG0, OBJ, 1D */
   for (;;)
      ;
}
