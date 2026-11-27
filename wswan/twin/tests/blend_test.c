/* Unit test of wswan/frontend/blend.c: exact weights, frame schedule, kernels (NEON vs scalar when built for ARM), motion.
 * Prints PASS/FAIL lines; exit code 1 on any failure. Copyright CBOC-Vita. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "blend.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS  "); else { printf("FAIL  "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void)
{
	wsb b;
	enum { W = 16, H = 4 };
	uint16_t f[W * H], out[W * H];
	long i, total_frames = 0, hist[4] = { 0, 0, 0, 0 };
	long N = 600000;
	int bad_sum = 0, bad_order = 0;

	if (wsb_init(&b, W, H, 3072000, 40704, 60000, 1001)) { printf("FAIL  init\n"); return 1; }
	CHECK(b.te == 2442240000ull && b.td == 3075072000ull, "tick lengths te=%llu td=%llu (Td/Te = %.6f)", (unsigned long long) b.te, (unsigned long long) b.td, (double) b.td / b.te);

	/* schedule: frames needed per refresh, weights */
	{
		uint64_t last_frame = 0;
		for (i = 0; i < N; i++) {
			uint64_t idx[3]; unsigned w[3]; int need = wsb_frames_needed(&b), n, k;
			for (k = 0; k < need; k++) wsb_submit(&b, f);
			n = wsb_weights(&b, idx, w);
			if (w[0] + w[1] + w[2] != 256) bad_sum++;
			for (k = 1; k < n; k++) if (idx[k] != idx[k - 1] + 1) bad_order++;
			if (idx[0] < last_frame) bad_order++;
			last_frame = idx[0];
			hist[n]++; total_frames += need;
			wsb_present(&b, out);
		}
	}
	CHECK(bad_sum == 0, "weights sum to 256 in all %ld refreshes", N);
	CHECK(bad_order == 0, "frame indices consecutive and never go back");
	CHECK(hist[1] == 0 && hist[2] > 0 && hist[3] > 0, "a refresh blends 2 frames (%ld) or 3 (%ld), never 1", hist[2], hist[3]);
	{
		double avg = (double) total_frames / N, want = (3072000.0 / 40704.0) / (60000.0 / 1001.0);
		CHECK(fabs(avg - want) < 1e-5, "emulated frames per refresh over %ld refreshes = %.6f (exact ratio %.6f)", N, avg, want);
	}

	/* exact integer overlap: sum of overlaps of one refresh equals Td (checked through a 1-pixel constant frame) */
	wsb_reset(&b);
	{
		int ok = 1, k;
		for (i = 0; i < 5000; i++) {
			int need = wsb_frames_needed(&b);
			for (k = 0; k < need; k++) { uint16_t c[W * H]; int j; for (j = 0; j < W * H; j++) c[j] = 0xF81F; wsb_submit(&b, c); }
			wsb_present(&b, out);
			for (k = 0; k < W * H; k++) if (out[k] != 0xF81F) ok = 0;
		}
		CHECK(ok, "blending identical frames returns them bit for bit");
	}

	/* kernels */
	{
		uint16_t A[1003], B[1003], C[1003], o1[1003], o2[1003];
		int ok = 1; unsigned seed = 12345;
		for (i = 0; i < 1003; i++) { seed = seed * 1664525u + 1013904223u; A[i] = seed >> 8; seed = seed * 1664525u + 1013904223u; B[i] = seed >> 8; seed = seed * 1664525u + 1013904223u; C[i] = seed >> 8; }
		for (i = 0; i < 200; i++) {
			unsigned wa = (unsigned) (rand() % 257), wb = (unsigned) (rand() % (257 - wa)), wc = 256 - wa - wb;
			wsb_mix3_scalar(o1, A, B, C, wa, wb, wc, 1003);
			wsb_mix3(o2, A, B, C, wa, wb, wc, 1003);
			if (memcmp(o1, o2, sizeof o1)) ok = 0;
		}
		CHECK(ok, "wsb_mix3 (%s) equals the scalar reference on 200 random weight sets x 1003 pixels", wsb_has_neon() ? "NEON" : "scalar build, no NEON here");
		/* reference value: 2 of 3 weights, hand computed: R5 20, 10 -> (20*100 + 10*156 + 128) >> 8 = 14 */
		{
			uint16_t p = 20 << 11, q = 10 << 11, z = 0, r;
			wsb_mix3_scalar(&r, &p, &q, &z, 100, 156, 0, 1);
			CHECK((r >> 11) == 14 && (r & 0x7FF) == 0, "hand computed channel value (20*100+10*156+128)>>8 = 14, got %u", r >> 11);
		}
	}

	/* motion: a bright pixel moves one pixel per emulated frame; the blended output's centroid advances by exactly
	 * the emulated time covered (centroid = average position over the refresh interval, in pixels of 1 frame each). */
	{
		enum { MW = 64 };
		wsb m; uint16_t fr[MW], ou[MW]; double prev = -1; int mono = 1, k; double maxerr = 0;
		wsb_init(&m, MW, 1, 3072000, 40704, 60000, 1001);
		long j = 0;
		for (i = 0; i < 40; i++) {
			int need = wsb_frames_needed(&m);
			for (k = 0; k < need; k++) { for (int x = 0; x < MW; x++) fr[x] = (x == (j % 60)) ? 0xFFFF : 0; wsb_submit(&m, fr); j++; }
			wsb_present(&m, ou);
			if (i > 2 && i < 30) {      /* away from the wrap at 60 */
				double s = 0, c = 0; uint64_t idx[3]; unsigned w[3];
				for (int x = 0; x < MW; x++) { double v = ((ou[x] >> 11) & 31) + 0.0; s += v * x; c += v; }
				double cen = s / c;
				/* expected: the weights say how much of each frame; frame position = frame number */
				m.t -= m.td; wsb_weights(&m, idx, w); m.t += m.td;
				double want = 0; for (k = 0; k < 3; k++) want += (double) w[k] / 256.0 * (double) (idx[k] % 60);
				if (fabs(cen - want) > maxerr) maxerr = fabs(cen - want);
				if (prev >= 0 && cen <= prev) mono = 0;
				prev = cen;
			}
		}
		CHECK(mono, "blended motion advances every refresh (no stall, no step back)");
		CHECK(maxerr < 0.6, "blended centroid matches the weighted frame positions (max error %.3f px, 5 bit channel rounding)", maxerr);
		wsb_free(&m);
	}

	/* ghosting (LCD persistence): a step from black to white approaches white geometrically */
	{
		wsb g; uint16_t a[4] = { 0, 0, 0, 0 }, w[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF }, o[4]; int k, n;
		wsb_init(&g, 4, 1, 3072000, 40704, 60000, 1001); g.ghost = 128;
		for (n = 0; n < 4; n++) { int need = wsb_frames_needed(&g); for (k = 0; k < need; k++) wsb_submit(&g, a); wsb_present(&g, o); }
		for (n = 0; n < 8; n++) { int need = wsb_frames_needed(&g); for (k = 0; k < need; k++) wsb_submit(&g, w); wsb_present(&g, o); }
		CHECK((o[0] >> 11) >= 30, "ghost 128: after 8 refreshes of white the output reached %u/31", o[0] >> 11);
		wsb_free(&g);
	}
	wsb_free(&b);
	printf("%s\n", fails ? "blend_test: FAILED" : "blend_test: all passed");
	return fails ? 1 : 0;
}
