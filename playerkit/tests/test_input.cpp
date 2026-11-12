/* Unit tests: pk_input. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_input.h"
#include <string.h>

using namespace pk;

namespace {
struct Rig {
	Mapper m;
	PadSample pads[MAX_PLAYERS];
	uint64_t ms;
	uint32_t frame;
	InputResult r;
	Rig(const PadModel &model = model_snes(), bool cross = true)
		: ms(1000), frame(0)
	{
		MapperConfig c;
		m.configure(model, profile_defaults(model, cross), c);
		pads[0].connected = true;
	}
	void set_profile(const Profile &p) { m.set_profile(p); }
	/* one frame at 60 Hz */
	void step(uint32_t buttons, uint8_t lx = 128, uint8_t ly = 128, uint8_t back = 0)
	{
		pads[0].buttons = buttons; pads[0].lx = lx; pads[0].ly = ly; pads[0].back = back;
		ms += 16; frame++;
		m.update(pads, ms, frame, &r);
	}
	void steps(int n, uint32_t buttons) { for (int i = 0; i < n; i++) step(buttons); }
};
}  // namespace

TEST(input_phys_names_and_parse)
{
	CHECK_STR(phys_format(0), "NONE");
	CHECK_STR(phys_format(PH_SELECT | PH_L), phys_format(PH_L | PH_SELECT));
	uint32_t m;
	CHECK(phys_parse("select + l", &m) && m == (PH_SELECT | PH_L));
	CHECK(phys_parse("CROSS", &m) && m == PH_CROSS);
	CHECK(!phys_parse("", &m));
	CHECK(!phys_parse("banana", &m));
	for (int b = 0; b < PH_COUNT; b++) {
		CHECK(phys_parse(phys_name(b), &m) && m == (1u << b));
	}
	CHECK_STR(phys_label(PH_CROSS, true), "Cross");
	CHECK(!phys_label(PH_LS_UP, true).empty());
}

TEST(input_models_match_host_flags)
{
	const PadModel &s = model_snes();
	CHECK_EQ(s.n, 12);
	CHECK(model_by_name("snes") == &s);
	CHECK(model_by_name("nes") == &model_nes());
	CHECK(model_by_name("zx") == 0);
	/* every out_bit distinct */
	for (int i = 0; i < s.n; i++) for (int j = i + 1; j < s.n; j++) CHECK(s.btn[i].out_bit != s.btn[j].out_bit);
	const PadModel &n = model_nes();
	for (int i = 0; i < n.n; i++) for (int j = i + 1; j < n.n; j++) CHECK(n.btn[i].out_bit != n.btn[j].out_bit);
}

TEST(input_default_mapping_and_enter_swap)
{
	Rig r;
	r.step(PH_CROSS);
	CHECK(r.r.pad[0] == model_snes().btn[0].out_bit);        /* Cross -> B */
	r.step(PH_CIRCLE);
	CHECK(r.r.pad[0] == model_snes().btn[1].out_bit);        /* Circle -> A */
	r.step(PH_UP | PH_LEFT);
	CHECK(r.r.pad[0] == (model_snes().btn[8].out_bit | model_snes().btn[10].out_bit));
	/* Enter = Circle: the SNES layout is fixed (B stays on Cross), but the profile still builds */
	Profile p = profile_defaults(model_snes(), false);
	CHECK(profile_is_default(p, model_snes(), false));
	/* NES: A is on the Enter button */
	Rig n(model_nes(), true);
	n.step(PH_CROSS);
	CHECK(n.r.pad[0] != 0);
	Rig n2(model_nes(), false);
	n2.step(PH_CIRCLE);
	CHECK(n2.r.pad[0] != 0);
	CHECK_EQ(n2.r.pad[1], 0);
}

TEST(input_four_players_independent)
{
	Rig r;
	r.pads[1].connected = true; r.pads[1].buttons = PH_SQUARE;
	r.step(PH_CROSS);
	CHECK(r.r.pad[0] != 0);
	CHECK(r.r.pad[1] == model_snes().btn[2].out_bit);   /* Square -> Y */
	CHECK_EQ(r.r.pad[2], 0);
	/* a pad that is not connected sends nothing */
	r.pads[1].connected = false;
	r.step(0);
	CHECK_EQ(r.r.pad[1], 0);
}

