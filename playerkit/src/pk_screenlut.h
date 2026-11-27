/*
 * The screen profile for a player's picture: the 65536 entry RGB565 table of pk_screen.h, built once for the profile in
 * use, and one pass that writes a picture through it into a private tightly packed copy. A neutral profile (LCD, PS TV,
 * and Auto on a handheld) returns the picture itself and costs nothing. The copy is what the pixel art scaler and the
 * texture upload then read, so the colours are right whatever the picture is drawn with.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_SCREENLUT_H
#define PK_SCREENLUT_H

#include "pk_screen.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace pk {

class ScreenLut {
public:
	ScreenLut() : lut_(0), buf_(0), cap_(0), built_(-1) {}
	~ScreenLut() { free(lut_); free(buf_); }
	ScreenLut(const ScreenLut &) = delete;
	ScreenLut &operator=(const ScreenLut &) = delete;

	/* resolved: a pk_screen_resolve() result. NULL when the profile does not change colours (or there is no memory). */
	const uint16_t *table(int resolved)
	{
		if (!pk_screen_active(resolved)) return 0;
		if (!lut_) lut_ = (uint16_t *) malloc(65536 * sizeof(uint16_t));
		if (!lut_) return 0;
		if (built_ != resolved) { pk_screen_lut565(resolved, 0, lut_); built_ = resolved; }
		return lut_;
	}

	/* The w x h picture (src, pitch in bytes) as the profile shows it: `src` itself and *pitch untouched for a neutral
	 * profile, else a tightly packed private copy (*pitch = w * 2), valid until the next call. */
	const uint16_t *frame(int resolved, const uint16_t *src, int w, int h, int *pitch_bytes)
	{
		const uint16_t *t = table(resolved);
		if (!t || !src || w <= 0 || h <= 0) return src;
		const size_t n = (size_t) w * (size_t) h;
		if (n > cap_) {
			uint16_t *nb = (uint16_t *) realloc(buf_, n * sizeof(uint16_t));
			if (!nb) return src;
			buf_ = nb; cap_ = n;
		}
		for (int y = 0; y < h; y++) {
			const uint16_t *s = (const uint16_t *) ((const uint8_t *) src + (size_t) y * (size_t) *pitch_bytes);
			uint16_t *d = buf_ + (size_t) y * (size_t) w;
			for (int x = 0; x < w; x++) d[x] = t[s[x]];
		}
		*pitch_bytes = w * 2;
		return buf_;
	}

private:
	uint16_t *lut_, *buf_;
	size_t cap_;
	int built_;
};

}  // namespace pk

#endif
