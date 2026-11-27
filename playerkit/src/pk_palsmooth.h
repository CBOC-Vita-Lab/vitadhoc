/*
 * Smooth 50 Hz: an even cadence for 50 Hz (PAL) games on a 60 Hz screen. Header only, so the Linux twins prove the very
 * code the Vita runs.
 *
 * True 50 Hz runs 5 frames in every 6 refreshes and shows the last picture again on the sixth: the picture steps
 * 1 1 1 1 1 0 frames per refresh, a visible hitch every tenth of a second in a scroll. Smooth 50 Hz runs the same
 * 5 frames in 6 (the game's speed and its sound are exactly as before) but takes the frame that the sixth refresh
 * would have waited for one refresh early. That refresh shows an even mix of the previous and the new picture, the next
 * refresh shows the new picture alone: the picture steps  .5 .5 1 1 1 1  instead of  0 1 1 1 1 1  (the sum of the
 * squared deviations from the ideal 5/6 step falls from 0.83 to 0.33). Only that one refresh in six differs, and only
 * where something moves: on a still picture the mix is the picture.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_PALSMOOTH_H
#define PK_PALSMOOTH_H

#include <stdint.h>
#include <stddef.h>

namespace pk {

/* The accumulator of snes/vita/pal_pace.h with the early run. step() is for one refresh: it returns the frames to
 * run (0 or 1); *early is set when that frame is the early one, whose picture the caller shows mixed with the one before it
 * and then shows alone on the next refresh. With smooth off, step() is exactly the plain accumulator. */
struct PalSmooth {
	double ratio = 1.0, acc = 0.0;
	bool smooth = true;
	bool borrowed = false;       /* the last refresh ran its frame early: this one's missing frame is that, and it runs none */
	void set(double fps, double display_hz) { ratio = fps / display_hz; acc = 0.0; borrowed = false; }
	int step(bool *early)
	{
		*early = false;
		acc += ratio;
		if (acc >= 1.0 - 1e-9) { acc -= 1.0; borrowed = false; return 1; }
		if (borrowed || !smooth) { borrowed = false; return 0; }
		acc -= 1.0; borrowed = true; *early = true;
		return 1;
	}
	/* a pass that covers several refreshes (a late pass): the frames of all of them, never early */
	int owed(unsigned refreshes)
	{
		int n = 0;
		borrowed = false;
		for (unsigned i = 0; i < refreshes; i++) {
			acc += ratio;
			if (acc >= 1.0 - 1e-9) { acc -= 1.0; n++; }
		}
		return n;
	}
};

/* dst = the mean of a and b, per colour channel, for 16 bit pixels. `keep` is every bit except the lowest of each channel
 * (0xF7DE for RGB565, the only format of the players). Pitches in bytes; dst may be a or b. */
static inline void mix16_half(uint8_t *dst, int dpitch, const uint8_t *a, int apitch, const uint8_t *b, int bpitch,
                              int w, int h, uint16_t keep = 0xF7DE)
{
	for (int y = 0; y < h; y++) {
		uint16_t *d = (uint16_t *) (dst + (size_t) y * dpitch);
		const uint16_t *pa = (const uint16_t *) (a + (size_t) y * apitch), *pb = (const uint16_t *) (b + (size_t) y * bpitch);
		for (int x = 0; x < w; x++) {
			const uint32_t u = pa[x], v = pb[x];
			d[x] = (uint16_t) ((u & v) + (((u ^ v) & keep) >> 1));
		}
	}
}

/* The setting ("PAL games speed", key pal_speed in player.ini): 0 True 50 Hz, 1 Fast 60 Hz (the values of 1.4), 2 Smooth 50 Hz (default).
 * The menu lists Smooth 50 Hz, True 50 Hz, Fast 60 Hz. */
enum { PAL_TRUE = 0, PAL_FAST = 1, PAL_SMOOTH = 2 };
static inline int pal_row_of(int mode) { return mode == PAL_SMOOTH ? 0 : mode == PAL_TRUE ? 1 : 2; }
static inline int pal_mode_of(int row) { return row == 0 ? PAL_SMOOTH : row == 1 ? PAL_TRUE : PAL_FAST; }
static inline int pal_mode_clamp(int v) { return v == PAL_TRUE || v == PAL_FAST ? v : PAL_SMOOTH; }

}   // namespace pk

#endif
