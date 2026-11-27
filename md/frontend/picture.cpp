/* See picture.h. Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE). */
#include "picture.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void pic_defaults(pic_settings *s)
{
	s->shape = PIC_ORIGINAL;
	s->pixel_sq = 0;
	s->quality = PIC_SHARP;
	s->glow = 5;
	s->scan = 0;
	s->grid = 0;
	s->crt = 0;
	s->scaler = SCALER_AUTO;
	s->gg_colors = 1;
	s->use_game = 0;
	s->wide = 0;
	s->tier_steps = 0;
}

pic_info pic_info_from_video(const struct md_video_info *vi)
{
	pic_info g;
	g.w = vi->w; g.h = vi->h;
	g.logical_rows = vi->h > vi->native_h * 3 / 2 ? vi->h / 2 : vi->h;      /* interlaced double height: two picture rows per console line */
	g.dar = vi->dar; g.par = vi->par; g.system = vi->system;
	if (vi->gg_band) { g.w = 160; g.logical_rows = 144; g.par = 1.2; g.dar = 1.2 * 160.0 / 144.0; }   /* the band's picture is shaped like the Game Gear's own window */
	return g;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int pic_load(const char *path, pic_settings *s)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	char line[128];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		int v = atoi(eq + 1);
		if (!strcmp(line, "shape")) s->shape = clampi(v, 0, PIC_SHAPES - 1);
		else if (!strcmp(line, "pixel_square")) s->pixel_sq = v ? 1 : 0;
		else if (!strcmp(line, "quality")) s->quality = v ? PIC_SMOOTH : PIC_SHARP;
		else if (!strcmp(line, "glow")) s->glow = clampi(v, 0, 10);
		else if (!strcmp(line, "scanlines")) s->scan = clampi(v, 0, 10);
		else if (!strcmp(line, "lcd_grid")) s->grid = clampi(v, 0, 10);
		else if (!strcmp(line, "crt_mask")) s->crt = clampi(v, 0, 10);
		else if (!strcmp(line, "scaler")) s->scaler = clampi(v, 0, SCALER_CHOICES - 1);
		else if (!strcmp(line, "gg_colors")) s->gg_colors = v ? 1 : 0;
		else if (!strcmp(line, "own_settings")) s->use_game = v ? 1 : 0;
		else if (!strcmp(line, "wide_view")) s->wide = clampi(v, 0, 2);
	}
	fclose(f);
	return 1;
}

int pic_save(const char *path, const pic_settings *s)
{
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return 1;
	fprintf(f, "shape=%d\npixel_square=%d\nquality=%d\nglow=%d\nscanlines=%d\nlcd_grid=%d\ncrt_mask=%d\nscaler=%d\ngg_colors=%d\nown_settings=%d\nwide_view=%d\n",
	    s->shape, s->pixel_sq, s->quality, s->glow, s->scan, s->grid, s->crt, s->scaler, s->gg_colors, s->use_game, s->wide);
	int bad = ferror(f);
	if (fclose(f) != 0 || bad) { remove(tmp); return 2; }
	remove(path);
	if (rename(tmp, path) != 0) { remove(tmp); return 3; }
	return 0;
}

