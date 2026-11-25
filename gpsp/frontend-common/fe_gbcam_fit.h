/* The Game Boy Camera's picture from a bigger greyscale one (frontend-common;
 * gbcore.h gbcore_set_camera_image): the largest 8:7 area in the middle of
 * the source, shrunk to 128x112 by averaging the source pixels each output
 * pixel covers; optionally mirrored left to right (a front camera, which
 * shows the player as a mirror does).  Pure; the Vita's camera, a picture
 * file and the twin's test feed all go through it. */
#ifndef FE_GBCAM_FIT_H
#define FE_GBCAM_FIT_H

#include <stdint.h>

#define FE_GBCAM_W 128
#define FE_GBCAM_H 112

static inline void fe_gbcam_fit(const uint8_t *src, int sw, int sh, int pitch,
                                int mirror, uint8_t *out)
{
   int cw = sw, ch = sh, x0, y0, x, y;
   if (!src || sw < 1 || sh < 1)
      return;
   if (sw * FE_GBCAM_H > sh * FE_GBCAM_W)
      cw = sh * FE_GBCAM_W / FE_GBCAM_H;       /* wider than 8:7: trim the sides */
   else
      ch = sw * FE_GBCAM_H / FE_GBCAM_W;       /* taller: trim top and bottom */
   if (cw < 1) cw = 1;
   if (ch < 1) ch = 1;
   x0 = (sw - cw) / 2;
   y0 = (sh - ch) / 2;
   for (y = 0; y < FE_GBCAM_H; y++)
   {
      int ya = y0 + y * ch / FE_GBCAM_H, yb = y0 + (y + 1) * ch / FE_GBCAM_H;
      if (yb <= ya) yb = ya + 1;
      for (x = 0; x < FE_GBCAM_W; x++)
      {
         int xa = x0 + x * cw / FE_GBCAM_W, xb = x0 + (x + 1) * cw / FE_GBCAM_W;
         unsigned sum = 0, n = 0;
         int yy, xx;
         if (xb <= xa) xb = xa + 1;
         for (yy = ya; yy < yb; yy++)
            for (xx = xa; xx < xb; xx++, n++)
               sum += src[(size_t)yy * (size_t)pitch + (size_t)xx];
         out[y * FE_GBCAM_W + (mirror ? FE_GBCAM_W - 1 - x : x)] = (uint8_t)(sum / n);
      }
   }
}

#endif
