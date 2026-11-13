/* Unit tests: pk_look (the look governor), against a deterministic model of games with cost profiles.
 * The model is a stand-in for a console: nothing here has run on a Vita.
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_look.h"
#include "pk_pace.h"
#include <math.h>
#include <string.h>
#include <functional>
#include <vector>

using namespace pk;

namespace {

/* rows: 1 = scaling quality (xBRZ), 2 = wide view */
const LookRung kLadder[5] = {
	{ "xBRZ 4x -> 3x",   1, 0.12f, 10 },
	{ "xBRZ 3x -> 2x",   1, 0.12f, 25 },
	{ "wide scout off",  2, 0.08f, 30 },
	{ "xBRZ 2x -> off",  1, 0.20f, 45 },
	{ "wide view off",   2, 0.10f, 60 },
};
const double kTrue[5] = { 0.12, 0.12, 0.08, 0.20, 0.10 };

struct Rng {
	uint32_t s;
	explicit Rng(uint32_t seed = 12345) : s(seed) {}
	double next() { s = s * 1664525u + 1013904223u; return ((s >> 8) & 0xFFFF) / 32768.0 - 1.0; }   /* -1..1 */
};

/* One game: the load it would have at the FULL look at second t, what each rung really saves, when scenes change. */
struct Sim {
	LookGovernor g;
	std::function<double(int)> base;            /* load at the full look */
	std::function<bool(int)> scene;             /* a scene change (safe point) this second */
	std::function<bool(int)> hitch;             /* one 70 ms frame this second */
	double wobble = 0.03;
	double truth[5];
	Rng rng;
	int t = 0;
	std::vector<int> change_t;                  /* the seconds the applied look changed */
	std::vector<int> bad_sec;                   /* 1 when 6 or more refreshes of the second missed */
	std::vector<uint32_t> mask_at;

	Sim() : rng(777) { for (int i = 0; i < 5; i++) truth[i] = kTrue[i]; scene = [](int) { return false; }; hitch = [](int) { return false; }; }

	void setup(float guess_scale = 1.f, bool all_ladder = true)
	{
		LookRung r[5];
		for (int i = 0; i < 5; i++) { r[i] = kLadder[i]; r[i].est_saving *= guess_scale; }
		CHECK(g.set_ladder(r, all_ladder ? 5 : 3));
	}

	void run(int seconds)
	{
		for (int k = 0; k < seconds; k++, t++) {
			uint32_t before = g.generation();
			if (scene(t)) g.safe_point(LOOK_SAFE_SCENE);
			double load = base(t);
			for (int i = 0; i < 5; i++) if (g.applied(i)) load -= truth[i];
			if (load < 0.05) load = 0.05;
			int missed = 0;
			double sum = 0;
			for (int f = 0; f < 60; f++) {
				double c = load * (1.0 + wobble * rng.next());
				if (hitch(t) && f == 7) c += 4.2;                  /* a 70 ms frame */
				sum += c;
				if (c > 1.0) missed++;
			}
			double mean = sum / 60.0;
			LookSecond s;
			s.emu = (float) (0.55 * mean); s.draw = (float) (0.40 * mean); s.filter = (float) (0.60 * mean);
			s.refreshes = 60; s.missed = missed; s.parallel = true; s.pace_half = missed >= 6;
			g.second(s);
			if (g.generation() != before || g.generation() != gen_seen) { change_t.push_back(t); gen_seen = g.generation(); }
			bad_sec.push_back(missed >= 6 ? 1 : 0);
			mask_at.push_back(g.mask());
		}
	}
	uint32_t gen_seen = 0;
	int bad_in(int from, int to) const { int n = 0; for (int i = from; i < to && i < (int) bad_sec.size(); i++) n += bad_sec[i]; return n; }
	int changes_in(int from, int to) const { int n = 0; for (size_t i = 0; i < change_t.size(); i++) if (change_t[i] >= from && change_t[i] < to) n++; return n; }
};

}  // namespace

