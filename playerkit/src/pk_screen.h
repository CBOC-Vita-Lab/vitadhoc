/*
 * Screen colour profile and the CRT mask: the maths both shared looks are made of.  Header only, plain C (the
 * main app is C, the players are C++), no platform calls, so the host twin and the unit tests run exactly the
 * code the console runs.
 *
 * SCREEN PROFILE (Settings > Display > Screen: Auto / OLED (PCH-1000) / LCD (PCH-2000) / PS TV)
 *   An old game was drawn for a dim, flat screen.  The first Vita's OLED is more saturated and contrasty than
 *   that, so the same pixels look over-coloured.  The profile is one small correction folded into the colour
 *   table every console already puts the picture through (a 65536 entry RGB565 -> RGB565 table), so it costs
 *   no extra pass over the frame.  The transform, in the gamma encoded 0..1 values of the picture:
 *
 *       y  = 0.299 r + 0.587 g + 0.114 b
 *       c' = y + (c - y) * sat           (sat: how much colour is kept)
 *       c'' = c' ^ curve                 (curve: a little above 1 deepens the mid-tones)
 *
 *   Profile    sat    curve   what it is for
 *   OLED       0.92   1.06    the PCH-1000's OLED: 8 % less colour, mid-tones a little deeper.  White, black and
 *                             every grey's end points stay exactly where they were.
 *   LCD        1      1      the PCH-2000's LCD is the reference the picture is made for: unchanged.
 *   PS TV      1      1      a television is set up by its owner: unchanged.
 *   The numbers are estimates from how the two panels look side by side, not measurements (nothing here has run
 *   on a console); they are deliberately mild.  Auto is PS TV on a PlayStation TV and the reference (LCD) on a
 *   handheld: the system gives no reliable way to tell the 1000 from the 2000 (sceKernelGetModelForCDialog is
 *   the same for both), so a 1000 owner picks OLED once.
 *   A profile composes with a console's own colour table (GBA colour correction, Game Boy, Game Gear ...): the
 *   console's table runs first, then the profile; the neutral profiles leave the console's table untouched.
 *
 * CRT MASK (Settings > Display > CRT mask, 0 off, 1..10)
 *   A phosphor / aperture grille pattern, computed once on the CPU into a small repeating RGBA texture and
 *   drawn over the picture as ONE extra quad with a multiply blend (dst * texel).  Lottes' aperture grille: a
 *   three pixel period, one column per phosphor, each column passes its own colour fully and the other two at a
 *   dark level; the strength says how dark.  The period follows the on-screen pixel scale: the mask is made for
 *   the whole number of screen pixels closest to one third of a source pixel's width (1 px columns when a source
 *   pixel is 3 screen pixels wide or less, 2 px columns at 6, ...), so a column is never a fraction of a pixel
 *   and the picture does not shimmer.  Strength s (1..10): the dark level is 1 - 0.45 * s / 10 of the full
 *   colour.  A multiply cannot brighten, so the mask darkens: on average by 3 % x strength (a third of the
 *   0.45 dip, twice: 30 % at 10, 3 % at 1).  See pk_crt_*.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
/* (not PK_SCREEN_H: that is the screen's height in pk_gfx.h, and a guard of that name erased it) */
#ifndef PK_SCREEN_PROFILE_H
#define PK_SCREEN_PROFILE_H

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PK_SCREEN_AUTO = 0, PK_SCREEN_OLED, PK_SCREEN_LCD, PK_SCREEN_PSTV, PK_SCREEN_CHOICES };

/* The row's words. */
static inline const char *pk_screen_name(int choice)
{
	return choice == PK_SCREEN_OLED ? "OLED (PCH-1000)" : choice == PK_SCREEN_LCD ? "LCD (PCH-2000)"
	     : choice == PK_SCREEN_PSTV ? "PS TV" : "Auto";
}
/* the value as a word in the files ("auto", "oled", "lcd", "pstv") */
static inline const char *pk_screen_tag(int choice)
{
	return choice == PK_SCREEN_OLED ? "oled" : choice == PK_SCREEN_LCD ? "lcd" : choice == PK_SCREEN_PSTV ? "pstv" : "auto";
}
static inline int pk_screen_clamp(int choice) { return choice < 0 || choice >= PK_SCREEN_CHOICES ? PK_SCREEN_AUTO : choice; }

/* Auto -> what it means on this machine; the others as chosen.  is_pstv: sceKernelIsPSVitaTV() (0 on a handheld). */
static inline int pk_screen_resolve(int choice, int is_pstv)
{
	choice = pk_screen_clamp(choice);
	if (choice == PK_SCREEN_AUTO)
		return is_pstv ? PK_SCREEN_PSTV : PK_SCREEN_LCD;
	return choice;
}
/* 1 when the (resolved) profile changes the picture at all */
static inline int pk_screen_active(int resolved) { return resolved == PK_SCREEN_OLED; }

static inline void pk_screen_params(int resolved, float *sat, float *curve)
{
	*sat = resolved == PK_SCREEN_OLED ? 0.92f : 1.0f;
	*curve = resolved == PK_SCREEN_OLED ? 1.06f : 1.0f;
}

