/* The NEON scalers against the C scalers, byte for byte (pk_scale.cpp). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 *
 *   scale_neon_check [quick]    the check: many generated pictures x every scaler x whole / striped / partial (dirty-region) calls,
 *                               NEON output (scale_use_neon(true)) against C output (false), plus xBR against xbr_reference_int on
 *                               the small pictures. On a build without NEON both sides are the C loops (a self check of the
 *                               harness and of the reference). Exit status 0 = identical everywhere.
 *   scale_neon_check bench K S N [U [G]]
 *                               N frames of scaler K (0 MMPX2, 1 XBR2, 2 XBR3) on picture size S (0 160x144, 1 240x160, 2 256x224,
 *                               3 320x224) of a pixel-art test picture; U = 1 NEON (default), 0 C; G = the picture generator (7 light pixel art default, 8 busy art, 0 noise, 10 tiles). Prints the time and a checksum.
 */
#include "pk_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

using namespace pk;

static uint32_t g_seed = 1;
static unsigned rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

typedef std::vector<uint16_t> Pic;

static Pic gen(int kind, int w, int h, uint32_t seed)
{
	g_seed = seed * 2654435761u + 12345u;
	Pic p((size_t) w * h, 0);
	static const uint16_t pal[8] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x8410, 0xFFE0, 0x07FF };
	switch (kind) {
	case 0: for (size_t i = 0; i < p.size(); i++) p[i] = (uint16_t) (rnd() ^ (rnd() << 7)); break;                       /* full colour noise */
	case 1: for (size_t i = 0; i < p.size(); i++) p[i] = pal[rnd() & 3]; break;                                          /* four colours */
	case 2: for (size_t i = 0; i < p.size(); i++) p[i] = (uint16_t) 0x4A69; break;                                      /* flat */
	case 3: for (size_t i = 0; i < p.size(); i++) p[i] = (rnd() % 53 == 0) ? pal[rnd() & 7] : (uint16_t) 0x2104; break;   /* flat with specks */
	case 4: for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) p[(size_t) y * w + x] = (uint16_t) ((((x * 31) / w) << 11) | (((y * 63) / h) << 5) | ((x + y) & 31)); break;  /* gradients */
	case 5: for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) p[(size_t) y * w + x] = (uint16_t) (((x >> 2) << 11) | ((y >> 1) << 5) | ((x ^ y) & 31) >> 3); break; /* steps */
	case 6: case 7: case 8: {                                                                                            /* pixel art: blocks, diagonals, lines, dither */
		const int nblk = kind == 6 ? 14 : kind == 7 ? 90 : 400;
		const uint16_t bg = pal[rnd() % 3];
		for (size_t i = 0; i < p.size(); i++) p[i] = bg;
		for (int n = 0; n < nblk; n++) {
			const int x0 = rnd() % w, y0 = rnd() % h, bw = 1 + rnd() % 17, bh = 1 + rnd() % 11;
			const uint16_t c = kind == 8 ? (uint16_t) (rnd() & 0xFFFF) : pal[rnd() % 8];
			for (int y = y0; y < y0 + bh && y < h; y++) for (int x = x0; x < x0 + bw && x < w; x++) p[(size_t) y * w + x] = c;
		}
		for (int n = 0; n < nblk / 3 + 2; n++) {
			int x = rnd() % w, y = rnd() % h;
			const int dx = (rnd() & 1) ? 1 : -1, dy = (rnd() & 1) ? 1 : -1, len = 4 + rnd() % 30;
			const uint16_t c = pal[1 + rnd() % 7];
			for (int k = 0; k < len; k++, x += dx, y += dy) if (x >= 0 && x < w && y >= 0 && y < h) p[(size_t) y * w + x] = c;
		}
		for (int n = 0; n < nblk / 6 + 1; n++) {                                                                         /* checker dither patches */
			const int x0 = rnd() % w, y0 = rnd() % h, bw = 3 + rnd() % 14, bh = 3 + rnd() % 9;
			const uint16_t c = pal[rnd() % 8], d = pal[rnd() % 8];
			for (int y = y0; y < y0 + bh && y < h; y++) for (int x = x0; x < x0 + bw && x < w; x++) p[(size_t) y * w + x] = ((x ^ y) & 1) ? c : d;
		}
		break;
	}
	case 9: {                                                                                                            /* near-equal colours: luminance ties and thresholds */
		for (size_t i = 0; i < p.size(); i++) { const unsigned r = rnd(); p[i] = (uint16_t) (0x7BEF ^ ((r & 3) << 11) ^ ((r >> 2 & 3) << 5) ^ (r >> 4 & 1)); }
		break;
	}
	default: {                                                                                                           /* tile map look: 8x8 tiles of a few patterns */
		for (int ty = 0; ty < (h + 7) / 8; ty++) for (int tx = 0; tx < (w + 7) / 8; tx++) {
			const unsigned pat = rnd() & 7; const uint16_t a = pal[rnd() & 7], b = pal[rnd() & 7];
			for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
				const int X = tx * 8 + x, Y = ty * 8 + y;
				if (X >= w || Y >= h) continue;
				bool on = pat == 0 ? 0 : pat == 1 ? 1 : pat == 2 ? (x == y) : pat == 3 ? (x + y == 7) : pat == 4 ? (x < 4) : pat == 5 ? (y < 3) : pat == 6 ? (x & 1) : ((x * y) & 4);
				p[(size_t) Y * w + X] = on ? a : b;
			}
		}
		break;
	}
	}
	return p;
}
static const int kGenKinds = 11;