TEST(look_ladder_must_hide_the_least_visible_first)
{
	LookGovernor g;
	CHECK(g.set_ladder(kLadder, 5));
	LookRung bad[3] = { { "a", 1, 0.1f, 30 }, { "b", 1, 0.1f, 20 }, { "c", 1, 0.1f, 40 } };
	CHECK(!g.set_ladder(bad, 3));
	LookRung zero[1] = { { "a", 1, 0.0f, 30 } };
	CHECK(!g.set_ladder(zero, 1));
	LookRung many[13];
	for (int i = 0; i < 13; i++) { many[i].name = "x"; many[i].row = 1; many[i].est_saving = 0.01f; many[i].visibility = i + 1; }
	CHECK(!g.set_ladder(many, 13));
	CHECK(g.set_ladder(many, 12));
	CHECK(!g.set_ladder(0, 3));
}

TEST(look_load_is_the_larger_stage_when_parallel_and_the_sum_when_not)
{
	LookSecond s; s.emu = 0.5f; s.draw = 0.2f; s.filter = 0.4f;
	s.parallel = true;  CHECK(fabsf(LookGovernor::load_of(s) - 0.6f) < 1e-5f);
	s.parallel = false; CHECK(fabsf(LookGovernor::load_of(s) - 1.1f) < 1e-5f);
	s.emu = 0.9f; s.parallel = true; CHECK(fabsf(LookGovernor::load_of(s) - 0.9f) < 1e-5f);
}

TEST(look_light_game_never_steps_and_stores_nothing)
{
	Sim m;
	m.setup();
	m.base = [](int) { return 0.55; };
	m.scene = [](int t) { return t % 15 == 0; };
	m.hitch = [](int t) { return t % 20 == 5; };
	m.g.start();
	m.run(600);
	CHECK_EQ(m.g.changes(), 0);
	CHECK_EQ(m.g.level(), 0);
	CHECK_EQ(m.g.generation(), 0u);
	CHECK(m.g.memory_action() == LOOK_MEM_NONE);
	CHECK_EQ(m.g.seconds_at_level(0), 600);
}

TEST(look_a_70ms_hitch_never_causes_a_step)
{
	Sim m;
	m.setup();
	m.base = [](int) { return 0.85; };
	m.hitch = [](int t) { return t % 7 == 3; };         /* one every 7 s, in a game that is busy but fits */
	m.g.start();
	m.run(600);
	CHECK_EQ(m.g.changes(), 0);
	/* and a hitch right at the start of the game */
	Sim n;
	n.setup();
	n.base = [](int) { return 0.80; };
	n.hitch = [](int t) { return t == 0 || t == 1 || t == 4; };
	n.g.start();
	n.run(60);
	CHECK_EQ(n.g.changes(), 0);
}

TEST(look_few_refreshes_are_no_vote)
{
	LookGovernor g;
	g.set_ladder(kLadder, 5);
	g.start();
	LookSecond s; s.emu = 2.0f; s.refreshes = 10; s.missed = 10; s.parallel = true;
	for (int i = 0; i < 100; i++) CHECK(!g.second(s));
	CHECK_EQ(g.seconds(), 0);
	CHECK_EQ(g.changes(), 0);
}

TEST(look_borderline_game_does_not_flap)
{
	Sim m;
	m.setup();
	m.wobble = 0.04;
	m.base = [](int t) { return 1.0 + 0.05 * sin(2.0 * M_PI * t / 30.0); };   /* 0.95 .. 1.05 */
	m.scene = [](int t) { return t % 25 == 0; };
	m.g.start();
	m.run(600);
	CHECK(m.g.changes() <= 3);
	CHECK(m.g.failed_climbs() <= 1);
	CHECK(m.bad_in(120, 600) <= 24);                    /* settled: at most 5 % of the seconds missed */
	CHECK(m.changes_in(300, 600) <= 1);
}