TEST(input_stick_modes_and_hysteresis)
{
	Rig r;
	r.step(0, 128, 128);
	CHECK_EQ(r.r.pad[0], 0);
	r.step(0, 128 + 80, 128);                            /* right, past the engage point */
	CHECK(r.r.pad[0] == model_snes().btn[11].out_bit);
	r.step(0, 128 + 60, 128);                            /* between off and on: stays engaged */
	CHECK(r.r.pad[0] == model_snes().btn[11].out_bit);
	r.step(0, 128 + 40, 128);                            /* below off: released */
	CHECK_EQ(r.r.pad[0], 0);
	r.step(0, 128 + 60, 128);                            /* in the band from rest: not engaged */
	CHECK_EQ(r.r.pad[0], 0);
	/* stick off */
	Profile p = profile_defaults(model_snes(), true);
	p.stick_left = STICK_OFF;
	r.set_profile(p);
	r.step(0, 255, 128);
	CHECK_EQ(r.r.pad[0], 0);
	/* custom: the direction is its own input, bindable to a button */
	p.stick_left = STICK_CUSTOM;
	profile_assign(p, model_snes(), 0, 0, PH_LS_UP, 0);   /* B = left stick up */
	r.set_profile(p);
	r.step(0, 128, 128 - 100);
	CHECK(r.r.pad[0] == model_snes().btn[0].out_bit);
}

TEST(input_back_touch_zones)
{
	Rig r;
	Profile p = profile_defaults(model_snes(), true);
	p.back_touch = true;
	profile_assign(p, model_snes(), 0, 6, PH_BACK_TL, 0);   /* Start = rear top left */
	r.set_profile(p);
	r.step(0, 128, 128, 0);
	CHECK_EQ(r.r.pad[0], 0);
	/* hold long enough for the Start guard to let it through */
	for (int i = 0; i < 12; i++) r.step(0, 128, 128, 1);
	CHECK(r.r.pad[0] == model_snes().btn[6].out_bit);
	p.back_touch = false;
	r.set_profile(p);
	r.step(0, 128, 128, 1);
	CHECK_EQ(r.r.pad[0], 0);
}

TEST(input_assign_rules)
{
	const PadModel &m = model_snes();
	Profile p = profile_defaults(m, true);
	int stolen = -2;
	/* the new binding steals the input from the old holder */
	CHECK_EQ(profile_assign(p, m, 0, 1, PH_CROSS, &stolen), ASSIGN_OK);   /* A = Cross (was B's) */
	CHECK_EQ(stolen, 0);
	CHECK_EQ(p.bind[0][0], 0);
	CHECK_EQ(p.bind[0][1], PH_CROSS);
	/* a game button takes one input only */
	CHECK_EQ(profile_assign(p, m, 0, 1, PH_CROSS | PH_L, 0), ASSIGN_SINGLE_ONLY);
	/* shortcut chords */
	CHECK_EQ(profile_assign(p, m, 0, ACT_QSAVE, PH_SELECT | PH_R | PH_L, 0), ASSIGN_OK);
	CHECK_EQ(profile_assign(p, m, 0, ACT_QSAVE, PH_START | PH_SELECT, 0), ASSIGN_RESERVED);
	CHECK_EQ(profile_assign(p, m, 0, ACT_QSAVE, PH_L | PH_R | PH_CROSS | PH_CIRCLE | PH_UP, 0), ASSIGN_TOO_MANY);
	CHECK_EQ(profile_assign(p, m, 1, ACT_FF, PH_L, 0), ASSIGN_NOT_PLAYER1);
	CHECK_EQ(profile_assign(p, m, 0, 99, PH_L, 0), ASSIGN_BAD_ACTION);
	CHECK(assign_result_text(ASSIGN_RESERVED)[0] != 0);
	/* clearing */
	CHECK_EQ(profile_assign(p, m, 0, 2, 0, 0), ASSIGN_OK);
	CHECK_EQ(p.bind[0][2], 0);
}

