/* zapper_map.h -- the NES Zapper on the touch screen: where a touch of the
 * screen points in the NES picture.
 *
 * Pure (no Vita calls) so the host test (hostcore/tests/zapper_map_test.c)
 * runs exactly what the console runs.  The app draws the shown picture --
 * `show_w` x `show_h` of the core's frame: the NES's 256 columns, or more when
 * the wide view shows its sides (the extra columns are the same on both
 * sides), and the rows left after the app's overscan crop -- into the
 * rectangle (rx, ry, rw, rh) of the 960 x 544 screen.  `top` is the first
 * shown row's place in the core's 240-row frame.  The Zapper reads the core's
 * own picture, so the answer is a pixel of that 256 x 240 frame.
 *
 * Returns 1 and the pixel when the touch is on a pixel of the NES frame; 0 and
 * (-1, -1) when it is off the picture (beside it, on the wide view's extra
 * columns the NES never drew, or the picture has no size): the gun then points
 * off the screen, as a Zapper aimed at the wall does. */
#ifndef GBADHOC_ZAPPER_MAP_H
#define GBADHOC_ZAPPER_MAP_H

#define ZAPPER_NES_W 256
#define ZAPPER_NES_H 240

static inline int zapper_map(int tx, int ty, int rx, int ry, int rw, int rh,
                             int show_w, int show_h, int top, int *nx, int *ny)
{
   long sx, sy;
   *nx = *ny = -1;
   if (rw <= 0 || rh <= 0 || show_w < ZAPPER_NES_W || show_h <= 0)
      return 0;
   if (tx < rx || tx >= rx + rw || ty < ry || ty >= ry + rh)
      return 0;
   /* the middle of the touched screen pixel, in picture pixels */
   sx = ((2L * (tx - rx) + 1) * show_w) / (2L * rw);
   sy = ((2L * (ty - ry) + 1) * show_h) / (2L * rh);
   sx -= (show_w - ZAPPER_NES_W) / 2;
   sy += top;
   if (sx < 0 || sx >= ZAPPER_NES_W || sy < 0 || sy >= ZAPPER_NES_H)
      return 0;
   *nx = (int)sx;
   *ny = (int)sy;
   return 1;
}

#endif
