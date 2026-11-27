/* Unit tests: pk_handoff (frame hand-over mailbox, adaptive depth), the pad latch, and the model-vs-implementation
 * agreement on a simulated display. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "handoff_stress.h"
#include "pacing_model.h"
#include "pk_input.h"
#include <string.h>

using namespace pk;

namespace {

/* a virtual clock and semaphores that never block: for single-thread tests that use the try_ calls */
struct VirtualSys : HandoffSys {
	uint64_t t;
	VirtualSys() : t(1000000) {}
	uint64_t now_us() { return t; }
	void *sem_create() { return new int(0); }
	void sem_destroy(void *s) { delete (int *) s; }
	void sem_post(void *) {}
	bool sem_wait(void *, uint32_t timeout_us)
	{
		t += timeout_us == 0xFFFFFFFFu || timeout_us > 1000 ? 1000 : timeout_us;     /* time passes while "waiting" */
		return false;
	}
};

struct Rig {
	VirtualSys sys;
	Handoff h;
	explicit Rig(int depth = 2, bool adaptive = false)
	{
		HandoffConfig c;
		c.frame_bytes = 64;
		c.depth.max_depth = depth;
		c.adaptive = adaptive;
		h.init(c, &sys);
	}
	/* one picture with number n */
	bool put(uint32_t n)
	{
		HandoffFrame *f = h.try_begin_write();
		if (!f) return false;
		memset(f->data, (int) (n & 0xFF), 64);
		f->w = 8; f->h = 8; f->pitch = 8; f->seq = n; f->emu_us = 3000;
		h.commit(f);
		return true;
	}
};

}  // namespace

/* ---- adaptive depth ---------------------------------------------------------------- */

static int feed(DepthGovernor &g, double share, int frames, int span = 1)
{
	int d = g.depth();
	for (int i = 0; i < frames; i++) d = g.add((uint32_t) (share * 16683), span);
	return d;
}

TEST(handoff_governor_shrinks_when_over_budget_and_restores_after_headroom)
{
	DepthGovernor g;                    /* max 2, min 1 */
	CHECK_EQ(feed(g, 0.70, 120), 2);    /* headroom: full depth */
	CHECK_EQ(feed(g, 1.10, 40), 2);     /* 0.7 s over budget: not yet (must last a second) */
	CHECK_EQ(feed(g, 1.10, 60), 1);     /* sustained: the queue only adds delay, shrink */
	CHECK_EQ(feed(g, 0.70, 120), 1);    /* headroom for 2 s: too soon (the small depth is kept at least 4 s) */
	CHECK_EQ(feed(g, 0.70, 200), 2);    /* now back */
	CHECK_EQ(g.changes(), 2);
}

TEST(handoff_governor_hysteresis_band_changes_nothing)
{
	DepthGovernor g;
	/* a mean between 88 % and 100 % of the budget, for ten minutes */
	for (int i = 0; i < 36000; i++) g.add((uint32_t) ((0.94 + 0.05 * ((i % 7) / 6.0 - 0.5)) * 16683), 1);
	CHECK_EQ(g.changes(), 0);
	CHECK_EQ(g.depth(), 2);
}

TEST(handoff_governor_cannot_flap)
{
	/* the worst case for a sticky rule: 3 s heavy, 3 s light, forever (10 minutes) */
	DepthGovernor g;
	for (int cycle = 0; cycle < 100; cycle++) { feed(g, 1.2, 180); feed(g, 0.6, 180); }
	/* each failed restore doubles the wait (4 s -> 32 s): a handful of changes, not hundreds */
	CHECK(g.changes() <= 40);
	/* and a steady 10-minute stretch of a heavy load costs no more than the one shrink */
	DepthGovernor g2;
	feed(g2, 1.2, 36000);
	CHECK_EQ(g2.changes(), 1);
	CHECK_EQ(g2.depth(), 1);
}

