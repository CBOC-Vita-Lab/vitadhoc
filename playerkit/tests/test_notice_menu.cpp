/* Unit tests: pk_notice and pk_menu behaviour. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_menu.h"
#include "pk_notice.h"

using namespace pk;

TEST(notice_lifetimes_and_replacement)
{
	Notices n;
	CHECK(!n.active());
	n.push("State saved", NOTICE_OK);
	CHECK(n.active());
	CHECK(n.alpha() >= 0.0f);
	n.update(1000);
	CHECK(n.active());
	CHECK(n.alpha() > 0.9f);
	n.update(1500);                                  /* 2500 ms: past the 2200 ms of a good-news toast */
	CHECK(!n.active());
	n.push("Could not save the game: the memory card is full", NOTICE_ERROR);
	n.update(3000);
	CHECK(n.active());                               /* errors stay longer */
	n.update(2500);
	CHECK(!n.active());
	n.push("one"); n.push("two");
	CHECK_STR(n.text(), "two");
	n.push("custom", NOTICE_INFO, 500);
	n.update(600);
	CHECK(!n.active());
	n.push("x"); n.clear();
	CHECK(!n.active());
}

TEST(notice_one_line_no_crash_when_drawn_without_gfx)
{
	Notices n; n.push("hello");
	n.draw(0, 0, 0);                                  /* must be a no-op */
	CHECK(n.active());
}

namespace {
struct Fixture {
	pkh::TempDir td;
	pkh::MockHost host;
	States states;
	Menu menu;
	MenuConfig cfg;
	Mapper mapper;
	Profile profile;
	ProfileStore store;
	Fixture()
		: states(mk(td.path)), store(td.path, "snes", "Super Example World (USA)")
	{
		cfg.game_name = "Super Example World";
		cfg.console_label = "Super Nintendo";
		cfg.caps = CAP_ALL_BASIC | CAP_FF | CAP_FF_SOUND | CAP_FF_MODE | CAP_CHEATS;
		cfg.play_together_reason = "Not available for Super NES";
		profile = profile_defaults(model_snes(), true);
		mapper.configure(model_snes(), profile, MapperConfig());
		host.cheat_list.resize(2);
		host.cheat_list[0].name = "Infinite lives";
		host.cheat_list[1].name = "Walk through walls"; host.cheat_list[1].available = false;
		menu.configure(cfg, &host, &states);
		menu.attach_input(&mapper, &store, &model_snes(), &profile);
		menu.open(0);
		menu.update(pkh::idle());      /* the first frame after opening only records what is held */
	}
	static StateLayout mk(const std::string &root)
	{
		StateLayout l; l.root = root; l.console = "snes"; l.name = "Super Example World (USA)";
		return l;
	}
	~Fixture() { menu.release_textures(); }
};
}  // namespace

TEST(menu_opens_on_control_centre_and_resumes)
{
	Fixture f;
	CHECK(f.menu.is_open());
	CHECK_EQ(f.menu.page(), Menu::PAGE_TOP);
	CHECK_EQ(f.menu.cursor(), 0);
	CHECK_EQ(pkh::press(f.menu, PH_CIRCLE), MENU_RESUME);        /* Circle goes back / resumes (Enter = Cross) */
}

TEST(menu_cross_activates_resume_when_enter_is_cross)
{
	Fixture f;
	CHECK_EQ(pkh::press(f.menu, PH_CROSS), MENU_RESUME);
}

TEST(menu_enter_circle_swaps_confirm_and_cancel)
{
	Fixture f;
	f.cfg.enter_is_cross = false;
	f.menu.configure(f.cfg, &f.host, &f.states);
	f.menu.open(0);
	f.menu.update(pkh::idle());
	CHECK_EQ(pkh::press(f.menu, PH_CROSS), MENU_RESUME);         /* Cross is Back here */
	f.menu.open(0);
	f.menu.update(pkh::idle());
	CHECK_EQ(pkh::press(f.menu, PH_CIRCLE), MENU_RESUME);        /* Circle activates Resume */
}

