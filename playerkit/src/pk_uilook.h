/*
 * Text size and High contrast: the maths shared by the app's two text systems and the players' Text.  Header
 * only, plain C, no platform calls (the host twin and the unit tests run the code the console runs).
 *
 *   TEXT SIZE   Normal is 100, Large is PK_TEXT_LARGE_PCT (120): every face is drawn that many percent larger
 *               and measured that much wider and taller, so wrapping and fitting see the real size.
 *   HIGH CONTRAST   pk_text_tint(): the colour of every glyph (0xAABBGGRR) goes toward white when it is a light
 *               colour and toward black when it is a dark one: 70 % of the way for a grey or pale colour, 35 %
 *               for a coloured one (a red warning keeps its hue); alpha rises by half again, clamped at opaque
 *               (continuous and monotonic: a fade is still a fade).  The page colours have their own high
 *               contrast variants in each menu.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_UILOOK_H
#define PK_UILOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PK_TEXT_LARGE_PCT 120

static inline int pk_text_pct(int large) { return large ? PK_TEXT_LARGE_PCT : 100; }

static inline uint32_t pk_text_tint(uint32_t rgba)
{
	unsigned r = rgba & 0xFF, g = (rgba >> 8) & 0xFF, b = (rgba >> 16) & 0xFF, a = rgba >> 24;
	const unsigned y = (299 * r + 587 * g + 114 * b) / 1000;
	const unsigned t = y >= 128 ? 255 : 0;
	const unsigned mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	const unsigned mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	const int k = (mx - mn) < 48 ? 70 : 35;
	r = (unsigned) ((int) r + ((int) t - (int) r) * k / 100);
	g = (unsigned) ((int) g + ((int) t - (int) g) * k / 100);
	b = (unsigned) ((int) b + ((int) t - (int) b) * k / 100);
	a = a * 3 / 2;
	if (a > 255) a = 255;
	return (a << 24) | (b << 16) | (g << 8) | r;
}

#ifdef __cplusplus
}
#endif

#endif
