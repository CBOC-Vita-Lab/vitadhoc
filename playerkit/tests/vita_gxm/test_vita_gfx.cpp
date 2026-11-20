/*
 * The console's drawing path, tested without a console. pk_gfx_vita.cpp is compiled unchanged against
 * vita2d_fake.cpp (the real SDK types, a rasteriser that follows GXM's swizzle, ABGR colours and blending),
 * and playerkit's real menu is drawn through it and through the software gfx the other tests use.
 * The failure this exists for: text that drew as solid boxes on a real Vita because the glyph atlas used a
 * texture swizzle with no alpha (U8_111R instead of U8_R111) while every host test drew in software.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#include "harness.h"
#include "pk_gfx_soft.h"
#include "pk_gfx_vita.h"
#include <vita2d.h>
#include "vita2d_fake.h"
#include <math.h>
#include <stdio.h>
#include <thread>

using namespace pk;

static int g_fail = 0, g_checks = 0;
#define EXPECT(cond) do { g_checks++; if (!(cond)) { fprintf(stderr, "vita_gxm: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static double contrast(double l1, double l2) { if (l1 < l2) { double t = l1; l1 = l2; l2 = t; } return (l1 + 0.05) / (l2 + 0.05); }

struct Rig {
	pkh::TempDir tmp;
	pkh::MockHost host;
	Menu menu;
	Rig(uint32_t accent)
	{
		MenuConfig cfg;
		cfg.game_name = "Super Example World (USA)";
		cfg.console_label = "Super Nintendo";
		cfg.caps = CAP_ALL_BASIC | CAP_FF | CAP_FF_SOUND | CAP_FF_MODE | CAP_CHEATS;
		cfg.accent = accent;
		cfg.exit_label = "Exit";
		cfg.play_together_reason = "Not available for Super NES: the player is a separate program and has no link";
		cfg.unsupported_reason = "Not available for Super NES";
		menu.configure(cfg, &host, 0);
		menu.open(0);
		for (int t = 0; t < 300; t += 16) menu.update(pkh::idle(16));
	}
	void page(int p) { menu.goto_page((Menu::PageId) p); for (int t = 0; t < 300; t += 16) menu.update(pkh::idle(16)); }
};

static const int kPages[] = { Menu::PAGE_TOP, Menu::PAGE_SETTINGS, Menu::PAGE_PICTURE, Menu::PAGE_SPEED };
static const uint32_t kAccents[] = { 0x7B6CFF, 0x4FC3F7, 0xE8604C, 0x2EE6A6, 0xFF4FA3, 0xFFB020 };   /* the console accents and the Neon Link family */

/* 1. plain text through the console path: glyphs must be coverage, not boxes */
static void text_is_not_boxes()
{
	fakegxm::reset();
	fakegxm::clear(0xFF2A1408);                       /* a dark blue-ish panel, ABGR */
	VitaGfx g;
	Text t(&g);
	const uint32_t ink = PK_HEX(0xF5F7FC, 255);
	const char *s = "Resume game";
	t.draw(F_LABEL, 100, 100, s, ink);
	const float w = t.width(F_LABEL, s);
	const int lh = t.line_h(F_LABEL);
	const uint32_t *fb = fakegxm::framebuffer();
	int bg = 0, bright = 0, total = 0;
	for (int y = 100; y < 100 + lh; y++)
		for (int x = 100; x < 100 + (int) w; x++) {
			uint32_t c = fb[y * 960 + x];
			total++;
			if (c == (0xFF2A1408u)) bg++;
			else if ((c & 255) > 200 && ((c >> 8) & 255) > 200) bright++;
		}
	/* text covers a fraction of its line box: most pixels stay panel, some are ink. A solid box is ~0% panel. */
	EXPECT(bg * 100 / total > 35);
	EXPECT(bright * 100 / total > 3);
	/* every glyph quad: the texture swizzle must put the byte in alpha */
	const std::vector<fakegxm::TexDraw> &d = fakegxm::tex_draws();
	EXPECT(d.size() >= 8);
	for (size_t i = 0; i < d.size(); i++) {
		fakegxm::Swizzle sw = fakegxm::decode_u8(d[i].format);
		EXPECT(sw.known);
		EXPECT(sw.alpha_from_data && !sw.alpha_const1);      /* coverage is alpha */
		EXPECT(!sw.rgb_from_data);                           /* colour is the tint alone */
		EXPECT(d[i].uploaded);                               /* the atlas was filled before the first glyph */
		EXPECT(d[i].same_thread);                            /* and written on the drawing thread */
	}
	EXPECT(fakegxm::bad_calls() == 0);
}

