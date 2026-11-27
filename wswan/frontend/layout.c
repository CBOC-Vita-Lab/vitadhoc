/* See layout.h. Copyright CBOC-Vita. MIT licence. */
#include "layout.h"
#include "host.h"

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(WSL_NO_NEON)
#include <arm_neon.h>
#define WSL_NEON 1
#endif

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
	/* source (x, y) -> destination column y, row (W - 1 - x).  Done in 8 x 8 blocks (both sides are multiples of 8): a block
	   reads 8 rows of 8 pixels and writes 8 rows of 8, so the strided writes of a pixel-by-pixel loop stay in a few cache
	   lines; on ARM the block is transposed in registers. */
	int bx, by;
	for (by = 0; by < WSWAN_HEIGHT; by += 8)
		for (bx = 0; bx < WSWAN_WIDTH; bx += 8) {
			const uint16_t *s = src + by * WSWAN_WIDTH + bx;
			uint16_t *d = dst + (WSWAN_WIDTH - 1 - (bx + 7)) * WSWAN_HEIGHT + by;
#ifdef WSL_NEON
			/* rows r0..r7 of the block; the output row k (k = 0..7) is source column 7 - k read down the rows */
			uint16x8_t r0 = vld1q_u16(s), r1 = vld1q_u16(s + WSWAN_WIDTH), r2 = vld1q_u16(s + 2 * WSWAN_WIDTH),
			           r3 = vld1q_u16(s + 3 * WSWAN_WIDTH), r4 = vld1q_u16(s + 4 * WSWAN_WIDTH), r5 = vld1q_u16(s + 5 * WSWAN_WIDTH),
			           r6 = vld1q_u16(s + 6 * WSWAN_WIDTH), r7 = vld1q_u16(s + 7 * WSWAN_WIDTH);
			uint16x8x2_t t0 = vtrnq_u16(r0, r1), t1 = vtrnq_u16(r2, r3), t2 = vtrnq_u16(r4, r5), t3 = vtrnq_u16(r6, r7);
			uint32x4x2_t u0 = vtrnq_u32(vreinterpretq_u32_u16(t0.val[0]), vreinterpretq_u32_u16(t1.val[0]));
			uint32x4x2_t u1 = vtrnq_u32(vreinterpretq_u32_u16(t0.val[1]), vreinterpretq_u32_u16(t1.val[1]));
			uint32x4x2_t u2 = vtrnq_u32(vreinterpretq_u32_u16(t2.val[0]), vreinterpretq_u32_u16(t3.val[0]));
			uint32x4x2_t u3 = vtrnq_u32(vreinterpretq_u32_u16(t2.val[1]), vreinterpretq_u32_u16(t3.val[1]));
			/* columns c = 0..7 of the block, as 8 pixels down the rows */
			uint16x8_t c0 = vreinterpretq_u16_u32(vcombine_u32(vget_low_u32(u0.val[0]), vget_low_u32(u2.val[0])));
			uint16x8_t c1 = vreinterpretq_u16_u32(vcombine_u32(vget_low_u32(u1.val[0]), vget_low_u32(u3.val[0])));
			uint16x8_t c2 = vreinterpretq_u16_u32(vcombine_u32(vget_low_u32(u0.val[1]), vget_low_u32(u2.val[1])));
			uint16x8_t c3 = vreinterpretq_u16_u32(vcombine_u32(vget_low_u32(u1.val[1]), vget_low_u32(u3.val[1])));
			uint16x8_t c4 = vreinterpretq_u16_u32(vcombine_u32(vget_high_u32(u0.val[0]), vget_high_u32(u2.val[0])));
			uint16x8_t c5 = vreinterpretq_u16_u32(vcombine_u32(vget_high_u32(u1.val[0]), vget_high_u32(u3.val[0])));
			uint16x8_t c6 = vreinterpretq_u16_u32(vcombine_u32(vget_high_u32(u0.val[1]), vget_high_u32(u2.val[1])));
			uint16x8_t c7 = vreinterpretq_u16_u32(vcombine_u32(vget_high_u32(u1.val[1]), vget_high_u32(u3.val[1])));
			vst1q_u16(d + 7 * WSWAN_HEIGHT, c0); vst1q_u16(d + 6 * WSWAN_HEIGHT, c1);
			vst1q_u16(d + 5 * WSWAN_HEIGHT, c2); vst1q_u16(d + 4 * WSWAN_HEIGHT, c3);
			vst1q_u16(d + 3 * WSWAN_HEIGHT, c4); vst1q_u16(d + 2 * WSWAN_HEIGHT, c5);
			vst1q_u16(d + 1 * WSWAN_HEIGHT, c6); vst1q_u16(d, c7);
#else
			int x, y;
			for (y = 0; y < 8; y++)
				for (x = 0; x < 8; x++)
					d[(7 - x) * WSWAN_HEIGHT + y] = s[y * WSWAN_WIDTH + x];
#endif
		}
}

/* the pixel-by-pixel definition (tests compare the blocked version with it) */
void wsl_rotate_ccw_ref(uint16_t *dst, const uint16_t *src)
{
	int x, y;
	for (y = 0; y < WSWAN_HEIGHT; y++)
		for (x = 0; x < WSWAN_WIDTH; x++)
			dst[(WSWAN_WIDTH - 1 - x) * WSWAN_HEIGHT + y] = src[y * WSWAN_WIDTH + x];
}
