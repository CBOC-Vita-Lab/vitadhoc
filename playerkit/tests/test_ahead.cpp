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

/* the picture thread: play() with the drawing thread's cost and misses fed once per frame */
template <class P, class M> static unsigned play_pic(AheadGovernor &g, int frames, P pic_us, M missed)
{
	unsigned ahead = 0;
	for (int t = 0; t < frames; t++) {
		const bool a = g.want();
		const uint32_t c = a ? 3000u * 220 / 100 + 500 : 3000u;
		if (a) ahead++;
		g.add(c, a);
		g.add_picture(pic_us(t), missed(t));
	}
	return ahead;
}

TEST(ahead_picture_thread_light_changes_nothing)
{
	AheadGovernor g;
	const unsigned n = play_pic(g, 3600, [](int) { return 6000u; }, [](int) { return 0u; });      /* xBRZ at 36 % of a frame */
	CHECK(g.on());
	CHECK(n >= 3600u - 121u - 5u);
	CHECK_EQ(g.stops(), 0u);
	CHECK_EQ(g.picture_stops(), 0u);
}

TEST(ahead_picture_thread_saturated_holds_it_off)
{
	AheadGovernor g;
	/* the emulating thread is light (3 ms) but the drawing thread needs 15 ms of a 16.7 ms frame: it never starts */
	CHECK(play_pic(g, 3600, [](int) { return 15000u; }, [](int) { return 0u; }) == 0);
	CHECK_EQ(g.starts(), 0u);
	/* missed refreshes alone hold it off too (one in ten) */
	AheadGovernor h;
	CHECK(play_pic(h, 3600, [](int) { return 6000u; }, [](int t) { return t % 10 == 0 ? 1u : 0u; }) == 0);
	CHECK_EQ(h.starts(), 0u);
}

TEST(ahead_picture_thread_going_late_stops_it_and_it_comes_back)
{
	AheadGovernor g;
	play_pic(g, 600, [](int) { return 6000u; }, [](int) { return 0u; });
	CHECK(g.on());
	/* the scene gets heavy for the picture side: three missed refreshes stop it at once */
	g.add_picture(6000, 1); g.add_picture(6000, 1); CHECK(g.on());
	g.add_picture(6000, 1);
	CHECK(!g.on());
	CHECK_EQ(g.picture_stops(), 1u);
	CHECK_EQ(g.stops(), 1u);
	/* it stays off while the drawing cost is high, and a stop by cost alone needs the average over 95 % */
	AheadGovernor h;
	play_pic(h, 600, [](int) { return 6000u; }, [](int) { return 0u; });
	CHECK(h.on());
	for (int i = 0; i < 40; i++) { h.add(3000, true); h.add_picture(17500, 0); }
	CHECK(!h.on());
	/* the scene eases: after the wait and a quiet settle time it is back */
	const unsigned before = h.starts();
	play_pic(h, 4000, [](int) { return 6000u; }, [](int) { return 0u; });
	CHECK(h.on());
	CHECK_EQ(h.starts(), before + 1);
}

TEST(ahead_picture_thread_hysteresis_does_not_flap)
{
	AheadGovernor g;
	/* the drawing cost sits between the start level (80 %) and the stop level (95 %): once on it stays on, once off it stays off */
	AheadGovernor on;
	play_pic(on, 600, [](int) { return 6000u; }, [](int) { return 0u; });
	CHECK(on.on());
	for (int i = 0; i < 3000; i++) { on.add(on.want() ? 6100u : 3000u, on.want()); on.add_picture(14500, 0); }     /* 87 % */
	CHECK(on.on());
	CHECK_EQ(on.stops(), 0u);
	CHECK(play_pic(g, 3600, [](int) { return 14500u; }, [](int) { return 0u; }) == 0);
	CHECK_EQ(g.starts(), 0u);
}

TEST(ahead_picture_thread_unused_and_manual_modes)
{
	/* ON stays on whatever the picture side does; a player that never reports it is judged on the emulating thread alone */
	AheadGovernor g;
	g.set_mode(AHEAD_ON);
	for (int i = 0; i < 200; i++) g.add_picture(20000, 5);
	CHECK(g.want());
	AheadGovernor h;
	CHECK(play(h, 3600, [](int) { return 3000u; }) > 3000);
}

