/* See layout.h. Copyright CBOC-Vita. MIT licence. */
#include "layout.h"
#include "host.h"

unsigned wsl_map_pad(unsigned v, int vertical, int pres)
{
	unsigned k = 0;
	if (v & WSL_START) k |= WSWAN_START;
	if (!vertical) {
		if (v & WSL_UP) k |= WSWAN_X1;
		if (v & WSL_RIGHT) k |= WSWAN_X2;
		if (v & WSL_DOWN) k |= WSWAN_X3;
		if (v & WSL_LEFT) k |= WSWAN_X4;
		if (v & WSL_RS_UP) k |= WSWAN_Y1;
		if (v & WSL_RS_RIGHT) k |= WSWAN_Y2;
		if (v & WSL_RS_DOWN) k |= WSWAN_Y3;
		if (v & WSL_RS_LEFT) k |= WSWAN_Y4;
		if (v & (WSL_CIRCLE | WSL_TRIANGLE)) k |= WSWAN_A;
		if (v & (WSL_CROSS | WSL_SQUARE)) k |= WSWAN_B;
	} else {
		if (v & WSL_L) k |= WSWAN_A;
		if (v & WSL_R) k |= WSWAN_B;
		if (pres == WSL_TURNED) {
			if (v & WSL_UP) k |= WSWAN_Y1;
			if (v & WSL_RIGHT) k |= WSWAN_Y2;
			if (v & WSL_DOWN) k |= WSWAN_Y3;
			if (v & WSL_LEFT) k |= WSWAN_Y4;
			if (v & WSL_TRIANGLE) k |= WSWAN_X1;
			if (v & WSL_CIRCLE) k |= WSWAN_X2;
			if (v & WSL_CROSS) k |= WSWAN_X3;
			if (v & WSL_SQUARE) k |= WSWAN_X4;
		} else {
			if (v & WSL_LEFT) k |= WSWAN_Y1;
			if (v & WSL_UP) k |= WSWAN_Y2;
			if (v & WSL_RIGHT) k |= WSWAN_Y3;
			if (v & WSL_DOWN) k |= WSWAN_Y4;
			if (v & WSL_SQUARE) k |= WSWAN_X1;
			if (v & WSL_TRIANGLE) k |= WSWAN_X2;
			if (v & WSL_CIRCLE) k |= WSWAN_X3;
			if (v & WSL_CROSS) k |= WSWAN_X4;
		}
	}
	return k;
}

void wsl_layout(int vertical, int pres, int sw, int sh, int integer, wsl_rect *r)
{
	int rot = vertical && pres == WSL_UPRIGHT;
	int w = rot ? WSWAN_HEIGHT : WSWAN_WIDTH, h = rot ? WSWAN_WIDTH : WSWAN_HEIGHT;
	float s;
	r->rotated = rot; r->src_w = w; r->src_h = h;
	if (integer) {
		int k = 1;
		while ((k + 1) * w <= sw && (k + 1) * h <= sh) k++;
		s = (float) k;
	} else {
		float sx = (float) sw / w, sy = (float) sh / h;
		s = sx < sy ? sx : sy;
	}
	r->scale = s;
	r->w = (int) (w * s + 0.5f); r->h = (int) (h * s + 0.5f);
	if (r->w > sw) r->w = sw;
	if (r->h > sh) r->h = sh;
	r->x = (sw - r->w) / 2; r->y = (sh - r->h) / 2;
}

void wsl_rotate_ccw(uint16_t *dst, const uint16_t *src)
{
	/* source (x, y) -> destination column y, row (W - 1 - x) */
	int x, y;
	for (y = 0; y < WSWAN_HEIGHT; y++)
		for (x = 0; x < WSWAN_WIDTH; x++)
			dst[(WSWAN_WIDTH - 1 - x) * WSWAN_HEIGHT + y] = src[y * WSWAN_WIDTH + x];
}
