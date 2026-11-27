/* See picture.h. Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE). */
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
	s->overscan = PIC_OVERSCAN_AUTO;
	s->no_sprite_limit = 1;
	s->fourscore = 0;
	s->region = 0;
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
		else if (!strcmp(line, "overscan")) s->overscan = clampi(v, 0, 2);
		else if (!strcmp(line, "no_sprite_limit")) s->no_sprite_limit = v ? 1 : 0;
		else if (!strcmp(line, "fourscore")) s->fourscore = clampi(v, 0, 2);
		else if (!strcmp(line, "region")) s->region = clampi(v, 0, 3);
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
	fprintf(f, "shape=%d\npixel_square=%d\nquality=%d\nglow=%d\nscanlines=%d\nlcd_grid=%d\noverscan=%d\nno_sprite_limit=%d\nfourscore=%d\nregion=%d\nown_settings=%d\n",
	    s->shape, s->pixel_sq, s->quality, s->glow, s->scan, s->grid, s->overscan, s->no_sprite_limit, s->fourscore, s->region, s->use_game);
	int bad = ferror(f);
	if (fclose(f) != 0 || bad) { remove(tmp); return 2; }
	remove(path);
	if (rename(tmp, path) != 0) { remove(tmp); return 3; }
	return 0;
}

int pic_overscan_rows(const pic_settings *s, int is_pal)
{
	if (s->overscan == PIC_OVERSCAN_ON) return 8;
	if (s->overscan == PIC_OVERSCAN_OFF) return 0;
	return is_pal ? 0 : 8;
}

void pic_geometry(const pic_settings *s, int src_w, int src_h, int crop_rows,
                  int scr_w, int scr_h, pic_rect *r)
{
	if (crop_rows < 0) crop_rows = 0;
	if (crop_rows > src_h / 8) crop_rows = src_h / 8;
	int rows = src_h - 2 * crop_rows;
	r->sx = 0; r->sw = src_w;
	r->sy = crop_rows; r->sh = rows;
	r->logical_rows = rows;
	if (s->shape == PIC_ZOOM && src_w >= 32) { r->sx = 8; r->sw = src_w - 16; }
	const float par = 8.0f / 7.0f;
	float dw, dh;
	switch (s->shape) {
	case PIC_FULL:
		dw = (float) scr_w; dh = (float) scr_h;
		break;
	case PIC_PIXEL: {
		int k = scr_h / rows;
		if (k < 1) k = 1;
		float p = s->pixel_sq ? 1.0f : par;
		while (k > 1 && src_w * p * k > scr_w) k--;
		dh = (float) (rows * k);
		dw = src_w * p * k;
		break;
	}
	default:   /* ORIGINAL and ZOOM (ZOOM already narrowed the source columns above) */
		dh = (float) scr_h;
		dw = dh * r->sw * par / rows;
		if (dw > scr_w) { dh = dh * scr_w / dw; dw = (float) scr_w; }
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
		g->level = 2;    /* never a level that drops picture extras: only frames go */
		g->recovered = 0;
		g->slow = g->fast = 0;
		g->since_change = 0;
	} else if (g->level > 0 && g->fast >= g->hold * g->backoff) {
		g->level = 0;
		g->recovered = 1;
		g->slow = g->fast = 0;
		g->since_change = 0;
	}
	if (g->level == 0 && g->since_change > 3600) g->backoff = 1;
}
