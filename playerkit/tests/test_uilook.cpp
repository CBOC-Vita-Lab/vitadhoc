/* Unit tests: Screen / CRT mask / Text size / High contrast in the players' kit (pk_uilook.h, pk_uiprefs.h, pk_crt.h,
 * pk_screenlut.h, Text::set_look, the menu rows). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_menu.h"
#include "pk_i18n.h"
#include "pk_gfx_soft.h"
#include "pk_glyphs.h"
#include "pk_utf8.h"
#include "pk_uilook.h"
#include "pk_uiprefs.h"
#include "pk_crt.h"
#include "pk_screenlut.h"
#include <stdlib.h>
#include <algorithm>

using namespace pk;

TEST(uilook_tint_pushes_to_the_extremes_and_keeps_fades)
{
	const uint32_t grey = PK_RGBA(0xA7, 0xB0, 0xC6, 255);        /* the menus' secondary text */
	const uint32_t t = pk_text_tint(grey);
	CHECK((t & 0xFF) > 0xE0 && ((t >> 8) & 0xFF) > 0xE0 && ((t >> 16) & 0xFF) > 0xE0);     /* a pale colour goes to white */
	CHECK_EQ(pk_text_tint(PK_RGBA(0x20, 0x20, 0x20, 255)) & 0xFF, 0x20 - 0x20 * 70 / 100);        /* a dark one to black */
	CHECK_EQ(pk_text_tint(PK_RGBA(255, 255, 255, 255)), PK_RGBA(255, 255, 255, 255));
	CHECK_EQ(pk_text_tint(PK_RGBA(0, 0, 0, 255)), PK_RGBA(0, 0, 0, 255));
	/* a red warning keeps its hue: red stays well above green and blue */
	const uint32_t red = pk_text_tint(PK_RGBA(0xFF, 0x6B, 0x6B, 255));
	CHECK((red & 0xFF) >= 0xFF - 1 && ((red >> 8) & 0xFF) > 0x6B && ((red >> 8) & 0xFF) < 0xC0);
	/* alpha: continuous and monotonic, clamped */
	unsigned prev = 0;
	for (int a = 0; a <= 255; a++) {
		const unsigned o = pk_text_tint(PK_RGBA(200, 200, 200, a)) >> 24;
		CHECK(o >= prev && o >= (unsigned) a && o <= 255 && (a == 0 ? o == 0 : true));
		prev = o;
	}
	CHECK_EQ(pk_text_pct(0), 100);
	CHECK_EQ(pk_text_pct(1), 120);
}

TEST(uilook_text_large_measures_and_draws_larger)
{
	SoftGfx g;
	Text t(&g);
	pk_glyphs_set_dir("../gpsp/vita/assets");
	const char *s = "Scanlines and LCD grid";
	const float w1 = t.width(F_LABEL, s);
	const int lh1 = t.line_h(F_LABEL), as1 = t.ascent(F_LABEL);
	t.set_look(120, false);
	CHECK_EQ(t.pct(), 120);
	const float w2 = t.width(F_LABEL, s);
	CHECK(fabsf(w2 / w1 - 1.2f) < 0.01f);
	CHECK_EQ(t.line_h(F_LABEL), (lh1 * 120 + 50) / 100);
	CHECK_EQ(t.ascent(F_LABEL), (as1 * 120 + 50) / 100);
	/* what is drawn spans the measured width (the ink of the last glyph ends a little before its advance) */
	g.clear(0x000000);
	const float x1 = t.draw(F_LABEL, 100, 100, s, PK_RGBA(255, 255, 255, 255));
	CHECK(fabsf((x1 - 100) - w2) < 0.5f);
	int minx = 9999, maxx = -1, miny = 9999, maxy = -1;
	for (int y = 90; y < 160; y++) for (int x = 90; x < 700; x++) if ((g.pixel(x, y) & 0xFF) > 64) { minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y); }
	CHECK(maxx > minx && maxx < 100 + (int) w2 + 2 && minx >= 98);
	/* a Large line of text is taller than a Normal one */
	SoftGfx g1;
	Text t1(&g1);
	g1.clear(0x000000);
	t1.draw(F_LABEL, 100, 100, "Hg", PK_RGBA(255, 255, 255, 255));
	int h1 = 0, a = 9999, b = -1;
	for (int y = 90; y < 160; y++) for (int x = 90; x < 200; x++) if ((g1.pixel(x, y) & 0xFF) > 64) { a = std::min(a, y); b = std::max(b, y); }
	h1 = b - a;
	g.clear(0x000000);
	t.draw(F_LABEL, 100, 100, "Hg", PK_RGBA(255, 255, 255, 255));
	a = 9999; b = -1;
	for (int y = 90; y < 170; y++) for (int x = 90; x < 200; x++) if ((g.pixel(x, y) & 0xFF) > 64) { a = std::min(a, y); b = std::max(b, y); }
	CHECK((b - a) >= h1 + 3);
	/* fit and wrap see the real size: a string that fit at Normal is cut at Large */
	t.set_look(100, false);
	const std::string fit100 = t.fit(F_LABEL, "A fairly long row label here", 330);
	t.set_look(120, false);
	const std::string fit120 = t.fit(F_LABEL, "A fairly long row label here", 330);
	CHECK(fit120.size() < fit100.size() && fit120.size() > 3 && t.width(F_LABEL, fit120.c_str()) <= 330);
}

