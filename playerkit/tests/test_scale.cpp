/* Unit tests: pk_scale (MMPX 2x, xBR lv2 2x/3x). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_scale.h"
#include <stdlib.h>
#include <string.h>
#include <set>
#include <vector>

using namespace pk;

namespace {

uint32_t g_seed = 1;
unsigned rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

/* a little pixel art: blocks of a few colours, diagonals and lines */
std::vector<uint16_t> art(int w, int h, uint32_t seed)
{
	g_seed = seed;
	static const uint16_t pal[6] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x8410 };
	std::vector<uint16_t> p((size_t) w * h, pal[0]);
	for (int n = 0; n < 14; n++) {
		const int x0 = rnd() % w, y0 = rnd() % h, bw = 2 + rnd() % 9, bh = 2 + rnd() % 7;
		const uint16_t c = pal[rnd() % 6];
		for (int y = y0; y < y0 + bh && y < h; y++) for (int x = x0; x < x0 + bw && x < w; x++) p[(size_t) y * w + x] = c;
	}
	for (int n = 0; n < 4; n++) {
		int x = rnd() % w, y = rnd() % h;
		const uint16_t c = pal[1 + rnd() % 5];
		for (int k = 0; k < 14 && x + k < w && y + k < h; k++) p[(size_t) (y + k) * w + x + k] = c;
	}
	return p;
}

const ScalerKind kKinds[3] = { SC_MMPX2, SC_XBR2, SC_XBR3 };

}  // namespace

TEST(scale_names_and_factors)
{
	CHECK_EQ(scaler_factor(SC_NONE), 1);
	CHECK_EQ(scaler_factor(SC_MMPX2), 2);
	CHECK_EQ(scaler_factor(SC_XBR2), 2);
	CHECK_EQ(scaler_factor(SC_XBR3), 3);
	CHECK_STR(scaler_name(SC_MMPX2), "MMPX 2x");
	CHECK_STR(scaler_name(SC_XBR3), "xBR 3x");
}

TEST(scale_flat_picture_stays_flat)
{
	scale_init();
	for (int k = 0; k < 3; k++) {
		const int f = scaler_factor(kKinds[k]), w = 33, h = 17;
		std::vector<uint16_t> s((size_t) w * h, 0x4208), d((size_t) w * f * h * f, 0);
		ScaleScratch sc;
		const unsigned busy = scale_rows(kKinds[k], &s[0], w, h, w, &d[0], w * f, 0, h, sc);
		CHECK_EQ(busy, 0u);
		for (size_t i = 0; i < d.size(); i++) if (d[i] != 0x4208) { CHECK(false); break; }
	}
}

TEST(scale_stripes_make_the_same_picture)
{
	scale_init();
	const int w = 61, h = 47;
	const std::vector<uint16_t> s = art(w, h, 7);
	for (int k = 0; k < 3; k++) {
		const int f = scaler_factor(kKinds[k]);
		std::vector<uint16_t> a((size_t) w * f * h * f, 1), b(a.size(), 2);
		ScaleScratch s1, s2, s3;
		scale_rows(kKinds[k], &s[0], w, h, w, &a[0], w * f, 0, h, s1);
		const int cuts[5] = { 0, 1, 13, 14, h };
		for (int i = 0; i < 4; i++) scale_rows(kKinds[k], &s[0], w, h, w, &b[0], w * f, cuts[i], cuts[i + 1], (i & 1) ? s2 : s3);
		CHECK(a == b);
	}
}

TEST(scale_mmpx_invents_no_colours_and_smooths)
{
	scale_init();
	const int w = 64, h = 48;
	const std::vector<uint16_t> s = art(w, h, 11);
	std::set<uint16_t> in(s.begin(), s.end());
	std::vector<uint16_t> d((size_t) w * 2 * h * 2);
	ScaleScratch sc;
	const unsigned busy = scale_rows(SC_MMPX2, &s[0], w, h, w, &d[0], w * 2, 0, h, sc);
	CHECK(busy > 0);
	for (size_t i = 0; i < d.size(); i++) if (!in.count(d[i])) { CHECK(false); break; }
	/* a one pixel diagonal gets its corner filled: 3 of the 4 sub pixels differ from the nearest-neighbour picture somewhere */
	size_t differs = 0;
	for (int y = 0; y < h * 2; y++) for (int x = 0; x < w * 2; x++) if (d[(size_t) y * w * 2 + x] != s[(size_t) (y / 2) * w + x / 2]) differs++;
	CHECK(differs > 20);
}

TEST(scale_xbr_fast_path_equals_the_literal_shader)
{
	scale_init();
	for (int sc_ = 2; sc_ <= 3; sc_++) {
		for (int seed = 1; seed <= 6; seed++) {
			const int w = 37 + seed, h = 29 + seed;
			const std::vector<uint16_t> s = art(w, h, 100 + seed);
			std::vector<uint16_t> a((size_t) w * sc_ * h * sc_), b(a.size());
			ScaleScratch scr;
			scale_rows(sc_ == 2 ? SC_XBR2 : SC_XBR3, &s[0], w, h, w, &a[0], w * sc_, 0, h, scr);
			xbr_reference(&s[0], w, h, w, sc_, &b[0], w * sc_);
			size_t bad = 0;
			for (size_t i = 0; i < a.size(); i++) bad += a[i] != b[i];
			CHECK_EQ(bad, 0u);
		}
	}
}

TEST(scale_xbr_smooths_a_diagonal_and_keeps_edges)
{
	scale_init();
	const int w = 16, h = 16;
	std::vector<uint16_t> s((size_t) w * h, 0);
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (x > y) s[(size_t) y * w + x] = 0xFFFF;
	std::vector<uint16_t> d((size_t) w * 2 * h * 2, 0);
	ScaleScratch sc;
	const unsigned busy = scale_rows(SC_XBR2, &s[0], w, h, w, &d[0], w * 2, 0, h, sc);
	CHECK(busy > 0);
	std::set<uint16_t> grey;
	for (size_t i = 0; i < d.size(); i++) if (d[i] != 0 && d[i] != 0xFFFF) grey.insert(d[i]);
	CHECK(!grey.empty());                 /* the edge got blended pixels ... */
	CHECK(d[0] == 0 && d[(size_t) (w * 2) * 2 - 1] == 0xFFFF);      /* ... and the far corners stayed what they were */
}

TEST(scale_bad_arguments_are_harmless)
{
	scale_init();
	ScaleScratch sc;
	uint16_t px[4] = { 1, 2, 3, 4 }, out[64];
	CHECK_EQ(scale_rows(SC_XBR2, 0, 2, 2, 2, out, 4, 0, 2, sc), 0u);
	CHECK_EQ(scale_rows(SC_XBR2, px, 0, 2, 2, out, 4, 0, 2, sc), 0u);
	CHECK_EQ(scale_rows(SC_XBR2, px, 2, 2, 2, out, 4, 5, 9, sc), 0u);
	CHECK_EQ(scale_rows(SC_NONE, px, 2, 2, 2, out, 4, 0, 2, sc), 0u);
	/* a picture of one pixel wide and high */
	CHECK_EQ(scale_rows(SC_MMPX2, px, 1, 1, 1, out, 2, 0, 1, sc), 0u);
	CHECK(out[0] == 1 && out[1] == 1 && out[2] == 1 && out[3] == 1);
}