TEST(handoff_governor_wrong_cost_guesses_only_move_between_min_and_max)
{
	/* costs read far too low or far too high for ten minutes: depth settles at one end, one change at most */
	DepthGovernor lo, hi;
	feed(lo, 0.05, 36000);
	feed(hi, 5.0, 36000);
	CHECK_EQ(lo.depth(), 2); CHECK_EQ(lo.changes(), 0);
	CHECK_EQ(hi.depth(), 1); CHECK_EQ(hi.changes(), 1);
	/* garbage, including zero and huge values, never leaves 1..max */
	DepthGovernor g;
	unsigned r = 12345;
	for (int i = 0; i < 20000; i++) {
		r = r * 1664525u + 1013904223u;
		const uint32_t c = (r >> 16) % 3 == 0 ? 0 : (r % 200000);
		const int d = g.add(c, 1 + (int) ((r >> 5) & 1));
		CHECK(d >= 1 && d <= 2);
	}
	/* half mode: each picture stands for two refreshes, the same rule applies */
	DepthGovernor h2;
	CHECK_EQ(feed(h2, 1.1, 60, 2), 1);
}

TEST(handoff_governor_config_is_clamped)
{
	DepthConfig c;
	c.max_depth = 99; c.min_depth = 0; c.window = 1000; c.budget_us = 0;
	DepthGovernor g;
	g.configure(c);
	CHECK_EQ(g.config().max_depth, HANDOFF_MAX_DEPTH);
	CHECK_EQ(g.config().min_depth, 1);
	CHECK(g.config().window <= DepthGovernor::MAX_WINDOW);
	CHECK(g.config().budget_us > 0);
}

/* ---- the mailbox, single thread -------------------------------------------------------- */

TEST(handoff_gate_and_order_in_full_mode)
{
	Rig r(2);
	CHECK(r.put(10));
	CHECK(r.put(11));
	CHECK(!r.put(12));                                  /* two in flight: the gate is closed, nothing is dropped */
	HandoffFrame *a = r.h.try_take();
	CHECK(a && a->seq == 10 && a->data[0] == 10);       /* oldest first */
	CHECK(!r.put(12));                                  /* one is being drawn: still two in flight */
	r.h.release(a, 5000);
	CHECK(r.put(12));
	HandoffFrame *b = r.h.try_take();
	CHECK(b && b->seq == 11);
	r.h.release(b, 5000);
	HandoffFrame *c = r.h.try_take();
	CHECK(c && c->seq == 12);
	r.h.release(c, 5000);
	CHECK(r.h.try_take() == 0);                         /* nothing new: nothing is shown again */
	HandoffCensus k = r.h.census();
	CHECK_EQ(k.produced, 3); CHECK_EQ(k.shown, 3); CHECK_EQ(k.dropped, 0);
}

TEST(handoff_half_mode_newest_wins_and_never_waits)
{
	Rig r(2);
	r.h.set_pace_mode(PACE_HALF);
	for (uint32_t n = 0; n < 6; n++) CHECK(r.put(n));   /* never refused */
	HandoffFrame *f = r.h.try_take();
	CHECK(f && f->seq == 5);                            /* the newest */
	CHECK_EQ(f->span, 2);                               /* each picture stands for two refreshes */
	r.h.release(f, 1000);
	CHECK(r.h.try_take() == 0);
	HandoffCensus k = r.h.census();
	CHECK_EQ(k.produced, 6); CHECK_EQ(k.shown, 1); CHECK_EQ(k.dropped, 5);
	CHECK(k.half);
}

TEST(handoff_force_drop_for_fast_forward_and_rewind)
{
	Rig r(3);
	r.h.set_force_drop(true);
	for (uint32_t n = 0; n < 20; n++) CHECK(r.put(n));
	HandoffFrame *f = r.h.try_take();
	CHECK(f && f->seq == 19);
	r.h.release(f, 1000);
	r.h.set_force_drop(false);
	CHECK(r.put(20)); CHECK(r.put(21)); CHECK(r.put(22)); CHECK(!r.put(23));    /* the gate is back */
}

