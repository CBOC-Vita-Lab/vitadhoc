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

/* The NEON loops (ARM builds only) are the console's fast path; the portable C++ loops stay as the reference and for every other CPU, and
 * make the same picture byte for byte (tests/scale_neon_check.cpp runs both). scale_neon_built() says whether this build has them;
 * scale_use_neon(false) switches a built-in NEON path off (tests, and timing the C loops on the same CPU). On by default. */
bool scale_neon_built();
void scale_use_neon(bool on);

/* Builds the tables (idempotent, call it once before any thread uses a scaler). */
void scale_init();

/* A thread's working memory; a thread must use its own. */
struct ScaleScratch {
	std::vector<uint16_t> lum;
	std::vector<uint16_t> pad;
};

/* Scales source rows [y0, y1) of the w x h picture `src` (pitch in pixels, RGB565) by the kind's factor. Source row y becomes
 * the factor's rows y * f .. y * f + f - 1 of `dst` (dpitch in pixels, at least w * f). The rows around the stripe are read from
 * the whole picture (the border pixel repeats), so two threads on disjoint stripes make exactly the picture one thread makes.
 * Returns the number of source pixels that were not flat (a cost figure). */
unsigned scale_rows(ScalerKind k, const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1,
                    ScaleScratch &sc);

/* The same for the columns [x0, x1) of those rows only (the output columns x0 * f .. x1 * f - 1); the rest of the output is not touched. */
unsigned scale_rect(ScalerKind k, const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1, int x0, int x1,
                    ScaleScratch &sc);

/* The literal transcription (slow; for tests and as the written definition of "xBR lv2 here"). scale 2 or 3. */
void xbr_reference(const uint16_t *src, int w, int h, int spitch, int scale, uint16_t *dst, int dpitch);

/* The definition of xBR here: the same, in integer arithmetic (see pk_scale.cpp). scale_rows() for SC_XBR2/3 must give exactly this picture. */
void xbr_reference_int(const uint16_t *src, int w, int h, int spitch, int scale, uint16_t *dst, int dpitch);

/* ---- the incremental scaler (ScaleCache) ------------------------------------------------------------------------------------------
 * Most pictures of a console game change in a part of their rows only (a HUD stands still, a menu blinks, sprites move over a flat
 * background) and a full-screen scroll moves rows up or down without changing them. ScaleCache keeps the last source picture and the
 * last scaled picture (in cached memory, one copy, whatever number of textures the caller rotates through) and per frame
 *   1. compares the new source rows with the kept ones (NEON on the console; exact compare, no hash),
 *   2. when many rows differ, looks for a vertical shift (the scaled rows of the last picture are moved, not computed again),
 *   3. computes only the output rows whose footprint (the scaler's reach: 3 source rows for MMPX, 2 for xBR) holds a changed row,
 *   4. hands the changed output rows to the caller's texture (blit): a texture that is a few pictures old gets every row that changed
 *      since it was written.
 * The picture is BIT IDENTICAL to scale_rows over the whole picture (tests/test_scale.cpp, md/twin/scale_proof.cpp: every frame of
 * every test ROM). A change of kind or size starts over. Threads: plan() on one thread, then run_chunk(i) for i < chunks() on any
 * threads (disjoint output rows), then, when all are done, blit() on one thread. */
struct TexSync {                  /* what one texture holds: which picture of the cache it is in step with */
	uint32_t epoch, gen;
	TexSync() : epoch(0), gen(0) {}
};

struct ScaleStats {               /* of the last plan() */
	int rows, blocks, computed, shifted, same, shift_k, shape, chunks;   /* blocks of 8 columns: all, computed, moved, kept; shape 0 none 1 vertical 2 horizontal */
	bool restarted;
};

class ScaleCache {
public:
	ScaleCache();
	~ScaleCache();
	void reset();                                     /* forget the kept pictures (the next plan() computes everything) */
	/* `allow_shift` 0 turns the vertical shift search off (tests). Returns the number of chunks (0 for SC_NONE or a bad size). */
	int plan(ScalerKind k, const uint16_t *src, int w, int h, int spitch, bool allow_shift = true);
	unsigned run_chunk(int i, ScaleScratch &sc);      /* returns the non flat count of its rows */
	int chunks() const;
	/* Copies the rows that changed since `t` was written into the texture memory (dpitch in pixels, at least w * factor). */
	int blit(uint16_t *dst, int dpitch, TexSync &t);
	const uint16_t *out() const;                      /* the cached scaled picture, pitch out_pitch() pixels */
	int out_pitch() const;
	const ScaleStats &stats() const;
private:
	struct Impl;
	Impl *p_;
	ScaleCache(const ScaleCache &);
	ScaleCache &operator=(const ScaleCache &);
};

/* Exact comparison of two rows of n pixels (NEON on the console, scalar elsewhere; the scalar one is the definition). */
bool rows_equal(const uint16_t *a, const uint16_t *b, int n);
bool rows_equal_scalar(const uint16_t *a, const uint16_t *b, int n);

}  // namespace pk

#endif