TEST(look_heavy_game_settles_on_the_highest_step_that_holds)
{
	Sim m;
	m.setup();
	m.base = [](int) { return 1.30; };
	m.scene = [](int t) { return t % 30 == 0; };
	m.g.start();
	m.run(600);
	/* 1.30 minus the first three rungs is 0.98: still over. The fourth rung makes 0.78 and holds. */
	CHECK_EQ(m.g.mask(), 0x0Fu);
	CHECK(m.g.changes() <= 6);
	CHECK_EQ(m.g.ups(), 0);
	CHECK(m.bad_in(60, 600) <= 2);
	CHECK(m.g.memory_action() == LOOK_MEM_WRITE);
	/* rows: xBRZ stepped three times, wide scout once */
	CHECK_EQ(m.g.steps_on_row(1), 3);
	CHECK_EQ(m.g.steps_on_row(2), 1);
	CHECK(m.g.eased(1));
}

TEST(look_heavy_scenes_that_come_and_go_every_20_s_settle)
{
	Sim m;
	m.setup();
	m.base = [](int t) { return (t / 20) % 2 == 0 ? 0.60 : 1.25; };
	m.scene = [](int t) { return t % 20 == 0; };
	m.g.start();
	m.run(600);
	CHECK(m.g.changes() <= 8);
	CHECK(m.changes_in(300, 600) <= 1);                 /* sticky: it stops moving */
	CHECK(m.g.failed_climbs() <= 2);
	CHECK(m.bad_in(300, 600) <= 12);                    /* at most 4 % of the seconds missed once settled */
	/* the final look holds the heavy scene */
	double load = 1.25;
	for (int i = 0; i < 5; i++) if (m.g.applied(i)) load -= kTrue[i];
	CHECK(load < 0.97);
}

TEST(look_wrong_cost_guesses_cost_at_most_a_few_changes)
{
	const float scales[3] = { 0.7f, 1.0f, 1.3f };
	for (int k = 0; k < 3; k++) {
		Sim m;
		m.setup(scales[k]);
		m.base = [](int) { return 1.30; };
		m.scene = [](int t) { return t % 30 == 0; };
		m.g.start();
		m.run(600);
		CHECK(m.g.changes() <= 7);
		CHECK(m.g.ups() <= 1);
		CHECK(m.bad_in(120, 600) <= 6);
	}
}

TEST(look_a_badly_low_guess_is_learned_from_the_first_step)
{
	/* the first rung really frees 0.45 of a refresh but is guessed at 0.12: the load before and after the step
	 * teaches the true cost, so the governor does not climb into the overload again */
	LookRung ladder[2] = { { "big", 1, 0.12f, 20 }, { "small", 2, 0.05f, 50 } };
	LookGovernor g;
	CHECK(g.set_ladder(ladder, 2));
	g.start();
	Rng rng(5);
	int bad_late = 0;
	for (int t = 0; t < 600; t++) {
		double load = 1.0 - (g.applied(0) ? 0.45 : 0.0);
		int missed = 0; double sum = 0;
		for (int f = 0; f < 60; f++) { double c = load * (1 + 0.03 * rng.next()); sum += c; if (c > 1.0) missed++; }
		if (t % 20 == 0) g.safe_point(LOOK_SAFE_SCENE);
		LookSecond s; s.emu = (float) (0.5 * sum / 60); s.draw = (float) (0.5 * sum / 60); s.refreshes = 60; s.missed = missed; s.parallel = false;
		g.second(s);
		if (t >= 20 && missed >= 6) bad_late++;
	}
	CHECK_EQ(g.failed_climbs(), 0);
	CHECK(g.saving(0) > 0.2f);                           /* 0.12 learned up towards 0.45 */
	CHECK(g.applied(0));
	CHECK_EQ(bad_late, 0);
	CHECK(g.changes() <= 3);
}