TEST(handoff_buffer_is_never_shared_while_written)
{
	Rig r(2);
	HandoffFrame *w = r.h.try_begin_write();
	CHECK(w != 0);
	memset(w->data, 0xAA, 64);
	/* a second buffer is a different one, and nothing written is visible to the drawing side until commit */
	HandoffFrame *w2 = r.h.try_begin_write();
	CHECK(w2 != 0 && w2 != w && w2->data != w->data);
	r.h.abandon(w2);
	CHECK(r.h.try_take() == 0);
	w->seq = 1; w->w = w->h = w->pitch = 8;
	r.h.commit(w);
	HandoffFrame *t = r.h.try_take();
	CHECK(t == w);
	/* while the drawing side holds it, the host cannot be handed that buffer again */
	HandoffFrame *w3 = r.h.try_begin_write();
	CHECK(w3 != 0 && w3 != t);
	r.h.abandon(w3);
	r.h.release(t, 100);
	/* last_shown keeps the picture on screen readable until the next release */
	const HandoffFrame *ls = r.h.last_shown();
	CHECK(ls == t && ls->data[0] == 0xAA);
	HandoffFrame *w4 = r.h.try_begin_write();
	CHECK(w4 != t);                                     /* not recycled while it is the one on screen */
	r.h.abandon(w4);
}

TEST(handoff_flush_discard_and_disable)
{
	Rig r(3);
	CHECK(r.put(1)); CHECK(r.put(2));
	CHECK(!r.h.flush(3000));                            /* something is queued and nobody draws: times out, says so */
	CHECK(r.h.discard(3000));                           /* queued pictures dropped, nothing in the drawing side's hands */
	CHECK_EQ(r.h.census().dropped, 2);
	CHECK(r.put(3));
	HandoffFrame *f = r.h.try_take();
	CHECK(f != 0);
	CHECK(!r.h.flush(3000));                            /* being drawn */
	r.h.release(f, 100);
	CHECK(r.h.flush(3000));                             /* now quiet */
	/* off (two-player lockstep): the host draws inline */
	r.h.set_enabled(false);
	CHECK(!r.h.enabled());
	CHECK(r.h.begin_write() == 0);
	CHECK(r.h.try_begin_write() == 0);
	r.h.set_enabled(true);
	CHECK(r.put(4));
}

TEST(handoff_costs_for_the_pacer_are_the_larger_of_the_two_threads)
{
	Rig r(2);
	CHECK(r.put(1));
	HandoffFrame *f = r.h.try_take();
	r.h.release(f, 9000);                               /* emulation said 3000 */
	uint32_t c = 0;
	CHECK(r.h.poll_cost(&c)); CHECK_EQ(c, 9000);
	CHECK(!r.h.poll_cost(&c));
	CHECK(r.put(2));
	f = r.h.try_take();
	r.h.release(f, 1000);
	CHECK(r.h.poll_cost(&c)); CHECK_EQ(c, 3000);
}

TEST(handoff_census_line_and_time_lost_is_visible)
{
	Rig r(2);
	char line[400];
	CHECK(!r.h.census_line(r.sys.t, line, sizeof line));        /* the first call opens the window */
	/* a second of wall time in which 60 pictures were made and shown, and the game ran all 60 frames */
	for (int i = 0; i < 60; i++) {
		CHECK(r.put((uint32_t) i));
		r.h.release(r.h.try_take(), 4000);
		r.h.note_emulated(1);
		r.sys.t += 16683;
	}
	CHECK(r.h.census_line(r.sys.t, line, sizeof line));
	CHECK(strstr(line, "handoff:") != 0);
	CHECK(strstr(line, "produced=60") != 0);
	CHECK(strstr(line, "shown=60") != 0);
	CHECK(strstr(line, "repeated=0") != 0);
	CHECK(strstr(line, "drift_ms=") != 0);
	CHECK(strstr(line, "mode=full") != 0);
	CHECK(!r.h.census_line(r.sys.t + 10, line, sizeof line));   /* once a second */
	/* the next second only 45 frames were emulated in 1 s of wall time: 250 ms of game time is lost, and it shows */
	for (int i = 0; i < 45; i++) { r.put(100u + i); r.h.release(r.h.try_take(), 4000); r.h.note_emulated(1); }
	r.sys.t += 1000000;
	CHECK(r.h.census_line(r.sys.t, line, sizeof line));
	HandoffCensus k = r.h.census();
	CHECK(k.emu_ms - k.wall_ms < -200);
	CHECK(strstr(line, "drift_ms=-") != 0);
	CHECK(strstr(line, "repeated=14") != 0);                    /* 59 refreshes due in the second, 45 pictures shown */
	/* a stop that was meant (a menu) is not lost time */
	r.h.time_baseline();
	CHECK(r.h.census().emu_ms == 0);
}

