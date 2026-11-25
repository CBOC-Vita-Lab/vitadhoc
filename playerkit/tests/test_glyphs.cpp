/* Unit tests: UTF-8 helpers, the on-demand glyph cache, and Text with Japanese, Korean and accented text.
 * Needs the font files of the package (gpsp/vita/assets; PK_FONT_DIR overrides the place).
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_glyphs.h"
#include "pk_utf8.h"
#include "pk_gfx_soft.h"
#include <stdlib.h>
#include <string.h>

using namespace pk;

static void fonts_dir()
{
	const char *d = getenv("PK_FONT_DIR");
	pk_glyphs_set_dir(d && *d ? d : "../gpsp/vita/assets");
}

static bool valid_utf8(const std::string &s)
{
	const char *p = s.c_str();
	while (*p) {
		int n;
		uint32_t cp = pk_u8_next(p, &n);
		if (cp >= PK_U8_RAW) return false;
		p += n;
	}
	return true;
}

TEST(utf8_decode_and_reject)
{
	int n;
	CHECK_EQ(pk_u8_next("A", &n), 'A');                       CHECK_EQ(n, 1);
	CHECK_EQ(pk_u8_next("\xC3\xA9", &n), 0xE9);               CHECK_EQ(n, 2);
	CHECK_EQ(pk_u8_next("\xE3\x81\x82", &n), 0x3042);         CHECK_EQ(n, 3);          /* あ */
	CHECK_EQ(pk_u8_next("\xF0\x9F\x98\x80", &n), 0x1F600);    CHECK_EQ(n, 4);
	CHECK_EQ(pk_u8_next("\x80", &n), PK_U8_RAW + 0x80);       CHECK_EQ(n, 1);          /* a lone continuation byte (the UI's button glyphs) */
	CHECK_EQ(pk_u8_next("\x81 ", &n), PK_U8_RAW + 0x81);      CHECK_EQ(n, 1);
	CHECK_EQ(pk_u8_next("\xC0\x80", &n), PK_U8_RAW + 0xC0);                            /* overlong */
	CHECK_EQ(pk_u8_next("\xE0\x80\x80", &n), PK_U8_RAW + 0xE0);
	CHECK_EQ(pk_u8_next("\xED\xA0\x80", &n), PK_U8_RAW + 0xED);                        /* a surrogate */
	CHECK_EQ(pk_u8_next("\xF4\x90\x80\x80", &n), PK_U8_RAW + 0xF4);                    /* past U+10FFFF */
	CHECK_EQ(pk_u8_next("\xE3\x81", &n), PK_U8_RAW + 0xE3);   CHECK_EQ(n, 1);          /* cut off by the end */
	char b[5];
	CHECK_EQ(pk_u8_put(0x3042, b), 3);
	CHECK(memcmp(b, "\xE3\x81\x82", 3) == 0);
	CHECK_EQ(pk_u8_put(0xD800, b), 1);
}

TEST(utf8_clip_floor_prev_copy)
{
	const char *s = "ab\xE3\x81\x82\xE3\x81\x84";               /* abあい: 8 bytes */
	CHECK_EQ(pk_u8_floor(s, 8), 8);
	CHECK_EQ(pk_u8_floor(s, 7), 5);
	CHECK_EQ(pk_u8_floor(s, 4), 2);
	CHECK_EQ(pk_u8_floor(s, 3), 2);
	CHECK_EQ(pk_u8_floor(s, 2), 2);
	CHECK_EQ(pk_u8_floor(s, 1), 1);
	CHECK_EQ(pk_u8_count(s), 4);
	CHECK(pk_u8_prev(s, s + 5) == s + 2);
	CHECK(pk_u8_prev(s, s + 8) == s + 5);
	CHECK(pk_u8_prev(s, s + 2) == s + 1);
	char d[7];
	pk_u8_copy(d, sizeof d, s);                                 /* room for 6 + NUL: 'a' 'b' 'あ' and half of 'い' */
	CHECK_STR(d, "ab\xE3\x81\x82");
	char t[16];
	strcpy(t, s);
	CHECK_EQ(pk_u8_clip(t, 3), 2);
	CHECK_STR(t, "ab");
}