TEST(look_a_failed_climb_doubles_the_cool_down_and_the_changes_die_out)
{
	/* the worst case for the rule: the heavy scene is forgotten fast (peak_hold_s = 20 s) and comes back every
	 * 160 s, so the governor climbs into it again and again. The cool-down doubles each time. */
	LookConfig c;
	c.peak_hold_s = 20;
	Sim m;
	m.g.configure(c);
	m.setup();
	m.base = [](int t) { return (t % 160) < 40 ? 1.30 : 0.50; };
	m.scene = [](int t) { return t % 40 == 0; };
	m.g.start();
	m.run(1000);
	CHECK(m.g.failed_climbs() >= 1);
	CHECK(m.g.cool_now_s() >= 60);
	CHECK(m.g.changes() <= 16);
	CHECK(m.changes_in(500, 1000) <= 4);                 /* fewer and fewer */
	/* with the default memory of the worst scene it does not climb into it at all */
	Sim n;
	n.setup();
	n.base = [](int t) { return (t % 160) < 40 ? 1.30 : 0.50; };
	n.scene = [](int t) { return t % 40 == 0; };
	n.g.start();
	n.run(1000);
	CHECK_EQ(n.g.failed_climbs(), 0);
	CHECK(n.g.changes() <= 2);
}

TEST(look_never_touches_a_row_set_by_hand)
{
	Sim m;
	m.setup();
	m.base = [](int) { return 1.30; };
	m.scene = [](int t) { return t % 30 == 0; };
	m.g.set_user_row(1, true);                           /* the quality row is by hand */
	m.g.start();
	const uint32_t xbrz_bits = (1u << 0) | (1u << 1) | (1u << 3);
	for (int k = 0; k < 6; k++) {
		m.run(100);
		CHECK_EQ(m.g.mask() & xbrz_bits, 0u);
		for (size_t i = 0; i < m.mask_at.size(); i++) CHECK_EQ(m.mask_at[i] & xbrz_bits, 0u);
	}
	CHECK_EQ(m.g.mask(), (1u << 2) | (1u << 4));        /* only the wide-view rungs were available */
	/* the wide row goes to hand mid-game: its steps are released, nothing else changes */
	m.g.set_user_row(2, true);
	CHECK_EQ(m.g.mask(), 0u);
	m.run(200);
	CHECK_EQ(m.g.mask(), 0u);
	CHECK(m.g.user_row(1) && m.g.user_row(2));
	/* back on Auto: the governor may use the row again */
	m.g.set_user_row(1, false);
	CHECK(!m.g.user_row(1));
	m.run(120);
	CHECK((m.g.mask() & xbrz_bits) != 0u);
}

TEST(look_a_step_waits_for_a_safe_point_or_the_force_time)
{
	LookConfig c;
	c.start_grace_s = 0;
	Sim m;
	m.g.configure(c);
	m.setup();
	m.base = [](int) { return 1.30; };
	m.g.start();
	m.run(11);
	CHECK_EQ(m.g.changes(), 0);                          /* wanted at second 4, no safe point yet */
	CHECK_EQ(m.g.pending(), -1);
	m.g.safe_point(LOOK_SAFE_SCENE);
	CHECK_EQ(m.g.downs(), 1);
	CHECK_EQ(m.g.forced(), 0);
	CHECK_EQ(m.g.pending(), 0);
	/* with no safe point at all the step is forced after force_s */
	Sim n;
	n.g.configure(c);
	n.setup();
	n.base = [](int) { return 1.30; };
	n.g.start();
	n.run(30);
	CHECK(n.g.forced() >= 1);
	/* a frontend that cross-fades needs no safe point */
	LookConfig x = c;
	x.crossfade = true;
	Sim f;
	f.g.configure(x);
	f.setup();
	f.base = [](int) { return 1.30; };
	f.g.start();
	f.run(6);
	CHECK(f.g.downs() >= 1);
	CHECK_EQ(f.g.forced(), 0);
}