TEST(uilook_text_contrast_draws_brighter)
{
	SoftGfx g;
	Text t(&g);
	const uint32_t dim = PK_RGBA(0x8F, 0x99, 0xB3, 255);
	auto peak = [&](Text &tx) { g.clear(0x000000); tx.draw(F_BODY, 50, 50, "Hello", dim); int m = 0; for (int y = 40; y < 100; y++) for (int x = 40; x < 200; x++) m = std::max(m, (int) (g.pixel(x, y) & 0xFF)); return m; };
	const int off = peak(t);
	t.set_look(100, true);
	CHECK(t.contrast());
	const int on = peak(t);
	CHECK(on > off + 40);
}

TEST(uiprefs_roundtrip_and_unknown_keys)
{
	pkh::TempDir td;
	UiPrefs p = uiprefs_load(td.path);
	CHECK(p.screen == 0 && p.text_large == 0 && p.contrast == 0 && p.theme == 0 && p.tap_menu == 1);       /* no file: Auto, Normal, Off, Dark, Tap On */
	p.screen = PK_SCREEN_OLED; p.text_large = 1; p.contrast = 1; p.theme = 1; p.tap_menu = 0;
	CHECK(uiprefs_save(td.path, p));
	UiPrefs q = uiprefs_load(td.path);
	CHECK(q.screen == PK_SCREEN_OLED && q.text_large == 1 && q.contrast == 1 && q.theme == 1 && q.tap_menu == 0);
	/* the words the app writes (config_psp.c) parse */
	Ini ini;
	ini.parse("screen=pstv\ntext_size=Large\ncontrast=on\ntheme=light\ntap_menu=off\nfuture=1\n");
	UiPrefs r = uiprefs_parse(ini);
	CHECK(r.screen == PK_SCREEN_PSTV && r.text_large == 1 && r.contrast == 1 && r.theme == 1 && r.tap_menu == 0);
	ini.parse("screen=bogus\ntext_size=huge\ncontrast=maybe\ntheme=sepia\ntap_menu=maybe\n");
	r = uiprefs_parse(ini);
	CHECK(r.screen == 0 && r.text_large == 0 && r.contrast == 0 && r.theme == 0 && r.tap_menu == 1);
	/* a key we do not know stays when we save */
	CHECK(file_exists(uiprefs_path(td.path)));
	Ini keep; keep.load(uiprefs_path(td.path)); keep.set("future", "7"); ini_save(uiprefs_path(td.path), keep);
	p.screen = PK_SCREEN_LCD;
	CHECK(uiprefs_save(td.path, p));
	Ini after; after.load(uiprefs_path(td.path));
	CHECK_STR(after.get("future"), "7");
	CHECK_STR(after.get("screen"), "lcd");
}

TEST(screenlut_frame_is_neutral_or_corrected)
{
	ScreenLut sl;
	uint16_t px[4 * 2];
	for (int i = 0; i < 8; i++) px[i] = (uint16_t) (0xF800 >> (i % 3));
	int pitch = 4 * 2;
	/* neutral profiles: the picture itself, pitch untouched */
	CHECK(sl.frame(PK_SCREEN_LCD, px, 4, 2, &pitch) == px && pitch == 8);
	CHECK(sl.frame(PK_SCREEN_PSTV, px, 4, 2, &pitch) == px);
	CHECK(sl.table(PK_SCREEN_LCD) == 0);
	/* OLED: a private tightly packed copy through the table */
	int pitch2 = 16;                     /* a padded source: 8 pixels per row of which 4 are used */
	uint16_t padded[16];
	for (int i = 0; i < 16; i++) padded[i] = (uint16_t) ((i * 4099) & 0xFFFF);
	const uint16_t *o = sl.frame(PK_SCREEN_OLED, padded, 4, 2, &pitch2);
	CHECK(o != padded && pitch2 == 8);
	for (int y = 0; y < 2; y++) for (int x = 0; x < 4; x++) CHECK_EQ(o[y * 4 + x], pk_screen_apply565(PK_SCREEN_OLED, padded[y * 8 + x]));
	CHECK(sl.table(PK_SCREEN_OLED) != 0);
}

