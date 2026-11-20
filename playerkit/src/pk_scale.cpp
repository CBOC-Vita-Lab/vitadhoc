/* See pk_scale.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 *
 * MMPX: Copyright 2020 Morgan McGuire and Mara Gagiu, MIT (https://casual-effects.com/research/McGuire2021PixelArt/index.html).
 * xBR lv2: Copyright (C) 2011-2016 Hyllian, MIT (libretro glsl-shaders, xbr/shaders/xbr-lv2.glsl). The permission notice of both
 * is in licenses/MMPX-MIT.txt and licenses/xBR-MIT.txt of the VitAdhoc source tree and is reproduced in THIRD_PARTY_NOTICES.md. */
#include "pk_scale.h"

#include <math.h>
#include <string.h>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

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

static unsigned mmpx_rows(const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1, int cx0, int cx1)
{
	uint16_t buf[(MM_BAND + 2 * MM_PAD) * (MM_SPAN + 2 * MM_PAD)];
	const uint16_t *rows[MM_BAND + 2 * MM_PAD];
	unsigned busy = 0;
	for (int ya = y0; ya < y1; ya += MM_BAND) {
		const int yb = ya + MM_BAND < y1 ? ya + MM_BAND : y1;
		for (int xa = cx0; xa < cx1; xa += MM_SPAN) {
			const int xb = xa + MM_SPAN < cx1 ? xa + MM_SPAN : cx1;
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
static unsigned xbr_rows(const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1, int cx0, int cx1, ScaleScratch &sc)
{
	SubRamp ramps[9];
	make_ramps(s, ramps);   /* a few dozen floats: cheap next to a picture */
	/* the luminance plane of the stripe and two rows / columns around it, the border repeated */
	const int lp = (cx1 - cx0) + 4, lr = y1 - y0 + 4;
	if ((int) sc.lum.size() < lp * lr) sc.lum.resize((size_t) lp * lr);
	float *L = &sc.lum[0];
	for (int r = 0; r < lr; r++) {
		int sy = y0 - 2 + r;
		sy = sy < 0 ? 0 : sy > h - 1 ? h - 1 : sy;
		const uint16_t *sp = src + (size_t) spitch * sy;
		float *d = L + (size_t) lp * r;
		for (int c = 0; c < lp; c++) {
			int sx = cx0 - 2 + c;
			sx = sx < 0 ? 0 : sx > w - 1 ? w - 1 : sx;
			d[c] = lum565(sp[sx]);
		}
	}
	unsigned busy = 0;
	const v4f ONE = vsplat(1.0f);
	for (int y = y0; y < y1; y++) {
		const float *lc = L + (size_t) lp * (y - y0 + 2) + 2;       /* lc[0] = the pixel (cx0, y) */
		int ym = y > 0 ? y - 1 : 0, yp = y < h - 1 ? y + 1 : h - 1;
		const uint16_t *rm = src + (size_t) spitch * ym, *r0 = src + (size_t) spitch * y, *rp = src + (size_t) spitch * yp;
		uint16_t *o = dst + (size_t) dpitch * (size_t) (y * s);
		for (int x = cx0; x < cx1; x++) {
			const int xl = x - cx0;
			const float lE = lc[xl], lB = lc[xl - lp], lD = lc[xl - 1], lF = lc[xl + 1], lH = lc[xl + lp];
			/* a flat spot: no lane of irlv0 is set, nothing to blend, the pixel is copied */
			if ((lE == lF || lE == lH) && (lE == lB || lE == lF) && (lE == lD || lE == lB) && (lE == lH || lE == lD)) {
				const uint16_t E = r0[x];
				uint16_t *q = o + (size_t) s * x;
				for (int sy = 0; sy < s; sy++) for (int sx = 0; sx < s; sx++) q[(size_t) dpitch * sy + sx] = E;
				continue;
			}
			busy++;
			const float lA1 = lc[xl - 1 - 2 * lp], lB1 = lc[xl - 2 * lp], lC1 = lc[xl + 1 - 2 * lp];
			const float lA = lc[xl - 1 - lp], lC = lc[xl + 1 - lp];
			const float lG = lc[xl - 1 + lp], lI = lc[xl + 1 + lp];
			const float lG5 = lc[xl - 1 + 2 * lp], lH5 = lc[xl + 2 * lp], lI5 = lc[xl + 1 + 2 * lp];
			const float lA0 = lc[xl - 2 - lp], lD0 = lc[xl - 2], lG0 = lc[xl - 2 + lp];
			const float lC4 = lc[xl + 2 - lp], lF4 = lc[xl + 2], lI4 = lc[xl + 2 + lp];
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

unsigned scale_rect(ScalerKind k, const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1, int x0, int x1,
                    ScaleScratch &sc)
{
	if (!src || !dst || w < 1 || h < 1) return 0;
	if (y0 < 0) y0 = 0;
	if (y1 > h) y1 = h;
	if (x0 < 0) x0 = 0;
	if (x1 > w) x1 = w;
	if (y1 <= y0 || x1 <= x0) return 0;
	scale_init();
	switch (k) {
	case SC_MMPX2: return mmpx_rows(src, w, h, spitch, dst, dpitch, y0, y1, x0, x1);
	case SC_XBR2: return xbr_rows<2>(src, w, h, spitch, dst, dpitch, y0, y1, x0, x1, sc);
	case SC_XBR3: return xbr_rows<3>(src, w, h, spitch, dst, dpitch, y0, y1, x0, x1, sc);
	default: return 0;
	}
}

unsigned scale_rows(ScalerKind k, const uint16_t *src, int w, int h, int spitch, uint16_t *dst, int dpitch, int y0, int y1, ScaleScratch &sc)
{
	return scale_rect(k, src, w, h, spitch, dst, dpitch, y0, y1, 0, w, sc);
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


/* ---- the incremental scaler ---------------------------------------------------------------------------------------------------- */

bool rows_equal_scalar(const uint16_t *a, const uint16_t *b, int n)
{
	for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;
	return true;
}

bool rows_equal(const uint16_t *a, const uint16_t *b, int n)
{
#if defined(__ARM_NEON)
	int i = 0;
	for (; i + 32 <= n; i += 32) {
		const uint16x8_t x0 = veorq_u16(vld1q_u16(a + i), vld1q_u16(b + i)), x1 = veorq_u16(vld1q_u16(a + i + 8), vld1q_u16(b + i + 8));
		const uint16x8_t x2 = veorq_u16(vld1q_u16(a + i + 16), vld1q_u16(b + i + 16)), x3 = veorq_u16(vld1q_u16(a + i + 24), vld1q_u16(b + i + 24));
		const uint64x2_t q = vreinterpretq_u64_u16(vorrq_u16(vorrq_u16(x0, x1), vorrq_u16(x2, x3)));
		if (vgetq_lane_u64(q, 0) | vgetq_lane_u64(q, 1)) return false;
	}
	for (; i + 8 <= n; i += 8) {
		const uint64x2_t q = vreinterpretq_u64_u16(veorq_u16(vld1q_u16(a + i), vld1q_u16(b + i)));
		if (vgetq_lane_u64(q, 0) | vgetq_lane_u64(q, 1)) return false;
	}
	for (; i < n; i++) if (a[i] != b[i]) return false;
	return true;
#else
	return memcmp(a, b, (size_t) n * 2u) == 0;
#endif
}

/* The picture is cut into blocks of 8 columns; a row's blocks are one 64 bit mask (pictures up to 512 columns wide; wider ones are
 * scaled in full every time). `eq` = the block's pixels equal the kept picture's (at the shift in question). An output block can be
 * taken from the kept scaled picture when every block its footprint touches is equal: the blocks to its left and right in the rows
 * y - F .. y + F (F = 3 source pixels / rows at most, so one block of margin is enough). */
typedef uint64_t u64;
enum { BLK = 8, MAXB = 64 };

struct ScaleCache::Impl {
	struct Chunk { int y0, y1, x0, x1; };
	ScalerKind kind;
	int w, h, f, ow, nb;
	bool valid;
	uint32_t frame, gen;
	std::vector<uint16_t> prev, out;
	std::vector<uint32_t> ver;
	std::vector<u64> m0, ms, keep, mv, tmp;       /* per row: equal blocks unshifted, equal blocks at the shift, reusable blocks (kept, shifted) */
	std::vector<int16_t> dxr, pdx;                /* per row: the horizontal shift in use, and last picture's */
	std::vector<Chunk> chunks;
	const uint16_t *src;
	int spitch;
	ScaleStats st;
	Impl() : kind(SC_NONE), w(0), h(0), f(1), ow(0), nb(0), valid(false), frame(0), gen(0), src(0), spitch(0) { memset(&st, 0, sizeof st); }
};

ScaleCache::ScaleCache() : p_(new Impl) {}
ScaleCache::~ScaleCache() { delete p_; }
void ScaleCache::reset() { p_->valid = false; p_->chunks.clear(); }
int ScaleCache::chunks() const { return (int) p_->chunks.size(); }
const uint16_t *ScaleCache::out() const { return p_->out.empty() ? 0 : &p_->out[0]; }
int ScaleCache::out_pitch() const { return p_->ow; }
const ScaleStats &ScaleCache::stats() const { return p_->st; }

static inline int pop64(u64 v) { return __builtin_popcountll(v); }

/* Equal blocks of the row `c` against the row `p` shifted: c[x] == p[x + dx]. A block whose shifted pixels leave the picture is not equal. */
static u64 eq_blocks(const uint16_t *c, const uint16_t *p, int w, int nb, int dx)
{
	u64 m = 0;
	for (int b = 0; b < nb; b++) {
		const int x0 = b * BLK, x1 = x0 + BLK < w ? x0 + BLK : w;
		if (x0 + dx < 0 || x1 + dx > w) continue;
		if (rows_equal(c + x0, p + x0 + dx, x1 - x0)) m |= (u64) 1 << b;
	}
	return m;
}

/* The blocks of output row y that can be reused: all blocks of the window of rows y - F .. y + F equal (and next to equal blocks left and
 * right). `shifted` rows (a shift of the whole picture) need the window and its source inside the picture; the unshifted picture clamps
 * at its edge on both sides alike, so there the edge counts as equal. `dyk` the rows' source offset, `dxs` per row (or null). */
static u64 reusable(const u64 *m, const int16_t *dxs, int y, int h, int nb, int F, int dyk, bool edge_ok)
{
	int lo = y - F, hi = y + F;
	if (dyk) { if (lo < 0 || hi >= h || lo + dyk < 0 || hi + dyk >= h) return 0; }
	else { if (lo < 0) lo = 0; if (hi > h - 1) hi = h - 1; }
	u64 a = nb >= 64 ? ~(u64) 0 : (((u64) 1 << nb) - 1);
	const u64 all = a;
	for (int j = lo; j <= hi; j++) {
		if (dxs && dxs[j] != dxs[y]) return 0;
		a &= m[j];
	}
	u64 l = a << 1, r = a >> 1;
	if (edge_ok) { l |= 1; r |= (u64) 1 << (nb - 1); }
	return a & l & r & all;
}

/* Samples some changed rows for the best shift: the score of a shift is the equal blocks over the sample. `horizontal`: dx in [-D, D]. */
static void shift_scores(const uint16_t *cur, int cp, const uint16_t *prev, int w, int h, int nb, const u64 *m0, bool horizontal, int range, int *score)
{
	const int n = 2 * range + 1;
	for (int i = 0; i < n; i++) score[i] = 0;
	const u64 full = nb >= 64 ? ~(u64) 0 : (((u64) 1 << nb) - 1);
	int nc = 0;
	for (int y = 0; y < h; y++) nc += m0[y] != full;
	if (!nc) return;
	const int take = nc < 8 ? nc : 8;
	int idx = 0, next = 0;
	for (int y = 0; y < h && next < take; y++) {
		if (m0[y] == full) continue;
		if (idx++ != (next * nc) / take + nc / (2 * take)) continue;       /* the middle of each eighth of the changed rows */
		next++;
		for (int k = -range; k <= range; k++) {
			if (k == 0) continue;
			if (horizontal) score[k + range] += pop64(eq_blocks(cur + (size_t) cp * y, prev + (size_t) w * y, w, nb, k));
			else if (y + k >= 0 && y + k < h) score[k + range] += pop64(eq_blocks(cur + (size_t) cp * y, prev + (size_t) w * (y + k), w, nb, 0));
		}
	}
}

int ScaleCache::plan(ScalerKind k, const uint16_t *src, int w, int h, int spitch, bool allow_shift)
{
	Impl &d = *p_;
	d.chunks.clear();
	memset(&d.st, 0, sizeof d.st);
	if (!src || k == SC_NONE || w < 1 || h < 1 || w > 4096 || h > 4096) { d.valid = false; return 0; }
	scale_init();
	const int f = scaler_factor(k), F = k == SC_MMPX2 ? 3 : 2;
	const int nb = (w + BLK - 1) / BLK;
	const bool restart = !d.valid || d.kind != k || d.w != w || d.h != h;
	if (restart) {
		d.kind = k; d.w = w; d.h = h; d.f = f; d.ow = w * f; d.nb = nb;
		d.prev.assign((size_t) w * h, 0);
		d.out.assign((size_t) d.ow * (size_t) (h * f), 0);
		d.ver.assign((size_t) h, 0);
		d.m0.assign((size_t) h, 0); d.ms.assign((size_t) h, 0); d.keep.assign((size_t) h, 0); d.mv.assign((size_t) h, 0); d.tmp.assign((size_t) h, 0);
		d.dxr.assign((size_t) h, 0); d.pdx.assign((size_t) h, 0);
		d.gen++;
		d.valid = true;
	}
	d.src = src; d.spitch = spitch;
	d.frame++;
	const uint32_t frame = d.frame;
	d.st.rows = h; d.st.restarted = restart; d.st.blocks = h * nb;
	const u64 full = nb >= 64 ? ~(u64) 0 : (((u64) 1 << nb) - 1);
	const bool usable = nb <= MAXB && !restart;
	u64 *m0 = &d.m0[0], *keep = &d.keep[0], *mv = &d.mv[0];
	for (int y = 0; y < h; y++) mv[y] = 0;
	int shape = 0;               /* 0 none, 1 vertical, 2 horizontal */
	int kk = 0;
	if (usable) {
		for (int y = 0; y < h; y++) m0[y] = eq_blocks(src + (size_t) spitch * y, &d.prev[(size_t) w * y], w, nb, 0);
		int kept = 0;
		for (int y = 0; y < h; y++) { keep[y] = reusable(m0, 0, y, h, nb, F, 0, true); kept += pop64(keep[y]); }
		int best = kept;
		if (allow_shift && kept * 10 < h * nb * 7 && h >= 16 && nb >= 4) {
			/* a vertical shift of the whole picture */
			{
				const int K = h / 4 < 24 ? h / 4 : 24;
				int sc_[49];
				shift_scores(src, spitch, &d.prev[0], w, h, nb, m0, false, K, sc_);
				int bk = 0, bs = 0;
				for (int q = -K; q <= K; q++) if (sc_[q + K] > bs) { bs = sc_[q + K]; bk = q; }
				if (bk) {
					for (int y = 0; y < h; y++) d.ms[y] = (y + bk >= 0 && y + bk < h) ? eq_blocks(src + (size_t) spitch * y, &d.prev[(size_t) w * (y + bk)], w, nb, 0) : 0;
					int tot = 0;
					for (int y = 0; y < h; y++) { d.tmp[y] = reusable(&d.ms[0], 0, y, h, nb, F, bk, false) & ~keep[y]; tot += pop64(keep[y] | d.tmp[y]); }
					if (tot > best) { best = tot; shape = 1; kk = bk; for (int y = 0; y < h; y++) mv[y] = d.tmp[y]; }
				}
			}
			/* a horizontal shift, per row (parallax layers scroll at their own speeds) */
			{
				const int D = 16;
				int sc_[2 * D + 1];
				shift_scores(src, spitch, &d.prev[0], w, h, nb, m0, true, D, sc_);
				int top[3] = { 0, 0, 0 }, nt = 0;
				for (int t = 0; t < 3; t++) {
					int bk = 0, bs = 0;
					for (int q = -D; q <= D; q++) { if (!q) continue; bool used = false; for (int u = 0; u < nt; u++) used |= top[u] == q; if (!used && sc_[q + D] > bs) { bs = sc_[q + D]; bk = q; } }
					if (bk && bs > 0) top[nt++] = bk;
				}
				if (nt) {
					std::vector<int16_t> &dx = d.dxr;
					std::vector<u64> &msh = d.ms;
					for (int y = 0; y < h; y++) {
						int cands[6], nc = 0;
						if (y > 0 && dx[y - 1]) cands[nc++] = dx[y - 1];
						if (d.pdx[y]) cands[nc++] = d.pdx[y];
						for (int t = 0; t < nt; t++) cands[nc++] = top[t];
						int bd = 0, bp = -1;
						u64 bm = 0;
						for (int c = 0; c < nc; c++) {
							bool dup = false;
							for (int e = 0; e < c; e++) dup |= cands[e] == cands[c];
							if (dup) continue;
							const u64 mm = eq_blocks(src + (size_t) spitch * y, &d.prev[(size_t) w * y], w, nb, cands[c]);
							const int pc = pop64(mm);
							if (pc > bp) { bp = pc; bd = cands[c]; bm = mm; }
						}
						dx[y] = (int16_t) bd; msh[y] = bm;
					}
					int tot = 0;
					for (int y = 0; y < h; y++) { d.tmp[y] = dx[y] ? (reusable(&msh[0], &dx[0], y, h, nb, F, 0, false) & ~keep[y]) : 0; tot += pop64(keep[y] | d.tmp[y]); }
					if (tot > best) { best = tot; shape = 2; for (int y = 0; y < h; y++) mv[y] = d.tmp[y]; }
				}
			}
		}
	} else {
		for (int y = 0; y < h; y++) { m0[y] = 0; keep[y] = 0; mv[y] = 0; }
	}
	if (shape != 2) for (int y = 0; y < h; y++) d.dxr[y] = 0;
	d.st.shift_k = shape == 1 ? kk : shape == 2 ? 1 : 0;
	d.st.shape = shape;
	/* the moves: whole runs of blocks, in an order that never reads what a move has already written */
	const size_t orow = (size_t) d.ow;
	int moved = 0, kept_blocks = 0;
	for (int y = 0; y < h; y++) kept_blocks += pop64(keep[y]);
	if (shape == 1) {
		for (int yy = 0; yy < h; yy++) {
			const int y = kk > 0 ? yy : h - 1 - yy;
			u64 r = mv[y];
			while (r) {
				const int a = __builtin_ctzll(r);
				int e = a;
				while (e < nb && (r >> e & 1)) e++;
				r &= ~((((u64) 1 << (e - a)) - 1) << a) ;
				const int px0 = a * BLK, px1 = e * BLK < w ? e * BLK : w;
				for (int sb = 0; sb < f; sb++)
					memmove(&d.out[(size_t) (y * f + sb) * orow + (size_t) px0 * f], &d.out[(size_t) ((y + kk) * f + sb) * orow + (size_t) px0 * f], (size_t) (px1 - px0) * f * 2u);
				moved += e - a;
			}
			if (mv[y]) d.ver[y] = frame;
		}
	} else if (shape == 2) {
		for (int y = 0; y < h; y++) {
			u64 r = mv[y];
			if (!r) continue;
			const int dx = d.dxr[y];
			/* runs, ascending for a source on the right, descending for one on the left */
			int runs_a[33], runs_e[33], nr = 0;
			while (r && nr < 33) {
				const int a = __builtin_ctzll(r);
				int e = a;
				while (e < nb && (r >> e & 1)) e++;
				r &= ~((((u64) 1 << (e - a)) - 1) << a);
				runs_a[nr] = a; runs_e[nr] = e; nr++;
			}
			for (int q = 0; q < nr; q++) {
				const int qi = dx > 0 ? q : nr - 1 - q;
				const int a = runs_a[qi], e = runs_e[qi];
				const int px0 = a * BLK, px1 = e * BLK < w ? e * BLK : w;
				for (int sb = 0; sb < f; sb++)
					memmove(&d.out[(size_t) (y * f + sb) * orow + (size_t) px0 * f], &d.out[(size_t) (y * f + sb) * orow + (size_t) (px0 + dx) * f], (size_t) (px1 - px0) * f * 2u);
				moved += e - a;
			}
			d.ver[y] = frame;
		}
	}
	/* what is left is computed: per stripe of 8 rows, the columns from the first to the last block that is not reusable */
	const int CH = 8;
	int computed_blocks = 0;
	for (int y0 = 0; y0 < h; y0 += CH) {
		const int y1 = y0 + CH < h ? y0 + CH : h;
		u64 need = 0;
		for (int y = y0; y < y1; y++) { const u64 n = ~(keep[y] | mv[y]) & full; need |= n; computed_blocks += pop64(n); }
		if (!need) continue;
		const int bl = __builtin_ctzll(need), bh = 63 - __builtin_clzll(need);
		Impl::Chunk c; c.y0 = y0; c.y1 = y1; c.x0 = bl * BLK; c.x1 = (bh + 1) * BLK < w ? (bh + 1) * BLK : w;
		d.chunks.push_back(c);
		for (int y = y0; y < y1; y++) if (~(keep[y] | mv[y]) & full) d.ver[y] = frame;
	}
	d.st.computed = computed_blocks; d.st.shifted = moved; d.st.same = kept_blocks;
	/* the kept picture is now this one; the shift of each row is the next picture's first guess */
	for (int y = 0; y < h; y++)
		if (!usable || m0[y] != full) memcpy(&d.prev[(size_t) w * y], src + (size_t) spitch * y, (size_t) w * 2u);
	for (int y = 0; y < h; y++) d.pdx[y] = d.dxr[y];
	d.st.chunks = (int) d.chunks.size();
	return (int) d.chunks.size();
}

unsigned ScaleCache::run_chunk(int i, ScaleScratch &sc)
{
	Impl &d = *p_;
	if (i < 0 || i >= (int) d.chunks.size()) return 0;
	const Impl::Chunk &c = d.chunks[i];
	return scale_rect(d.kind, d.src, d.w, d.h, d.spitch, &d.out[0], d.ow, c.y0, c.y1, c.x0, c.x1, sc);
}

int ScaleCache::blit(uint16_t *dst, int dpitch, TexSync &t)
{
	Impl &d = *p_;
	if (!d.valid || !dst || dpitch < d.ow) return 0;
	const bool all = t.gen != d.gen;
	int rows = 0, y = 0;
	while (y < d.h) {
		if (!all && d.ver[y] <= t.epoch) { y++; continue; }
		int e = y + 1;
		while (e < d.h && (all || d.ver[e] > t.epoch)) e++;
		const size_t r0 = (size_t) y * d.f, r1 = (size_t) e * d.f;
		if (dpitch == d.ow) memcpy(dst + r0 * (size_t) dpitch, &d.out[r0 * (size_t) d.ow], (r1 - r0) * (size_t) d.ow * 2u);
		else for (size_t r = r0; r < r1; r++) memcpy(dst + r * (size_t) dpitch, &d.out[r * (size_t) d.ow], (size_t) d.ow * 2u);
		rows += e - y;
		y = e;
	}
	t.epoch = d.frame; t.gen = d.gen;
	return rows;
}

}  // namespace pk