TEST(input_profile_ini_round_trip)
{
	const PadModel &m = model_snes();
	Profile p = profile_defaults(m, true);
	p.stick_left = STICK_CUSTOM; p.stick_right = STICK_MOVE; p.back_touch = true;
	profile_assign(p, m, 0, 0, PH_TRIANGLE, 0);
	profile_assign(p, m, 1, 3, PH_CIRCLE, 0);
	profile_assign(p, m, 0, ACT_FF, PH_R | PH_RS_RIGHT, 0);
	p.turbo_half[0] = 1; p.turbo_toggle[1] = 1;
	Profile q;
	Ini ini; ini.parse(profile_to_ini(p, m).serialize());
	CHECK_EQ(profile_parse(ini, m, true, q), 0);
	CHECK_EQ(q.stick_left, STICK_CUSTOM);
	CHECK_EQ(q.stick_right, STICK_MOVE);
	CHECK(q.back_touch);
	CHECK_EQ(q.bind[0][0], PH_TRIANGLE);
	CHECK_EQ(q.bind[1][3], PH_CIRCLE);
	CHECK_EQ(q.bind[0][ACT_FF], PH_R | PH_RS_RIGHT);
	CHECK_EQ(q.turbo_half[0], 1);
	CHECK_EQ(q.turbo_toggle[1], 1);
	CHECK(!profile_is_default(q, m, true));
	CHECK(profile_is_default(profile_defaults(m, true), m, true));
}

TEST(input_profile_parse_rejects_bad_values)
{
	const PadModel &m = model_snes();
	Ini ini;
	ini.parse("stick=sideways\nbind_a=banana\nbind_b=cross+l\ndeadzone=5\nturbo_a_rate=7\nbind_quicksave=start+select\n");
	Profile q;
	int rej = profile_parse(ini, m, true, q);
	CHECK(rej >= 5);
	Profile d = profile_defaults(m, true);
	CHECK_EQ(q.stick_left, d.stick_left);
	CHECK_EQ(q.bind[0][1], d.bind[0][1]);
	CHECK_EQ(q.bind[0][0], d.bind[0][0]);
	CHECK_EQ(q.bind[0][ACT_QSAVE], d.bind[0][ACT_QSAVE]);
	/* an empty file is the defaults */
	Ini e;
	CHECK_EQ(profile_parse(e, m, true, q), 0);
	CHECK(profile_is_default(q, m, true));
}

TEST(input_profile_store_global_and_per_game)
{
	pkh::TempDir td;
	const PadModel &m = model_snes();
	ProfileStore st(td.path, "snes", "Game One");
	Profile p, out;
	CHECK_EQ(st.load_effective(m, true, out), 0);
	CHECK(profile_is_default(out, m, true));
	p = profile_defaults(m, true);
	profile_assign(p, m, 0, 0, PH_SQUARE, 0);
	CHECK_EQ(st.save_global(m, p), W_OK);
	st.load_effective(m, true, out);
	CHECK_EQ(out.bind[0][0], PH_SQUARE);
	CHECK(!out.own);
	/* a per-game profile wins for that game only */
	Profile g = p; profile_assign(g, m, 0, 0, PH_TRIANGLE, 0);
	CHECK_EQ(st.save_for(m, g, true), W_OK);
	st.load_effective(m, true, out);
	CHECK_EQ(out.bind[0][0], PH_TRIANGLE);
	CHECK(out.own);
	ProfileStore other(td.path, "snes", "Game Two");
	other.load_effective(m, true, out);
	CHECK_EQ(out.bind[0][0], PH_SQUARE);
	/* "all games" again: the game's file goes away */
	CHECK_EQ(st.save_for(m, g, false), W_OK);
	CHECK(!file_exists(st.game_path()));
	st.load_effective(m, true, out);
	CHECK_EQ(out.bind[0][0], PH_SQUARE);
	/* a damaged global file is read tolerantly */
	CHECK(fs().write_all(st.global_path(), (const uint8_t *) "bind_b=???\nstick=move\n\x01\x02", 25));
	st.load_effective(m, true, out);
	CHECK_EQ(out.stick_left, STICK_MOVE);
}