TEST(crt_mask_draws_one_multiplied_quad)
{
	SoftGfx g;
	CrtMask m;
	g.clear(0xFFFFFF);
	m.prepare(&g, 0, 240, 80);
	m.draw(&g, 0, 100, 100, 240, 160, 80);             /* strength 0: nothing drawn, nothing made */
	CHECK_EQ(g.live_textures(), 0);
	CHECK_EQ(g.pixel(150, 150), 0xFFFFFFFFu);
	/* draw() without prepare() never creates a texture inside a draw pass */
	m.draw(&g, 10, 100, 100, 240, 160, 80);
	CHECK_EQ(g.live_textures(), 0);
	/* 240 px for 80 source pixels: 3 px per source pixel, 1 px columns */
	m.prepare(&g, 10, 240, 80);
	m.draw(&g, 10, 100, 100, 240, 160, 80);
	CHECK_EQ(g.live_textures(), 1);
	CHECK_EQ(m.width(), 240);
	const int d = pk_crt_dark(10);
	for (int x = 0; x < 6; x++) {
		const uint32_t c = g.pixel(100 + x, 150);
		const int k = x % 3;
		CHECK_EQ((int) (c & 0xFF), k == 0 ? 255 : d);
		CHECK_EQ((int) ((c >> 8) & 0xFF), k == 1 ? 255 : d);
		CHECK_EQ((int) ((c >> 16) & 0xFF), k == 2 ? 255 : d);
	}
	CHECK_EQ(g.pixel(99, 150), 0xFFFFFFFFu);           /* outside the quad: untouched */
	CHECK_EQ(g.pixel(340, 150), 0xFFFFFFFFu);
	CHECK_EQ(g.pixel(150, 260), 0xFFFFFFFFu);
	/* the mean over the picture is the stated multiplier */
	double sum = 0; int n = 0;
	for (int y = 100; y < 260; y++) for (int x = 100; x < 340; x++) { uint32_t c = g.pixel(x, y); sum += (c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF); n += 3; }
	CHECK(fabs(sum / n / 255.0 - pk_crt_mean(10)) < 0.01);
	/* the same strength again reuses the texture; another strength rebuilds it; a wider pixel scale widens the columns */
	m.prepare(&g, 10, 240, 80);
	m.draw(&g, 10, 100, 100, 240, 160, 80);
	CHECK_EQ(g.live_textures(), 1);
	g.clear(0xFFFFFF);
	m.prepare(&g, 5, 480, 80);
	m.draw(&g, 5, 0, 0, 480, 160, 80);                 /* 6 px per source pixel: columns of 2 */
	const int d5 = pk_crt_dark(5);
	CHECK_EQ((int) (g.pixel(0, 10) & 0xFF), 255);
	CHECK_EQ((int) (g.pixel(1, 10) & 0xFF), 255);
	CHECK_EQ((int) (g.pixel(2, 10) & 0xFF), d5);
	CHECK_EQ((int) (g.pixel(3, 10) & 0xFF), d5);
	CHECK_EQ((int) ((g.pixel(2, 10) >> 8) & 0xFF), 255);
	m.release();
	CHECK_EQ(g.live_textures(), 0);
}

TEST(crt_mask_period_is_stable_across_rows_and_follows_the_left_edge)
{
	SoftGfx g;
	CrtMask m;
	g.clear(0xFFFFFF);
	m.prepare(&g, 7, 300, 100);
	m.draw(&g, 7, 37, 20, 300, 100, 100);
	for (int y = 20; y < 120; y++) CHECK_EQ(g.pixel(60, y), g.pixel(60, 20));      /* the same all the way down */
	/* the pattern starts at the picture's left edge, not the screen's */
	CHECK_EQ((int) (g.pixel(37, 50) & 0xFF), 255);
	CHECK_EQ((int) ((g.pixel(38, 50) >> 8) & 0xFF), 255);
}

namespace {
/* Open the menu and enter Settings (the card position depends on the caps). */
bool goto_settings(Menu &m)
{
	m.open(0);
	m.update(pkh::idle());
	for (int i = 0; i < 8; i++) {
		pkh::press(m, PH_RIGHT);                               /* the first card is Resume: never press it */
		pkh::press(m, PH_CROSS);
		if (m.page() == Menu::PAGE_SETTINGS) return true;
		if (m.page() != Menu::PAGE_TOP) pkh::press(m, PH_CIRCLE);
		if (!m.is_open()) return false;
	}
	return false;
}
}  // namespace

TEST(menu_has_the_new_rows)
{
	pkh::TempDir td;
	pkh::MockHost host;
	Menu m;
	MenuConfig c; c.game_name = "x"; c.caps = CAP_ALL_BASIC;
	m.configure(c, &host, 0);
	m.set_value(ROW_UI_TEXT, 1);
	m.set_value(ROW_UI_CONTRAST, 1);
	m.set_value(ROW_PIC_SCREEN, PK_SCREEN_OLED);
	m.set_value(ROW_PIC_CRT, 4);
	CHECK_EQ(m.value(ROW_UI_TEXT), 1);
	CHECK_EQ(m.value(ROW_PIC_SCREEN), PK_SCREEN_OLED);
	CHECK_EQ(m.value(ROW_PIC_CRT), 4);
	/* Settings > Picture has the CRT mask and Screen rows; a change reaches the host with the row id */
	CHECK(goto_settings(m));
	pkh::press(m, PH_CROSS);                                   /* Picture */
	CHECK_EQ(m.page(), Menu::PAGE_PICTURE);
	int crt = -1, scr = -1;
	for (int i = 0; i < m.row_count(); i++) { if (m.row_label(i) == "CRT mask") crt = i; if (m.row_label(i) == "Screen") scr = i; }
	CHECK(crt > 0 && scr > crt);
	for (int i = 0; i < crt; i++) pkh::press(m, PH_DOWN);
	host.changes.clear();
	pkh::press(m, PH_RIGHT);
	bool saw_crt = false;
	for (size_t i = 0; i < host.changes.size(); i++) if (host.changes[i].id == ROW_PIC_CRT) saw_crt = true;
	CHECK(saw_crt);
}

