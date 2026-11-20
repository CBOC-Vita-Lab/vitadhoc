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

/* ---- the incremental scaler ------------------------------------------------------------------------------------------------------- */

TEST(rows_equal_matches_the_scalar_definition)
{
	std::vector<uint16_t> a(300), b(300);
	for (size_t i = 0; i < a.size(); i++) a[i] = (uint16_t) rnd();
	for (int off = 0; off < 4; off++) {
		for (int n = 0; n <= 100; n++) {
			b = a;
			CHECK(rows_equal(&a[off], &b[off], n));
			CHECK(rows_equal_scalar(&a[off], &b[off], n));
			for (int i = 0; i < n; i++) {
				b = a;
				b[(size_t) off + i] ^= (uint16_t) (1u << (rnd() % 16));
				CHECK(!rows_equal(&a[off], &b[off], n));
				CHECK(rows_equal_scalar(&a[off], &b[off], n) == rows_equal(&a[off], &b[off], n));
			}
		}
	}
	/* a difference outside the compared range does not count */
	b = a; b[64] ^= 1;
	CHECK(rows_equal(&a[0], &b[0], 64));
	CHECK(!rows_equal(&a[0], &b[0], 65));
}

namespace {

/* a picture that is a window into a bigger world of tiles (so that a scroll moves real content), with sprites on top */
struct World {
	int ww, wh;
	std::vector<uint16_t> px;
	World(int w, int h, uint32_t seed) : ww(w), wh(h), px((size_t) w * h)
	{
		g_seed = seed;
		static const uint16_t pal[7] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x8410, 0x4208 };
		for (int ty = 0; ty < h / 8; ty++)
			for (int tx = 0; tx < w / 8; tx++) {
				const unsigned kind = rnd() % 5;
				const uint16_t c0 = pal[rnd() % 7], c1 = pal[rnd() % 7];
				for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
					uint16_t c = c0;
					if (kind == 1 && ((x + y) & 3) == 0) c = c1;
					else if (kind == 2 && x > y) c = c1;
					else if (kind == 3 && (x == 3 || y == 5)) c = c1;
					px[(size_t) (ty * 8 + y) * w + tx * 8 + x] = c;
				}
			}
	}
	void window(int w, int h, int ox, int oy, const int *rowdx, std::vector<uint16_t> &out) const
	{
		out.assign((size_t) w * h, 0);
		for (int y = 0; y < h; y++) {
			const int sy = ((y + oy) % wh + wh) % wh;
			const int dx = rowdx ? rowdx[y] : 0;
			for (int x = 0; x < w; x++) out[(size_t) y * w + x] = px[(size_t) sy * ww + ((x + ox + dx) % ww + ww) % ww];
		}
	}
};

void sprite(std::vector<uint16_t> &p, int w, int h, int sx, int sy, uint16_t c)
{
	for (int y = sy; y < sy + 12 && y < h; y++)
		for (int x = sx; x < sx + 9 && x < w; x++) if (y >= 0 && x >= 0 && ((x - sx + y - sy) & 7) != 0) p[(size_t) y * w + x] = c;
}

/* runs `frames` pictures through a ScaleCache with 3 rotating textures and compares each with the full scale */
int run_sequence(ScalerKind kind, int w, int h, int frames, int mode, bool allow_shift, unsigned *moved_rows_out)
{
	World world(256, 256, 77u + (unsigned) mode);
	ScaleCache cache;
	ScaleScratch sc;
	const int f = scaler_factor(kind), ow = w * f, tp = 1024;
	std::vector<uint16_t> tex[3], ref, pic;
	TexSync ts[3];
	for (int i = 0; i < 3; i++) tex[i].assign((size_t) tp * 512, 0);
	int bad = 0;
	unsigned moved = 0;
	std::vector<int> rowdx((size_t) h);
	for (int n = 0; n < frames; n++) {
		int ox = 0, oy = 0;
		const int *rd = 0;
		switch (mode) {
		case 0: break;                                                   /* static, a sprite moves */
		case 1: oy = n * 3; break;                                       /* vertical scroll 3 per frame */
		case 2: ox = n * 2; break;                                       /* horizontal scroll */
		case 3: ox = n * 3 - 40; oy = n * 2; break;                      /* diagonal */
		case 4: for (int y = 0; y < h; y++) rowdx[y] = (y < h / 3 ? n : y < 2 * h / 3 ? 2 * n : 0); rd = &rowdx[0]; ox = 0; break;     /* parallax bands */
		case 5: for (int y = 0; y < h; y++) rowdx[y] = (int) ((n + y) % 7) - 3; rd = &rowdx[0]; break;      /* every row its own shift */
		case 6: ox = (n % 9 == 0) ? 50 * n : n; oy = (n % 13 == 0) ? 31 : 0; break;      /* jumps */
		default: break;
		}
		world.window(w, h, ox, oy, rd, pic);
		sprite(pic, w, h, (n * 5) % w, (n * 3) % h, (uint16_t) (0x3333 + n));
		if (mode == 6 && n == frames / 2) { for (size_t i = 0; i < pic.size(); i++) pic[i] = (uint16_t) (rnd() & 7) * 0x2108; }      /* a full change */
		ref.assign((size_t) ow * h * f, 0);
		scale_rows(kind, &pic[0], w, h, w, &ref[0], ow, 0, h, sc);
		const int nch = cache.plan(kind, &pic[0], w, h, w, allow_shift);
		for (int c = 0; c < nch; c++) cache.run_chunk(c, sc);
		const int ti = n % 3;
		cache.blit(&tex[ti][0], tp, ts[ti]);
		for (int y = 0; y < h * f; y++) {
			if (memcmp(&tex[ti][(size_t) y * tp], &ref[(size_t) y * ow], (size_t) ow * 2) != 0) { bad++; break; }
			if (memcmp(cache.out() + (size_t) y * ow, &ref[(size_t) y * ow], (size_t) ow * 2) != 0) { bad++; break; }
		}
		moved += (unsigned) cache.stats().shifted;
	}
	if (moved_rows_out) *moved_rows_out = moved;
	return bad;
}

}  // namespace