TEST(input_menu_chord_start_select)
{
	Rig r;
	int fired_at = -1;
	for (int i = 0; i < 30 && fired_at < 0; i++) { r.step(PH_START | PH_SELECT); if (r.r.menu) fired_at = i; }
	CHECK(fired_at >= 0);
	CHECK(fired_at <= 16);
	CHECK(r.r.menu_player == 0);
	/* the game never saw Start or Select while the chord was held */
	Rig q;
	for (int i = 0; i < 12; i++) { q.step(PH_START | PH_SELECT); CHECK_EQ(q.r.pad[0] & (model_snes().btn[6].out_bit | model_snes().btn[7].out_bit), 0); }
}

TEST(input_menu_chord_fires_once_until_released)
{
	Rig r;
	int fires = 0;
	for (int i = 0; i < 60; i++) { r.step(PH_START | PH_SELECT); if (r.r.menu) fires++; }
	CHECK_EQ(fires, 1);
	r.step(0);
	for (int i = 0; i < 20; i++) { r.step(PH_START | PH_SELECT); if (r.r.menu) fires++; }
	CHECK_EQ(fires, 2);
}

TEST(input_menu_chord_legacy_lr_start)
{
	Rig r;
	bool fired = false;
	for (int i = 0; i < 30; i++) { r.step(PH_L | PH_R | PH_START); if (r.r.menu) fired = true; }
	CHECK(fired);
	Rig n(model_nes());              /* the NES player has no legacy chord */
	fired = false;
	for (int i = 0; i < 30; i++) { n.step(PH_L | PH_R | PH_START); if (n.r.menu) fired = true; }
	CHECK(!fired);
}

TEST(input_chord_guard_start_tap_reaches_game)
{
	Rig r;
	const uint32_t START = model_snes().btn[6].out_bit;
	bool saw = false;
	for (int i = 0; i < 4; i++) { r.step(PH_START); saw |= (r.r.pad[0] & START) != 0; }
	for (int i = 0; i < 8; i++) { r.step(0); saw |= (r.r.pad[0] & START) != 0; }
	CHECK(saw);                       /* a quick tap is delivered (as a short press) */
}

TEST(input_chord_guard_hold_passes_after_wait)
{
	Rig r;
	const uint32_t START = model_snes().btn[6].out_bit;
	r.step(PH_START);
	CHECK_EQ(r.r.pad[0] & START, 0);                 /* held back at first */
	for (int i = 0; i < 12; i++) r.step(PH_START);
	CHECK(r.r.pad[0] & START);                       /* passes after the wait */
	r.step(0);
	r.step(0);
	CHECK_EQ(r.r.pad[0] & START, 0);
}

TEST(input_chord_guard_spoiled_by_partner)
{
	Rig r;
	const uint32_t SELECT = model_snes().btn[7].out_bit;
	/* Select + L is the quick-save chord: the game must not see Select */
	r.step(PH_SELECT);
	r.step(PH_SELECT | PH_L);
	for (int i = 0; i < 12; i++) { r.step(PH_SELECT | PH_L); CHECK_EQ(r.r.pad[0] & SELECT, 0); }
	/* guard off: Select is immediate */
	Rig g;
	MapperConfig c; c.chord_guard = false;
	g.m.configure(model_snes(), profile_defaults(model_snes(), true), c);
	g.step(PH_SELECT);
	CHECK(g.r.pad[0] & SELECT);
}

TEST(input_shortcuts_fire_once_on_edge)
{
	Rig r;
	int q = 0, l = 0;
	for (int i = 0; i < 20; i++) { r.step(PH_SELECT | PH_L); if (r.r.fired & FIRE_QSAVE) q++; if (r.r.fired & FIRE_QLOAD) l++; }
	CHECK_EQ(q, 1);
	CHECK_EQ(l, 0);
	r.step(0);
	for (int i = 0; i < 5; i++) { r.step(PH_SELECT | PH_R); if (r.r.fired & FIRE_QLOAD) l++; }
	CHECK_EQ(l, 1);
}

TEST(input_fast_forward_held_and_press)
{
	Rig r;
	r.step(0, 128, 128);
	/* default FF is the right stick pushed right */
	r.pads[0].rx = 255;
	r.step(0);
	CHECK(r.r.fired & FIRE_FF_HELD);
	CHECK(r.r.fired & FIRE_FF_PRESS);
	r.step(0);
	CHECK(r.r.fired & FIRE_FF_HELD);
	CHECK(!(r.r.fired & FIRE_FF_PRESS));
	r.pads[0].rx = 128;
	r.step(0);
	CHECK(!(r.r.fired & FIRE_FF_HELD));
}

