/* zapper_map_test.c -- the NES Zapper on the touch screen (../zapper_map.h):
 * where a touch of the 960 x 544 screen points in the NES picture, for the
 * screen sizes the app can draw it at, with and without the wide view and
 * the overscan crop.  Exit 0 = all agree. */
#include <stdio.h>
#include "../zapper_map.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the map and its answer in one line */
static int at(int tx, int ty, int rx, int ry, int rw, int rh, int sw, int sh, int top, int *x, int *y)
{
   return zapper_map(tx, ty, rx, ry, rw, rh, sw, sh, top, x, y);
}

int main(void)
{
   int x, y, i;

   /* 3x: 768 x 720 would not fit; the whole-number 2x: 512 x 448 (cropped 256 x 224, top 8) centred on 960 x 544 */
   {
      int rx = 224, ry = 48, rw = 512, rh = 448;
      CHECK(at(rx, ry, rx, ry, rw, rh, 256, 224, 8, &x, &y) && x == 0 && y == 8);            /* top left of the shown picture: row 8 of the frame */
      CHECK(at(rx + rw - 1, ry + rh - 1, rx, ry, rw, rh, 256, 224, 8, &x, &y) && x == 255 && y == 231);
      CHECK(at(rx + 256, ry + 224, rx, ry, rw, rh, 256, 224, 8, &x, &y) && x == 128 && y == 120);   /* the middle */
      CHECK(at(rx + 1, ry + 1, rx, ry, rw, rh, 256, 224, 8, &x, &y) && x == 0 && y == 8);          /* two screen pixels are one NES pixel */
      CHECK(at(rx + 2, ry + 2, rx, ry, rw, rh, 256, 224, 8, &x, &y) && x == 1 && y == 9);
      /* off the picture: beside it, above it, below it */
      CHECK(!at(rx - 1, ry + 10, rx, ry, rw, rh, 256, 224, 8, &x, &y) && x == -1 && y == -1);
      CHECK(!at(rx + rw, ry + 10, rx, ry, rw, rh, 256, 224, 8, &x, &y));
      CHECK(!at(rx + 10, ry - 1, rx, ry, rw, rh, 256, 224, 8, &x, &y));
      CHECK(!at(rx + 10, ry + rh, rx, ry, rw, rh, 256, 224, 8, &x, &y));
   }
   /* the whole 240 rows, no crop: the full frame */
   {
      int rx = 192, ry = 32, rw = 576, rh = 480;     /* 2.25x */
      CHECK(at(rx, ry, rx, ry, rw, rh, 256, 240, 0, &x, &y) && x == 0 && y == 0);
      CHECK(at(rx + rw - 1, ry + rh - 1, rx, ry, rw, rh, 256, 240, 0, &x, &y) && x == 255 && y == 239);
   }
   /* a picture that fills the screen's height: 544 rows for 224 shown (2.43x): every shown row is reachable, none is skipped twice */
   {
      int rx = 169, ry = 0, rw = 622, rh = 544, last = 7, ok = 1;
      for (i = 0; i < rh; i++)
      {
         if (!at(rx + 10, ry + i, rx, ry, rw, rh, 256, 224, 8, &x, &y)) { ok = 0; break; }
         if (y < last || y > last + 1) ok = 0;       /* never goes back, never jumps over a row */
         last = y;
      }
      CHECK(ok && last == 231);
      last = -1; ok = 1;
      for (i = 0; i < rw; i++)
      {
         if (!at(rx + i, ry + 10, rx, ry, rw, rh, 256, 224, 8, &x, &y)) { ok = 0; break; }
         if (x < last || x > last + 1) ok = 0;
         last = x;
      }
      CHECK(ok && last == 255);
   }
   /* the wide view: 304 columns shown (24 of the sides on each), centred; only the NES's own 256 are on the gun's frame */
   {
      int rx = 40, ry = 0, rw = 608 * 0 + 880, rh = 544, sw = 304, sh = 224;     /* 304/224 -> 740 wide at the screen's height; use a stretch to 880 */
      CHECK(at(rx, ry + 100, rx, ry, rw, rh, sw, sh, 8, &x, &y) == 0 && x == -1);                  /* the left side column: not the NES's */
      CHECK(at(rx + rw - 1, ry + 100, rx, ry, rw, rh, sw, sh, 8, &x, &y) == 0);                    /* the right side */
      /* the first NES column is column 24 of the 304: its first screen pixel */
      CHECK(at(rx + (24 * rw + sw - 1) / sw, ry + 100, rx, ry, rw, rh, sw, sh, 8, &x, &y) && x <= 1);
      CHECK(at(rx + rw / 2, ry + rh / 2, rx, ry, rw, rh, sw, sh, 8, &x, &y) && x >= 127 && x <= 129 && y >= 119 && y <= 121);
      /* every NES column 0..255 is reachable exactly through the shown 24..279 */
      {
         int seen0 = 0, seen255 = 0, bad = 0;
         for (i = 0; i < rw; i++)
            if (at(rx + i, ry + 200, rx, ry, rw, rh, sw, sh, 8, &x, &y))
            {
               if (x == 0) seen0 = 1;
               if (x == 255) seen255 = 1;
               if (x < 0 || x > 255) bad = 1;
            }
         CHECK(seen0 && seen255 && !bad);
      }
   }
   /* a picture with no size, or a frame narrower than the NES's: nothing to aim at */
   CHECK(!at(10, 10, 0, 0, 0, 100, 256, 224, 0, &x, &y) && x == -1 && y == -1);
   CHECK(!at(10, 10, 0, 0, 100, 0, 256, 224, 0, &x, &y));
   CHECK(!at(10, 10, 0, 0, 100, 100, 240, 160, 0, &x, &y));     /* a GBA-sized picture is no NES's */
   CHECK(!at(10, 10, 0, 0, 100, 100, 256, 0, 0, &x, &y));
   /* a crop that puts the shown rows past the frame's end answers off */
   CHECK(!at(10, 99, 0, 0, 100, 100, 256, 224, 40, &x, &y));

   if (fails) return 1;
   printf("ok zapper_map\n");
   return 0;
}