struct Sz { int w, h; };
static const Sz kSizes[] = { { 160, 144 }, { 240, 160 }, { 256, 224 }, { 320, 224 }, { 256, 192 }, { 1, 1 }, { 2, 3 }, { 7, 5 }, { 8, 8 }, { 9, 4 },
                             { 15, 17 }, { 33, 17 }, { 64, 9 }, { 100, 40 }, { 321, 30 }, { 513, 12 }, { 17, 70 } };

static long g_fail = 0, g_runs = 0;

static bool same(const std::vector<uint16_t> &a, const std::vector<uint16_t> &b, const char *what, int kind, int gk, int w, int h, int extra)
{
	g_runs++;
	if (a == b) return true;
	size_t i = 0;
	while (i < a.size() && a[i] == b[i]) i++;
	g_fail++;
	if (g_fail < 20) fprintf(stderr, "MISMATCH %s scaler=%s gen=%d %dx%d extra=%d first diff at %zu (%04x vs %04x)\n", what, scaler_name((ScalerKind) kind), gk, w, h, extra, i, a[i], b[i]);
	return false;
}

/* one picture, one scaler: whole, striped, and a few partial rects, NEON against C */
static void check_one(ScalerKind k, const Pic &src, int w, int h, int pitch_pad, int gk)
{
	const int f = scaler_factor(k), sp = w + pitch_pad, dp = w * f + (pitch_pad ? 5 : 0);
	Pic s2((size_t) sp * h, 0x1234);
	for (int y = 0; y < h; y++) memcpy(&s2[(size_t) y * sp], &src[(size_t) y * w], (size_t) w * 2);
	const size_t osz = (size_t) dp * h * f;
	Pic a(osz, 0xBEEF), b(osz, 0xBEEF);
	ScaleScratch sa, sb;
	scale_use_neon(false); const unsigned na = scale_rows(k, &s2[0], w, h, sp, &a[0], dp, 0, h, sa);
	scale_use_neon(true);  const unsigned nb = scale_rows(k, &s2[0], w, h, sp, &b[0], dp, 0, h, sb);
	same(a, b, "whole", k, gk, w, h, pitch_pad);
	if (na != nb) { g_fail++; fprintf(stderr, "BUSY COUNT differs %s gen=%d %dx%d: %u vs %u\n", scaler_name(k), gk, w, h, na, nb); }
	/* stripes on two threads' worth of scratch */
	Pic c(osz, 0xBEEF);
	const int cuts[4] = { 0, h / 3, (2 * h) / 3, h };
	for (int i = 0; i < 3; i++) scale_rows(k, &s2[0], w, h, sp, &c[0], dp, cuts[i], cuts[i + 1], i & 1 ? sa : sb);
	same(a, c, "stripes(neon)", k, gk, w, h, pitch_pad);
	/* partial regions: the output must equal the whole picture's inside the rectangle and stay untouched outside it */
	for (int t = 0; t < 4; t++) {
		int x0 = rnd() % w, x1 = x0 + 1 + rnd() % (w - x0), y0 = rnd() % h, y1 = y0 + 1 + rnd() % (h - y0);
		if (t == 0) { x0 = 0; x1 = w < 5 ? w : 5; }
		if (t == 1) { x0 = w > 3 ? w - 3 : 0; x1 = w; }
		Pic d(osz, 0xBEEF), e(osz, 0xBEEF);
		scale_use_neon(false); scale_rect(k, &s2[0], w, h, sp, &d[0], dp, y0, y1, x0, x1, sa);
		scale_use_neon(true);  scale_rect(k, &s2[0], w, h, sp, &e[0], dp, y0, y1, x0, x1, sb);
		same(d, e, "rect", k, gk, w, h, x0 * 1000 + x1);
		/* inside = whole picture's pixels, outside = canary */
		Pic want(osz, 0xBEEF);
		for (int y = y0 * f; y < y1 * f; y++) for (int x = x0 * f; x < x1 * f; x++) want[(size_t) y * dp + x] = a[(size_t) y * dp + x];
		same(want, e, "rect-vs-whole", k, gk, w, h, x0 * 1000 + x1);
	}
}