void pic_geometry(const pic_settings *s, const pic_info *in, int crop_top, int crop_bottom, int scr_w, int scr_h, pic_rect *r)
{
	const int w = in->w > 0 ? in->w : 320, h = in->h > 0 ? in->h : 224;
	const int rows = in->logical_rows > 0 ? in->logical_rows : h;
	const double par = in->par > 0.0 ? in->par : 1.0;
	/* the picture's own shape: what the console put on a television of its time */
	const double dar = in->dar > 0.0 ? in->dar : (double) w * par / rows;
	r->sx = 0; r->sw = w;
	r->sy = 0; r->sh = h;
	r->logical_rows = rows;
	const int tall = h > rows;                  /* interlaced double height: two picture rows per console line */
	float dw, dh;
	switch (s->shape) {
	case PIC_FULL:
		dw = (float) scr_w; dh = (float) scr_h;
		break;
	case PIC_PIXEL: {
		int k = scr_h / rows;
		if (k < 1) k = 1;
		const double pw = s->pixel_sq ? 1.0 : (par > 0.0 ? par : 1.0);
		while (k > 1 && w * pw * k > scr_w) k--;
		dh = (float) (rows * k);
		dw = (float) (w * pw * k);
		break;
	}
	case PIC_ZOOM: {
		if (crop_top < 0) crop_top = 0;
		if (crop_bottom < 0) crop_bottom = 0;
		const int maxc = h / 8;
		if (crop_top > maxc) crop_top = maxc;
		if (crop_bottom > maxc) crop_bottom = maxc;
		if (tall) { crop_top &= ~1; crop_bottom &= ~1; }
		r->sy = crop_top;
		r->sh = h - crop_top - crop_bottom;
		r->logical_rows = tall ? r->sh / 2 : r->sh;
		float full_h = (float) scr_h * rows / r->logical_rows;
		float full_w = (float) (full_h * dar);
		if (full_w > scr_w) { full_h = full_h * scr_w / full_w; full_w = (float) scr_w; }
		dh = full_h * r->logical_rows / rows;
		dw = full_w;
		break;
	}
	default:
		dh = (float) scr_h;
		dw = (float) (scr_h * dar);
		if (dw > scr_w) { dh = (float) (scr_w / dar); dw = (float) scr_w; }
		break;
	}
	r->dw = dw; r->dh = dh;
	r->dx = (scr_w - dw) * 0.5f;
	r->dy = (scr_h - dh) * 0.5f;
}

int pic_wide_margin(const pic_settings *s, const pic_info *in, int scr_w, int scr_h, int max_m)
{
	if (s->shape == PIC_FULL || !in || in->w <= 0) return 0;
	pic_rect r;
	pic_geometry(s, in, 0, 0, scr_w, scr_h, &r);
	if (r.dw <= 0.0f || r.sw <= 0) return 0;
	const float per_col = r.dw / (float) r.sw;
	const int m = (int) floorf((scr_w / per_col - (float) in->w) * 0.5f);
	if (m < 1) return 0;
	return m > max_m ? max_m : m;
}

void pic_geometry_wide(const pic_settings *s, const pic_info *in, int wide_w, int crop_top, int crop_bottom, int scr_w, int scr_h, pic_rect *r)
{
	pic_geometry(s, in, crop_top, crop_bottom, scr_w, scr_h, r);
	const float per_col = r->dw / (float) (in->w > 0 ? in->w : 1);
	float dw = per_col * wide_w;
	r->sx = 0; r->sw = wide_w;
	if (dw > scr_w) {
		int cut = (int) ceilf((dw - scr_w) * 0.5f / per_col);
		if (cut * 2 >= wide_w) cut = 0;
		r->sx = cut; r->sw = wide_w - 2 * cut;
		dw = per_col * r->sw;
	}
	r->dw = dw;
	r->dx = (scr_w - dw) * 0.5f;
}

void pic_prescale(const pic_rect *r, int *kx, int *ky)
{
	int x = (int) ceilf(r->dw / r->sw), y = (int) ceilf(r->dh / r->sh);
	if (x < 1) x = 1;
	if (y < 1) y = 1;
	while (x > 1 && r->sw * x > 1024) x--;
	while (y > 1 && r->sh * y > 1024) y--;
	*kx = x; *ky = y;
}

int pic_blank_rows(const unsigned short *px, int pitch_bytes, int w, int h, int from_top, int max_rows)
{
	int n = 0;
	for (; n < max_rows && n < h; n++) {
		int y = from_top ? n : h - 1 - n;
		const unsigned short *row = (const unsigned short *) ((const unsigned char *) px + (size_t) y * pitch_bytes);
		for (int x = 0; x < w; x++)
			if (row[x]) return n;
	}
	return n;
}