/* 2. the real menu, every page, every accent: contrast of every text quad against what is behind it */
static void menu_contrast()
{
	double worst = 99;
	for (size_t a = 0; a < sizeof kAccents / sizeof *kAccents; a++)
		for (size_t p = 0; p < sizeof kPages / sizeof *kPages; p++) {
			fakegxm::reset();
			fakegxm::clear(0xFF301810);
			VitaGfx g;
			Text t(&g);
			Rig r(kAccents[a]);
			if (kPages[p] != Menu::PAGE_TOP) r.page(kPages[p]);
			r.menu.draw(&g, &t);
			const std::vector<fakegxm::TexDraw> &d = fakegxm::tex_draws();
			EXPECT(d.size() > 20);
			for (size_t i = 0; i < d.size(); i++) {
				if ((d[i].tint >> 24) == 0) continue;
				/* the ink as it lands on the panel (a faded text blends toward it) */
				double al = (double) (d[i].tint >> 24) / 255.0;
				double eff = d[i].ink_lum * al + d[i].bg_lum * (1 - al);
				double cr = contrast(eff, d[i].bg_lum);
				if (al > 0.6 && cr < worst) worst = cr;
				if (al > 0.6 && cr < 4.5) fprintf(stderr, "  low contrast %.2f: page %d accent %06X tint %08X at %.0f,%.0f bg lum %.3f\n", cr, kPages[p], kAccents[a], d[i].tint, d[i].dx, d[i].dy, d[i].bg_lum);
				EXPECT(!(al > 0.6 && cr < 4.5));
			}
			r.menu.release_textures();
			EXPECT(fakegxm::bad_calls() == 0);
		}
	printf("vita_gxm: worst text contrast over %d pages x %d accents: %.2f:1\n", (int) (sizeof kPages / sizeof *kPages), (int) (sizeof kAccents / sizeof *kAccents), worst);
}

/* 3. the console path and the software path agree on what the menu looks like */
static void same_as_soft()
{
	for (size_t p = 0; p < sizeof kPages / sizeof *kPages; p++) {
		fakegxm::reset();
		fakegxm::clear(0xFF301810);
		VitaGfx vg;
		Text vt(&vg);
		Rig rv(0x7B6CFF);
		if (kPages[p] != Menu::PAGE_TOP) rv.page(kPages[p]);
		rv.menu.draw(&vg, &vt);
		SoftGfx sg;
		sg.clear(0xFF301810);
		Text st(&sg);
		Rig rs(0x7B6CFF);
		if (kPages[p] != Menu::PAGE_TOP) rs.page(kPages[p]);
		rs.menu.draw(&sg, &st);
		const uint32_t *a = fakegxm::framebuffer(), *b = sg.pixels();
		long bad = 0;
		for (int i = 0; i < 960 * 544; i++) {
			int dr = (int) (a[i] & 255) - (int) (b[i] & 255), dg = (int) ((a[i] >> 8) & 255) - (int) ((b[i] >> 8) & 255), db = (int) ((a[i] >> 16) & 255) - (int) ((b[i] >> 16) & 255);
			if (abs(dr) > 64 || abs(dg) > 64 || abs(db) > 64) bad++;
		}
		printf("vita_gxm: page %d console-vs-soft: %.3f%% of pixels differ by > 64\n", kPages[p], 100.0 * (double) bad / (960.0 * 544.0));
		EXPECT(bad * 1000 < 960L * 544 * 8);       /* under 0.8%: edges and rounding only */
		rv.menu.release_textures();
		rs.menu.release_textures();
	}
}

/* 4. a texture made on one thread and drawn on another is reported (the draw-thread hazard) */
static void thread_hazard_is_seen()
{
	fakegxm::reset();
	VitaGfx g;
	Texture *t = g.tex_create(8, 8, TEX_A8);
	uint8_t px[64];
	memset(px, 200, sizeof px);
	std::thread th([&]() { g.tex_update(t, px, 8); });
	th.join();
	g.tex_draw(t, 0, 0, 8, 8, 0, 0, 8, 8, 0xFFFFFFFFu, false);
	EXPECT(fakegxm::tex_draws().size() == 1 && !fakegxm::tex_draws()[0].same_thread);
	g.tex_free(t);
}

/* 5. the main app's own texture table (gpsp/vita/vback_vita2d.c): its A8 slot, the one every in-process console's menu text
 * and the 2.0 overlays use, must be the alpha-from-byte swizzle too. The file is C for the console; read it as text. */
static void app_text_format()
{
	FILE *f = fopen("../gpsp/vita/vback_vita2d.c", "r");
	if (!f) { fprintf(stderr, "vita_gxm: FAIL cannot open the app's vback_vita2d.c\n"); g_fail++; return; }
	std::string src;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
	fclose(f);
	size_t a = src.find("gxm_fmt[4]");
	EXPECT(a != std::string::npos);
	std::string tab = a == std::string::npos ? "" : src.substr(a, 400);
	size_t i565 = tab.find("U5U6U5_RGB"), ibgr = tab.find("U5U6U5_BGR"), ia8 = tab.find("U8_"), i32 = tab.find("U8U8U8U8_ABGR");
	EXPECT(i565 < ibgr && ibgr < ia8 && ia8 < i32);       /* slot 2 = VB_FMT_A8 */
	EXPECT(tab.compare(ia8, 7, "U8_R111") == 0);
	fakegxm::Swizzle sw = fakegxm::decode_u8(SCE_GXM_TEXTURE_FORMAT_U8_R111);
	EXPECT(sw.alpha_from_data && !sw.rgb_from_data);
}

int main()
{
	text_is_not_boxes();
	menu_contrast();
	same_as_soft();
	thread_hazard_is_seen();
	app_text_format();
	printf("vita_gxm: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