/* ---- threads ---------------------------------------------------------------------------- */

TEST(handoff_threads_full_mode_every_picture_in_order_untorn)
{
	for (int depth = 1; depth <= 4; depth++) {
		pkstress::Cfg c;
		c.depth = depth; c.frames = 1500;
		pkstress::Out o = pkstress::run(c);
		CHECK_EQ(o.failures, 0);
		CHECK_EQ(o.shown, 1500);
		CHECK_EQ(o.dropped, 0);
	}
}

TEST(handoff_threads_all_modes_flush_discard_disable)
{
	for (unsigned seed = 1; seed <= 4; seed++) {
		pkstress::Cfg c;
		c.modes = true; c.depth = 1 + (int) (seed % 3); c.seed = seed; c.frames = 1500;
		pkstress::Out o = pkstress::run(c);
		CHECK_EQ(o.failures, 0);
		CHECK_EQ(o.shown + o.dropped, o.produced);
	}
}

TEST(handoff_shutdown_wakes_both_sides)
{
	PosixHandoffSys sys;
	Handoff h;
	HandoffConfig c;
	c.frame_bytes = 64; c.depth.max_depth = 1;
	CHECK(h.init(c, &sys));
	std::atomic<int> took(-1), wrote(-1);
	std::thread consumer([&]() { took = h.take() ? 1 : 0; });          /* waits: nothing queued */
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	h.shutdown();
	consumer.join();
	CHECK_EQ(took.load(), 0);
	CHECK(h.begin_write() == 0);                                         /* after the end: NULL at once */
	/* a producer stuck at the gate is woken too */
	Handoff h2;
	CHECK(h2.init(c, &sys));
	HandoffFrame *f = h2.begin_write();
	f->seq = 1; f->w = f->h = f->pitch = 8;
	h2.commit(f);
	std::thread producer([&]() { wrote = h2.begin_write() ? 1 : 0; });   /* depth 1, one in flight: waits */
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	h2.shutdown();
	producer.join();
	CHECK_EQ(wrote.load(), 0);
}

TEST(handoff_gate_gives_up_on_a_drawing_thread_that_never_answers)
{
	PosixHandoffSys sys;
	Handoff h;
	HandoffConfig c;
	c.frame_bytes = 64; c.depth.max_depth = 1; c.gate_timeout_us = 30000;
	CHECK(h.init(c, &sys));
	HandoffFrame *f = h.begin_write();
	f->seq = 1; f->w = f->h = f->pitch = 8;
	h.commit(f);                                   /* in flight, nobody takes it */
	const uint64_t t0 = sys.now_us();
	HandoffFrame *g = h.begin_write();
	const uint64_t waited = sys.now_us() - t0;
	CHECK(g != 0);                                 /* the game is not held up for good */
	CHECK(waited >= 25000 && waited < 1000000);
	CHECK_EQ(h.census().gate_timeouts, 1);
	h.abandon(g);
}

/* ---- the model and the implementation agree ------------------------------------------------ */