/* The ceilings (tiers: 0 xBR 3x, 1 xBR 2x, 2 none = sharp bilinear; MMPX 2x is a look the player picks, not a rung):
 *   Game Gear 160x144 (and its 256x144 wide band)           xBR 3x (480x432 shown on 544 rows: 3x is not wasted, 144 x 3 = 432)
 *   Master System / SG-1000 256x192 (wide view up to 338x192)  xBR 3x (576 rows from 192: the picture is fitted down to 544, so the extra pixels
 *                                                            are averaged away; the look governor steps it to xBR 2x and sharp when the frame budget needs it)
 *   Mega Drive (256 or 320 wide, 224 / 240 rows), Master System games of 224 / 240 rows   xBR 2x
 * 3x on 224..240 rows makes 672..720 rows for a 544 row screen: more than the screen can use (md-perf2/sidebyside has the pictures and the
 * differences). Since the integer NEON xBR (pk_scale) xBR 2x costs less than MMPX 2x, so MMPX is not a rung.
 * Not for pictures taller than 256 rows (interlace) or wider than 352 (the wide picture's size is not asked: the normal picture's is). */
int pic_scaler_ceiling(int system, int w, int h)
{
	if (w > 352 || h > 256 || w < 1 || h < 1) return 2;
	const long px = (long) w * h;
	if (px <= 160L * 144L + 1) return 0;           /* 160x144 */
	if (system == 3 && w <= 256 && h <= 144) return 0;     /* MD_SYS_GG: the wide band */
	if ((system == 2 || system == 4) && w <= 256 && h <= 192) return 0;   /* MD_SYS_SMS / MD_SYS_SG: 256x192 */
	return 1;                                       /* 256x224 .. 352x256 */
}

int pic_scaler_effective(const pic_settings *s, int ceiling, int steps)
{
	static const int kTier[3] = { SCALER_XBR3, SCALER_XBR2, SCALER_OFF };
	switch (s->scaler) {
	case SCALER_OFF: return SCALER_OFF;
	case SCALER_MMPX2: case SCALER_XBR2: case SCALER_XBR3: return s->scaler;    /* chosen by hand: held as chosen */
	default: break;
	}
	int t = ceiling + steps;
	if (t < 0) t = 0;
	if (t > 2) t = 2;
	return kTier[t];
}

const unsigned short *pic_lcd_table(void)
{
	static unsigned short *t;
	if (t) return t;
	unsigned short *n = (unsigned short *) malloc(65536 * sizeof *n);
	if (!n) return 0;
	/* Precompute monotonic inverse-power thresholds (94 powf calls instead of 196,608) with exact fallback near boundaries. */
	float thr5[31], thr6[63];
	const float inv = 1.0f / 1.1f;
	for (int k = 0; k < 31; k++) {
		const float y = ((k + 0.5f) / 31.0f - 0.05f) / 0.93f;
		thr5[k] = y <= 0.0f ? -1.0f : (y >= 1.0f ? 2.0f : powf(y, inv));
	}
	for (int k = 0; k < 63; k++) {
		const float y = ((k + 0.5f) / 63.0f - 0.05f) / 0.93f;
		thr6[k] = y <= 0.0f ? -1.0f : (y >= 1.0f ? 2.0f : powf(y, inv));
	}
	auto quant = [](float x, const float *thr, int maxq) -> int {
		if (x <= 0.0f) return (int) (0.05f * maxq + 0.5f);
		int lo = 0, hi = maxq;
		while (lo < hi) {
			const int mid = (lo + hi) >> 1;
			if (x < thr[mid]) hi = mid;
			else lo = mid + 1;
		}
		if ((lo > 0 && fabsf(x - thr[lo - 1]) < 1e-4f) || (lo < maxq && fabsf(x - thr[lo]) < 1e-4f)) {
			const float o = 0.05f + 0.93f * powf(x, 1.1f);
			int q = (int) (o * maxq + 0.5f);
			return q < 0 ? 0 : q > maxq ? maxq : q;
		}
		return lo;
	};
	for (unsigned v = 0; v < 65536; v++) {
		float r = ((v >> 11) & 31) / 31.0f, g = ((v >> 5) & 63) / 63.0f, b = (v & 31) / 31.0f;
		/* the Game Gear's STN screen: blacks sit at a dark grey, colours lose some saturation, gamma a little high */
		const float y = 0.299f * r + 0.587f * g + 0.114f * b;
		r = y + (r - y) * 0.85f; g = y + (g - y) * 0.85f; b = y + (b - y) * 0.85f;
		const int R = quant(r, thr5, 31), G = quant(g, thr6, 63), B = quant(b, thr5, 31);
		n[v] = (unsigned short) ((R << 11) | (G << 5) | B);
	}
	t = n;
	return t;
}