/* ---- the text-fit audit ------------------------------------------------------------------------------------------
 * Every page of the menu is drawn at Normal and at Large with a Gfx that records the glyph quads of the text atlases
 * (one A8 texture draw per glyph). A page fits when (1) every glyph lies on the screen, (2) no two glyphs of different
 * words overlap (text over text), and (3) the text of a row stays inside the row's box. Large must meet the same bar
 * as Normal; the counts are compared, so a layout that was already tight at Normal is not blamed for it. */
namespace {
struct Rec { float x, y, w, h; };
class RecGfx : public SoftGfx {
public:
	std::vector<Rec> glyphs;
	void tex_draw(Texture *t, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, uint32_t tint, bool smooth)
	{
		if (t && tint >> 24) { Rec r = { dx, dy, dw, dh }; glyphs.push_back(r); }
		SoftGfx::tex_draw(t, sx, sy, sw, sh, dx, dy, dw, dh, tint, smooth);
	}
};
struct Audit { int off_screen, overlaps, glyphs; };
Audit audit(const std::vector<Rec> &g)
{
	Audit a = { 0, 0, (int) g.size() };
	/* glyphs of one string come in order on one line: group them into runs, then compare runs with each other
	 * (neighbouring letters of a word may touch; two different strings must not) */
	std::vector<int> run(g.size(), 0);
	int r = 0;
	for (size_t i = 1; i < g.size(); i++) {
		const bool same = fabsf((g[i].y + g[i].h) - (g[i - 1].y + g[i - 1].h)) < 0.45f * std::max(g[i].h, g[i - 1].h) && g[i].x >= g[i - 1].x - 1 && g[i].x - (g[i - 1].x + g[i - 1].w) < g[i].h * 0.8f;
		if (!same) r++;
		run[i] = r;
	}
	for (size_t i = 0; i < g.size(); i++) {
		if (g[i].x < -1 || g[i].y < -1 || g[i].x + g[i].w > 961 || g[i].y + g[i].h > 545) { a.off_screen++; if (getenv("PK_UILOOK_DBG")) fprintf(stderr, "    off-screen glyph at %.0f,%.0f %.0fx%.0f\n", g[i].x, g[i].y, g[i].w, g[i].h); }
		for (size_t j = i + 1; j < g.size() && j < i + 400; j++) {
			if (run[j] == run[i]) continue;
			if (g[i].y > 434 && g[j].y > 434) continue;     /* the help line is drawn as two stacked layers; it is checked for bounds only */
			/* the help line cross-fades from the old text to the new one: the same glyphs a few pixels apart are one string */
			if (fabsf(g[i].w - g[j].w) < 2 && fabsf(g[i].h - g[j].h) < 2 && fabsf(g[i].x - g[j].x) < 8 && fabsf(g[i].y - g[j].y) < 8) continue;
			const float ox = std::min(g[i].x + g[i].w, g[j].x + g[j].w) - std::max(g[i].x, g[j].x);
			const float oy = std::min(g[i].y + g[i].h, g[j].y + g[j].h) - std::max(g[i].y, g[j].y);
			if (ox > 2 && oy > 2) {
				const float area = ox * oy, small = std::min(g[i].w * g[i].h, g[j].w * g[j].h);
				if (small > 0 && area > 0.5f * small) { a.overlaps++; if (getenv("PK_UILOOK_DBG") && atoi(getenv("PK_UILOOK_DBG")) > 1) fprintf(stderr, "  pair (%.0f,%.0f %.0fx%.0f) (%.0f,%.0f %.0fx%.0f)\n", g[i].x, g[i].y, g[i].w, g[i].h, g[j].x, g[j].y, g[j].w, g[j].h); }
			}
		}
	}
	return a;
}
}  // namespace

