/* See picture.h. Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE). */
#include "picture.h"
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
	s->hires_blend = 0;
	s->interlace_blend = 0;
	s->multitap = 0; /* 0 auto, 1 on, 2 off */
	s->use_game = 0;
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
		else if (!strcmp(line, "hires_blend")) s->hires_blend = v ? 1 : 0;
		else if (!strcmp(line, "interlace_blend")) s->interlace_blend = v ? 1 : 0;
		else if (!strcmp(line, "multitap")) s->multitap = clampi(v, 0, 2);
		else if (!strcmp(line, "own_settings")) s->use_game = v ? 1 : 0;
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
	fprintf(f, "shape=%d\npixel_square=%d\nquality=%d\nglow=%d\nscanlines=%d\nlcd_grid=%d\nhires_blend=%d\ninterlace_blend=%d\nmultitap=%d\nown_settings=%d\n",
	    s->shape, s->pixel_sq, s->quality, s->glow, s->scan, s->grid, s->hires_blend, s->interlace_blend, s->multitap, s->use_game);
	int bad = ferror(f);
	if (fclose(f) != 0 || bad) { remove(tmp); return 2; }
	remove(path);
	if (rename(tmp, path) != 0) { remove(tmp); return 3; }
	return 0;
}

void pic_geometry(const pic_settings *s, int src_w, int src_h, int crop_top, int crop_bottom,
                  int scr_w, int scr_h, pic_rect *r)
{
	int tall = src_h >= 400;                  /* interlace / 478 lines: two picture rows per SNES line */
	int lr_full = tall ? src_h / 2 : src_h;
	r->sx = 0; r->sw = src_w;
	r->sy = 0; r->sh = src_h;
	r->logical_rows = lr_full;
	float dw, dh;
	switch (s->shape) {
	case PIC_FULL:
		dw = (float) scr_w; dh = (float) scr_h;
		break;
	case PIC_PIXEL: {
		/* integer height scale; 8:7 pixels are 1.1667 (7/6) wide against the 4:3 picture of 256x224 */
		int k = scr_h / lr_full;
		if (k < 1) k = 1;
		float par = s->pixel_sq ? 1.0f : 8.0f / 7.0f;
		while (k > 1 && 256.0f * par * k > scr_w) k--;
		dh = (float) (lr_full * k);
		dw = 256.0f * par * k;
		break;
	}
	case PIC_ZOOM: {
		if (crop_top < 0) crop_top = 0;
		if (crop_bottom < 0) crop_bottom = 0;
		int maxc = src_h / 8;
		if (crop_top > maxc) crop_top = maxc;
		if (crop_bottom > maxc) crop_bottom = maxc;
		if (tall) { crop_top &= ~1; crop_bottom &= ~1; }
		r->sy = crop_top;
		r->sh = src_h - crop_top - crop_bottom;
		r->logical_rows = tall ? r->sh / 2 : r->sh;
		float full_h = (float) scr_h * lr_full / r->logical_rows;   /* the whole picture, scaled so the visible part fills the height */
		float full_w = full_h * 4.0f / 3.0f;
		if (full_w > scr_w) { full_h = full_h * scr_w / full_w; full_w = (float) scr_w; }
		dh = full_h * r->logical_rows / lr_full;
		dw = full_w;
		break;
	}
	default:
		dh = (float) scr_h;
		dw = scr_h * 4.0f / 3.0f;
		if (dw > scr_w) { dh = scr_w * 3.0f / 4.0f; dw = (float) scr_w; }
		break;
	}
	r->dw = dw; r->dh = dh;
	r->dx = (scr_w - dw) * 0.5f;
	r->dy = (scr_h - dh) * 0.5f;
}

void pic_prescale(const pic_rect *r, int *kx, int *ky)
{
	int x = (int) ceilf(r->dw / r->sw), y = (int) ceilf(r->dh / r->sh);
	if (x < 1) x = 1;
	if (y < 1) y = 1;
	/* keep the intermediate picture within 1024x1024 */
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

void pic_gov_init(pic_governor *g)
{
	memset(g, 0, sizeof *g);
	g->hold = 600;
	g->backoff = 1;
}

void pic_gov_update(pic_governor *g, unsigned work_us, unsigned budget_us)
{
	g->frames++;
	g->since_change++;
	if (work_us > budget_us) { g->slow++; g->fast = 0; }
	else if (work_us < budget_us * 3 / 4) { g->fast++; g->slow = g->slow > 0 ? g->slow - 1 : 0; }
	else g->fast = 0;
	/* sustained: 24 slow frames beyond the forgiveness in a row of work */
	if (g->slow >= 24 && g->level < 2) {
		if (g->frames > 0 && g->recovered && g->since_change < 600) g->backoff = g->backoff < 8 ? g->backoff * 2 : 8; /* went down again soon after going up */
		g->level++;
		g->recovered = 0;
		g->slow = g->fast = 0;
		g->since_change = 0;
	} else if (g->level > 0 && g->fast >= g->hold * g->backoff) {
		g->level--;
		g->recovered = 1;
		g->slow = g->fast = 0;
		g->since_change = 0;
	}
	if (g->level == 0 && g->since_change > 3600) g->backoff = 1;
}
