/* mmpx_vita.h -- MMPX 2x pixel-art scaler for RGB565 pictures.
 *
 * MMPX (McGuire and Gagiu, "MMPX Style-Preserving Pixel Art Magnification",
 * 2021) rebuilds the stair-steps of lines and curves at 2x from a 5x5 look
 * around each pixel and invents no colours.  It is a pure compare-and-copy
 * kernel, so it costs about two thirds of xBRZ 2x (measured: docs/DEFAULTS-1.4.md), and it
 * keeps flat areas exactly as drawn.  The rules are the authors' (MIT, see
 * licenses/MMPX-MIT.txt); this file pair is a 16-bit rewrite of their
 * reference implementation.  Output is the same as the reference's on the
 * RGBA8 expansion of the picture (checked bit for bit on real frames with a
 * harness outside the tree; tests/upscale checks that stripes, rectangles
 * and the slide/ring paths equal the whole picture), with the edge handling of the original (the border
 * pixel repeats).
 *
 * Where it is used (upscale_vita.c scale_piece): the slot makes a 2x picture
 * with it instead of xBRZ when mmpx_wanted() says so:
 *   - the look governor's Auto ladder, between xBRZ 3x and sharp bilinear
 *     (look_glue.cpp, mmpx_set_auto);
 *   - the Settings choice "MMPX 2x" (mmpx_set_choice; wired by the menu).
 * xBRZ 2x chosen by hand stays xBRZ.
 *
 * Copyright 2020 Morgan McGuire and Mara Gagiu (the rules, MIT);
 * CBOC-Vita (this port, MIT). */
#ifndef GBADHOC_MMPX_VITA_H
#define GBADHOC_MMPX_VITA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rows [y0, y1), columns [x0, x1) of a tightly packed w x h RGB565 picture,
 * scaled 2x.  Source pixel (x0, y0)'s 2x2 block is written at `at`, rows
 * `dp` pixels apart; every colour goes through `lut` (565 -> RGBA8). */
void mmpx_rect(const uint16_t *src, int w, int h, const uint32_t *lut,
               uint32_t *at, size_t dp, int y0, int y1, int x0, int x1);

/* The governor's Auto ladder is on its MMPX 2x step (look_glue.cpp). */
void mmpx_set_auto(int on);
/* The player picked MMPX 2x by hand (the Settings filter choice). */
void mmpx_set_choice(int on);
/* A 2x picture of the given upscaler kind should be made with MMPX. */
int  mmpx_wanted(int kind);

#ifdef __cplusplus
}
#endif
#endif