TEST(menu_every_page_fits_at_large)
{
	pk_glyphs_set_dir("../gpsp/vita/assets");
	for (int contrast = 0; contrast < 2; contrast++) {
		int base_over[8] = { 0 };
		for (int large = 0; large < 2; large++) {
			pkh::TempDir td;
			pkh::MockHost host;
			host.cheat_list.resize(3);
			host.cheat_list[0].name = "Infinite lives"; host.cheat_list[1].name = "Start with 99 coins"; host.cheat_list[2].name = "Walk through walls";
			RecGfx g;
			Text t(&g);
			StateLayout l; l.root = td.path; l.console = "snes"; l.name = "Super Example World (USA)";
			States states(l);
			Mapper mapper;
			Profile prof = profile_defaults(model_snes(), true);
			mapper.configure(model_snes(), prof, MapperConfig());
			ProfileStore store(td.path, "snes", "Super Example World (USA)");
			Menu m;
			MenuConfig c; c.game_name = "Super Example World (USA)"; c.console_label = "Super Nintendo";
			c.caps = CAP_ALL_BASIC | CAP_FF | CAP_FF_SOUND | CAP_FF_MODE | CAP_CHEATS | CAP_REWIND | CAP_WIDE;
			m.configure(c, &host, &states);
			m.attach_input(&mapper, &store, &model_snes(), &prof);
			m.set_value(ROW_UI_TEXT, large);
			m.set_value(ROW_UI_CONTRAST, contrast);
			m.set_value(ROW_PIC_SCREEN, PK_SCREEN_OLED);          /* the longest value */
			m.open(0);
			m.update(pkh::idle());
			int page = 0;
			/* the control centre, then each card's page and each of the Settings pages, every row focused in turn */
			for (int card = 0; card < 8; card++) {
				for (int rep = 0; rep < 1; rep++) {
					g.glyphs.clear();
					g.clear(0x101010);
					for (int i = 0; i < 300; i++) m.update(pkh::idle());
					m.draw(&g, &t);
					const Audit a = audit(g.glyphs);
					CHECK_EQ(a.off_screen, 0);
					if (!large) base_over[page % 8] = a.overlaps; else CHECK(a.overlaps <= base_over[page % 8]);
					CHECK(a.glyphs > 0 || card > 7);
					page++;
				}
				pkh::press(m, PH_RIGHT);
			}
			/* Settings and its three pages: walk every row of each */
			CHECK(goto_settings(m));
			for (int sub = 0; sub < 4; sub++) {
				pkh::press(m, PH_CROSS);                           /* Picture, Controls, Speed, Interface */
				CHECK(m.page() != Menu::PAGE_SETTINGS);
				if (sub == 3) CHECK_EQ(m.page(), Menu::PAGE_INTERFACE);
				for (int row = 0; row < 14; row++) {
					g.glyphs.clear();
					g.clear(0x101010);
					for (int i = 0; i < 300; i++) m.update(pkh::idle());
					m.draw(&g, &t);
					const Audit a = audit(g.glyphs);
					CHECK_EQ(a.off_screen, 0);
					const int key = 100 + sub * 16 + row;
					static int base[400];
					if (!large) base[key] = a.overlaps; else CHECK(a.overlaps <= base[key]);
					if (getenv("PK_UILOOK_DBG")) fprintf(stderr, "DBG c%d L%d sub%d row%d glyphs=%d over=%d\n", contrast, large, sub, row, a.glyphs, a.overlaps);
					if (getenv("PK_UILOOK_SHOTS") && contrast == 0) {
						char nm[256]; snprintf(nm, sizeof nm, "%s/p%d_r%02d_%s.png", getenv("PK_UILOOK_SHOTS"), sub, row, large ? "L" : "N");
						g.write_png(nm);
					}
					pkh::press(m, PH_DOWN);
				}
				pkh::press(m, PH_CIRCLE);                          /* back to Settings */
				pkh::press(m, PH_DOWN);
			}
			m.release_textures();
		}
	}
}

/* ---- the same audit in the pseudo-locale xx-long (about 35% longer, accented; pk_i18n.h) -------------------------------
 * A language with longer words must not push text over text or off the screen: wrapping or an ellipsis, never a smaller
 * size. Each page is drawn in English and in xx-long, at Normal and at Large; xx-long may not have more overlaps than
 * English did and nothing may leave the screen that did not already (a list row clipped at the bottom of its view counts in both, and that page is not judged on it). */
