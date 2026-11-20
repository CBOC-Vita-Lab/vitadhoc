/* Unit tests: pk_ahead ("Quick response": when a player runs one frame ahead). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_ahead.h"

using namespace pk;

/* a game whose plain frame costs plain(t) microseconds; run-ahead costs ratio x that + overhead. Returns the frames run ahead. */
template <class F> static unsigned play(AheadGovernor &g, int frames, F plain, int ratio_x100 = 220, uint32_t overhead = 500)
{
	unsigned ahead = 0;
	for (int t = 0; t < frames; t++) {
		const bool a = g.want();
		const uint32_t p = plain(t);
		const uint32_t c = a ? (uint32_t) ((uint64_t) p * (unsigned) ratio_x100 / 100 + overhead) : p;
		if (a) ahead++;
		g.add(c, a);
	}
	return ahead;
}

TEST(ahead_off_and_on_modes)
{
	AheadGovernor g;
	g.set_mode(AHEAD_OFF);
	CHECK(!g.want());
	CHECK(play(g, 2000, [](int) { return 1000u; }) == 0);
	g.set_mode(AHEAD_ON);
	CHECK(g.want());
	/* On is the player's word: even a game far too heavy keeps it */
	CHECK(play(g, 500, [](int) { return 15000u; }) == 500);
	g.set_blocked(true);
	CHECK(!g.want());
	g.set_blocked(false);
	CHECK(g.want());
}

TEST(ahead_auto_starts_on_a_light_game_and_stays)
{
	AheadGovernor g;
	CHECK(g.mode() == AHEAD_AUTO);
	CHECK(!g.want());                                   /* never before the game has shown what it costs */
	const unsigned n = play(g, 3600, [](int) { return 3000u; });      /* 3 ms plain, 7.1 ms ahead: 43 % of a frame */
	CHECK(g.on());
	CHECK(n >= 3600u - 121u - 5u);                      /* from the settle time on */
	CHECK_EQ(g.starts(), 1u);
	CHECK_EQ(g.stops(), 0u);
	CHECK(g.ratio_x100() >= 215 && g.ratio_x100() <= 245);        /* learned from the two averages */
}

TEST(ahead_auto_never_starts_on_a_heavy_game)
{
	AheadGovernor g;
	/* 5 ms plain: projected 12.1 ms, 73 % of a frame */
	CHECK(play(g, 3600, [](int) { return 5000u; }) == 0);
	CHECK_EQ(g.starts(), 0u);
}

TEST(ahead_auto_stops_when_the_game_gets_heavy_and_comes_back)
{
	AheadGovernor g;
	/* light for 20 s, heavy (6 ms plain: 13.7 ms ahead) for 20 s, light again for 60 s */
	auto cost = [](int t) { return (t >= 1200 && t < 2400) ? 6000u : 2500u; };
	unsigned ahead = 0, ahead_heavy = 0, late = 0;
	for (int t = 0; t < 6000; t++) {
		const bool a = g.want();
		const uint32_t p = cost(t);
		const uint32_t c = a ? p * 22 / 10 + 500 : p;
		if (a) { ahead++; if (t >= 1200 && t < 2400) ahead_heavy++; }
		if (c > 16683) late++;
		g.add(c, a);
	}
	CHECK(ahead_heavy < 200);                           /* it left within a few seconds ... */
	CHECK_EQ(late, 0u);                                 /* ... and never made a frame late (13.7 ms fits; it leaves on the average) */
	CHECK(g.on());                                      /* back after the heavy part */
	CHECK(g.stops() >= 1 && g.stops() <= 3);
	CHECK(ahead > 3000);
}

TEST(ahead_auto_late_frames_stop_it_at_once)
{
	AheadGovernor g;
	play(g, 600, [](int) { return 3000u; });
	CHECK(g.on());
	/* spikes: three frames over a whole frame within two seconds */
	g.add(17000, true); g.add(7000, true); g.add(18000, true); CHECK(g.on());
	g.add(17500, true);
	CHECK(!g.on());
	CHECK_EQ(g.stops(), 1u);
	/* and it does not flap: the wait doubles when it had to stop soon after a start */
	AheadGovernor h;
	unsigned starts_before = 0;
	for (int round = 0; round < 6; round++) {
		/* a game that looks light when plain and is always late when run ahead (a state that is very slow to copy) */
		for (int t = 0; t < 3000; t++) { const bool a = h.want(); h.add(a ? 17500u : 3000u, a); }
		if (round == 0) starts_before = h.starts();
	}
	CHECK(starts_before >= 1);
	CHECK(h.starts() <= starts_before + 6);             /* 18000 frames: without the doubling it would try about 40 times */
	CHECK(h.frames_ahead() < 18000u / 20);              /* under 5 % of the frames */
}

TEST(ahead_blocked_frames_teach_nothing)
{
	AheadGovernor g;
	play(g, 600, [](int) { return 3000u; });
	CHECK(g.on());
	const uint32_t before = g.plain_us();
	g.set_blocked(true);                                   /* fast forward: three frames a refresh */
	for (int i = 0; i < 300; i++) g.add(15000, false);
	CHECK(!g.want());
	CHECK_EQ(g.plain_us(), before);
	g.set_blocked(false);
	CHECK(!g.want());                                   /* it settles again first */
	play(g, 200, [](int) { return 3000u; });
	CHECK(g.on());
}

TEST(ahead_pal_frame_budget)
{
	AheadGovernor g;
	g.set_frame_us(20000);
	/* 4 ms plain: projected 9.8 ms = 49 % of a 20 ms frame (it would be 59 % of a 60 Hz frame) */
	CHECK(play(g, 1200, [](int) { return 4000u; }) > 900);
	AheadGovernor n;
	CHECK(play(n, 1200, [](int) { return 4000u; }) == 0);
}