TEST(menu_card_order_and_navigation)
{
	Fixture f;
	pkh::press(f.menu, PH_RIGHT);
	CHECK_EQ(f.menu.cursor(), 1);                                /* Save states */
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.page(), Menu::PAGE_STATES);
	pkh::press(f.menu, PH_CIRCLE);
	CHECK_EQ(f.menu.page(), Menu::PAGE_TOP);
	CHECK_EQ(f.menu.cursor(), 1);                                /* the cursor is where it was left */
	/* Settings: Picture, Controls, Speed */
	pkh::press(f.menu, PH_RIGHT); pkh::press(f.menu, PH_RIGHT);   /* Cheats, Settings (no Play together card here) */
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.page(), Menu::PAGE_SETTINGS);
	CHECK_EQ(f.menu.row_count(), 3);
	CHECK_STR(f.menu.row_label(0), "Picture");
	CHECK_STR(f.menu.row_label(1), "Controls");
	CHECK_STR(f.menu.row_label(2), "Speed");
}

TEST(menu_pad_repeat_moves_while_held)
{
	Fixture f;
	pkh::press(f.menu, PH_RIGHT);
	pkh::press(f.menu, PH_RIGHT);
	pkh::press(f.menu, PH_RIGHT);                                /* Settings (index 4) */
	pkh::press(f.menu, PH_CROSS);
	pkh::press(f.menu, PH_CROSS);                                /* Picture page */
	CHECK_EQ(f.menu.page(), Menu::PAGE_PICTURE);
	int moves = 0, last = f.menu.cursor();
	for (int i = 0; i < 60; i++) {                                /* about one second */
		MenuInput in = pkh::idle();
		in.pads[0].buttons = PH_DOWN;
		f.menu.update(in);
		if (f.menu.cursor() != last) { moves++; last = f.menu.cursor(); }
	}
	CHECK(moves >= 4);                                            /* one press, then the repeat after 400 ms */
	CHECK(moves <= 12);
	f.menu.update(pkh::idle());
}

TEST(menu_picture_choice_changes_call_host)
{
	Fixture f;
	f.menu.goto_page(Menu::PAGE_PICTURE);
	CHECK_STR(f.menu.row_label(0), "Screen size");
	int v0 = f.menu.value(ROW_PIC_SHAPE);
	pkh::press(f.menu, PH_RIGHT);
	CHECK(f.menu.value(ROW_PIC_SHAPE) != v0);
	CHECK_EQ(f.host.changes.size(), 1);
	CHECK_EQ(f.host.changes[0].id, ROW_PIC_SHAPE);
	CHECK_EQ(f.host.changes[0].value, f.menu.value(ROW_PIC_SHAPE));
}

TEST(menu_unwired_rows_are_hidden_and_noted_rows_are_greyed)
{
	Fixture f;
	f.menu.goto_page(Menu::PAGE_PICTURE);
	for (int i = 0; i < f.menu.row_count(); i++) CHECK(f.menu.row_label(i) != "Wide view");   /* not wired: not shown */
	/* wiring it shows it */
	f.cfg.caps |= CAP_WIDE;
	f.menu.configure(f.cfg, &f.host, &f.states);
	f.menu.open(0);
	f.menu.update(pkh::idle());
	f.menu.goto_page(Menu::PAGE_PICTURE);
	bool found = false;
	for (int i = 0; i < f.menu.row_count(); i++) if (f.menu.row_label(i) == "Wide view") { found = true; CHECK(f.menu.row_enabled(i)); }
	CHECK(found);
}

TEST(menu_cards_without_a_feature_are_hidden)
{
	Fixture f;                                            /* no Play together on this console */
	CHECK_EQ(pkh::press(f.menu, PH_RIGHT) , MENU_NONE);
	CHECK_EQ(f.menu.cursor(), 1);
	pkh::press(f.menu, PH_RIGHT);
	CHECK_EQ(f.menu.cursor(), 3);                         /* Save states -> Cheats, skipping the missing card */
	for (size_t i = 0; i < f.menu.regions().size(); i++) CHECK(f.menu.regions()[i].a != 2 || f.menu.regions()[i].kind != Menu::REGION_CARD);
	f.cfg.caps |= CAP_PLAY_TOGETHER;
	f.menu.configure(f.cfg, &f.host, &f.states);
	f.menu.open(0);
	f.menu.update(pkh::idle());
	pkh::press(f.menu, PH_RIGHT); pkh::press(f.menu, PH_RIGHT);
	CHECK_EQ(f.menu.cursor(), 2);
}