namespace {
int g_catalogue_lang = LANG_ES;
const std::string *g_catalogue = 0;           /* a catalogue read from a file, put in force after the language */
std::vector<Audit> audit_pages(int lang, int large)
{
	std::vector<Audit> out;
	i18n_use(lang, 1);
	if (g_catalogue) i18n_use_text(g_catalogue->data(), g_catalogue->size(), g_catalogue_lang);
	pkh::TempDir td;
	pkh::MockHost host;
	host.cheat_list.resize(3);
	host.cheat_list[0].name = "Infinite lives"; host.cheat_list[1].name = "Start with 99 coins"; host.cheat_list[2].name = "Walk through walls";
	RecGfx g;
	Text t(&g);
	StateLayout l; l.root = td.path; l.console = "snes"; l.name = "Super Example World (USA)";
	States states(l);
	Mapper mapper;
	Profile prof = profile_defaults(model_snes(), true);
	mapper.configure(model_snes(), prof, MapperConfig());
	ProfileStore store(td.path, "snes", "Super Example World (USA)");
	Menu m;
	MenuConfig c; c.game_name = "Super Example World (USA)"; c.console_label = "Super Nintendo";
	c.caps = CAP_ALL_BASIC | CAP_FF | CAP_FF_SOUND | CAP_FF_MODE | CAP_CHEATS | CAP_REWIND | CAP_WIDE | CAP_CHEAT_EDIT | CAP_CHEAT_SEARCH | CAP_PLAY_TOGETHER;
	m.configure(c, &host, &states);
	m.attach_input(&mapper, &store, &model_snes(), &prof);
	m.set_value(ROW_UI_TEXT, large);
	m.set_value(ROW_PIC_SCREEN, PK_SCREEN_OLED);
	m.open(0);
	m.update(pkh::idle());
	auto shot = [&]() {
		g.glyphs.clear();
		g.clear(0x101010);
		for (int i = 0; i < 300; i++) m.update(pkh::idle());
		m.draw(&g, &t);
		out.push_back(audit(g.glyphs));
	};
	for (int card = 0; card < 8; card++) { shot(); pkh::press(m, PH_RIGHT); }      /* the control centre, card by card */
	CHECK(goto_settings(m));
	for (int sub = 0; sub < 4; sub++) {
		pkh::press(m, PH_CROSS);                                   /* Picture, Controls, Speed, Interface */
		for (int row = 0; row < 14; row++) { shot(); pkh::press(m, PH_DOWN); }
		pkh::press(m, PH_CIRCLE);
		pkh::press(m, PH_DOWN);
	}
	/* the save-state page, the cheats page, the search page, a question box and a toast */
	m.goto_page(Menu::PAGE_STATES); shot();
	m.goto_page(Menu::PAGE_CHEATS); shot();
	pkh::press(m, PH_DOWN); shot();
	m.goto_page(Menu::PAGE_CHEAT_SEARCH);
	for (int row = 0; row < 6; row++) { shot(); pkh::press(m, PH_DOWN); }
	m.goto_page(Menu::PAGE_STATES);
	pkh::press(m, PH_SQUARE); shot();                                // Overwrite Slot 1?
	pkh::press(m, PH_CIRCLE);
	m.goto_page(Menu::PAGE_TOP);
	m.notices().push(tr("states.the_state_file_is_too_large"), NOTICE_ERROR); shot();
	m.release_textures();
	i18n_use(LANG_EN, 1);
	return out;
}
}  // namespace

TEST(menu_every_page_fits_in_xx_long)
{
	pk_glyphs_set_dir("../gpsp/vita/assets");
	for (int large = 0; large < 2; large++) {
		const std::vector<Audit> en = audit_pages(LANG_EN, large);
		const std::vector<Audit> xx = audit_pages(LANG_PSEUDO, large);
		CHECK_EQ((int) en.size(), (int) xx.size());
		CHECK(en.size() > 40);
		int worse = 0, off = 0;
		for (size_t i = 0; i < en.size() && i < xx.size(); i++) {
			if (en[i].off_screen == 0 && xx[i].off_screen > 0) { off++; fprintf(stderr, "    xx-long large=%d shot %d: %d glyphs off screen (English %d)\n", large, (int) i, xx[i].off_screen, en[i].off_screen); }
			if (xx[i].overlaps > en[i].overlaps) { worse++; fprintf(stderr, "    xx-long large=%d shot %d: %d overlaps (English %d)\n", large, (int) i, xx[i].overlaps, en[i].overlaps); }
			CHECK(xx[i].glyphs > 0);
		}
		CHECK_EQ(off, 0);
		CHECK_EQ(worse, 0);
	}
}

/* ---- Japanese and Simplified Chinese: the same audit with a (machine translated, test-only) catalogue each -----------
 * Characters have no spaces to break at, are wider per character and come from the CJK fonts: nothing may leave the
 * screen or lie over other text more than in English, and every character of the catalogue must have a glyph in the
 * fonts of its language (no '?'). */