static void check_reference(ScalerKind k, const Pic &src, int w, int h, int gk)
{
	if (k == SC_MMPX2) return;
	const int f = scaler_factor(k);
	Pic a((size_t) w * f * h * f, 0), r((size_t) w * f * h * f, 1);
	ScaleScratch sc;
	scale_use_neon(true);
	scale_rows(k, &src[0], w, h, w, &a[0], w * f, 0, h, sc);
	xbr_reference_int(&src[0], w, h, w, f, &r[0], w * f);
	same(r, a, "vs xbr_reference_int", k, gk, w, h, 0);
}

static int bench(int argc, char **argv)
{
	const int k = atoi(argv[2]), si = atoi(argv[3]), n = atoi(argv[4]), use = argc > 5 ? atoi(argv[5]) : 1, pic = argc > 6 ? atoi(argv[6]) : 7;
	static const Sz bs[4] = { { 160, 144 }, { 240, 160 }, { 256, 224 }, { 320, 224 } };
	const int w = bs[si].w, h = bs[si].h, f = scaler_factor((ScalerKind) (k + 1));
	Pic src = gen(pic, w, h, 5);
	Pic out((size_t) w * f * h * f);
	ScaleScratch sc;
	scale_init();
	scale_use_neon(use != 0);
	scale_rows((ScalerKind) (k + 1), &src[0], w, h, w, &out[0], w * f, 0, h, sc);        /* warm: tables, scratch */
	unsigned busy = 0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < n; i++) busy = scale_rows((ScalerKind) (k + 1), &src[0], w, h, w, &out[0], w * f, 0, h, sc);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	unsigned long long sum = 0;
	for (size_t i = 0; i < out.size(); i++) sum = sum * 31 + out[i];
	printf("bench scaler=%s %dx%d frames=%d neon=%d busy=%u sum=%llx time_ms_per_frame=%.3f\n", scaler_name((ScalerKind) (k + 1)), w, h, n, use, busy, sum,
	       ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / (n ? n : 1));
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 4 && !strcmp(argv[1], "bench")) return bench(argc, argv);
	scale_init();
	printf("scale_neon_check: neon_built=%d\n", scale_neon_built() ? 1 : 0);
#if defined(__ARM_NEON) && !defined(PK_SCALE_NO_NEON)
	/* a NEON build whose start-up self check switched the NEON loops off would compare the C loops with themselves and pass */
	if (!scale_neon_built()) { printf("scale_neon_check FAILED: the start-up self check switched the NEON loops off\n"); return 1; }
#endif
	const ScalerKind kinds[3] = { SC_MMPX2, SC_XBR2, SC_XBR3 };
	const int quick = argc > 1 && !strcmp(argv[1], "quick");
	for (size_t si = 0; si < sizeof kSizes / sizeof kSizes[0]; si++) {
		const int w = kSizes[si].w, h = kSizes[si].h;
		const bool big = (long) w * h > 20000;
		for (int gk = 0; gk < kGenKinds; gk++) {
			if (quick && big && gk % 3 != 0) continue;
			for (int seed = 0; seed < (big ? 1 : 2); seed++) {
				const Pic src = gen(gk, w, h, (uint32_t) (seed * 7 + gk * 131 + si));
				for (int ki = 0; ki < 3; ki++) {
					check_one(kinds[ki], src, w, h, 0, gk);
					if (seed == 0 && (si & 1)) check_one(kinds[ki], src, w, h, 3, gk);             /* a pitch wider than the picture */
					if (!big) check_reference(kinds[ki], src, w, h, gk);
				}
			}
		}
	}
	printf("scale_neon_check: %ld comparisons, %ld mismatches\n", g_fail ? g_runs : g_runs, g_fail);
	if (g_fail) { printf("scale_neon_check FAILED\n"); return 1; }
	printf("scale_neon_check OK\n");
	return 0;
}