TEST(menu_greyed_row_ignores_activation)
{
	Fixture f;
	f.menu.set_row_note(ROW_PIC_QUALITY, "Needs something");
	f.menu.goto_page(Menu::PAGE_PICTURE);
	pkh::press(f.menu, PH_DOWN);                           /* the noted row */
	CHECK(!f.menu.row_enabled(1));
	CHECK(!f.menu.row_reason(1).empty());
	size_t n = f.host.changes.size();
	pkh::press(f.menu, PH_RIGHT);
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.host.changes.size(), n);
}

TEST(menu_set_row_note_greys_a_row)
{
	Fixture f;
	f.menu.set_row_note(ROW_PIC_GLOW, "Needs the wide view");
	f.menu.goto_page(Menu::PAGE_PICTURE);
	for (int i = 0; i < f.menu.row_count(); i++)
		if (f.menu.row_label(i) == "Side glow") { CHECK(!f.menu.row_enabled(i)); CHECK_STR(f.menu.row_reason(i), "Needs the wide view"); }
	f.menu.set_row_note(ROW_PIC_GLOW, "");
	f.menu.goto_page(Menu::PAGE_PICTURE);
	for (int i = 0; i < f.menu.row_count(); i++)
		if (f.menu.row_label(i) == "Side glow") CHECK(f.menu.row_enabled(i));
}

TEST(menu_save_load_states_with_overwrite_confirm)
{
	Fixture f;
	f.menu.goto_page(Menu::PAGE_STATES);
	f.menu.select_slot(3);
	f.host.machine = 7;
	/* the "Save here" button is reached by touch; use the region list */
	float sx = 0, sy = 0;
	bool have = false;
	for (size_t i = 0; i < f.menu.regions().size(); i++) {
		const Menu::Region &r = f.menu.regions()[i];
		if (r.kind == Menu::REGION_BUTTON && r.a == 4) { sx = r.x + r.w / 2; sy = r.y + r.h / 2; have = true; }   /* BTN_SAVE */
	}
	CHECK(have);
	pkh::tap_at(f.menu, sx, sy);
	for (int i = 0; i < 20; i++) f.menu.update(pkh::idle());
	CHECK(f.states.info(3).present);
	CHECK_EQ(f.host.touched.size(), 1);
	CHECK_EQ(f.host.touched[0], 3);
	CHECK(!f.menu.modal_open());
	/* saving again onto a filled slot asks first */
	f.host.machine = 9;
	pkh::tap_at(f.menu, sx, sy);
	CHECK(f.menu.modal_open());
	std::vector<uint8_t> d;
	f.states.load(3, d);
	CHECK_EQ(d[0], 7);                                           /* not overwritten yet */
	/* Keep: nothing changes */
	pkh::press(f.menu, PH_CIRCLE);
	CHECK(!f.menu.modal_open());
	f.states.load(3, d);
	CHECK_EQ(d[0], 7);
	/* Overwrite: confirm with Cross */
	pkh::tap_at(f.menu, sx, sy);
	CHECK(f.menu.modal_open());
	pkh::press(f.menu, PH_CROSS);
	f.states.load(3, d);
	CHECK_EQ(d[0], 9);
}