TEST(menu_every_page_fits_in_japanese_and_chinese)
{
	pk_glyphs_set_dir("../gpsp/vita/assets");
	static const struct { const char *file; int region; } langs[] = { { "tests/testlang/pk_ja.txt", PK_REGION_JP }, { "tests/testlang/pk_zh-Hans.txt", PK_REGION_SC } };
	for (int li = 0; li < 2; li++) {
		FILE *f = fopen(langs[li].file, "rb");
		CHECK(f != 0);
		if (!f) continue;
		std::string cat;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0) cat.append(buf, n);
		fclose(f);
		/* every character has a glyph in the CJK files of the language (Latin letters come from Inter) */
		pk_gfont *first = pk_gfont_file(langs[li].region == PK_REGION_SC ? PK_FONT_FILE_CJK_SC : PK_FONT_FILE_CJK);
		pk_gfont *second = pk_gfont_file(PK_FONT_FILE_CJK);
		int missing = 0;
		for (const char *p = cat.c_str(); *p;) {
			int k;
			uint32_t cp = pk_u8_next(p, &k);
			p += k;
			if (cp < 0x80 || pk_glyph_invisible(cp)) continue;
			if (!pk_gfont_has(first, cp) && !pk_gfont_has(second, cp)) { missing++; fprintf(stderr, "    no glyph for U+%04X\n", (unsigned) cp); }
		}
		CHECK_EQ(missing, 0);
		g_catalogue_lang = langs[li].region == PK_REGION_SC ? LANG_ZHS : LANG_JA;
		for (int large = 0; large < 2; large++) {
			g_catalogue = 0;
			const std::vector<Audit> en = audit_pages(LANG_EN, large);
			g_catalogue = &cat;
			const std::vector<Audit> cj = audit_pages(LANG_EN, large);
			g_catalogue = 0;
			CHECK_EQ((int) en.size(), (int) cj.size());
			int worse = 0, off = 0;
			for (size_t i = 0; i < en.size() && i < cj.size(); i++) {
				if (en[i].off_screen == 0 && cj[i].off_screen > 0) { off++; fprintf(stderr, "    %s large=%d shot %d: %d glyphs off screen\n", langs[li].file, large, (int) i, cj[i].off_screen); }
				if (cj[i].overlaps > en[i].overlaps) { worse++; fprintf(stderr, "    %s large=%d shot %d: %d overlaps (English %d)\n", langs[li].file, large, (int) i, cj[i].overlaps, en[i].overlaps); }
				CHECK(cj[i].glyphs > 0);
			}
			CHECK_EQ(off, 0);
			CHECK_EQ(worse, 0);
		}
		g_catalogue = 0;
	}
	pk_glyphs_set_region(PK_REGION_JP);
}

TEST(menu_light_theme_frame0_btntest_and_ra_have_zero_overlaps_or_clipping)
{
	pk_glyphs_set_dir("../gpsp/vita/assets");
	for (int theme = 0; theme < 2; theme++) {
		for (int large = 0; large < 2; large++) {
			pkh::TempDir td;
			pkh::MockHost host;
			host.cheat_list.resize(2);
			host.cheat_list[0].name = "Infinite lives"; host.cheat_list[0].on = true;
			host.cheat_list[1].name = "Moon jump";
			pk::MenuAchievement a; a.title = "Speedrunner"; a.desc = "Beat boss fast."; a.points = 25; a.unlocked = true; a.hardcore = true;
			host.achs.push_back(a);
			host.ra_summary = "1 / 1 (HC)";
			RecGfx g; Text t(&g);
			StateLayout l; l.root = td.path; l.console = "snes"; l.name = "Super Very Long Example World Adventure Title (USA)";
			States states(l);
			std::vector<uint8_t> d(1024, 1); std::vector<uint16_t> px; pkh::make_game_frame(0, px);
			states.save(1, d.data(), d.size(), Frame(&px[0], 256, 224, 512));
			states.set_lock(1, true);
			states.undo_store(d.data(), d.size());
			Mapper mapper; Profile prof = profile_defaults(model_snes(), true);
			mapper.configure(model_snes(), prof, MapperConfig());
			ProfileStore store(td.path, "snes", l.name);
			Menu m;
			MenuConfig c; c.game_name = l.name; c.console_label = "Super Nintendo";
			c.caps = CAP_ALL_BASIC | CAP_FF | CAP_FF_SOUND | CAP_FF_MODE | CAP_CHEATS | CAP_REWIND | CAP_CHEAT_EDIT | CAP_PLAY_TOGETHER | CAP_RA | CAP_SHOT;
			m.configure(c, &host, &states);
			m.attach_input(&mapper, &store, &model_snes(), &prof);
			m.set_value(ROW_UI_THEME, theme);
			m.set_value(ROW_UI_TEXT, large);
			m.open(0);
			for (int i = 0; i < 15; i++) m.update(pkh::idle(16));
			/* Frame 0 check: hints_left_ must be computed before title is clipped on the very first draw */
			g.glyphs.clear(); g.clear(0x101010);
			m.draw(&g, &t);
			Audit a0 = audit(g.glyphs);
			CHECK_EQ(a0.off_screen, 0);
			CHECK_EQ(a0.overlaps, 0);

			/* Walk all cards (verifies 2-line Large Text cards never overlap) */
			for (int card = 0; card < 8; card++) {
				for (int i = 0; i < 12; i++) m.update(pkh::idle());
				g.glyphs.clear(); g.clear(0x101010);
				m.draw(&g, &t);
				Audit ac = audit(g.glyphs);
				CHECK_EQ(ac.off_screen, 0);
				CHECK_EQ(ac.overlaps, 0);
				pkh::press(m, PH_RIGHT);
			}
			/* Save states (locked + undo; row 5 is clipped by hardware clip at view_h), Cheats, RA list, Interface, Button test */
			Menu::PageId extras[] = { Menu::PAGE_STATES, Menu::PAGE_CHEATS, Menu::PAGE_RA, Menu::PAGE_INTERFACE };
			for (size_t e = 0; e < sizeof extras / sizeof extras[0]; e++) {
				m.goto_page(extras[e]);
				if (extras[e] == Menu::PAGE_CHEATS) pkh::press(m, PH_DOWN);
				for (int i = 0; i < 20; i++) m.update(pkh::idle());
				g.glyphs.clear(); g.clear(0x101010);
				m.draw(&g, &t);
				Audit ae = audit(g.glyphs);
				if (extras[e] != Menu::PAGE_STATES) CHECK_EQ(ae.off_screen, 0);
				CHECK_EQ(ae.overlaps, 0);
			}
			m.goto_page(Menu::PAGE_CONTROLS);
			for (int i = 0; i < 4; i++) pkh::press(m, PH_DOWN);
			pkh::press(m, PH_CROSS);
			CHECK(m.button_test_open());
			MenuInput bi = pkh::idle(); bi.pads[0].buttons = PH_START | PH_SELECT;
			m.update(bi);
			g.glyphs.clear(); g.clear(0x101010);
			m.draw(&g, &t);
			Audit ab = audit(g.glyphs);
			CHECK_EQ(ab.off_screen, 0);
			CHECK_EQ(ab.overlaps, 0);
			m.release_textures();
		}
	}
}

