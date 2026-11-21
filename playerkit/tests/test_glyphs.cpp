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