TEST(look_overload_at_the_start_is_cured_at_once)
{
	Sim m;
	m.setup();
	m.base = [](int) { return 1.30; };
	m.g.start();
	m.run(4);
	CHECK(m.g.downs() >= 1);                             /* start grace: three bad seconds, applied at once */
	CHECK_EQ(m.g.forced(), 0);
}

TEST(look_a_step_up_waits_for_a_safe_point_and_is_never_forced)
{
	Sim m;
	LookConfig c;
	c.peak_hold_s = 60;                                  /* the heavy scene is forgotten after a minute */
	m.g.configure(c);
	m.setup();
	m.base = [](int t) { return t < 40 ? 1.30 : 0.45; };
	m.g.start();
	m.run(40);
	int lvl = m.g.level();
	CHECK(lvl >= 3);
	m.run(200);
	CHECK_EQ(m.g.ups(), 0);                              /* no safe point: still down */
	CHECK_EQ(m.g.pending(), +1);
	CHECK(m.g.safe_point(LOOK_SAFE_SCENE));
	CHECK_EQ(m.g.ups(), 1);
	CHECK_EQ(m.g.level(), lvl - 1);
	/* one step at a time: the next waits for the cool-down and the next safe point */
	m.run(5);
	CHECK_EQ(m.g.ups(), 1);
}

TEST(look_pacing_in_half_mode_counts_as_overload_and_blocks_a_climb)
{
	LookGovernor g;
	g.set_ladder(kLadder, 5);
	g.start();
	LookSecond s; s.emu = 0.5f; s.draw = 0.2f; s.filter = 0.2f; s.refreshes = 60; s.missed = 0; s.parallel = false; s.pace_half = true;   /* load 0.9 */
	for (int i = 0; i < 4; i++) g.second(s);
	CHECK(g.downs() >= 1);
	/* a light second with pacing still half never fits a climb */
	LookGovernor h;
	h.set_ladder(kLadder, 5);
	h.start();
	LookSecond heavy; heavy.emu = 1.4f; heavy.refreshes = 60; heavy.missed = 30; heavy.parallel = true;
	for (int i = 0; i < 5; i++) h.second(heavy);
	h.safe_point(LOOK_SAFE_SCENE);
	CHECK(h.level() >= 1);
	LookSecond light; light.emu = 0.2f; light.refreshes = 60; light.parallel = true; light.pace_half = true;
	for (int i = 0; i < 100; i++) h.second(light);
	CHECK_EQ(h.pending(), 0);
	CHECK_EQ(h.ups(), 0);
}

TEST(look_memory_round_trips_and_starts_steady)
{
	Sim m;
	m.setup();
	m.base = [](int) { return 1.30; };
	m.scene = [](int t) { return t % 30 == 0; };
	m.g.start();
	m.run(300);
	CHECK(m.g.memory_action() == LOOK_MEM_WRITE);
	std::string text = m.g.serialize(444);
	CHECK(text.compare(0, 9, "pk-look 1") == 0);

	LookGovernor h;
	CHECK(h.set_ladder(kLadder, 5));
	CHECK(h.load_memory(text.data(), text.size(), 444));
	CHECK_STR(h.serialize(444), text);                   /* what was written is what is read */
	h.start();
	CHECK_EQ(h.mask(), m.g.mask());                      /* the game starts at the look that held */
	CHECK_EQ(h.generation(), 1u);
	/* the same game, started from the memory, needs no step at all */
	Sim n;
	CHECK(n.g.set_ladder(kLadder, 5));
	CHECK(n.g.load_memory(text.data(), text.size(), 444));
	n.base = [](int) { return 1.30; };
	n.g.start();
	n.run(120);
	CHECK_EQ(n.g.downs(), 0);
	CHECK(n.bad_in(0, 120) <= 1);

	/* another clock: the learned costs come along, the look does not */
	LookGovernor o;
	o.set_ladder(kLadder, 5);
	CHECK(o.load_memory(text.data(), text.size(), 500));
	o.start();
	CHECK_EQ(o.mask(), 0u);
	CHECK(o.memory_action() == LOOK_MEM_NONE);           /* nothing learned here: the file stays as it is */
}

