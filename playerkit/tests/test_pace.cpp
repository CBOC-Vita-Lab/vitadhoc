/* Unit tests: pk_pace (steady pacing decision). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_pace.h"
#include <stdlib.h>
#include <vector>

using namespace pk;

/* runs the Pacer over a cost sequence; returns the mode after each frame and counts the changes */
static std::vector<PaceMode> run(Pacer &p, const std::vector<uint32_t> &costs, int *changes)
{
	std::vector<PaceMode> out;
	*changes = 0;
	for (size_t i = 0; i < costs.size(); i++) {
		bool ch = false;
		out.push_back(p.add(costs[i], &ch));
		if (ch) (*changes)++;
		/* in half mode one drawn frame stands for two refreshes: the player feeds half as many */
	}
	return out;
}

/* a simple game-loop model: emulates `seconds` of 60 Hz, feeding a drawn-frame cost from fn(t_ms) */
template <class F> static int simulate(Pacer &p, double seconds, F fn, int *half_frames = 0)
{
	int changes = 0, halfn = 0;
	double t = 0;
	while (t < seconds * 1000) {
		uint32_t c = fn(t);
		bool ch = false;
		PaceMode m = p.add(c, &ch);
		if (ch) changes++;
		if (m == PACE_HALF) halfn++;
		t += 16.683 * (p.mode() == PACE_HALF ? 2 : 1);
	}
	if (half_frames) *half_frames = halfn;
	return changes;
}

TEST(pace_light_load_stays_full)
{
	Pacer p;
	std::vector<uint32_t> c(600, 9000);
	int ch;
	std::vector<PaceMode> m = run(p, c, &ch);
	CHECK_EQ(ch, 0);
	CHECK_EQ(m.back(), PACE_FULL);
}

TEST(pace_short_bursts_do_not_switch)
{
	Pacer p;
	/* a single 60 ms hitch, and a 3-frame burst, in otherwise light play */
	std::vector<uint32_t> c(300, 10000);
	c[100] = 60000;
	c[200] = c[201] = c[202] = 25000;
	int ch;
	run(p, c, &ch);
	CHECK_EQ(ch, 0);
	CHECK_EQ(p.mode(), PACE_FULL);
	/* bursts every 2 s of 4 frames (under 10% of a second) */
	Pacer q;
	int changes = simulate(q, 30, [](double t) -> uint32_t { return (((int) t) % 2000) < 67 ? 22000u : 11000u; });
	CHECK_EQ(changes, 0);
}

TEST(pace_sustained_overload_goes_half_within_a_second)
{
	Pacer p;
	std::vector<uint32_t> c(60, 10000);
	for (int i = 0; i < 120; i++) c.push_back(19000);      /* a heavy scene from frame 60 */
	int ch;
	std::vector<PaceMode> m = run(p, c, &ch);
	CHECK_EQ(ch, 1);
	int first_half = -1;
	for (size_t i = 0; i < m.size(); i++) if (m[i] == PACE_HALF) { first_half = (int) i; break; }
	CHECK(first_half >= 60);
	CHECK(first_half <= 60 + 60);      /* within one second of the scene starting */
	/* average just under budget but with many misses also goes half */
	Pacer q;
	std::vector<uint32_t> d;
	for (int i = 0; i < 120; i++) d.push_back(i % 4 == 0 ? 20000 : 14000);   /* 25% of frames miss */
	run(q, d, &ch);
	CHECK_EQ(q.mode(), PACE_HALF);
}

TEST(pace_recovers_only_after_sustained_headroom)
{
	Pacer p;
	int changes = 0, half = 0;
	/* 5 s heavy, then light: back to full no sooner than 3 s after the load went (min_half 2 s also met) */
	double went_full = -1, t = 0;
	while (t < 15000) {
		uint32_t c = t < 5000 ? 20000 : 9000;
		bool ch = false;
		PaceMode m = p.add(c, &ch);
		if (ch) { changes++; if (m == PACE_FULL) went_full = t; }
		if (m == PACE_HALF) half++;
		t += 16.683 * (p.mode() == PACE_HALF ? 2 : 1);
	}
	CHECK_EQ(changes, 2);
	CHECK(went_full >= 5000 + 3000 - 40);
	CHECK(went_full <= 5000 + 3000 + 200);
	CHECK(half > 0);
	/* headroom too thin (85% of the budget): stays half for good */
	Pacer q;
	changes = simulate(q, 20, [](double t) -> uint32_t { return t < 3000 ? 20000u : 14200u; });
	CHECK_EQ(changes, 1);
	CHECK_EQ(q.mode(), PACE_HALF);
}

TEST(pace_boundary_never_flaps)
{
	/* costs wandering across the budget line for a minute: at most one change */
	Pacer p;
	srand(7);
	int changes = simulate(p, 60, [](double) -> uint32_t { return 15500u + (uint32_t) (rand() % 2400); });
	CHECK(changes <= 1);
	/* costs wandering around the recovery line in half mode */
	Pacer q;
	srand(11);
	changes = simulate(q, 60, [](double t) -> uint32_t { return t < 2000 ? 20000u : 12500u + (uint32_t) (rand() % 1800); });
	CHECK(changes <= 3);
	/* square wave: 1.5 s heavy, 1.5 s light, repeated: never back to full in the light halves (needs 3 s) */
	Pacer r;
	int half = 0;
	changes = simulate(r, 60, [](double t) -> uint32_t { return ((int) (t / 1500)) % 2 == 0 ? 20000u : 8000u; }, &half);
	CHECK_EQ(changes, 1);
}

TEST(pace_disabled_is_always_full)
{
	Pacer p;
	p.set_enabled(false);
	std::vector<uint32_t> c(300, 30000);
	int ch;
	std::vector<PaceMode> m = run(p, c, &ch);
	CHECK_EQ(ch, 0);
	CHECK_EQ(m.back(), PACE_FULL);
	Pacer::Counts k = p.take_counts();
	CHECK_EQ(k.drawn, 300);
	CHECK_EQ(k.over, 300);
	CHECK_EQ(p.take_counts().drawn, 0);
}

TEST(pace_decide_is_pure)
{
	PaceConfig c;
	std::vector<PaceSample> s(100);
	for (size_t i = 0; i < s.size(); i++) { s[i].cost_us = 18000; s[i].span = 1; }
	CHECK_EQ(pace_decide(c, PACE_FULL, &s[0], (int) s.size(), 5000), PACE_HALF);
	CHECK_EQ(pace_decide(c, PACE_FULL, &s[0], (int) s.size(), 5000), PACE_HALF);
	CHECK_EQ(pace_decide(c, PACE_FULL, &s[0], 10, 5000), PACE_FULL);      /* too few samples */
	CHECK_EQ(pace_decide(c, PACE_FULL, &s[0], 40, 5000), PACE_FULL);      /* less than a second */
	for (size_t i = 0; i < s.size(); i++) { s[i].cost_us = 9000; s[i].span = 2; }
	CHECK_EQ(pace_decide(c, PACE_HALF, &s[0], (int) s.size(), 1000), PACE_HALF);     /* not long enough in half */
	CHECK_EQ(pace_decide(c, PACE_HALF, &s[0], (int) s.size(), 5000), PACE_FULL);
	CHECK_EQ(pace_decide(c, PACE_HALF, &s[0], 30, 5000), PACE_HALF);      /* 1 s of history is not 3 s */
}