/* One RGB565 value (libretro order, R in the high bits) through the profile. */
static inline uint16_t pk_screen_apply565(int resolved, uint16_t v)
{
	float sat, curve, c[3], y;
	unsigned o[3];
	int i;
	if (!pk_screen_active(resolved))
		return v;
	pk_screen_params(resolved, &sat, &curve);
	c[0] = (float)(v >> 11) / 31.0f;
	c[1] = (float)((v >> 5) & 63) / 63.0f;
	c[2] = (float)(v & 31) / 31.0f;
	y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
	for (i = 0; i < 3; i++)
	{
		float x = y + (c[i] - y) * sat;
		if (x < 0.0f) x = 0.0f;
		if (x > 1.0f) x = 1.0f;
		x = powf(x, curve);
		o[i] = (unsigned)(x * (i == 1 ? 63.0f : 31.0f) + 0.5f);
	}
	return (uint16_t)((o[0] << 11) | (o[1] << 5) | o[2]);
}

/* out[v] = profile(base[v]) for all 65536 values; base NULL = the identity (a console with no table of its own).
 * out may be base (in place).  A neutral profile copies base (or writes the identity).
 * Uses the 94 monotonic inverse-power thresholds instead of 196,608 powf() calls (~2000x faster on Cortex-A9),
 * with exact powf() fallback within 1e-4 of a threshold so every one of the 65536 entries is bit-identical to pk_screen_apply565(). */
static inline unsigned pk_screen_quant_(float x, float curve, const float *thr, int maxv)
{
	int k;
	if (x <= 0.0f) return 0;
	if (x >= 1.0f) return (unsigned)maxv;
	k = (int)(x * (float)maxv + 0.5f);
	if (k > maxv) k = maxv;
	while (k > 0 && x < thr[k - 1]) k--;
	while (k < maxv && x >= thr[k]) k++;
	if ((k > 0 && x - thr[k - 1] < 1e-4f) || (k < maxv && thr[k] - x < 1e-4f))
		return (unsigned)(powf(x, curve) * (float)maxv + 0.5f);
	return (unsigned)k;
}

static inline void pk_screen_lut565(int resolved, const uint16_t *base, uint16_t *out)
{
	unsigned v;
	float sat, curve, inv_c, thr5[31], thr6[63];
	int k;
	if (!pk_screen_active(resolved))
	{
		for (v = 0; v < 65536; v++)
			out[v] = base ? base[v] : (uint16_t)v;
		return;
	}
	pk_screen_params(resolved, &sat, &curve);
	inv_c = 1.0f / curve;
	for (k = 0; k < 31; k++) thr5[k] = powf(((float)k + 0.5f) / 31.0f, inv_c);
	for (k = 0; k < 63; k++) thr6[k] = powf(((float)k + 0.5f) / 63.0f, inv_c);
	for (v = 0; v < 65536; v++)
	{
		uint16_t b = base ? base[v] : (uint16_t)v;
		float c0 = (float)(b >> 11) / 31.0f;
		float c1 = (float)((b >> 5) & 63) / 63.0f;
		float c2 = (float)(b & 31) / 31.0f;
		float y = 0.299f * c0 + 0.587f * c1 + 0.114f * c2;
		unsigned r = pk_screen_quant_(y + (c0 - y) * sat, curve, thr5, 31);
		unsigned g = pk_screen_quant_(y + (c1 - y) * sat, curve, thr6, 63);
		unsigned bl = pk_screen_quant_(y + (c2 - y) * sat, curve, thr5, 31);
		out[v] = (uint16_t)((r << 11) | (g << 5) | bl);
	}
}

/* ---- CRT mask ----------------------------------------------------------- */

/* Columns of the repeating pattern: the three phosphors.  The mask texture is `period` x 1 for the three columns
 * (period = 3 * col_w), RGBA8 bytes R,G,B,A, one row high: the quad draws it stretched down the picture
 * (a row of an aperture grille is the same all the way down).  Returns the width in texels. */
enum { PK_CRT_MAX_W = 24 };

/* The phosphor column width in screen pixels for a source pixel that is `scale` screen pixels wide (float, e.g.
 * 3.4 for Fit): the whole number nearest scale / 3, at least 1, at most PK_CRT_MAX_W / 3. */
static inline int pk_crt_col_w(float scale)
{
	int w = (int)floorf(scale / 3.0f + 0.5f);
	if (w < 1) w = 1;
	if (w > PK_CRT_MAX_W / 3) w = PK_CRT_MAX_W / 3;
	return w;
}

/* The dark level 0..255 of a phosphor column's two other colours at strength 1..10. */
static inline int pk_crt_dark(int strength)
{
	int s = strength < 0 ? 0 : strength > 10 ? 10 : strength;
	return 255 - (int)(255.0f * 0.45f * (float)s / 10.0f + 0.5f);
}

/* Fills rgba (4 * 3 * col_w bytes) and returns 3 * col_w, or 0 when strength is 0.  Column k passes colour k at
 * 255 and the other two at pk_crt_dark(); alpha 255 (unused by the multiply). */
static inline int pk_crt_make(int strength, int col_w, uint8_t *rgba)
{
	int x, k, d = pk_crt_dark(strength);
	if (strength <= 0 || col_w < 1 || col_w > PK_CRT_MAX_W / 3)
		return 0;
	for (x = 0; x < 3 * col_w; x++)
	{
		k = x / col_w;
		rgba[4 * x + 0] = (uint8_t)(k == 0 ? 255 : d);
		rgba[4 * x + 1] = (uint8_t)(k == 1 ? 255 : d);
		rgba[4 * x + 2] = (uint8_t)(k == 2 ? 255 : d);
		rgba[4 * x + 3] = 255;
	}
	return 3 * col_w;
}

/* The mean multiplier of the mask (0..1) at a strength: what the picture's brightness is multiplied by on
 * average ((255 + 2 d) / 3 / 255). */
static inline float pk_crt_mean(int strength)
{
	return (255.0f + 2.0f * (float)pk_crt_dark(strength)) / 765.0f;
}

#ifdef __cplusplus
}
#endif

#endif