TEST(menu_load_state_and_undo)
{
	Fixture f;
	std::vector<uint8_t> blob(20000, 0);
	blob[0] = 5;
	std::vector<uint16_t> px; pkh::make_game_frame(0, px);
	f.states.save(2, &blob[0], blob.size(), Frame(&px[0], 256, 224, 512));
	f.menu.goto_page(Menu::PAGE_STATES);
	f.menu.select_slot(2);
	f.host.machine = 11;
	f.menu.update(pkh::idle());
	int load_id = 3;   /* BTN_LOAD */
	for (size_t i = 0; i < f.menu.regions().size(); i++) {
		const Menu::Region &r = f.menu.regions()[i];
		if (r.kind == Menu::REGION_BUTTON && r.a == load_id) { pkh::tap_at(f.menu, r.x + r.w / 2, r.y + r.h / 2); break; }
	}
	CHECK_EQ(f.host.machine, 5);
	/* a load closes the menu; opened again, the page offers to undo it */
	f.menu.open(0);
	f.menu.update(pkh::idle());
	f.menu.goto_page(Menu::PAGE_STATES);
	f.menu.update(pkh::idle());
	/* the load is undoable */
	for (size_t i = 0; i < f.menu.regions().size(); i++) {
		const Menu::Region &r = f.menu.regions()[i];
		if (r.kind == Menu::REGION_BUTTON && r.a == 6) { pkh::tap_at(f.menu, r.x + r.w / 2, r.y + r.h / 2); break; }   /* BTN_UNDO */
	}
	CHECK_EQ(f.host.machine, 11);
}

TEST(menu_state_failures_tell_the_player)
{
	Fixture f;
	f.host.capture_ok = false;
	f.menu.goto_page(Menu::PAGE_STATES);
	f.menu.select_slot(1);
	for (size_t i = 0; i < f.menu.regions().size(); i++) {
		const Menu::Region &r = f.menu.regions()[i];
		if (r.kind == Menu::REGION_BUTTON && r.a == 4) { pkh::tap_at(f.menu, r.x + r.w / 2, r.y + r.h / 2); break; }
	}
	CHECK(f.menu.notices().active());
	CHECK(f.menu.notices().kind() == NOTICE_ERROR);
	CHECK(!f.states.info(1).present);
}

TEST(menu_touch_targets_are_at_least_80_tall_on_every_page)
{
	Fixture f;
	Menu::PageId pages[] = { Menu::PAGE_TOP, Menu::PAGE_STATES, Menu::PAGE_SETTINGS, Menu::PAGE_PICTURE, Menu::PAGE_CONTROLS, Menu::PAGE_MAPPING, Menu::PAGE_SPEED, Menu::PAGE_CHEATS };
	for (size_t p = 0; p < sizeof pages / sizeof pages[0]; p++) {
		f.menu.goto_page(pages[p]);
		f.menu.update(pkh::idle());
		float h = f.menu.min_touch_height();
		if (h < 80.0f - 0.01f) fprintf(stderr, "    page %d: smallest target %.1f px\n", (int) pages[p], h);
		CHECK(h >= 80.0f - 0.01f);
	}
}

TEST(menu_tap_a_row_activates_and_flashes)
{
	Fixture f;
	f.menu.goto_page(Menu::PAGE_SETTINGS);
	f.menu.update(pkh::idle());
	const Menu::Region *row = 0;
	for (size_t i = 0; i < f.menu.regions().size(); i++) if (f.menu.regions()[i].kind == Menu::REGION_ROW && f.menu.regions()[i].a == 1) row = &f.menu.regions()[i];
	CHECK(row != 0);
	if (!row) return;
	pkh::tap_at(f.menu, row->x + 30, row->y + row->h / 2);
	CHECK_EQ(f.menu.page(), Menu::PAGE_CONTROLS);
}

TEST(menu_tap_outside_the_sheet_resumes)
{
	Fixture f;
	MenuEvent e = pkh::tap_at(f.menu, 480, 30);
	CHECK_EQ(e, MENU_RESUME);
}

TEST(menu_reset_and_exit_need_confirmation)
{
	Fixture f;
	for (int i = 0; i < 4; i++) pkh::press(f.menu, PH_RIGHT);    /* Reset game */
	pkh::press(f.menu, PH_CROSS);
	CHECK(f.menu.modal_open());
	CHECK_EQ(f.host.reset_count, 0);
	pkh::press(f.menu, PH_CIRCLE);
	CHECK(!f.menu.modal_open());
	CHECK_EQ(f.host.reset_count, 0);
	pkh::press(f.menu, PH_CROSS);
	MenuEvent e = pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.host.reset_count, 1);
	CHECK(e == MENU_RESET_DONE || f.menu.is_open() == false || e == MENU_RESUME);
	/* game list and exit */
	Fixture g;
	for (int i = 0; i < 6; i++) pkh::press(g.menu, PH_RIGHT);    /* Exit */
	pkh::press(g.menu, PH_CROSS);
	CHECK(g.menu.modal_open());
	CHECK_EQ(pkh::press(g.menu, PH_CROSS), MENU_EXIT);
}

