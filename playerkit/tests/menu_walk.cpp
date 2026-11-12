/*
 * Scripted walk through every menu page with the software pk_gfx: renders one
 * PNG per screen so the menu can be looked at. Also asserts the walk does what
 * it should (it is a test as well as a camera).
 *
 *   menu_walk <output folder>
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#include "harness.h"
#include "pk_gfx_soft.h"
#include <stdio.h>
#include <time.h>

using namespace pk;

static int g_fail = 0;
#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "menu_walk: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

struct Walk {
	SoftGfx gfx;
	Text text;
	Texture *game;
	std::string out;
	std::vector<std::string> shots;
	std::vector<std::vector<uint8_t> > thumbs_rgb;   /* downscaled copies for the contact sheets */
	Walk(const std::string &o) : gfx(), text(&gfx), game(0), out(o)
	{
		std::vector<uint16_t> px;
		pkh::make_game_frame(0, px);
		game = gfx.tex_create(256, 224, TEX_RGB565);
		gfx.tex_update(game, &px[0], 512);
	}
	~Walk() { gfx.tex_free(game); }

	void paint_game()
	{
		gfx.clear(0x050505);
		/* side glow, the way the players draw it */
		gfx.gradient_v(0, 0, 960, 544, PK_HEX(0x2a3f7a, 255), PK_HEX(0x7a3a2a, 255));
		gfx.fill_rect(0, 0, 118, 544, PK_HEX(0x000000, 150));
		gfx.fill_rect(842, 0, 118, 544, PK_HEX(0x000000, 150));
		gfx.tex_draw(game, 0, 0, 256, 224, 118, 0, 724, 544, 0xFFFFFFFFu, false);
	}
	void snap(Menu &m, const char *name)
	{
		paint_game();
		m.draw(&gfx, &text);
		std::string p = out + "/" + name + ".png";
		bool ok = gfx.write_png(p);
		EXPECT(ok);
		shots.push_back(p);
		/* a half-size copy for the contact sheet */
		std::vector<uint8_t> half(480 * 272 * 3);
		for (int y = 0; y < 272; y++)
			for (int x = 0; x < 480; x++) {
				int r = 0, g = 0, b = 0;
				for (int k = 0; k < 4; k++) {
					uint32_t c = gfx.pixel(x * 2 + (k & 1), y * 2 + (k >> 1));
					r += c & 255; g += (c >> 8) & 255; b += (c >> 16) & 255;
				}
				uint8_t *d = &half[((size_t) y * 480 + x) * 3];
				d[0] = (uint8_t) (r / 4); d[1] = (uint8_t) (g / 4); d[2] = (uint8_t) (b / 4);
			}
		thumbs_rgb.push_back(half);
	}
	void sheet(const char *name, size_t first, size_t count)
	{
		std::vector<uint8_t> big(960 * 544 * 3, 16);
		for (size_t i = 0; i < count && first + i < thumbs_rgb.size(); i++) {
			int ox = (int) (i % 2) * 480, oy = (int) (i / 2) * 272;
			for (int y = 0; y < 272; y++) memcpy(&big[((size_t) (oy + y) * 960 + ox) * 3], &thumbs_rgb[first + i][(size_t) y * 480 * 3], 480 * 3);
		}
		write_png_rgb(out + "/" + name + ".png", 960, 544, big.data());
	}
};

static void advance(Menu &m, int ms)
{
	for (int t = 0; t < ms; t += 16) m.update(pkh::idle(16));
}

static int64_t g_clock;
static int64_t fake_clock() { return g_clock; }