namespace {
/* a pkmodel::Box that is a pk::Handoff on a virtual clock */
struct ImplBox : pkmodel::Box {
	VirtualSys sys;
	Handoff h;
	HandoffFrame *w, *cur;
	ImplBox(int depth, bool adaptive, uint32_t budget) : w(0), cur(0)
	{
		HandoffConfig c;
		c.frame_bytes = 16;
		c.refresh_us = budget;
		c.depth.max_depth = depth;
		c.adaptive = adaptive;
		h.init(c, &sys);
	}
	bool acquire(bool drop)
	{
		h.set_pace_mode(drop ? PACE_HALF : PACE_FULL);
		w = h.try_begin_write();
		return w != 0;
	}
	void commit(uint32_t seq, uint32_t emu_us, bool drop)
	{
		h.set_pace_mode(drop ? PACE_HALF : PACE_FULL);
		w->seq = seq; w->emu_us = emu_us; w->w = w->h = w->pitch = 4;
		h.commit(w);
		w = 0;
	}
	bool take(bool drop, uint32_t *seq, uint32_t *emu_us, int *span)
	{
		h.set_pace_mode(drop ? PACE_HALF : PACE_FULL);
		cur = h.try_take();
		if (!cur) return false;
		*seq = cur->seq; *emu_us = cur->emu_us; *span = cur->span;
		return true;
	}
	void release(uint32_t draw_us) { h.release(cur, draw_us); cur = 0; }
	bool poll_cost(uint32_t *c) { return h.poll_cost(c); }
	int depth() { return h.depth(); }
	long dropped() { return h.census().dropped; }
	int depth_changes() { return (int) h.census().depth_changes; }
};
}  // namespace

TEST(handoff_model_and_implementation_agree_on_a_simulated_display)
{
	struct Case { double D; double E; int depth; bool adaptive; bool steady; double wobble; unsigned seed; };
	const Case cases[] = {
		{ 0.80, 0.30, 1, false, true, 0.08, 1 }, { 0.90, 0.30, 2, false, true, 0.08, 2 }, { 1.00, 0.05, 2, false, false, 0.08, 3 },
		{ 1.00, 0.05, 4, false, false, 0.08, 4 }, { 1.10, 0.30, 2, false, true, 0.08, 5 }, { 1.05, 0.05, 1, false, false, 0.15, 6 },
		{ 1.00, 0.05, 2, true, false, 0.08, 7 }, { 1.10, 0.05, 3, true, false, 0.15, 8 }, { 0.95, 0.30, 2, true, true, 0.20, 9 },
		{ 1.20, 0.30, 3, false, true, 0.08, 10 }, { 0.85, 0.10, 2, true, true, 0.25, 11 },
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		pkmodel::Params p;
		p.handoff = true; p.D = cases[i].D; p.E = cases[i].E; p.depth = cases[i].depth; p.adaptive = cases[i].adaptive;
		p.steady = cases[i].steady; p.wobble = cases[i].wobble; p.seed = cases[i].seed; p.seconds = 40;
		if (i == 8) { p.D_profile.push_back(std::make_pair(0.0, 0.7)); p.D_profile.push_back(std::make_pair(12.0, 1.15)); p.D_profile.push_back(std::make_pair(25.0, 0.7)); }
		pkmodel::Sim a(p), b(p);
		pkmodel::Result ra = a.run();
		ImplBox box(p.depth, p.adaptive, (uint32_t) p.R);
		pkmodel::Result rb = b.run(&box);
		/* the same pictures shown at the same refreshes */
		CHECK_EQ(a.shown_list.size(), b.shown_list.size());
		bool same = a.shown_list.size() == b.shown_list.size();
		for (size_t k = 0; same && k < a.shown_list.size(); k++)
			if (a.shown_list[k].seq != b.shown_list[k].seq || a.shown_list[k].t != b.shown_list[k].t) { same = false; CHECK(same); }
		CHECK_EQ(a.dropped_total, b.dropped_total);
		CHECK_EQ(ra.depth_changes, rb.depth_changes);
		CHECK(ra.fps == rb.fps);
		CHECK(ra.delay_mean_ms == rb.delay_mean_ms);
		CHECK(ra.repeats_per_s == rb.repeats_per_s);
		CHECK(ra.speed_pct == rb.speed_pct);
		CHECK(ra.depth_avg == rb.depth_avg);
		if (ra.fps < 20 || ra.shown < 100) CHECK(false);        /* the cases are real runs */
	}
}

/* ---- the pad is latched per emulated frame ---------------------------------------------------- */

