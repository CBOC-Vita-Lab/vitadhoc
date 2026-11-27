/* Unit tests: pk_screen.h (the screen colour profile and the CRT mask). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_screen.h"
#include <stdlib.h>
#include <string.h>
#include <vector>

static inline int R(unsigned v) { return (int) (v >> 11); }
static inline int G(unsigned v) { return (int) ((v >> 5) & 63); }
static inline int B(unsigned v) { return (int) (v & 31); }

TEST(screen_names_and_resolve)
{
	CHECK_STR(pk_screen_name(PK_SCREEN_AUTO), "Auto");
	CHECK_STR(pk_screen_name(PK_SCREEN_OLED), "OLED (PCH-1000)");
	CHECK_STR(pk_screen_name(PK_SCREEN_LCD), "LCD (PCH-2000)");
	CHECK_STR(pk_screen_name(PK_SCREEN_PSTV), "PS TV");
	/* Auto: a PS TV is a PS TV, a handheld is the reference (no reliable way to tell the OLED from the LCD) */
	CHECK_EQ(pk_screen_resolve(PK_SCREEN_AUTO, 1), PK_SCREEN_PSTV);
	CHECK_EQ(pk_screen_resolve(PK_SCREEN_AUTO, 0), PK_SCREEN_LCD);
	CHECK_EQ(pk_screen_resolve(PK_SCREEN_OLED, 1), PK_SCREEN_OLED);   /* chosen by hand: as chosen */
	CHECK_EQ(pk_screen_resolve(99, 0), PK_SCREEN_LCD);
	CHECK_EQ(pk_screen_resolve(-3, 1), PK_SCREEN_PSTV);
	CHECK(!pk_screen_active(PK_SCREEN_LCD) && !pk_screen_active(PK_SCREEN_PSTV) && pk_screen_active(PK_SCREEN_OLED));
}

TEST(screen_neutral_profiles_are_bit_exact)
{
	std::vector<uint16_t> lut(65536);
	for (int p = PK_SCREEN_LCD; p <= PK_SCREEN_PSTV; p++) {
		pk_screen_lut565(p, NULL, lut.data());
		int bad = 0;
		for (unsigned v = 0; v < 65536; v++) bad += lut[v] != v;
		CHECK_EQ(bad, 0);
	}
}

TEST(screen_oled_keeps_black_white_and_greys_ends)
{
	CHECK_EQ(pk_screen_apply565(PK_SCREEN_OLED, 0x0000), 0x0000);
	CHECK_EQ(pk_screen_apply565(PK_SCREEN_OLED, 0xFFFF), 0xFFFF);
	/* a grey ramp stays grey (equal 5/6/5 steps: the green has one more bit, so compare the scaled value) and only darkens */
	int worst = 0;
	for (int i = 0; i < 32; i++) {
		unsigned v = (unsigned) ((i << 11) | ((i * 63 / 31) << 5) | i);
		unsigned o = pk_screen_apply565(PK_SCREEN_OLED, (uint16_t) v);
		CHECK(R(o) <= i && B(o) <= i);
		CHECK(abs(R(o) - B(o)) <= 1);
		CHECK(abs(G(o) - R(o) * 63 / 31) <= 2);
		if (i - R(o) > worst) worst = i - R(o);
	}
	CHECK(worst >= 1 && worst <= 2);        /* mild: at most two of 31 steps in the mid-tones */
}

TEST(screen_oled_takes_a_little_colour_out_and_is_monotonic)
{
	/* saturated primaries lose some of their chroma, never more than a tenth of the range, and stay ordered */
	const uint16_t cols[] = { 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xC618 ^ 0x2000, 0x8410 };
	for (size_t k = 0; k < sizeof cols / sizeof cols[0]; k++) {
		unsigned v = cols[k], o = pk_screen_apply565(PK_SCREEN_OLED, (uint16_t) v);
		int hi_in = R(v) > B(v) ? R(v) : B(v), lo_in = R(v) < B(v) ? R(v) : B(v);
		int hi_o = R(o) > B(o) ? R(o) : B(o), lo_o = R(o) < B(o) ? R(o) : B(o);
		CHECK(hi_o - lo_o <= hi_in - lo_in);          /* never more colour than the input */
		CHECK(hi_in - lo_in - (hi_o - lo_o) <= 4);    /* and at most four 5-bit steps less */
	}
	CHECK(R(pk_screen_apply565(PK_SCREEN_OLED, 0xF800)) >= 28);  /* pure red gives up a little red (31 -> 29) and the green and blue rise */
	/* each channel is monotonic along a ramp of that channel with the others fixed at 0x80 grey */
	int prev = -1;
	for (int r = 0; r < 32; r++) {
		unsigned o = pk_screen_apply565(PK_SCREEN_OLED, (uint16_t) ((r << 11) | (32 << 5) | 16));
		CHECK(R(o) >= prev);
		prev = R(o);
	}
}