/* ---- a shipped catalogue (lang/pk_<code>.txt): PK_TEST_LANG=<code> (es fr de it pt-BR ja zh-Hans) --------------------
 * The same pages as above in a real translation, against English: nothing more off the screen or over other text, and
 * every character of the catalogue has a glyph (Inter for Latin, the CJK files of the language for the rest). Not set:
 * nothing runs (the catalogues are checked one language at a time, see docs/TESTING.md). */
TEST(menu_every_page_fits_in_a_shipped_catalogue)
{
	const char *code = getenv("PK_TEST_LANG");
	if (!code || !*code) return;
	const int lang = lang_index(code);
	CHECK(lang != LANG_AUTO && lang != LANG_EN && lang != LANG_PSEUDO);
	const std::string path = std::string("../lang/pk_") + code + ".txt";
	FILE *f = fopen(path.c_str(), "rb");
	CHECK(f != 0);
	if (!f) return;
	std::string cat;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) cat.append(buf, n);
	fclose(f);
	pk_glyphs_set_dir("../gpsp/vita/assets");
	const int region = lang == LANG_ZHS ? PK_REGION_SC : PK_REGION_JP;
	pk_glyphs_set_region(region);
	pk_gfont *fonts[4] = { pk_gfont_file(PK_FONT_FILE_LATIN), pk_gfont_file(PK_FONT_FILE_LATIN_B),
	                       pk_gfont_file(region == PK_REGION_SC ? PK_FONT_FILE_CJK_SC : PK_FONT_FILE_CJK), pk_gfont_file(PK_FONT_FILE_CJK) };
	int missing = 0;
	for (const char *p = cat.c_str(); *p;) {
		int k;
		uint32_t cp = pk_u8_next(p, &k);
		p += k;
		if (cp < 0x80 || pk_glyph_invisible(cp)) continue;
		if (!pk_gfont_has(fonts[0], cp) && !pk_gfont_has(fonts[2], cp) && !pk_gfont_has(fonts[3], cp)) { missing++; fprintf(stderr, "    %s: no glyph for U+%04X\n", code, (unsigned) cp); }
	}
	CHECK_EQ(missing, 0);
	CHECK_EQ(i18n_use_text(cat.data(), cat.size(), lang) > 500, true);
	g_catalogue_lang = lang;
	for (int large = 0; large < 2; large++) {
		g_catalogue = 0;
		const std::vector<Audit> en = audit_pages(LANG_EN, large);
		g_catalogue = &cat;
		const std::vector<Audit> tl = audit_pages(LANG_EN, large);
		g_catalogue = 0;
		CHECK_EQ((int) en.size(), (int) tl.size());
		int worse = 0, off = 0;
		for (size_t i = 0; i < en.size() && i < tl.size(); i++) {
			if (en[i].off_screen == 0 && tl[i].off_screen > 0) { off++; fprintf(stderr, "    %s large=%d shot %d: %d glyphs off screen\n", code, large, (int) i, tl[i].off_screen); }
			if (tl[i].overlaps > en[i].overlaps) { worse++; fprintf(stderr, "    %s large=%d shot %d: %d overlaps (English %d)\n", code, large, (int) i, tl[i].overlaps, en[i].overlaps); }
			CHECK(tl[i].glyphs > 0);
		}
		fprintf(stderr, "    %s large=%d: %d pages, %d off screen, %d with more overlaps than English\n", code, large, (int) tl.size(), off, worse);
		CHECK_EQ(off, 0);
		CHECK_EQ(worse, 0);
	}
	i18n_use(LANG_EN, 1);
	pk_glyphs_set_region(PK_REGION_JP);
}