TEST(look_memory_rejects_other_ladders_and_damage)
{
	Sim m;
	m.setup();
	m.base = [](int) { return 1.30; };
	m.g.start();
	m.run(100);
	std::string text = m.g.serialize(444);
	LookRung other[2] = { { "a", 1, 0.1f, 10 }, { "b", 2, 0.1f, 20 } };
	LookGovernor g;
	CHECK(g.set_ladder(other, 2));
	CHECK(!g.load_memory(text.data(), text.size(), 444));     /* a different ladder: not this game's memory */
	LookGovernor h;
	CHECK(h.set_ladder(kLadder, 5));
	CHECK(!h.load_memory(text.data(), 12, 444));              /* cut off */
	std::string t2 = text; t2.replace(t2.find("mask="), 6, "mask=9");
	size_t eol = t2.find('\n', t2.find("mask="));
	t2.replace(t2.find("mask="), eol - t2.find("mask="), "mask=999");
	CHECK(!h.load_memory(t2.data(), t2.size(), 444));         /* a look the ladder cannot make */
	std::string t3 = text; t3.replace(0, 9, "pk-look 2");
	CHECK(!h.load_memory(t3.data(), t3.size(), 444));
	CHECK(!h.load_memory("", 0, 444));
	CHECK(!h.load_memory("pk-look 1\nladder=zz\n", 20, 444));
	CHECK(h.load_memory(text.data(), text.size(), 444));
	CHECK_EQ(h.mask(), 0u);                                   /* before start(): nothing applied yet */
}

TEST(look_a_remembered_look_that_is_not_needed_is_forgotten)
{
	Sim m;
	m.setup();
	m.base = [](int) { return 1.30; };
	m.g.start();
	m.run(100);
	std::string text = m.g.serialize(444);

	Sim n;
	CHECK(n.g.set_ladder(kLadder, 5));
	CHECK(n.g.load_memory(text.data(), text.size(), 444));
	n.base = [](int) { return 0.30; };                        /* the same game, but light now */
	n.scene = [](int t) { return t % 25 == 0; };
	n.g.start();
	n.run(1000);
	CHECK_EQ(n.g.level(), 0);
	CHECK(n.g.memory_action() == LOOK_MEM_DELETE);
	CHECK_EQ(n.g.downs(), 0);
}

TEST(look_census_and_summary_lines)
{
	LookGovernor g;
	g.set_ladder(kLadder, 5);
	g.start();
	char b[512];
	CHECK(!g.census_line(b, sizeof b));                       /* nothing to say before the first second */
	Sim m;
	m.setup();
	m.base = [](int) { return 1.30; };
	m.g.start();
	m.run(60);
	CHECK(m.g.census_line(b, sizeof b));
	CHECK(strncmp(b, "look: lvl=", 10) == 0);
	CHECK(strstr(b, "load=") != 0 && strstr(b, "pace=") != 0 && strstr(b, "up_need=") != 0);
	std::string s = m.g.summary();
	CHECK(s.compare(0, 13, "look summary:") == 0);
	CHECK(s.find("memory write") != std::string::npos);
	CHECK(s.find("xBRZ 4x -> 3x") != std::string::npos);
	CHECK(m.g.events().size() >= 1);
	CHECK(m.g.events().size() <= 32);
}

TEST(look_auto_label_shows_the_current_choice)
{
	CHECK_STR(look_auto_label("xBRZ 4x", false), "Auto (xBRZ 4x)");
	CHECK_STR(look_auto_label("xBRZ 3x", true), "Auto (xBRZ 3x, eased)");
	CHECK_STR(look_auto_label("Wide view", false), "Auto (Wide view)");
}

