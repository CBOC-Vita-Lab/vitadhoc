/* See pk_scale.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 *
 * MMPX: Copyright 2020 Morgan McGuire and Mara Gagiu, MIT (https://casual-effects.com/research/McGuire2021PixelArt/index.html).
 * xBR lv2: Copyright (C) 2011-2016 Hyllian, MIT (libretro glsl-shaders, xbr/shaders/xbr-lv2.glsl). The permission notice of both
 * is in licenses/MMPX-MIT.txt and licenses/xBR-MIT.txt of the VitAdhoc source tree and is reproduced in THIRD_PARTY_NOTICES.md. */
#include "pk_scale.h"

#include <math.h>
#include <string.h>

namespace pk {

const char *scaler_name(ScalerKind k)
{
	switch (k) {
	case SC_MMPX2: return "MMPX 2x";
	case SC_XBR2: return "xBR 2x";
	case SC_XBR3: return "xBR 3x";
	default: return "off";
	}
}

/* ---- tables ----------------------------------------------------------------------------------------------------- */

static uint16_t g_mm_luma[65536];     /* MMPX's order of colours: R + G + B + 1 of the 8 bit expansion, as the reference */
static float g_ur[32], g_ug[64];     /* channel / max, 0..1 */
static float g_wr[32], g_wg[64], g_wb[32];   /* xBR: rgbw = (14.352, 28.176, 5.472) times the channel in 0..1 */
static volatile int g_ready;

void scale_init()
{
	if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
	for (unsigned v = 0; v < 65536; v++) {
		const unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
		g_mm_luma[v] = (uint16_t) (((r << 3) | (r >> 2)) + ((g << 2) | (g >> 4)) + ((b << 3) | (b >> 2)) + 1);
	}
	for (int i = 0; i < 32; i++) g_ur[i] = (float) i / 31.0f;
	for (int i = 0; i < 64; i++) g_ug[i] = (float) i / 63.0f;
	for (int i = 0; i < 32; i++) { g_wr[i] = ((float) i / 31.0f) * 14.352f; g_wb[i] = ((float) i / 31.0f) * 5.472f; }
	for (int i = 0; i < 64; i++) g_wg[i] = ((float) i / 63.0f) * 28.176f;
	__atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
}

static inline float lum565(unsigned v) { return (g_wr[(v >> 11) & 31] + g_wg[(v >> 5) & 63]) + g_wb[v & 31]; }

/* ---- MMPX ------------------------------------------------------------------------------------------------------- */

#define MM_PAD 3
#define MM_BAND 8
#define MM_SPAN 320

#define MM_EQ3(B, A0, A1, A2) ((((B) ^ (A0)) | ((B) ^ (A1)) | ((B) ^ (A2))) == 0)
#define MM_EQ2(B, A0, A1) ((((B) ^ (A0)) | ((B) ^ (A1))) == 0)
#define MM_EQ4(B, A0, A1, A2, A3) ((((B) ^ (A0)) | ((B) ^ (A1)) | ((B) ^ (A2)) | ((B) ^ (A3))) == 0)
#define MM_ANY3(B, A0, A1, A2) ((B) == (A0) || (B) == (A1) || (B) == (A2))
#define MM_NONE2(B, A0, A1) ((B) != (A0) && (B) != (A1))
#define MM_NONE4(B, A0, A1, A2, A3) ((B) != (A0) && (B) != (A1) && (B) != (A2) && (B) != (A3))

/* One pixel whose neighbourhood is not flat. r[k] is the buffer row y + k - 3 (k = 0..6), r[k][x] the pixel at column x. */
static inline void mmpx_pixel(const uint16_t *const r[7], int x, uint16_t out[4])
{
	const uint16_t A = r[2][x - 1], B = r[2][x], C = r[2][x + 1];
	const uint16_t D = r[3][x - 1], E = r[3][x], F = r[3][x + 1];
	const uint16_t G = r[4][x - 1], H = r[4][x], I = r[4][x + 1];
	const uint16_t Q = r[3][x - 2], R = r[3][x + 2];
	const uint16_t P = r[1][x], S = r[5][x];
#define Bl g_mm_luma[B]
#define Dl g_mm_luma[D]
#define El g_mm_luma[E]
#define Fl g_mm_luma[F]
#define Hl g_mm_luma[H]
	uint16_t J = E, K = E, L = E, M = E;
	if ((D == B && D != H && D != F) && (El >= Dl || E == A) && MM_ANY3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) J = D;
	if ((B == F && B != D && B != H) && (El >= Bl || E == C) && MM_ANY3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) K = B;
	if ((H == D && H != F && H != B) && (El >= Hl || E == G) && MM_ANY3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) L = H;
	if ((F == H && F != B && F != D) && (El >= Fl || E == I) && MM_ANY3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) M = F;
	if ((E != F && MM_EQ4(E, C, I, D, Q) && MM_EQ2(F, B, H)) && (F != r[3][x + 3])) K = M = F;
	if ((E != D && MM_EQ4(E, A, G, F, R) && MM_EQ2(D, B, H)) && (D != r[3][x - 3])) J = L = D;
	if ((E != H && MM_EQ4(E, G, I, B, P) && MM_EQ2(H, D, F)) && (H != r[6][x])) L = M = H;
	if ((E != B && MM_EQ4(E, A, C, H, S) && MM_EQ2(B, D, F)) && (B != r[0][x])) J = K = B;
	if (Bl < El && MM_EQ4(E, G, H, I, S) && MM_NONE4(E, A, D, C, F)) J = K = B;
	if (Hl < El && MM_EQ4(E, A, B, C, P) && MM_NONE4(E, D, G, I, F)) L = M = H;
	if (Fl < El && MM_EQ4(E, A, D, G, Q) && MM_NONE4(E, B, C, I, H)) K = M = F;
	if (Dl < El && MM_EQ4(E, C, F, I, R) && MM_NONE4(E, B, A, G, H)) J = L = D;
	if (H != B) {
		if (H != A && H != E && H != C) {
			if (MM_EQ3(H, G, F, R) && MM_NONE2(H, D, r[2][x + 2])) L = M;
			if (MM_EQ3(H, I, D, Q) && MM_NONE2(H, F, r[2][x - 2])) M = L;
		}
		if (B != I && B != G && B != E) {
			if (MM_EQ3(B, A, F, R) && MM_NONE2(B, D, r[4][x + 2])) J = K;
			if (MM_EQ3(B, C, D, Q) && MM_NONE2(B, F, r[4][x - 2])) K = J;
		}
	}
	if (F != D) {
		if (D != I && D != E && D != C) {
			if (MM_EQ3(D, A, H, S) && MM_NONE2(D, B, r[5][x + 1])) J = L;
			if (MM_EQ3(D, G, B, P) && MM_NONE2(D, H, r[1][x + 1])) L = J;
		}
		if (F != E && F != A && F != G) {
			if (MM_EQ3(F, C, H, S) && MM_NONE2(F, B, r[5][x - 1])) K = M;
			if (MM_EQ3(F, I, B, P) && MM_NONE2(F, H, r[1][x - 1])) M = K;
		}
	}
#undef Bl
#undef Dl
#undef El
#undef Fl
#undef Hl
	out[0] = J; out[1] = K; out[2] = L; out[3] = M;
}

static unsigned mmpx_rows(const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1)
{
	uint16_t buf[(MM_BAND + 2 * MM_PAD) * (MM_SPAN + 2 * MM_PAD)];
	const uint16_t *rows[MM_BAND + 2 * MM_PAD];
	unsigned busy = 0;
	for (int ya = y0; ya < y1; ya += MM_BAND) {
		const int yb = ya + MM_BAND < y1 ? ya + MM_BAND : y1;
		for (int xa = 0; xa < w; xa += MM_SPAN) {
			const int xb = xa + MM_SPAN < w ? xa + MM_SPAN : w;
			const int bw = xb - xa + 2 * MM_PAD, nrows = yb - ya + 2 * MM_PAD;
			for (int i = 0; i < nrows; i++) {
				int sy = ya - MM_PAD + i;
				uint16_t *d = buf + (size_t) bw * i;
				sy = sy < 0 ? 0 : sy > h - 1 ? h - 1 : sy;
				const uint16_t *s = src + (size_t) spitch * sy;
				const int lo = xa - MM_PAD, hi = xb + MM_PAD;
				rows[i] = d;
				for (int k = lo; k < 0; k++) d[k - lo] = s[0];
				const int in0 = lo < 0 ? 0 : lo, in1 = hi > w ? w : hi;
				if (in1 > in0) memcpy(d + (in0 - lo), s + in0, (size_t) (in1 - in0) * 2u);
				for (int k = w; k < hi; k++) d[k - lo] = s[w - 1];
			}
			for (int y = ya; y < yb; y++) {
				const uint16_t *const *r = rows + (y - ya);
				uint16_t *o = dst + (size_t) dpitch * 2u * (size_t) y + 2u * (size_t) xa;
				for (int x = MM_PAD; x < MM_PAD + (xb - xa); x++, o += 2) {
					const uint16_t E = r[3][x];
					if ((((r[2][x - 1] ^ E) | (r[2][x] ^ E) | (r[2][x + 1] ^ E) | (r[3][x - 1] ^ E) | (r[3][x + 1] ^ E) |
					      (r[4][x - 1] ^ E) | (r[4][x] ^ E) | (r[4][x + 1] ^ E)) == 0)) {
						o[0] = o[1] = o[dpitch] = o[dpitch + 1] = E;
					} else {
						uint16_t q[4];
						mmpx_pixel(r, x, q);
						o[0] = q[0]; o[1] = q[1]; o[dpitch] = q[2]; o[dpitch + 1] = q[3];
						busy++;
					}
				}
			}
		}
	}
	return busy;
}

/* ---- xBR lv2 ---------------------------------------------------------------------------------------------------- */

typedef float v4f __attribute__((vector_size(16)));
typedef int v4i __attribute__((vector_size(16)));

static inline v4f V4(float a, float b, float c, float d) { v4f v = { a, b, c, d }; return v; }
static inline v4f vsplat(float a) { return V4(a, a, a, a); }
static inline v4f vabs(v4f a) { const v4i m = { 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff }; return (v4f) ((v4i) a & m); }
static inline v4f vmask(v4i m) { return (v4f) (m & (v4i) vsplat(1.0f)); }      /* 1.0 where the comparison held, else 0.0 */
static inline v4f vstep(v4f edge, v4f x) { return vmask(x >= edge); }          /* GLSL step */
static inline v4f vmax(v4f a, v4f b) { return (v4f) (((v4i) (a > b) & (v4i) a) | (~(v4i) (a > b) & (v4i) b)); }
static inline v4f vclamp01(v4f a) { return vmax(vsplat(0.0f), (v4f) (((v4i) (a < vsplat(1.0f)) & (v4i) a) | (~(v4i) (a < vsplat(1.0f)) & (v4i) vsplat(1.0f)))); }
static inline v4f vdf(v4f a, v4f b) { return vabs(a - b); }
static inline v4f vdiff(v4f a, v4f b) { return vmask(a != b); }
static inline v4f veq(v4f a, v4f b) { return vmask(vdf(a, b) <= vsplat(15.0f)); }
static inline v4f vneq(v4f a, v4f b) { return vsplat(1.0f) - veq(a, b); }
static inline v4f vwd(v4f a, v4f b, v4f c, v4f d, v4f e, v4f f, v4f g, v4f h)
{
	return vdf(a, b) + vdf(a, c) + vdf(d, e) + vdf(d, f) + vsplat(4.0f) * vdf(g, h);
}

/* the shader's constants, lane order as in the GLSL */
static const float kAo[4] = { 1.0f, -1.0f, -1.0f, 1.0f }, kBo[4] = { 1.0f, 1.0f, -1.0f, -1.0f }, kCo[4] = { 1.5f, 0.5f, -0.5f, 0.5f };
static const float kAx[4] = { 1.0f, -1.0f, -1.0f, 1.0f }, kBx[4] = { 0.5f, 2.0f, -0.5f, -2.0f }, kCx[4] = { 1.0f, 1.0f, -0.5f, 0.0f };
static const float kAy[4] = { 1.0f, -1.0f, -1.0f, 1.0f }, kBy[4] = { 2.0f, 0.5f, -2.0f, -0.5f }, kCy[4] = { 2.0f, 0.0f, -1.0f, 0.5f };
static const float kCi = 0.25f;
static const float kLv2 = 2.0f;

static inline float clamp01f(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* The four ramps of one sub pixel (scale s, sub pixel sx, sy), per lane: what the shader computes from fract(texCoord * TextureSize). */
struct SubRamp { v4f f45i, f45, f30, f60; };
static void make_ramps(int s, SubRamp *out)
{
	const float S = (float) s;
	const float delta = 1.0f / S, dl[4] = { 0.5f / S, 1.0f / S, 0.5f / S, 1.0f / S }, du[4] = { 1.0f / S, 0.5f / S, 1.0f / S, 0.5f / S };
	for (int sy = 0; sy < s; sy++)
		for (int sx = 0; sx < s; sx++) {
			const float fpx = ((float) sx + 0.5f) / S, fpy = ((float) sy + 0.5f) / S;
			float a[4], b[4], c[4], d[4];
			for (int l = 0; l < 4; l++) {
				const float fx = kAo[l] * fpy + kBo[l] * fpx, fxl = kAx[l] * fpy + kBx[l] * fpx, fxu = kAy[l] * fpy + kBy[l] * fpx;
				a[l] = clamp01f((fx + delta - kCo[l] - kCi) / (2.0f * delta));
				b[l] = clamp01f((fx + delta - kCo[l]) / (2.0f * delta));
				c[l] = clamp01f((fxl + dl[l] - kCx[l]) / (2.0f * dl[l]));
				d[l] = clamp01f((fxu + du[l] - kCy[l]) / (2.0f * du[l]));
			}
			SubRamp &r = out[sy * s + sx];
			r.f45i = V4(a[0], a[1], a[2], a[3]);
			r.f45 = V4(b[0], b[1], b[2], b[3]);
			r.f30 = V4(c[0], c[1], c[2], c[3]);
			r.f60 = V4(d[0], d[1], d[2], d[3]);
		}
}

static inline void unpack565(unsigned v, float c[3])
{
	c[0] = g_ur[(v >> 11) & 31];
	c[1] = g_ug[(v >> 5) & 63];
	c[2] = g_ur[v & 31];
}
static inline unsigned pack565(const float c[3])
{
	int r = (int) (c[0] * 31.0f + 0.5f), g = (int) (c[1] * 63.0f + 0.5f), b = (int) (c[2] * 31.0f + 0.5f);
	r = r < 0 ? 0 : r > 31 ? 31 : r; g = g < 0 ? 0 : g > 63 ? 63 : g; b = b < 0 ? 0 : b > 31 ? 31 : b;
	return (unsigned) ((r << 11) | (g << 5) | b);
}
static inline void mix3(const float x[3], const float y[3], float a, float out[3])
{
	for (int k = 0; k < 3; k++) out[k] = x[k] * (1.0f - a) + y[k] * a;
}
static inline float cdf(const float a[3], const float b[3])
{
	const float d0 = fabsf(a[0] - b[0]), d1 = fabsf(a[1] - b[1]), d2 = fabsf(a[2] - b[2]);
	return d0 + d1 + d2;
}

/* the shader's last lines: the colour of one output pixel from its four maxima and the choice of side (px) */
static inline unsigned xbr_resolve(const float E[3], const float cand[4][3], const float mx[4])
{
	float res1[3] = { E[0], E[1], E[2] }, res2[3] = { E[0], E[1], E[2] }, t[3];
	if (mx[0] > 0.0f) { mix3(res1, cand[0], mx[0], t); res1[0] = t[0]; res1[1] = t[1]; res1[2] = t[2]; }
	if (mx[2] > 0.0f) { mix3(res1, cand[2], mx[2], t); res1[0] = t[0]; res1[1] = t[1]; res1[2] = t[2]; }
	if (mx[1] > 0.0f) { mix3(res2, cand[1], mx[1], t); res2[0] = t[0]; res2[1] = t[1]; res2[2] = t[2]; }
	if (mx[3] > 0.0f) { mix3(res2, cand[3], mx[3], t); res2[0] = t[0]; res2[1] = t[1]; res2[2] = t[2]; }
	const float d1 = cdf(E, res1), d2 = cdf(E, res2);
	return pack565(d2 >= d1 ? res2 : res1);
}

template <int s>
static unsigned xbr_rows(const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1, ScaleScratch &sc)
{
	SubRamp ramps[9];
	make_ramps(s, ramps);   /* a few dozen floats: cheap next to a picture */
	/* the luminance plane of the stripe and two rows / columns around it, the border repeated */
	const int lp = w + 4, lr = y1 - y0 + 4;
	if ((int) sc.lum.size() < lp * lr) sc.lum.resize((size_t) lp * lr);
	float *L = &sc.lum[0];
	for (int r = 0; r < lr; r++) {
		int sy = y0 - 2 + r;
		sy = sy < 0 ? 0 : sy > h - 1 ? h - 1 : sy;
		const uint16_t *sp = src + (size_t) spitch * sy;
		float *d = L + (size_t) lp * r;
		d[0] = d[1] = lum565(sp[0]);
		for (int x = 0; x < w; x++) d[x + 2] = lum565(sp[x]);
		d[w + 2] = d[w + 3] = d[w + 1];
	}
	unsigned busy = 0;
	const v4f ONE = vsplat(1.0f);
	for (int y = y0; y < y1; y++) {
		const float *lc = L + (size_t) lp * (y - y0 + 2) + 2;       /* lc[0] = the pixel (0, y) */
		int ym = y > 0 ? y - 1 : 0, yp = y < h - 1 ? y + 1 : h - 1;
		const uint16_t *rm = src + (size_t) spitch * ym, *r0 = src + (size_t) spitch * y, *rp = src + (size_t) spitch * yp;
		uint16_t *o = dst + (size_t) dpitch * (size_t) (y * s);
		for (int x = 0; x < w; x++) {
			const float lE = lc[x], lB = lc[x - lp], lD = lc[x - 1], lF = lc[x + 1], lH = lc[x + lp];
			/* a flat spot: no lane of irlv0 is set, nothing to blend, the pixel is copied */
			if ((lE == lF || lE == lH) && (lE == lB || lE == lF) && (lE == lD || lE == lB) && (lE == lH || lE == lD)) {
				const uint16_t E = r0[x];
				uint16_t *q = o + (size_t) s * x;
				for (int sy = 0; sy < s; sy++) for (int sx = 0; sx < s; sx++) q[(size_t) dpitch * sy + sx] = E;
				continue;
			}
			busy++;
			const float lA1 = lc[x - 1 - 2 * lp], lB1 = lc[x - 2 * lp], lC1 = lc[x + 1 - 2 * lp];
			const float lA = lc[x - 1 - lp], lC = lc[x + 1 - lp];
			const float lG = lc[x - 1 + lp], lI = lc[x + 1 + lp];
			const float lG5 = lc[x - 1 + 2 * lp], lH5 = lc[x + 2 * lp], lI5 = lc[x + 1 + 2 * lp];
			const float lA0 = lc[x - 2 - lp], lD0 = lc[x - 2], lG0 = lc[x - 2 + lp];
			const float lC4 = lc[x + 2 - lp], lF4 = lc[x + 2], lI4 = lc[x + 2 + lp];
			const v4f b = V4(lB, lD, lH, lF), c = V4(lC, lA, lG, lI), d = V4(lD, lH, lF, lB), e = vsplat(lE), f = V4(lF, lB, lD, lH);
			const v4f g = V4(lG, lI, lC, lA), hh = V4(lH, lF, lB, lD), i = V4(lI, lC, lA, lG);
			const v4f i4 = V4(lI4, lC1, lA0, lG5), i5 = V4(lI5, lC4, lA1, lG0), h5 = V4(lH5, lF4, lB1, lD0), f4 = V4(lF4, lB1, lD0, lH5);
			const v4f irlv0 = vdiff(e, f) * vdiff(e, hh);
			/* CORNER_C */
			const v4f irlv1 = irlv0 * (vneq(f, b) * vneq(f, c) + vneq(hh, d) * vneq(hh, g) +
			                           veq(e, i) * (vneq(f, f4) * vneq(f, i4) + vneq(hh, h5) * vneq(hh, i5)) + veq(e, g) + veq(e, c));
			const v4f irlv2l = vdiff(e, g) * vdiff(d, g), irlv2u = vdiff(e, c) * vdiff(b, c);
			const v4f wd1 = vwd(e, c, g, i, h5, f4, hh, f), wd2 = vwd(hh, d, i5, f, i4, b, e, i);
			const v4f edri = vstep(wd1, wd2) * irlv0;
			const v4f edr = vstep(wd1 + vsplat(0.1f), wd2) * vstep(vsplat(0.5f), irlv1);
			const v4f edr_l = vstep(vsplat(kLv2) * vdf(f, g), vdf(hh, c)) * irlv2l * edr;
			const v4f edr_u = vstep(vsplat(kLv2) * vdf(hh, c), vdf(f, g)) * irlv2u * edr;
			/* not flat, but nothing to blend (no lane's edge test passed): the pixel is copied */
			{
				const v4f any = edr_l + edr_u + edr + edri;
				if (any[0] + any[1] + any[2] + any[3] <= 0.0f) {
					const uint16_t E = r0[x];
					uint16_t *q = o + (size_t) s * x;
					for (int sy = 0; sy < s; sy++) for (int sx = 0; sx < s; sx++) q[(size_t) dpitch * sy + sx] = E;
					continue;
				}
			}
			const v4f px = vstep(vdf(e, f), vdf(e, hh));
			/* the colours that can be mixed in: the pixel and its four neighbours, read only now */
			const int xm = x > 0 ? x - 1 : 0, xp = x < w - 1 ? x + 1 : w - 1;
			float cE[3], cB[3], cD[3], cF[3], cH[3];
			unpack565(r0[x], cE); unpack565(rm[x], cB); unpack565(r0[xm], cD); unpack565(r0[xp], cF); unpack565(rp[x], cH);
			float cand[4][3];
			const float pxv[4] = { px[0], px[1], px[2], px[3] };
			for (int k = 0; k < 3; k++) {
				cand[0][k] = pxv[0] > 0.5f ? cF[k] : cH[k];
				cand[1][k] = pxv[1] > 0.5f ? cB[k] : cF[k];
				cand[2][k] = pxv[2] > 0.5f ? cD[k] : cB[k];
				cand[3][k] = pxv[3] > 0.5f ? cH[k] : cD[k];
			}
			const uint16_t E16 = r0[x];
			uint16_t *q = o + (size_t) s * x;
			for (int sy = 0; sy < s; sy++)
				for (int sx = 0; sx < s; sx++) {
					const SubRamp &rp_ = ramps[sy * s + sx];
					const v4f mxv = vmax(vmax(edr_l * rp_.f30, edr_u * rp_.f60), vmax(edr * rp_.f45, edri * rp_.f45i));
					const float m0 = mxv[0], m1 = mxv[1], m2 = mxv[2], m3 = mxv[3];
					if (m0 + m1 + m2 + m3 <= 0.0f) { q[(size_t) dpitch * sy + sx] = E16; continue; }
					const float mx[4] = { m0, m1, m2, m3 };
					q[(size_t) dpitch * sy + sx] = (uint16_t) xbr_resolve(cE, cand, mx);
				}
			(void) ONE;
		}
	}
	return busy;
}

unsigned scale_rows(ScalerKind k, const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1, ScaleScratch &sc)
{
	if (!src || !dst || w < 1 || h < 1) return 0;
	if (y0 < 0) y0 = 0;
	if (y1 > h) y1 = h;
	if (y1 <= y0) return 0;
	scale_init();
	switch (k) {
	case SC_MMPX2: return mmpx_rows(src, w, h, spitch, dst, dpitch, y0, y1);
	case SC_XBR2: return xbr_rows<2>(src, w, h, spitch, dst, dpitch, y0, y1, sc);
	case SC_XBR3: return xbr_rows<3>(src, w, h, spitch, dst, dpitch, y0, y1, sc);
	default: return 0;
	}
}

/* ---- the literal transcription of the fragment program ------------------------------------------------------- */

namespace {
struct Vec4 { float v[4]; };
Vec4 mk(float a, float b, float c, float d) { Vec4 r = { { a, b, c, d } }; return r; }
Vec4 spl(float a) { return mk(a, a, a, a); }
Vec4 add(Vec4 a, Vec4 b) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i]; return r; }
Vec4 sub(Vec4 a, Vec4 b) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] - b.v[i]; return r; }
Vec4 mul(Vec4 a, Vec4 b) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; return r; }
Vec4 divv(Vec4 a, Vec4 b) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] / b.v[i]; return r; }
Vec4 df(Vec4 a, Vec4 b) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = fabsf(a.v[i] - b.v[i]); return r; }
Vec4 diff(Vec4 a, Vec4 b) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] != b.v[i] ? 1.0f : 0.0f; return r; }
Vec4 step(Vec4 edge, Vec4 x) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = x.v[i] < edge.v[i] ? 0.0f : 1.0f; return r; }
Vec4 eq(Vec4 a, Vec4 b) { return step(df(a, b), spl(15.0f)); }
Vec4 neq(Vec4 a, Vec4 b) { return sub(spl(1.0f), eq(a, b)); }
Vec4 wd(Vec4 a, Vec4 b, Vec4 c, Vec4 d, Vec4 e, Vec4 f, Vec4 g, Vec4 h)
{
	return add(add(add(add(df(a, b), df(a, c)), df(d, e)), df(d, f)), mul(spl(4.0f), df(g, h)));
}
Vec4 vclamp(Vec4 a) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] < 0.0f ? 0.0f : a.v[i] > 1.0f ? 1.0f : a.v[i]; return r; }
Vec4 vmaxx(Vec4 a, Vec4 b) { Vec4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return r; }
Vec4 rot(Vec4 a, int x, int y, int z, int w) { return mk(a.v[x], a.v[y], a.v[z], a.v[w]); }   /* swizzle */
struct Rgb { float c[3]; };
}  // namespace