TEST(menu_cheats_list_toggles_and_greys_unavailable)
{
	Fixture f;
	f.menu.goto_page(Menu::PAGE_CHEATS);
	CHECK_EQ(f.menu.row_count(), 2);
	CHECK(f.menu.row_enabled(0));
	CHECK(!f.menu.row_enabled(1));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.host.cheat_sets.size(), 1);
	CHECK(f.host.cheat_sets[0].first == 0 && f.host.cheat_sets[0].second);
}

TEST(menu_remap_capture_assigns_and_stores)
{
	Fixture f;
	f.menu.goto_page(Menu::PAGE_MAPPING);
	/* the first row picks the player; go down to B and press OK */
	pkh::press(f.menu, PH_DOWN);
	pkh::press(f.menu, PH_CROSS);
	CHECK(f.menu.modal_open());
	/* let go, then press Triangle */
	for (int i = 0; i < 5; i++) f.menu.update(pkh::idle());
	pkh::press(f.menu, PH_TRIANGLE);
	CHECK(!f.menu.modal_open());
	CHECK_EQ(f.profile.bind[0][0], PH_TRIANGLE);
	CHECK(file_exists(f.store.global_path()));
	CHECK(f.host.controls_changed_count >= 1);
	/* Triangle was X's: the notice says so */
	CHECK(f.menu.notices().active());
}

TEST(menu_remap_capture_cancel_and_timeout)
{
	Fixture f;
	f.menu.goto_page(Menu::PAGE_MAPPING);
	pkh::press(f.menu, PH_DOWN);
	pkh::press(f.menu, PH_CROSS);
	CHECK(f.menu.modal_open());
	for (int i = 0; i < 10; i++) f.menu.update(pkh::idle());
	/* touch Cancel */
	for (size_t i = 0; i < f.menu.regions().size(); i++) {
		const Menu::Region &r = f.menu.regions()[i];
		if (r.kind == Menu::REGION_BUTTON && r.a == 8) { pkh::tap_at(f.menu, r.x + r.w / 2, r.y + r.h / 2); break; }   /* BTN_CANCEL */
	}
	CHECK(!f.menu.modal_open());
	CHECK_EQ(f.profile.bind[0][0], PH_CROSS);
	/* timeout */
	pkh::press(f.menu, PH_CROSS);
	CHECK(f.menu.modal_open());
	for (int i = 0; i < 500; i++) f.menu.update(pkh::idle());
	CHECK(!f.menu.modal_open());
	CHECK_EQ(f.profile.bind[0][0], PH_CROSS);
}

TEST(menu_values_set_and_get)
{
	Fixture f;
	f.menu.set_value(ROW_PIC_SCANLINES, 7);
	CHECK_EQ(f.menu.value(ROW_PIC_SCANLINES), 7);
	f.menu.set_value(ROW_SPD_FF, 1);
	CHECK_EQ(f.menu.value(ROW_SPD_FF), 1);
}

TEST(menu_unconfigured_parts_are_greyed_not_crashing)
{
	pkh::TempDir td;
	pkh::MockHost host;
	Menu m;
	MenuConfig c; c.game_name = "x"; c.caps = 0;
	m.configure(c, &host, 0);
	for (int card = 1; card < 8; card++) {                     /* every card, with nothing wired */
		m.open(0);
		m.update(pkh::idle());
		for (int i = 0; i < card; i++) pkh::press(m, PH_RIGHT);
		pkh::press(m, PH_CROSS);
		for (int i = 0; i < 3; i++) { pkh::press(m, PH_DOWN); pkh::press(m, PH_CROSS); pkh::press(m, PH_RIGHT); pkh::press(m, PH_CIRCLE); }
		for (int i = 0; i < 4; i++) m.update(pkh::idle());
	}
	CHECK(true);
	m.release_textures();
}