TEST(ahead_picture_sparse_misses_do_not_stop_it)
{
	/* a miss every 61 refreshes is two in any 120: under the limit of 3. (Each miss used to re-arm the window, so the count never aged out.) */
	AheadGovernor g;
	play_pic(g, 600, [](int) { return 6000u; }, [](int) { return 0u; });
	CHECK(g.on());
	for (int t = 0; t < 1200; t++) { g.add(3000, true); g.add_picture(6000, t % 61 == 0 ? 1u : 0u); }
	CHECK(g.on());
	CHECK_EQ(g.picture_stops(), 0u);
	/* one every 40 is three in a window: it stops */
	for (int t = 0; t < 400 && g.on(); t++) { g.add(3000, true); g.add_picture(6000, t % 40 == 0 ? 1u : 0u); }
	CHECK(!g.on());
	CHECK_EQ(g.picture_stops(), 1u);
	/* a stop uses the misses up: the window after it is clean for a start */
	CHECK_EQ(g.picture_misses() > 0, true);
}

TEST(ahead_picture_misses_age_out_of_the_window)
{
	AheadGovernor g;
	play_pic(g, 600, [](int) { return 6000u; }, [](int) { return 0u; });
	g.add_picture(6000, 1); g.add_picture(6000, 1);
	for (int t = 0; t < 130; t++) g.add_picture(6000, 0);       /* the two are older than the window now */
	g.add_picture(6000, 1);
	CHECK(g.on());
	CHECK_EQ(g.picture_stops(), 0u);
}

TEST(ahead_picture_average_fades_without_pictures_and_resets_when_unblocked)
{
	AheadGovernor g;
	for (int i = 0; i < 60; i++) g.add_picture(18000, 0);
	CHECK(g.picture_us() > 16000u);
	for (int i = 0; i < 400; i++) g.add_picture(0, 0);          /* nothing drawn: nothing is costing */
	CHECK(g.picture_us() < 1000u);
	for (int i = 0; i < 60; i++) g.add_picture(18000, 2);
	g.set_blocked(true);
	CHECK(g.picture_us() > 16000u);                              /* kept while blocked */
	g.set_blocked(false);
	CHECK_EQ(g.picture_us(), 0u);                                /* what was learned before the block is not the picture side now */
	/* and it can start again at once on a quiet picture side */
	g.set_mode(AHEAD_AUTO);
	play_pic(g, 600, [](int) { return 6000u; }, [](int) { return 0u; });
	CHECK(g.on());
}

TEST(ahead_picture_reset_on_a_quality_change)
{
	AheadGovernor g;
	for (int i = 0; i < 60; i++) g.add_picture(18000, 1);
	g.picture_reset();
	CHECK_EQ(g.picture_us(), 0u);
	play_pic(g, 600, [](int) { return 6000u; }, [](int) { return 0u; });
	CHECK(g.on());
}

TEST(ahead_picture_totals_deltas_and_wrap)
{
	PictureTotals p;
	uint32_t us, miss;
	p.rebase(1000, 10, 5);
	p.take(1000 + 3 * 5000, 13, 5, &us, &miss);
	CHECK_EQ(us, 5000u); CHECK_EQ(miss, 0u);
	p.take(1000 + 3 * 5000, 13, 7, &us, &miss);                  /* no picture finished: not measured */
	CHECK_EQ(us, 0u); CHECK_EQ(miss, 2u);
	/* 32-bit totals wrap: the difference is still right */
	p.rebase(0xFFFFFF00u, 0xFFFFFFFEu, 0xFFFFFFFFu);
	p.take(0xFFFFFF00u + 4000u, 2u, 1u, &us, &miss);             /* 4 pictures, wrapped; 2 misses */
	CHECK_EQ(us, 1000u); CHECK_EQ(miss, 2u);
	/* totals that went backwards (counters restarted without a rebase) are no interval */
	p.rebase(500000, 100, 50);
	p.take(10, 3, 1, &us, &miss);
	CHECK_EQ(us, 0u); CHECK_EQ(miss, 0u);
}