TEST(glyphs_cjk_font_loads_lazily_and_covers)
{
	fonts_dir();
	pk_gfont *cjk = pk_gfont_file(PK_FONT_FILE_CJK);
	CHECK(cjk != 0);
	size_t before = pk_glyphs_font_bytes();
	CHECK(pk_gfont_has(cjk, 0x3042));                        /* あ */
	CHECK(pk_glyphs_font_bytes() > before + 1000000);
	CHECK(pk_gfont_has(cjk, 0x30A2));                        /* ア */
	CHECK(pk_gfont_has(cjk, 0x6F22));                        /* 漢 */
	CHECK(pk_gfont_has(cjk, 0xFF21));                        /* Ａ fullwidth */
	CHECK(pk_gfont_has(cjk, 0xFF76));                        /* ｶ halfwidth katakana */
	CHECK(pk_gfont_has(cjk, 0xD55C));                        /* 한 */
	CHECK(pk_gfont_has(cjk, 0x00E9));                        /* é */
	CHECK(pk_gfont_has(cjk, 0x0416));                        /* Ж */
	CHECK(!pk_gfont_has(cjk, 0x1F600));                      /* no emoji */
	pk_gfont *lat = pk_gfont_file(PK_FONT_FILE_LATIN);
	CHECK(pk_gfont_has(lat, 0x00FC));                        /* ü */
	CHECK(pk_gfont_has(lat, 0x014D));                        /* ō */
	CHECK(pk_gfont_has(lat, 0x2014));                        /* — */
}

TEST(glyphs_cache_rasterises_reuses_and_evicts)
{
	fonts_dir();
	struct Sink { int uploads; int w, h; } sink = { 0, 0, 0 };
	pk_gc_spec sp;
	memset(&sp, 0, sizeof sp);
	sp.px = 24; sp.ascent = 24;
	sp.chain[0] = pk_gfont_file(PK_FONT_FILE_LATIN);
	sp.chain[1] = pk_gfont_file(PK_FONT_FILE_CJK);
	sp.budget_bytes = 20 * 1024;                             /* tiny: forces eviction */
	sp.min_slots = 16;
	struct Up { static void f(void *u, int x, int y, int w, int h, const uint8_t *, int) { Sink *s = (Sink *) u; s->uploads++; s->w = x + w; s->h = y + h; } };
	pk_gcache *c = pk_gc_new(&sp, Up::f, &sink);
	CHECK(c != 0);
	int aw, ah;
	pk_gc_atlas_size(c, &aw, &ah);
	CHECK(aw == 512 && ah > 0 && ah <= 512);
	pk_gglyph g;
	CHECK(pk_gc_get(c, 0x3042, &g));
	CHECK(g.w > 8 && g.h > 8 && g.adv > 22 && g.adv < 26);   /* a fullwidth kana is about one em wide */
	CHECK_EQ(sink.uploads, 1);
	pk_gglyph g2;
	CHECK(pk_gc_get(c, 0x3042, &g2));
	CHECK_EQ(sink.uploads, 1);                                /* the second time costs nothing */
	CHECK(g2.x == g.x && g2.y == g.y);
	CHECK(!pk_gc_get(c, 0x1F600, &g));                        /* no font has it */
	float a = 0;
	CHECK(pk_gc_adv(c, 0x00E9, &a) && a > 8 && a < 16);       /* é from Inter */
	CHECK(pk_gc_adv(c, 0x6F22, &a) && a > 22 && a < 26);
	/* more distinct glyphs than cells, all in one frame: the surplus is refused, nothing is overwritten */
	unsigned slots, used, ras, ev, miss;
	pk_gc_stats(c, &slots, &used, &ras, &ev, &miss);
	int fails = 0, oks = 0;
	for (uint32_t cp = 0x3041; cp < 0x3041 + 83; cp++) (pk_gc_get(c, cp, &g) ? oks : fails)++;      /* the hiragana */
	pk_gc_stats(c, &slots, &used, &ras, &ev, &miss);
	CHECK(oks + 1 >= (int) slots - 1 && fails > 0 && ev == 0);
	/* some frames later the cells are free to reuse */
	for (int i = 0; i < 6; i++) pk_gc_tick();
	oks = 0;
	for (uint32_t cp = 0x30A1; cp < 0x30A1 + 10; cp++) oks += pk_gc_get(c, cp, &g);      /* katakana: not in the atlas yet */
	pk_gc_stats(c, &slots, &used, &ras, &ev, &miss);
	CHECK_EQ(oks, 10);
	CHECK(ev >= 10);
	pk_gc_free(c);
}

