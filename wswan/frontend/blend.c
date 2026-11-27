/* See blend.h. Copyright CBOC-Vita. MIT licence. */
#include "blend.h"
#include <stdlib.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define WSB_NEON 1
#endif

int wsb_has_neon(void)
{
#ifdef WSB_NEON
	return 1;
#else
	return 0;
#endif
}

int wsb_init(wsb *b, unsigned w, unsigned h, uint64_t en, uint64_t ed, uint64_t dn, uint64_t dd)
{
	memset(b, 0, sizeof *b);
	if (!en || !ed || !dn || !dd) return 1;
	b->te = ed * dn;       /* ticks of 1/(en*dn) s */
	b->td = dd * en;
	b->pixels = w * h;
	b->ring = (uint16_t *) calloc((size_t) WSB_MAX_FRAMES * b->pixels, 2);
	b->prev = (uint16_t *) calloc(b->pixels, 2);
	if (!b->ring || !b->prev) { wsb_free(b); return 1; }
	return 0;
}
void wsb_free(wsb *b) { free(b->ring); free(b->prev); b->ring = b->prev = NULL; }
void wsb_reset(wsb *b) { b->t = 0; b->emu_done = 0; b->have_prev = 0; }

static void span(const wsb *b, uint64_t *first, uint64_t *last)
{
	*first = b->t / b->te;
	*last = (b->t + b->td - 1) / b->te;
}
int wsb_frames_needed(const wsb *b)
{
	uint64_t f, l; span(b, &f, &l);
	return l + 1 > b->emu_done ? (int) (l + 1 - b->emu_done) : 0;
}
void wsb_submit(wsb *b, const uint16_t *frame)
{
	memcpy(b->ring + (size_t) (b->emu_done % WSB_MAX_FRAMES) * b->pixels, frame, (size_t) b->pixels * 2);
	b->emu_done++;
}

int wsb_weights(const wsb *b, uint64_t idx[3], unsigned w[3])
{
	uint64_t f, l, ov[3], rem[3]; int n, i;
	span(b, &f, &l);
	n = (int) (l - f + 1);
	if (n > 3) n = 3;     /* cannot happen while td < 2*te; guard */
	for (i = 0; i < n; i++) {
		uint64_t j = f + i, s = j * b->te, e = s + b->te;
		uint64_t a0 = s > b->t ? s : b->t, a1 = e < b->t + b->td ? e : b->t + b->td;
		idx[i] = j; ov[i] = a1 > a0 ? a1 - a0 : 0;
	}
	/* 8 bit weights, largest remainder, exact: ov*256 / td */
	{
		unsigned sum = 0;
		for (i = 0; i < n; i++) {
			uint64_t num = ov[i] * 256u;       /* ov <= td < 2^56 for any real rate pair: fits */
			w[i] = (unsigned) (num / b->td); rem[i] = num % b->td; sum += w[i];
		}
		while (sum < 256) {
			int best = 0;
			for (i = 1; i < n; i++) if (rem[i] > rem[best]) best = i;
			w[best]++; rem[best] = 0; sum++;
		}
	}
	for (i = n; i < 3; i++) { idx[i] = idx[n - 1]; w[i] = 0; }
	return n;
}

void wsb_mix3_scalar(uint16_t *out, const uint16_t *a, const uint16_t *b, const uint16_t *c, unsigned wa, unsigned wb, unsigned wc, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		unsigned pa = a[i], pb = b[i], pc = c[i];
		unsigned r = ((pa >> 11) * wa + (pb >> 11) * wb + (pc >> 11) * wc + 128) >> 8;
		unsigned g = (((pa >> 5) & 63) * wa + ((pb >> 5) & 63) * wb + ((pc >> 5) & 63) * wc + 128) >> 8;
		unsigned bl = ((pa & 31) * wa + (pb & 31) * wb + (pc & 31) * wc + 128) >> 8;
		out[i] = (uint16_t) ((r << 11) | (g << 5) | bl);
	}
}

void wsb_mix3(uint16_t *out, const uint16_t *a, const uint16_t *b, const uint16_t *c, unsigned wa, unsigned wb, unsigned wc, size_t n)
{
	size_t i = 0;
#ifdef WSB_NEON
	const uint16x8_t m6 = vdupq_n_u16(63), m5 = vdupq_n_u16(31), rnd = vdupq_n_u16(128);
	for (; i + 8 <= n; i += 8) {
		uint16x8_t pa = vld1q_u16(a + i), pb = vld1q_u16(b + i), pc = vld1q_u16(c + i);
		uint16x8_t r = vmulq_n_u16(vshrq_n_u16(pa, 11), (uint16_t) wa);
		uint16x8_t g = vmulq_n_u16(vandq_u16(vshrq_n_u16(pa, 5), m6), (uint16_t) wa);
		uint16x8_t bl = vmulq_n_u16(vandq_u16(pa, m5), (uint16_t) wa);
		r = vmlaq_n_u16(r, vshrq_n_u16(pb, 11), (uint16_t) wb);
		g = vmlaq_n_u16(g, vandq_u16(vshrq_n_u16(pb, 5), m6), (uint16_t) wb);
		bl = vmlaq_n_u16(bl, vandq_u16(pb, m5), (uint16_t) wb);
		r = vmlaq_n_u16(r, vshrq_n_u16(pc, 11), (uint16_t) wc);
		g = vmlaq_n_u16(g, vandq_u16(vshrq_n_u16(pc, 5), m6), (uint16_t) wc);
		bl = vmlaq_n_u16(bl, vandq_u16(pc, m5), (uint16_t) wc);
		r = vshrq_n_u16(vaddq_u16(r, rnd), 8); g = vshrq_n_u16(vaddq_u16(g, rnd), 8); bl = vshrq_n_u16(vaddq_u16(bl, rnd), 8);
		vst1q_u16(out + i, vorrq_u16(vorrq_u16(vshlq_n_u16(r, 11), vshlq_n_u16(g, 5)), bl));
	}
#endif
	wsb_mix3_scalar(out + i, a + i, b + i, c + i, wa, wb, wc, n - i);
}

int wsb_present(wsb *b, uint16_t *out)
{
	uint64_t idx[3]; unsigned w[3]; int n, i;
	const uint16_t *f[3];
	n = wsb_weights(b, idx, w);
	for (i = 0; i < 3; i++) {
		uint64_t j = idx[i];
		if (b->emu_done == 0) { f[i] = b->prev; continue; }
		if (j >= b->emu_done) j = b->emu_done - 1;                       /* not run yet: newest */
		if (b->emu_done - j > WSB_MAX_FRAMES) j = b->emu_done - WSB_MAX_FRAMES;   /* too old */
		f[i] = b->ring + (size_t) (j % WSB_MAX_FRAMES) * b->pixels;
	}
	wsb_mix3(out, f[0], f[1], f[2], w[0], w[1], w[2], b->pixels);
	if (b->ghost > 0 && b->have_prev) wsb_mix3(out, out, b->prev, b->prev, 256 - b->ghost, b->ghost, 0, b->pixels);
	memcpy(b->prev, out, (size_t) b->pixels * 2);
	b->have_prev = 1;
	b->t += b->td;
	return n;
}