TEST(scale_cache_is_bit_identical_to_the_full_scale)
{
	scale_init();
	for (int k = 0; k < 3; k++)
		for (int mode = 0; mode <= 6; mode++) {
			CHECK_EQ(run_sequence(kKinds[k], 96, 64, 40, mode, true, 0), 0);
			CHECK_EQ(run_sequence(kKinds[k], 96, 64, 24, mode, false, 0), 0);
		}
	/* widths that are not a multiple of the block, tiny pictures, a picture wider than the block mask holds */
	CHECK_EQ(run_sequence(SC_XBR2, 83, 41, 30, 2, true, 0), 0);
	CHECK_EQ(run_sequence(SC_MMPX2, 83, 41, 30, 3, true, 0), 0);
	CHECK_EQ(run_sequence(SC_XBR3, 160, 144, 20, 2, true, 0), 0);
	CHECK_EQ(run_sequence(SC_XBR2, 13, 9, 20, 1, true, 0), 0);
	CHECK_EQ(run_sequence(SC_MMPX2, 3, 2, 10, 0, true, 0), 0);
	CHECK_EQ(run_sequence(SC_XBR2, 600, 40, 8, 2, true, 0), 0);
}

TEST(scale_cache_reuses_what_a_scroll_leaves_unchanged)
{
	scale_init();
	unsigned moved = 0;
	CHECK_EQ(run_sequence(SC_XBR2, 192, 128, 30, 2, true, &moved), 0);
	CHECK(moved > 1000);                 /* a horizontal scroll: most blocks come from the last picture */
	moved = 0;
	CHECK_EQ(run_sequence(SC_MMPX2, 192, 128, 30, 1, true, &moved), 0);
	CHECK(moved > 1000);                 /* a vertical one too */
	moved = 12345;
	CHECK_EQ(run_sequence(SC_XBR2, 192, 128, 30, 2, false, &moved), 0);
	CHECK_EQ(moved, 0u);                 /* the search is off: nothing is moved */
}

TEST(scale_cache_restarts_on_a_change_of_kind_or_size)
{
	scale_init();
	ScaleCache cache;
	ScaleScratch sc;
	World world(128, 128, 5);
	std::vector<uint16_t> pic, ref;
	const int sizes[4][2] = { { 64, 48 }, { 64, 48 }, { 80, 48 }, { 64, 50 } };
	const ScalerKind ks[4] = { SC_XBR2, SC_MMPX2, SC_MMPX2, SC_MMPX2 };
	for (int i = 0; i < 4; i++) {
		const int w = sizes[i][0], h = sizes[i][1], f = scaler_factor(ks[i]);
		world.window(w, h, 3, 0, 0, pic);
		const int n = cache.plan(ks[i], &pic[0], w, h, w);
		for (int c = 0; c < n; c++) cache.run_chunk(c, sc);
		ref.assign((size_t) w * f * h * f, 0);
		scale_rows(ks[i], &pic[0], w, h, w, &ref[0], w * f, 0, h, sc);
		CHECK(memcmp(cache.out(), &ref[0], ref.size() * 2) == 0);
		CHECK(cache.stats().restarted);       /* every change above is a restart (kind or size) */
	}
	/* the same picture again: nothing to compute */
	const int n1 = cache.plan(SC_MMPX2, &pic[0], 64, 50, 64);
	for (int c = 0; c < n1; c++) cache.run_chunk(c, sc);
	const int n2 = cache.plan(SC_MMPX2, &pic[0], 64, 50, 64);
	CHECK_EQ(n2, 0);
	CHECK_EQ(cache.plan(SC_NONE, &pic[0], 64, 50, 64), 0);
}