TEST(input_longer_chord_wins)
{
	Rig r;
	Profile p = profile_defaults(model_snes(), true);
	profile_assign(p, model_snes(), 0, ACT_SHOT, PH_SELECT | PH_L | PH_CROSS, 0);
	r.set_profile(p);
	r.step(PH_SELECT);
	r.step(PH_SELECT | PH_L | PH_CROSS);
	/* the three-button chord was completed in one step: quick save (a subset) yields */
	CHECK(!(r.r.fired & FIRE_QSAVE));
}

TEST(input_turbo_counted_in_frames)
{
	Rig r;
	const uint32_t A = model_snes().btn[1].out_bit;
	Profile p = profile_defaults(model_snes(), true);
	p.turbo_half[0] = 2;                                    /* 15 per second: 2 on, 2 off */
	profile_assign(p, model_snes(), 0, ACT_TURBO_A, PH_TRIANGLE, 0);
	r.set_profile(p);
	std::string pat;
	for (int i = 0; i < 12; i++) { r.step(PH_TRIANGLE); pat += (r.r.pad[0] & A) ? '1' : '0'; }
	CHECK_STR(pat, "110011001100");
	r.step(0);
	CHECK_EQ(r.r.pad[0] & A, 0);
	CHECK(!r.m.turbo_running(0));
	/* toggle mode */
	p.turbo_toggle[0] = 1;
	r.set_profile(p);
	r.step(PH_TRIANGLE); r.step(0);
	CHECK(r.m.turbo_running(0));
	r.step(PH_TRIANGLE); r.step(0);
	CHECK(!r.m.turbo_running(0));
}

TEST(input_menu_open_close_blocks_held_buttons)
{
	Rig r;
	for (int i = 0; i < 3; i++) r.step(PH_CROSS);
	r.m.menu_opened();
	for (int i = 0; i < 3; i++) r.step(PH_CROSS);
	CHECK_EQ(r.r.pad[0], 0);             /* held since before the menu: kept out */
	r.m.menu_closed(r.pads);
	r.step(PH_CROSS);
	CHECK_EQ(r.r.pad[0], 0);
	r.step(0);
	r.step(PH_CROSS);
	CHECK(r.r.pad[0] != 0);              /* a fresh press works */
}

TEST(input_menu_closed_with_chord_held_does_not_reopen)
{
	Rig r;
	for (int i = 0; i < 30; i++) r.step(PH_START | PH_SELECT);
	r.m.menu_opened();
	r.pads[0].buttons = PH_START | PH_SELECT;
	r.m.menu_closed(r.pads);
	bool again = false;
	for (int i = 0; i < 40; i++) { r.step(PH_START | PH_SELECT); if (r.r.menu) again = true; }
	CHECK(!again);
}

TEST(input_capture_next_reports_fresh_inputs)
{
	Mapper m;
	PadSample s; s.connected = true;
	CHECK_EQ(m.capture_next(s), 0);
	s.buttons = PH_CROSS;                        /* held when the page opened is not a choice ... */
	m.capture_reset();
	CHECK_EQ(m.capture_next(s), 0);
	CHECK_EQ(m.capture_next(s), 0);
	s.buttons = 0;
	CHECK_EQ(m.capture_next(s), 0);
	s.buttons = PH_TRIANGLE;
	CHECK_EQ(m.capture_next(s), PH_TRIANGLE);
	CHECK_EQ(m.capture_next(s), 0);              /* reported once */
	s.buttons = 0; m.capture_next(s);
	s.ly = 0;                                    /* left stick up, even when the profile has it off */
	CHECK_EQ(m.capture_next(s), PH_LS_UP);
	s.ly = 128; m.capture_next(s);
	s.back = 2;
	CHECK_EQ(m.capture_next(s), PH_BACK_TR);
}

TEST(input_enter_convention)
{
	CHECK_EQ(phys_confirm(true), PH_CROSS);
	CHECK_EQ(phys_confirm(false), PH_CIRCLE);
	CHECK_EQ(phys_cancel(true), PH_CIRCLE);
	CHECK_EQ(phys_cancel(false), PH_CROSS);
}