TEST(glyphs_bold_is_heavier)
{
	fonts_dir();
	long ink[2] = { 0, 0 };
	for (int b = 0; b < 2; b++) {
		struct Cap { std::vector<uint8_t> px; static void f(void *u, int, int, int w, int h, const uint8_t *p, int pitch) { Cap *c = (Cap *) u; c->px.clear(); for (int y = 0; y < h; y++) c->px.insert(c->px.end(), p + y * pitch, p + y * pitch + w); } } cap;
		pk_gc_spec sp;
		memset(&sp, 0, sizeof sp);
		sp.px = 24; sp.ascent = 24; sp.embolden = b ? 60 : 0;
		sp.chain[0] = pk_gfont_file(PK_FONT_FILE_CJK);
		sp.budget_bytes = 64 * 1024; sp.min_slots = 16;
		pk_gcache *c = pk_gc_new(&sp, Cap::f, &cap);
		pk_gglyph g;
		CHECK(pk_gc_get(c, 0x6F22, &g));
		for (size_t i = 0; i < cap.px.size(); i++) ink[b] += cap.px[i];
		pk_gc_free(c);
	}
	CHECK(ink[1] > ink[0] * 11 / 10);
}

TEST(text_utf8_width_fit_wrap_never_split_a_character)
{
	fonts_dir();
	SoftGfx gfx;
	Text t(&gfx);
	const char *jp = "\xE3\x82\xB9\xE3\x83\xBC\xE3\x83\x91\xE3\x83\xBC\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA\xE3\x83\x83\xE3\x83\x88\xE5\x86\x92\xE9\x99\xBA (\xE6\x97\xA5\xE6\x9C\xAC) Rev 1";    /* スーパーマリオット冒険 (日本) Rev 1 */
	float w = t.width(F_BODY, jp);
	CHECK(w > 200 && w < 500);
	CHECK(t.width(F_BODY, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") > 3 * 22);
	CHECK(t.width(F_BODY, "abc") < t.width(F_BODY, "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86"));
	/* a character the end of the string cut off (a name cut to a buffer) is nothing, not a '?' */
	CHECK(t.width(F_BODY, "abc\xE3\x81") == t.width(F_BODY, "abc"));
	CHECK(t.width(F_BODY, "abc\xE3\x81x") == t.width(F_BODY, "abc??x"));
	CHECK(pk_u8_cut_tail("\xE3\x81") && pk_u8_cut_tail("\xF0") && !pk_u8_cut_tail("\xE3\x81x") && !pk_u8_cut_tail("\x80"));
	/* a stray byte or a character without a glyph is a '?', nothing is lost from the layout */
	CHECK(t.width(F_BODY, "\x80") == t.width(F_BODY, "?"));
	CHECK(t.width(F_BODY, "\xF0\x9F\x98\x80") == t.width(F_BODY, "?"));
	for (int f = 0; f < F_COUNT; f++)
		for (int mw = 0; mw <= 420; mw += 7) {
			std::string r = t.fit(f, jp, (float) mw);
			CHECK(valid_utf8(r));
			CHECK(r == jp || (r.size() >= 3 && r.compare(r.size() - 3, 3, "...") == 0));
			CHECK(r == jp || t.width(f, r.c_str()) <= (float) mw || mw < t.width(f, "..."));
		}
	/* wrapped text: every drawn line valid; Japanese breaks anywhere between characters */
	int lines = t.wrap(F_BODY, 10, 10, 120, 4, 2, "\xE3\x82\xB9\xE3\x83\xBC\xE3\x83\x91\xE3\x83\xBC\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA\xE3\x81\xAE\xE5\x86\x92\xE9\x99\xBA\xE3\x80\x82\xE3\x81\x8A\xE3\x82\x8F\xE3\x82\x8A", 0xFFFFFFFFu);
	CHECK(lines >= 3 && lines <= 4);
	CHECK_EQ(t.wrap(F_BODY, 0, 0, 80, 1, 0, "plain ascii words that must wrap", 0xFFFFFFFFu), 1);
}

TEST(text_draws_japanese_ink_and_leaves_ascii_alone)
{
	fonts_dir();
	SoftGfx gfx;
	Text t(&gfx);
	gfx.clear(0x000000);
	t.draw(F_BODY, 20, 20, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 0xFFFFFFFFu);
	int ink = 0;
	for (int y = 20; y < 48; y++) for (int x = 20; x < 92; x++) ink += (gfx.pixel(x, y) & 255) > 128;
	CHECK(ink > 150);                                          /* three kanji: a lot of white pixels */
	t.draw(F_BODY, 20, 80, "?", 0xFFFFFFFFu);
	int q = 0;
	for (int y = 80; y < 108; y++) for (int x = 20; x < 40; x++) q += (gfx.pixel(x, y) & 255) > 128;
	CHECK(q > 10);
}

TEST(text_without_the_font_files_degrades_to_question_marks)
{
	pk_glyphs_set_dir("/nonexistent-dir-for-test");
	/* the fonts already loaded by earlier tests stay; a fresh Text must still work, whatever is loaded */
	SoftGfx gfx;
	Text t(&gfx);
	std::string r = t.fit(F_BODY, "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3\x81\x8A", 60);
	CHECK(valid_utf8(r));
	fonts_dir();
}

/* ---- Japanese and Chinese in every text path ---------------------------------------------------------------- */

TEST(cjk_line_break_rules)
{
	CHECK(!pk_u8_can_break('a', 'b'));                         /* never inside a Latin word */
	CHECK(pk_u8_can_break(0x65E5, 0x672C));                    /* 日|本 */
	CHECK(pk_u8_can_break('i', 0x672C));                       /* Latin|CJK */
	CHECK(pk_u8_can_break(0x672C, 'W'));                       /* CJK|Latin */
	CHECK(!pk_u8_can_break(0x672C, 0x3002));                   /* no line starts with 。 */
	CHECK(!pk_u8_can_break(0x672C, 0x3001));
	CHECK(!pk_u8_can_break(0x672C, 0xFF01));                   /* ！ */
	CHECK(!pk_u8_can_break(0x672C, 0x300D));                   /* 」 */
	CHECK(!pk_u8_can_break(0x672C, 0x201D));                   /* ” */
	CHECK(!pk_u8_can_break(0x30A2, 0x30FC));                   /* ー */
	CHECK(!pk_u8_can_break(0x3042, 0x3083));                   /* small kana */
	CHECK(!pk_u8_can_break(0x300C, 0x672C));                   /* no line ends with 「 */
	CHECK(!pk_u8_can_break(0x201C, 0x672C));                   /* “ */
	CHECK(!pk_u8_can_break(0xFF08, 0x672C));                   /* （ */
	CHECK(!pk_u8_can_break(0x672C, ')'));                      /* ASCII closers stay with the character before */
	CHECK(!pk_u8_can_break(0x672C, ','));
	CHECK(pk_u8_can_break(0x672C, 'A'));
}

TEST(cjk_wrap_never_starts_with_a_closing_mark_or_splits_a_character)
{
	fonts_dir();
	SoftGfx gfx;
	Text t(&gfx);
	const char *zh = "\xE5\xA4\x9A\xE6\x95\xB0\xE8\xAE\xA1\xE6\x95\xB0\xE5\x99\xA8\xE4\xB8\xBA""1\xE5\xAD\x97\xE8\x8A\x82\xE3\x80\x82\xE2\x80\x9C\xE4\xBB\xBB\xE6\x84\x8F\xE5\x9C\xB0\xE5\x9D\x80\xE2\x80\x9D\xE4\xB9\x9F\xE4\xBC\x9A\xE6\x9F\xA5\xE7\x9C\x8B\xE5\xA5\x87\xE6\x95\xB0\xE4\xBD\x8D\xE7\xBD\xAE\xE3\x80\x82 Wi-Fi\xE8\xBF\x9E\xE6\x8E\xA5\xEF\xBC\x88\xE5\xB7\xB2\xE5\xBC\x80\xEF\xBC\x89";
	for (int f = 0; f < F_COUNT; f++)
		for (int mw = 110; mw <= 300; mw += 3) {
			std::vector<std::pair<size_t, size_t> > L = t.break_lines(f, (float) mw, zh);
			std::string all;
			for (size_t i = 0; i < L.size(); i++) {
				std::string line = std::string(zh).substr(L[i].first, L[i].second - L[i].first);
				CHECK(valid_utf8(line));
				CHECK(!line.empty());
				int n;
				uint32_t first = pk_u8_next(line.c_str(), &n);
				CHECK(i == 0 || !pk_u8_no_line_start(first));
				const char *q = line.c_str(), *last = q;
				while (*q) { last = q; q += pk_u8_len(q); }
				CHECK(i + 1 == L.size() || !pk_u8_no_line_end(pk_u8_next(last, &n)));
				CHECK(i + 1 == L.size() || t.width(f, line.c_str()) <= (float) mw || pk_u8_count(line.c_str()) <= 2);
				CHECK(line.find("Wi-") == std::string::npos || line.find("Wi-Fi") != std::string::npos);   /* a Latin word is whole */
			}
		}
}

/* the ink of one glyph in a cache, the sum of its coverage */
static long glyph_ink(pk_gcache *c, std::vector<uint8_t> *cap, uint32_t cp)
{
	pk_gglyph g;
	cap->clear();
	if (!pk_gc_get(c, cp, &g) || g.w == 0) return 0;
	long s = 0;
	for (size_t i = 0; i < cap->size(); i++) s += (*cap)[i];
	return s;
}

TEST(cjk_fonts_cover_japanese_and_simplified_chinese_with_real_glyphs)
{
	fonts_dir();
	pk_gfont *jp = pk_gfont_file(PK_FONT_FILE_CJK), *sc = pk_gfont_file(PK_FONT_FILE_CJK_SC);
	CHECK(jp != 0 && sc != 0);
	const uint32_t zh[] = { 0x4E3A, 0x4F60, 0x5173, 0x52A8, 0x95E8, 0x8FDE, 0x6863, 0x8BBE, 0x7F6E, 0x6A21, 0x5F0F, 0x4FDD, 0x5B58, 0x72B6, 0x6001 };
	for (size_t i = 0; i < sizeof zh / sizeof zh[0]; i++) CHECK(pk_gfont_has(sc, zh[i]));
	const uint32_t ja[] = { 0x3042, 0x30A2, 0x30FC, 0x4E16, 0x754C, 0x9B3C, 0x6F22, 0xFF21 };
	for (size_t i = 0; i < sizeof ja / sizeof ja[0]; i++) CHECK(pk_gfont_has(jp, ja[i]));
	CHECK(!pk_gfont_has(jp, 0x95E8));                          /* 门 is simplified-only: why the Chinese file exists */
	/* a cache with the CJK fallback draws real ink for all of them, in either region */
	for (int region = 0; region < 2; region++) {
		pk_glyphs_set_region(region);
		std::vector<uint8_t> cap;
		struct Up { static void f(void *u, int, int, int w, int h, const uint8_t *p, int pitch) { std::vector<uint8_t> *c = (std::vector<uint8_t> *) u; c->clear(); for (int y = 0; y < h; y++) c->insert(c->end(), p + y * pitch, p + y * pitch + w); } };
		pk_gc_spec sp;
		memset(&sp, 0, sizeof sp);
		sp.px = 24; sp.ascent = 24; sp.cjk_fallback = 1;
		sp.budget_bytes = 64 * 1024; sp.min_slots = 16;
		pk_gcache *c = pk_gc_new(&sp, Up::f, &cap);
		if (region == 1) for (size_t i = 0; i < sizeof zh / sizeof zh[0]; i++) CHECK(glyph_ink(c, &cap, zh[i]) > 800);      /* (the Japanese file lacks some: the Chinese one is tried first) */
		for (size_t i = 0; i < sizeof ja / sizeof ja[0]; i++) CHECK(glyph_ink(c, &cap, ja[i]) > 300);
		pk_gc_free(c);
	}
	pk_glyphs_set_region(PK_REGION_JP);
}

TEST(cjk_region_switch_redraws_the_chinese_forms)
{
	fonts_dir();
	std::vector<uint8_t> cap;
	struct Up { static void f(void *u, int, int, int w, int h, const uint8_t *p, int pitch) { std::vector<uint8_t> *c = (std::vector<uint8_t> *) u; c->clear(); for (int y = 0; y < h; y++) c->insert(c->end(), p + y * pitch, p + y * pitch + w); } };
	pk_gc_spec sp;
	memset(&sp, 0, sizeof sp);
	sp.px = 24; sp.ascent = 24; sp.cjk_fallback = 1;
	sp.budget_bytes = 64 * 1024; sp.min_slots = 16;
	pk_gcache *c = pk_gc_new(&sp, Up::f, &cap);
	/* characters whose Japanese and Chinese forms differ */
	const uint32_t shared[] = { 0x76F4, 0x9AA8, 0x4EE4, 0x5237, 0x6D77, 0x8FD4, 0x8FC7, 0x9910, 0x7D20, 0x9023 };
	std::vector<std::vector<uint8_t> > jpv, scv;
	pk_glyphs_set_region(PK_REGION_JP);
	for (size_t i = 0; i < 10; i++) { glyph_ink(c, &cap, shared[i]); jpv.push_back(cap); }
	for (int k = 0; k < 6; k++) pk_gc_tick();
	pk_glyphs_set_region(PK_REGION_SC);
	for (size_t i = 0; i < 10; i++) { glyph_ink(c, &cap, shared[i]); scv.push_back(cap); }
	int differ = 0;
	for (size_t i = 0; i < 10; i++) differ += jpv[i] != scv[i];
	CHECK(differ >= 3);                                        /* the cache followed the region */
	for (int k = 0; k < 6; k++) pk_gc_tick();
	pk_glyphs_set_region(PK_REGION_JP);
	int same = 0;
	for (size_t i = 0; i < 10; i++) { glyph_ink(c, &cap, shared[i]); same += cap == jpv[i]; }
	CHECK_EQ(same, 10);                                        /* and back */
	pk_gc_free(c);
}