TEST(input_pad_latch_keeps_a_tap_between_emulated_frames)
{
	PadLatch l;
	PadSample pads[MAX_PLAYERS], out[MAX_PLAYERS];
	pads[0].connected = true;
	/* the pad is read every refresh, the core takes a sample every second refresh (pictures every 2nd frame) */
	pads[0].buttons = 0; l.feed(pads);
	pads[0].buttons = PH_CROSS; l.feed(pads);          /* down for one refresh only */
	pads[0].buttons = 0; l.feed(pads);
	l.take(out);
	CHECK((out[0].buttons & PH_CROSS) != 0);           /* the game still sees the tap */
	pads[0].buttons = 0; l.feed(pads);
	l.take(out);
	CHECK((out[0].buttons & PH_CROSS) == 0);           /* and it is used up: seen once, not stuck */
}

TEST(input_pad_latch_follows_held_buttons_and_samples)
{
	PadLatch l;
	PadSample pads[MAX_PLAYERS], out[MAX_PLAYERS];
	pads[0].connected = true; pads[0].lx = 10;
	pads[0].buttons = PH_LEFT; l.feed(pads);
	l.take(out);
	CHECK((out[0].buttons & PH_LEFT) != 0); CHECK_EQ(out[0].lx, 10);
	l.take(out);                                        /* still held, no new feed */
	CHECK((out[0].buttons & PH_LEFT) != 0);
	pads[0].buttons = 0; pads[0].lx = 128; l.feed(pads);
	l.take(out);
	CHECK((out[0].buttons & PH_LEFT) == 0); CHECK_EQ(out[0].lx, 128);
	/* an unplugged pad adds nothing; clear() forgets presses made before a menu */
	pads[1].connected = false; pads[1].buttons = PH_CROSS;
	pads[0].buttons = PH_CIRCLE; l.feed(pads);
	l.clear();
	l.take(out);
	CHECK(!out[0].connected); CHECK_EQ(out[0].buttons, 0); CHECK_EQ(out[1].buttons, 0);
}

TEST(input_short_tap_reaches_the_game_through_mapper_with_the_latch_not_without)
{
	/* Pads are read at 60 Hz, the core takes one sample per emulated frame, here every second read (pictures
	 * every 2nd frame, or a hand-over): a tap one read long is seen only through the latch. */
	MapperConfig mc;
	uint32_t cross_bit = 0;
	{
		PadSample one[MAX_PLAYERS];
		one[0].connected = true; one[0].buttons = PH_CROSS;
		Mapper probe;
		probe.configure(model_snes(), profile_defaults(model_snes(), true), mc);
		InputResult pr;
		for (int k = 0; k < 20; k++) probe.update(one, 1000 + k * 16, (uint32_t) k, &pr);
		cross_bit = pr.pad[0];
	}
	CHECK(cross_bit != 0);
	int seen_with = 0, seen_without = 0;
	for (int phase = 0; phase < 2; phase++) {
		Mapper a, b;
		a.configure(model_snes(), profile_defaults(model_snes(), true), mc);
		b.configure(model_snes(), profile_defaults(model_snes(), true), mc);
		PadLatch l;
		PadSample pads[MAX_PLAYERS], took[MAX_PLAYERS];
		pads[0].connected = true;
		InputResult r;
		uint64_t ms = 1000;
		bool gl = false, gp = false;
		for (int read = 0; read < 40; read++) {
			pads[0].buttons = (read == 20 + phase) ? (uint32_t) PH_CROSS : 0u;
			l.feed(pads);
			ms += 16;
			if (read % 2 == 1) {
				l.take(took);
				a.update(took, ms, (uint32_t) read, &r);
				if (r.pad[0] & cross_bit) gl = true;
				b.update(pads, ms, (uint32_t) read, &r);
				if (r.pad[0] & cross_bit) gp = true;
			}
		}
		seen_with += gl; seen_without += gp;
	}
	CHECK_EQ(seen_with, 2);          /* kept on both phases */
	CHECK_EQ(seen_without, 1);       /* the unlatched path loses it on the phase that falls between two samples (the fault) */
}