void xbr_reference(const uint16_t *src, int w, int h, int spitch, int scale, uint16_t *dst, int dpitch)
{
	scale_init();
	const float XS = (float) scale;
	const Vec4 delta = spl(1.0f / XS), delta_l = mk(0.5f / XS, 1.0f / XS, 0.5f / XS, 1.0f / XS), delta_u = rot(delta_l, 1, 0, 3, 2);
	const Vec4 Ao = mk(1, -1, -1, 1), Bo = mk(1, 1, -1, -1), Co = mk(1.5f, 0.5f, -0.5f, 0.5f);
	const Vec4 Ax = mk(1, -1, -1, 1), Bx = mk(0.5f, 2, -0.5f, -2), Cx = mk(1, 1, -0.5f, 0);
	const Vec4 Ay = mk(1, -1, -1, 1), By = mk(2, 0.5f, -2, -0.5f), Cy = mk(2, 0, -1, 0.5f);
	const Vec4 Ci = spl(0.25f);
	for (int oy = 0; oy < h * scale; oy++)
		for (int ox = 0; ox < w * scale; ox++) {
			const int px_ = ox / scale, py_ = oy / scale;
			const float fpx = ((float) (ox % scale) + 0.5f) / XS, fpy = ((float) (oy % scale) + 0.5f) / XS;
			auto at = [&](int dx, int dy) {
				int xx = px_ + dx, yy = py_ + dy;
				xx = xx < 0 ? 0 : xx > w - 1 ? w - 1 : xx; yy = yy < 0 ? 0 : yy > h - 1 ? h - 1 : yy;
				return (unsigned) src[(size_t) spitch * yy + xx];
			};
			auto col = [&](unsigned v) { Rgb r; unpack565(v, r.c); return r; };
			auto dotw = [&](unsigned v) { Rgb r = col(v); return r.c[0] * 14.352f + r.c[1] * 28.176f + r.c[2] * 5.472f; };
			const unsigned uA1 = at(-1, -2), uB1 = at(0, -2), uC1 = at(1, -2), uA = at(-1, -1), uB = at(0, -1), uC = at(1, -1);
			const unsigned uD = at(-1, 0), uE = at(0, 0), uF = at(1, 0), uG = at(-1, 1), uH = at(0, 1), uI = at(1, 1);
			const unsigned uG5 = at(-1, 2), uH5 = at(0, 2), uI5 = at(1, 2), uA0 = at(-2, -1), uD0 = at(-2, 0), uG0 = at(-2, 1);
			const unsigned uC4 = at(2, -1), uF4 = at(2, 0), uI4 = at(2, 1);
			const Vec4 b = mk(dotw(uB), dotw(uD), dotw(uH), dotw(uF));
			const Vec4 c = mk(dotw(uC), dotw(uA), dotw(uG), dotw(uI));
			const Vec4 d = rot(b, 1, 2, 3, 0), e = spl(dotw(uE)), f = rot(b, 3, 0, 1, 2), g = rot(c, 2, 3, 0, 1), hh = rot(b, 2, 3, 0, 1), i = rot(c, 3, 0, 1, 2);
			const Vec4 i4 = mk(dotw(uI4), dotw(uC1), dotw(uA0), dotw(uG5));
			const Vec4 i5 = mk(dotw(uI5), dotw(uC4), dotw(uA1), dotw(uG0));
			const Vec4 h5 = mk(dotw(uH5), dotw(uF4), dotw(uB1), dotw(uD0));
			const Vec4 f4 = rot(h5, 1, 2, 3, 0);
			const Vec4 fx = add(mul(Ao, spl(fpy)), mul(Bo, spl(fpx)));
			const Vec4 fx_l = add(mul(Ax, spl(fpy)), mul(Bx, spl(fpx)));
			const Vec4 fx_u = add(mul(Ay, spl(fpy)), mul(By, spl(fpx)));
			const Vec4 irlv0 = mul(diff(e, f), diff(e, hh));
			Vec4 irlv1 = mul(irlv0, add(add(add(mul(neq(f, b), neq(f, c)), mul(neq(hh, d), neq(hh, g))),
			                     mul(eq(e, i), add(mul(neq(f, f4), neq(f, i4)), mul(neq(hh, h5), neq(hh, i5))))), add(eq(e, g), eq(e, c))));
			const Vec4 irlv2l = mul(diff(e, g), diff(d, g)), irlv2u = mul(diff(e, c), diff(b, c));
			const Vec4 two_d = mul(spl(2.0f), delta), two_dl = mul(spl(2.0f), delta_l), two_du = mul(spl(2.0f), delta_u);
			Vec4 fx45i = vclamp(divv(sub(sub(add(fx, delta), Co), Ci), two_d));
			Vec4 fx45 = vclamp(divv(sub(add(fx, delta), Co), two_d));
			Vec4 fx30 = vclamp(divv(sub(add(fx_l, delta_l), Cx), two_dl));
			Vec4 fx60 = vclamp(divv(sub(add(fx_u, delta_u), Cy), two_du));
			const Vec4 wd1 = wd(e, c, g, i, h5, f4, hh, f), wd2 = wd(hh, d, i5, f, i4, b, e, i);
			const Vec4 edri = mul(step(wd1, wd2), irlv0);
			const Vec4 edr = mul(step(add(wd1, spl(0.1f)), wd2), step(spl(0.5f), irlv1));
			const Vec4 edr_l = mul(mul(step(mul(spl(2.0f), df(f, g)), df(hh, c)), irlv2l), edr);
			const Vec4 edr_u = mul(mul(step(mul(spl(2.0f), df(hh, c)), df(f, g)), irlv2u), edr);
			fx45 = mul(edr, fx45); fx30 = mul(edr_l, fx30); fx60 = mul(edr_u, fx60); fx45i = mul(edri, fx45i);
			const Vec4 pxv = step(df(e, f), df(e, hh));
			const Vec4 maximos = vmaxx(vmaxx(fx30, fx60), vmaxx(fx45, fx45i));
			const Rgb E = col(uE), Bc = col(uB), Dc = col(uD), Fc = col(uF), Hc = col(uH);
			auto mixc = [&](Rgb x, Rgb y, float a) { Rgb r; for (int k = 0; k < 3; k++) r.c[k] = x.c[k] * (1.0f - a) + y.c[k] * a; return r; };
			Rgb res1 = E;
			res1 = mixc(res1, mixc(Hc, Fc, pxv.v[0]), maximos.v[0]);
			res1 = mixc(res1, mixc(Bc, Dc, pxv.v[2]), maximos.v[2]);
			Rgb res2 = E;
			res2 = mixc(res2, mixc(Fc, Bc, pxv.v[1]), maximos.v[1]);
			res2 = mixc(res2, mixc(Dc, Hc, pxv.v[3]), maximos.v[3]);
			const float s1 = cdf(E.c, res1.c), s2 = cdf(E.c, res2.c);
			const Rgb res = s2 < s1 ? res1 : res2;     /* mix(res1, res2, step(c_df(E, res1), c_df(E, res2))) */
			dst[(size_t) dpitch * oy + ox] = (uint16_t) pack565(res.c);
		}
}

}  // namespace pk