TEST(screen_lut_composes_with_a_console_table)
{
	/* base table: invert; the profile runs after it, in place or not */
	std::vector<uint16_t> base(65536), out(65536), inplace;
	for (unsigned v = 0; v < 65536; v++) base[v] = (uint16_t) ~v;
	inplace = base;
	pk_screen_lut565(PK_SCREEN_OLED, base.data(), out.data());
	pk_screen_lut565(PK_SCREEN_OLED, inplace.data(), inplace.data());
	int bad = 0, diff = 0;
	for (unsigned v = 0; v < 65536; v++) {
		bad += out[v] != inplace[v];
		bad += out[v] != pk_screen_apply565(PK_SCREEN_OLED, base[v]);
		diff += out[v] != base[v];
	}
	CHECK_EQ(bad, 0);
	CHECK(diff > 30000);                                /* it does change the picture ... */
	pk_screen_lut565(PK_SCREEN_LCD, base.data(), out.data());
	CHECK(memcmp(out.data(), base.data(), 65536 * 2) == 0);   /* ... and the neutral one leaves the console's table as it is */
}

TEST(screen_oled_changes_are_small)
{
	int maxd = 0; long long sum = 0;
	for (unsigned v = 0; v < 65536; v++) {
		unsigned o = pk_screen_apply565(PK_SCREEN_OLED, (uint16_t) v);
		int d = abs(R(o) - R(v)); if (abs(G(o) - G(v)) / 2 > d) d = abs(G(o) - G(v)) / 2; if (abs(B(o) - B(v)) > d) d = abs(B(o) - B(v));
		if (d > maxd) maxd = d;
		sum += d;
	}
	CHECK(maxd <= 6);                         /* no channel moves more than 6 of 31 steps (a fifth of the range at worst) */
	CHECK(sum / 65536.0 < 1.5);               /* and on average it moves about one step */
}

TEST(crt_column_width_follows_the_pixel_scale)
{
	CHECK_EQ(pk_crt_col_w(1.0f), 1);
	CHECK_EQ(pk_crt_col_w(3.0f), 1);
	CHECK_EQ(pk_crt_col_w(3.4f), 1);        /* Fit */
	CHECK_EQ(pk_crt_col_w(4.5f), 2);        /* 4.5 / 3 = 1.5 rounds up */
	CHECK_EQ(pk_crt_col_w(6.0f), 2);
	CHECK_EQ(pk_crt_col_w(9.0f), 3);
	CHECK_EQ(pk_crt_col_w(100.0f), PK_CRT_MAX_W / 3);   /* the table is small whatever the scale */
	CHECK_EQ(pk_crt_col_w(0.1f), 1);
	/* monotonic */
	int prev = 0;
	for (float s = 0.5f; s < 40.0f; s += 0.25f) { int w = pk_crt_col_w(s); CHECK(w >= prev); prev = w; }
}

TEST(crt_mask_is_an_aperture_grille)
{
	uint8_t m[4 * PK_CRT_MAX_W + 4];
	CHECK_EQ(pk_crt_make(0, 1, m), 0);        /* off: nothing */
	CHECK_EQ(pk_crt_make(5, 0, m), 0);
	CHECK_EQ(pk_crt_make(5, 9, m), 0);
	for (int cw = 1; cw <= PK_CRT_MAX_W / 3; cw++)
		for (int s = 1; s <= 10; s++) {
			memset(m, 7, sizeof m);
			const int n = pk_crt_make(s, cw, m);
			CHECK_EQ(n, 3 * cw);
			const int d = pk_crt_dark(s);
			for (int x = 0; x < n; x++) {
				const int k = x / cw;
				for (int c = 0; c < 3; c++) CHECK_EQ(m[4 * x + c], c == k ? 255 : d);
				CHECK_EQ(m[4 * x + 3], 255);
			}
		}
	CHECK_EQ(m[4 * PK_CRT_MAX_W], 7);   /* nothing written past the period (cw 8 is the last one: the sentinel stays) */
}

TEST(crt_strength_scale_is_monotonic_and_mean_matches)
{
	int prev = 256;
	for (int s = 0; s <= 10; s++) {
		const int d = pk_crt_dark(s);
		CHECK(d < prev || (s == 0 && d == 255));
		prev = d;
		/* the mean multiplier of the pattern, from the bytes */
		uint8_t m[4 * 3];
		if (s > 0) {
			pk_crt_make(s, 1, m);
			double sum = 0;
			for (int x = 0; x < 3; x++) for (int c = 0; c < 3; c++) sum += m[4 * x + c];
			CHECK(fabs(sum / 9.0 / 255.0 - pk_crt_mean(s)) < 1e-6);
		}
	}
	CHECK_EQ(pk_crt_dark(0), 255);
	CHECK_EQ(pk_crt_dark(-4), 255);
	CHECK_EQ(pk_crt_dark(99), pk_crt_dark(10));
	CHECK(fabs(pk_crt_mean(10) - 0.70) < 0.01);     /* 30 % darker at 10 */
	CHECK(fabs(pk_crt_mean(1) - 0.97) < 0.01);      /* 3 % at 1 */
}