TEST(look_fuzz_keeps_its_promises_over_many_games)
{
	Rng pick(99);
	for (int game = 0; game < 120; game++) {
		Sim m;
		m.rng = Rng(1000 + game);
		const double lvl = 0.4 + 0.8 * (pick.next() + 1.0);          /* 0.4 .. 2.0 of a refresh at the full look */
		const double amp = 0.3 * (pick.next() + 1.0) / 2.0;
		const int period = 10 + (int) (30 * (pick.next() + 1.0));
		const int scene_every = 5 + (int) (25 * (pick.next() + 1.0));
		const uint32_t user = (uint32_t) ((pick.next() + 1.0) * 1.5);   /* 0, 1 or 2: rows 1 and 2 by hand or not */
		m.setup(0.7f + 0.3f * (float) (pick.next() + 1.0));
		m.base = [=](int t) { return lvl + amp * sin(2.0 * M_PI * t / period); };
		m.scene = [=](int t) { return t % scene_every == 0; };
		m.hitch = [](int t) { return t % 53 == 11; };
		if (user & 1) m.g.set_user_row(1, true);
		if (user & 2) m.g.set_user_row(2, true);
		uint32_t userbits = 0;
		for (int i = 0; i < 5; i++) if (m.g.user_row(kLadder[i].row)) userbits |= 1u << i;
		m.g.start();
		uint32_t gen = 0;
		for (int k = 0; k < 6; k++) {
			m.run(100);
			CHECK_EQ(m.g.mask() & userbits, 0u);
			CHECK(m.g.mask() < 32u);
			CHECK(m.g.generation() >= gen);
			gen = m.g.generation();
		}
		for (size_t i = 0; i < m.mask_at.size(); i++) CHECK_EQ(m.mask_at[i] & userbits, 0u);
		CHECK(m.g.changes() <= 14);                                  /* never a storm, whatever the game does */
		CHECK(m.g.failed_climbs() <= 3);
		if (lvl + amp < 0.80) CHECK_EQ(m.g.changes(), 0);              /* a game that always fits is left alone */
		if (m.g.memory_action() == LOOK_MEM_NONE) CHECK_EQ(m.g.mask() & ~userbits, 0u);
	}
}

TEST(look_with_the_real_pacer_in_the_loop)
{
	/* a heavy game: the pacer thins frames within a second, the governor lowers the look, the pacer goes back to
	 * every frame, and the governor does not climb while the pacer is half */
	Pacer pacer;
	LookGovernor g;
	g.set_ladder(kLadder, 5);
	g.start();
	Rng rng(31);
	int half_secs_late = 0, climbs_in_half = 0;
	for (int t = 0; t < 400; t++) {
		double base = 1.30;                                 /* heavy all through */
		double load = base;
		for (int i = 0; i < 5; i++) if (g.applied(i)) load -= kTrue[i];
		int drawn = pacer.mode() == PACE_HALF ? 30 : 60;
		int missed = 0;
		double sum = 0;
		for (int f = 0; f < drawn; f++) {
			double c = load * (1.0 + 0.03 * rng.next());
			sum += c;
			if (c > (pacer.mode() == PACE_HALF ? 2.0 : 1.0)) missed++;
			pacer.add((uint32_t) (c * 16683.0));
		}
		LookSecond s;
		double m = sum / drawn;
		s.emu = (float) (0.55 * m); s.draw = (float) (0.4 * m); s.filter = (float) (0.6 * m);
		s.refreshes = 60; s.missed = missed; s.parallel = true; s.pace_half = pacer.mode() == PACE_HALF;
		int ups = g.ups();
		if (t % 40 == 0) g.safe_point(LOOK_SAFE_SCENE);
		g.second(s);
		if (s.pace_half && g.ups() > ups) climbs_in_half++;
		if (t >= 100 && pacer.mode() == PACE_HALF) half_secs_late++;
	}
	CHECK_EQ(climbs_in_half, 0);
	CHECK_EQ(g.mask(), 0x0Fu);
	CHECK(half_secs_late <= 4);                                      /* after the look came down, every frame again */
	CHECK(pacer.mode() == PACE_FULL);
}