int main(int argc, char **argv)
{
	std::string out = argc > 1 ? argv[1] : "build/shots";
	make_dirs(out);
	pkh::TempDir tmp;
	setenv("TZ", "UTC", 1);
	tzset();
	set_clock_for_tests(fake_clock);

	StateLayout lay;
	lay.root = tmp.path;
	lay.console = "snes";
	lay.name = "Super Example World";
	States states(lay);
	pkh::MockHost host;
	/* five slots in use, saved on different days */
	static const int used[5] = { 1, 2, 3, 5, 8 };
	for (int i = 0; i < 5; i++) {
		g_clock = 1794132000 + i * 86400 * 3 + i * 4321;     /* November 2026 */
		host.machine = 10 + i;
		host.variant = i * 3 + 1;
		std::vector<uint8_t> data;
		host.capture_state(data);
		Frame f;
		host.current_frame(&f);
		SaveReport r = states.save(used[i], data.data(), data.size(), f, 3600 * (i + 1) + 600);
		EXPECT(r.status == ST_OK);
	}
	states.undo_store((const uint8_t *) "undo-state-bytes", 16);
	g_clock = 1794500000;
	host.variant = 0;
	host.cheat_list.resize(3);
	host.cheat_list[0].name = "Infinite lives"; host.cheat_list[0].on = true;
	host.cheat_list[1].name = "Start with 99 coins";
	host.cheat_list[2].name = "Walk through walls"; host.cheat_list[2].available = false;

	const PadModel &model = model_snes();
	Profile prof = profile_defaults(model, true);
	Mapper mapper;
	mapper.configure(model, prof, MapperConfig());
	ProfileStore store(tmp.path, "snes", lay.name);

	MenuConfig cfg;
	cfg.game_name = "Super Example World (USA)";
	cfg.console_label = "Super Nintendo";
	cfg.caps = CAP_ALL_BASIC | CAP_FF | CAP_FF_SOUND | CAP_FF_MODE | CAP_CHEATS;      /* no Wide view, no Rewind, no Play together */
	cfg.accent = 0x7B6CFF;
	cfg.exit_label = "Exit";
	cfg.play_together_reason = "Not available for Super NES: the player is a separate program and has no link";
	cfg.unsupported_reason = "Not available for Super NES";
	Menu menu;
	menu.configure(cfg, &host, &states);
	menu.attach_input(&mapper, &store, &model, &prof);
	menu.set_value(ROW_PIC_SHAPE, 1);
	menu.set_value(ROW_PIC_SCANLINES, 4);
	menu.set_value(ROW_PIC_GLOW, 6);
	menu.set_value(ROW_SPD_FF, 1);

	Walk w(out);
	menu.open(0);
	advance(menu, 200);
	w.snap(menu, "01_control_centre");
	EXPECT(menu.page() == Menu::PAGE_TOP);

	pkh::press(menu, PH_RIGHT);
	pkh::press(menu, PH_RIGHT);
	EXPECT(menu.cursor() == 3);                        /* Play together is not on this console, so it has no card */
	w.snap(menu, "02_control_centre_cheats");

	/* Save states */
	pkh::press(menu, PH_LEFT);
	EXPECT(menu.cursor() == 1);
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.page() == Menu::PAGE_STATES);
	pkh::press(menu, PH_DOWN);
	EXPECT(menu.selected_slot() == 2);
	advance(menu, 100);
	w.snap(menu, "03_save_states");
	pkh::press(menu, PH_SQUARE);
	EXPECT(menu.modal_open());
	w.snap(menu, "04_save_states_overwrite");
	pkh::press(menu, PH_CIRCLE);
	EXPECT(!menu.modal_open());
	/* an empty slot, with the toast after saving */
	pkh::press(menu, PH_DOWN);       /* slot 3 */
	pkh::press(menu, PH_DOWN);       /* slot 4: empty */
	pkh::press(menu, PH_DOWN);       /* slot 5 */
	pkh::press(menu, PH_UP);
	pkh::press(menu, PH_CROSS);      /* empty: saves at once */
	EXPECT(states.info(4).present);
	advance(menu, 100);
	w.snap(menu, "05_save_states_saved_toast");
	for (int i = 0; i < 9; i++) pkh::press(menu, PH_DOWN);
	w.snap(menu, "06_save_states_scrolled");
	pkh::press(menu, PH_CIRCLE);
	EXPECT(menu.page() == Menu::PAGE_TOP);

	/* Settings > Picture */
	for (int i = 0; i < 2; i++) pkh::press(menu, PH_RIGHT);
	EXPECT(menu.cursor() == 4);
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.page() == Menu::PAGE_SETTINGS);
	w.snap(menu, "07_settings");
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.page() == Menu::PAGE_PICTURE);
	advance(menu, 100);
	w.snap(menu, "08_picture");
	for (int i = 0; i < 5; i++) pkh::press(menu, PH_DOWN);
	advance(menu, 400);
	w.snap(menu, "09_picture_scrolled");
	/* a greyed row whose reason helps (Wide view, with nothing to apply it to, is hidden instead) */
	menu.set_row_note(ROW_PIC_GRID, "Needs Pixel perfect screen size");
	menu.goto_page(Menu::PAGE_PICTURE);
	for (int i = 0; i < 3; i++) pkh::press(menu, PH_DOWN);
	advance(menu, 400);
	w.snap(menu, "10_picture_greyed_with_reason");
	menu.set_row_note(ROW_PIC_GRID, "");
	menu.goto_page(Menu::PAGE_PICTURE);
	pkh::press(menu, PH_CIRCLE);
	pkh::press(menu, PH_DOWN);
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.page() == Menu::PAGE_CONTROLS);
	w.snap(menu, "11_controls");
	for (int i = 0; i < 3; i++) pkh::press(menu, PH_DOWN);
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.page() == Menu::PAGE_MAPPING);
	advance(menu, 200);
	w.snap(menu, "12_button_mapping");
	pkh::press(menu, PH_DOWN);
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.modal_kind() == 7 /* M_CAPTURE */);
	{
		/* the first frames only record what is already down */
		advance(menu, 100);
		w.snap(menu, "13_mapping_capture");
		pk::MenuInput in = pkh::idle();
		in.pads[0].buttons = PH_TRIANGLE;
		menu.update(in);
		menu.update(pkh::idle());
		EXPECT(!menu.modal_open());
		EXPECT(prof.bind[0][0] == PH_TRIANGLE);        /* B is now on Triangle */
		EXPECT(prof.bind[0][3] == 0);                  /* X lost it */
	}
	advance(menu, 100);
	w.snap(menu, "14_mapping_changed");
	pkh::press(menu, PH_CIRCLE);
	pkh::press(menu, PH_CIRCLE);
	pkh::press(menu, PH_DOWN);
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.page() == Menu::PAGE_SPEED);
	w.snap(menu, "15_speed");
	pkh::press(menu, PH_CIRCLE);
	pkh::press(menu, PH_CIRCLE);

	/* Cheats */
	pkh::press(menu, PH_LEFT);
	EXPECT(menu.cursor() == 3);
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.page() == Menu::PAGE_CHEATS);
	pkh::press(menu, PH_DOWN);
	pkh::press(menu, PH_CROSS);
	EXPECT(host.cheat_list[1].on);
	advance(menu, 100);
	w.snap(menu, "16_cheats");
	pkh::press(menu, PH_CIRCLE);

	/* Exit question, and an error toast */
	for (int i = 0; i < 4; i++) pkh::press(menu, PH_RIGHT);
	EXPECT(menu.cursor() == 7);
	pkh::press(menu, PH_CROSS);
	EXPECT(menu.modal_open());
	w.snap(menu, "17_exit_question");
	pkh::press(menu, PH_CIRCLE);
	menu.notices().push("Could not save the game: the memory card is full", NOTICE_ERROR);
	advance(menu, 400);
	w.snap(menu, "18_error_toast");

	/* touch: a finger down on a row, then the flash */
	menu.goto_page(Menu::PAGE_PICTURE);
	advance(menu, 100);
	{
		pk::MenuInput in = pkh::idle();
		in.touch.down = true; in.touch.x = 300; in.touch.y = 108 + 80 + 30;     /* second row */
		menu.update(in);
		advance(menu, 0);
		in.touch.down = true;
		menu.update(in);
		w.snap(menu, "19_touch_pressed");
		in.touch.down = false;
		menu.update(in);
		w.snap(menu, "20_touch_tap_flash");
	}

	w.sheet("contact_1", 0, 4);
	w.sheet("contact_2", 4, 4);
	w.sheet("contact_3", 8, 4);
	w.sheet("contact_4", 12, 4);
	w.sheet("contact_5", 16, 4);
	menu.release_textures();
	printf("menu_walk: %d screenshots in %s, %d failed checks\n", (int) w.shots.size(), out.c_str(), g_fail);
	return g_fail ? 1 : 0;
}
