/* Unit tests: pk_palsmooth (Smooth 50 Hz: even cadence for 50 Hz games on a 60 Hz screen). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_palsmooth.h"
#include <math.h>

using namespace pk;

/* 6000 refreshes: the frames run are the same count with or without the early run, and the picture position steps as promised */
TEST(palsmooth_runs_the_games_speed_and_evens_the_steps)
{
	for (int smooth = 0; smooth < 2; smooth++) {
		PalSmooth p; p.set(50.0070, 59.94); p.smooth = smooth != 0;
		int frames = 0, early = 0, run_prev = 1;
		double pos = 0, prev_pos = 0, ssd = 0; int n = 0;
		for (int r = 0; r < 6000; r++) {
			bool e;
			const int k = p.step(&e);
			frames += k;
			if (e) { early++; CHECK(k == 1); }
			/* what the picture shows: a run frame is shown whole, the early one as the mean of the old and the new, and the refresh after it the new one */
			if (e) pos += 0.5; else if (run_prev == 2) pos += 0.5; else pos += k;
			run_prev = e ? 2 : 1;
			if (r > 12) { const double step = pos - prev_pos; ssd += (step - 50.007 / 59.94) * (step - 50.007 / 59.94); n++; }
			prev_pos = pos;
		}
		/* 6000 refreshes at 50.007 / 59.94 frames each */
		CHECK(abs(frames - (int) (6000 * 50.0070 / 59.94)) <= 1);
		if (smooth) { CHECK(early > 900 && early < 1100); CHECK(ssd / n < 0.06); }
		else { CHECK(early == 0); CHECK(ssd / n > 0.1); }
	}
}

TEST(palsmooth_late_pass_is_never_early)
{
	PalSmooth p; p.set(50.0, 60.0); p.smooth = true;
	int sum = 0;
	for (int i = 0; i < 600; i++) sum += p.owed(2);
	CHECK(sum == 1000);
}

TEST(palsmooth_mix_is_the_mean_per_channel)
{
	uint16_t a[4] = { 0x0000, 0xFFFF, 0xF800, 0x07E0 }, b[4] = { 0xFFFF, 0xFFFF, 0x0000, 0x07E0 }, d[4];
	mix16_half((uint8_t *) d, 8, (const uint8_t *) a, 8, (const uint8_t *) b, 8, 4, 1);
	CHECK(d[1] == 0xFFFF);
	CHECK(d[3] == 0x07E0);
	CHECK((d[0] >> 11) == 15 && ((d[0] >> 5) & 63) == 31 && (d[0] & 31) == 15);      /* black + white */
	CHECK((d[2] >> 11) == 15 && ((d[2] >> 5) & 63) == 0 && (d[2] & 31) == 0);        /* red + black */
	/* the same picture twice is itself */
	uint16_t e[3] = { 0x1234, 0xABCD, 0x7FFF }, f[3];
	mix16_half((uint8_t *) f, 6, (const uint8_t *) e, 6, (const uint8_t *) e, 6, 3, 1);
	CHECK(f[0] == e[0] && f[1] == e[1] && f[2] == e[2]);
}

TEST(palsmooth_setting_maps)
{
	for (int m = 0; m < 3; m++) CHECK(pal_mode_of(pal_row_of(m)) == m);
	CHECK(pal_mode_clamp(7) == PAL_SMOOTH && pal_mode_clamp(-1) == PAL_SMOOTH && pal_mode_clamp(0) == PAL_TRUE && pal_mode_clamp(1) == PAL_FAST);
}
