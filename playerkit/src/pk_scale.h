/*
 * playerkit pixel-art scalers for RGB565 pictures, done on the CPU (a player's drawing thread, or two threads that each take
 * a stripe of rows). Portable C++11 (GCC vector extensions, which are NEON on the console and SSE on a PC).
 *
 *   SC_MMPX2   MMPX (McGuire and Gagiu, 2021), 2x, invents no colours. Rules: MIT, Copyright 2020 Morgan McGuire and Mara Gagiu
 *              (licenses/MMPX-MIT.txt); this is a 16 bit rewrite of the reference, written for VitAdhoc's main app and shared here.
 *   SC_XBR2/3  Hyllian's xBR level 2 (the "lv2" shader, MIT, Copyright (C) 2011-2016 Hyllian, licenses/xBR-MIT.txt) at scale 2 and
 *              3: edge detection on the luminance of a 5x5 look, smooth diagonals with a blend along the edge. Ported from the
 *              shader's own source (CORNER_C, SMOOTH_TIPS, the shader's default parameters). xBR is NOT xBRZ (xBRZ is GPL and is
 *              never linked into a player). xbr_reference() is a literal, per output pixel transcription of the shader's
 *              fragment program; scale_rows() must give the same picture (tests/test_scale.cpp).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_SCALE_H
#define PK_SCALE_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace pk {

enum ScalerKind { SC_NONE = 0, SC_MMPX2, SC_XBR2, SC_XBR3, SC_KINDS };

inline int scaler_factor(ScalerKind k) { return k == SC_MMPX2 || k == SC_XBR2 ? 2 : k == SC_XBR3 ? 3 : 1; }
const char *scaler_name(ScalerKind k);      /* "MMPX 2x", "xBR 2x", "xBR 3x", "off" */

/* Builds the tables (idempotent, call it once before any thread uses a scaler). */
void scale_init();

/* A thread's working memory; a thread must use its own. */
struct ScaleScratch {
	std::vector<float> lum;
	std::vector<uint16_t> pad;
};

/* Scales source rows [y0, y1) of the w x h picture `src` (pitch in pixels, RGB565) by the kind's factor. Source row y becomes
 * the factor's rows y * f .. y * f + f - 1 of `dst` (dpitch in pixels, at least w * f). The rows around the stripe are read from
 * the whole picture (the border pixel repeats), so two threads on disjoint stripes make exactly the picture one thread makes.
 * Returns the number of source pixels that were not flat (a cost figure). */
unsigned scale_rows(ScalerKind k, const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1,
                    ScaleScratch &sc);

/* The literal transcription (slow; for tests and as the written definition of "xBR lv2 here"). scale 2 or 3. */
void xbr_reference(const uint16_t *src, int w, int h, int spitch, int scale, uint16_t *dst, int dpitch);

}  // namespace pk

#endif
