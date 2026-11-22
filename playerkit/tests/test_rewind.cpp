/* Unit tests: pk_rewind (the delta codec, the ring, the session) and pk_ff. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include "pk_test.h"
#include "pk_rewind.h"
#include "pk_ff.h"
#include "pk_speed.h"

#include <stdlib.h>
#include <string.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

using namespace pk;

namespace {
struct Rng {
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 2654435761u + 88172645463325252ull) {}
	uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t) (s >> 11); }
	uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

/* a machine whose state is a function of the frame number: a counter in the first 8 bytes, a few bytes that change every frame,
 * a block that changes now and then, and a large part that never does */
struct Machine {
	std::vector<uint8_t> st;
	uint32_t frame = 0;
	Rng r;
	unsigned churn;
	Machine(size_t n, unsigned churn_, uint64_t seed) : st(n, 0), r(seed), churn(churn_) {
		for (size_t i = 64; i < n; i++) st[i] = (uint8_t) (i * 31 + 7);
	}
	void step() {
		frame++;
		memcpy(&st[0], &frame, 4);
		for (unsigned k = 0; k < churn; k++) st[16 + r.below((uint32_t) (st.size() - 16))] = (uint8_t) r.next();
		if ((frame % 37) == 0) for (int k = 0; k < 600 && 600 < (int) st.size(); k++) st[256 + (size_t) k] = (uint8_t) (r.next());
	}
};

uint32_t frame_of(const uint8_t *st) { uint32_t f; memcpy(&f, st, 4); return f; }

RewindConfig cfg_for(size_t n, int seconds, size_t budget, int imin = 2, int imax = 30)
{
	RewindConfig c;
	c.state_bytes = n; c.seconds = seconds; c.budget_bytes = budget; c.fps = 60.0; c.interval_min = imin; c.interval_max = imax;
	return c;
}

/* run `frames` frames of the machine, capturing as the player does (after every frame) */
void run(Rewind &rw, Machine &m, int frames, std::vector<std::vector<uint8_t> > *hist = 0)
{
	for (int i = 0; i < frames; i++) {
		m.step();
		uint8_t *b = rw.capture_begin();
		if (b) {
			memcpy(b, m.st.data(), m.st.size());
			rw.capture_commit(m.st.size());
			if (hist) { if (hist->size() <= m.frame) hist->resize(m.frame + 1); (*hist)[m.frame] = m.st; }
		}
	}
}
}  // namespace

/* ---- the codec ---- */

TEST(rewind_codec_equal_buffers_are_nothing)
{
	std::vector<uint8_t> a(1000, 5), out(delta_bound(1000));
	CHECK_EQ(delta_encode(a.data(), a.data(), 1000, out.data(), out.size()), 0);
	CHECK_EQ(delta_encode(a.data(), a.data(), 0, out.data(), out.size()), 0);
	uint8_t z = 1;
	CHECK(delta_apply(&z, 1, out.data(), 0));
	CHECK_EQ(z, 1);
}

TEST(rewind_codec_exhaustive_small)
{
	/* every pair of 10 byte buffers whose bytes are 0 or a changing value: 2^10 x 2^10 = 1,048,576 pairs, both directions */
	const int n = 10;
	std::vector<uint8_t> a(n), b(n), out(delta_bound(n)), t(n);
	long pairs = 0;
	for (int x = 0; x < (1 << n); x++) {
		for (int i = 0; i < n; i++) a[i] = (x >> i) & 1 ? (uint8_t) (0x80 + i) : (uint8_t) 3;
		for (int y = 0; y < (1 << n); y++) {
			for (int i = 0; i < n; i++) b[i] = (y >> i) & 1 ? (uint8_t) (0x40 + i) : (uint8_t) 3;
			size_t len = delta_encode(a.data(), b.data(), n, out.data(), out.size());
			if (len == (size_t) -1 || len > delta_bound(n)) { CHECK(false); return; }
			t = a;
			if (!delta_apply(t.data(), n, out.data(), len) || t != b) { CHECK(false); return; }
			if (!delta_apply(t.data(), n, out.data(), len) || t != a) { CHECK(false); return; }
			pairs++;
		}
	}
	CHECK_EQ(pairs, 1 << 20);
}

TEST(rewind_codec_random_round_trip)
{
	Rng r(1);
	long cases = 0;
	for (int iter = 0; iter < 6000; iter++) {
		const size_t n = r.below(3000);
		std::vector<uint8_t> a(n), b(n), out(delta_bound(n) + 8), t;
		for (size_t i = 0; i < n; i++) a[i] = (uint8_t) r.next();
		b = a;
		const uint32_t mode = r.below(5);
		if (mode == 0) for (size_t i = 0; i < n; i++) b[i] = (uint8_t) r.next();                        /* everything differs */
		else if (mode == 1) { for (uint32_t k = 0; k < r.below(20); k++) if (n) b[r.below((uint32_t) n)] ^= (uint8_t) (1 + r.below(255)); }
		else if (mode == 2) { size_t s = r.below((uint32_t) n + 1), e = s + r.below((uint32_t) (n - s + 1)); for (size_t i = s; i < e; i++) b[i] = (uint8_t) r.next(); }
		else if (mode == 3) { for (size_t i = 0; i < n; i += 5) b[i] ^= 1; }                              /* gaps of exactly 4: the fold edge */
		else { for (size_t i = 0; i < n; i += 1 + r.below(9)) b[i] ^= (uint8_t) (1 + r.below(255)); }
		size_t len = delta_encode(a.data(), b.data(), n, out.data(), out.size());
		CHECK(len != (size_t) -1);
		CHECK(len <= delta_bound(n));
		t = a;
		CHECK(delta_apply(t.data(), n ? n : 1, out.data(), len) || n == 0);
		CHECK(t == b);
		CHECK(delta_apply(t.data(), n ? n : 1, out.data(), len) || n == 0);
		CHECK(t == a);
		/* a cap that is one byte short is refused, never overrun */
		if (len > 0) {
			std::vector<uint8_t> small(len - 1 + 1);
			size_t l2 = delta_encode(a.data(), b.data(), n, small.data(), len - 1);
			CHECK_EQ(l2, (size_t) -1);
		}
		cases++;
	}
	CHECK_EQ(cases, 6000);
}

TEST(rewind_codec_damaged_streams_are_refused_not_followed)
{
	Rng r(2);
	std::vector<uint8_t> buf(500, 0), ref;
	for (int iter = 0; iter < 20000; iter++) {
		const size_t len = r.below(64);
		std::vector<uint8_t> d(len);
		for (size_t i = 0; i < len; i++) d[i] = (uint8_t) r.next();
		std::vector<uint8_t> work = buf;
		bool ok = delta_apply(work.data(), work.size(), d.data(), len);
		(void) ok;     /* either answer is fine; what matters is that nothing outside the buffer is touched (ASan) */
	}
	/* a count that reaches past the end */
	uint8_t d1[] = { 0x00, 0x7F, 1, 2 };
	CHECK(!delta_apply(buf.data(), buf.size(), d1, sizeof d1));
	/* a skip that reaches past the end */
	uint8_t d2[] = { 0xFF, 0xFF, 0x7F, 0x01, 0x05 };
	CHECK(!delta_apply(buf.data(), buf.size(), d2, sizeof d2));
	/* an unterminated number */
	uint8_t d3[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
	CHECK(!delta_apply(buf.data(), buf.size(), d3, sizeof d3));
}

/* ---- the ring and the session ---- */

TEST(rewind_back_equals_captured_states)
{
	const size_t N = 20000;
	Machine m(N, 6, 3);
	Rewind rw;
	CHECK(rw.open(cfg_for(N, 60, 4u << 20)));
	std::vector<std::vector<uint8_t> > hist;
	run(rw, m, 600, &hist);                 /* 10 s */
	RewindStats s = rw.stats();
	CHECK(s.entries > 100);
	CHECK(s.interval == 2);
	/* hold the control: every landing is exactly one of the captured states, and they go strictly backwards */
	uint32_t last = 0xFFFFFFFFu;
	unsigned landings = 0;
	for (int refresh = 0; refresh < 400; refresh++) {
		RewindAct a = rw.refresh(true, false);
		if (a == RW_LAND) {
			const uint32_t f = frame_of(rw.cur());
			CHECK(f < last || last == 0xFFFFFFFFu);
			CHECK(f < hist.size() && !hist[f].empty());
			CHECK(f < hist.size() && memcmp(rw.cur(), hist[f].data(), N) == 0);
			last = f;
			landings++;
			rw.landed();
		}
		if (a == RW_STAND && rw.entries() == 0) break;
	}
	CHECK(landings > 100);
	CHECK(rw.chip().find("(oldest)") != std::string::npos);
	CHECK_STR(rw.take_notice(), "This is as far back as rewind goes");
	CHECK_STR(rw.take_notice(), "");
	RewindAct a = rw.refresh(false, false);
	CHECK(a == RW_PLAY);
	CHECK(!rw.active());
}

TEST(rewind_five_seconds_back_is_the_state_of_five_seconds_ago)
{
	const size_t N = 30000;
	Machine m(N, 4, 4);
	Rewind rw;
	CHECK(rw.open(cfg_for(N, 60, 6u << 20)));
	std::vector<std::vector<uint8_t> > hist;
	run(rw, m, 900, &hist);
	const uint32_t now = m.frame;
	/* hold until about five seconds are behind the game */
	uint32_t at = 0;
	for (int i = 0; i < 2000; i++) {
		RewindAct a = rw.refresh(true, false);
		if (a == RW_LAND) { at = frame_of(rw.cur()); rw.landed(); if (now - at >= 300) break; }
	}
	CHECK(now - at >= 300);
	CHECK(now - at < 300 + 40);               /* stopped within one landing of five seconds (the speed ramp can jump 16 frames a refresh) */
	CHECK(memcmp(rw.cur(), hist[at].data(), N) == 0);
	CHECK(rw.back_seconds() >= 5 && rw.back_seconds() <= 6);
	/* let go and play on: new states continue from the landed one, and a second rewind lands on them */
	CHECK(rw.refresh(false, false) == RW_PLAY);
	Machine m2 = m;
	m2.frame = at + 1000;                      /* another timeline */
	memcpy(&m2.st[0], &m2.frame, 4);
	run(rw, m2, 40, &hist);
	uint32_t first = 0;
	for (int i = 0; i < 5 && !first; i++) {
		RewindAct a = rw.refresh(true, false);
		if (a == RW_LAND) { first = frame_of(rw.cur()); rw.landed(); }
	}
	CHECK(first >= at + 1000);                 /* on the new timeline, not the old one */
	CHECK(memcmp(rw.cur(), hist[first].data(), N) == 0);
}

TEST(rewind_budget_is_respected_and_the_oldest_moment_is_what_goes)
{
	const size_t N = 16000;
	Machine m(N, 500, 5);                      /* a busy game: many bytes change */
	Rewind rw;
	const size_t budget = 420u << 10;
	CHECK(rw.open(cfg_for(N, 60, budget)));
	std::vector<std::vector<uint8_t> > hist;
	run(rw, m, 3000, &hist);
	RewindStats s = rw.stats();
	CHECK(s.used_bytes <= s.ring_bytes);
	CHECK(s.total_bytes <= budget);
	CHECK(s.ring_bytes + 2 * N <= budget);
	CHECK(s.dropped > 0);                      /* it did not all fit: the oldest steps went */
	CHECK(s.interval > 2);                     /* and the interval grew to keep more */
	CHECK(s.frames < 3000);
	/* what is held is still exact, newest to oldest */
	uint32_t last = 0xFFFFFFFFu;
	unsigned n = 0;
	for (int i = 0; i < 20000; i++) {
		RewindAct a = rw.refresh(true, false);
		if (a == RW_LAND) {
			const uint32_t f = frame_of(rw.cur());
			CHECK(f < last);
			CHECK(f < hist.size() && !hist[f].empty() && memcmp(rw.cur(), hist[f].data(), N) == 0);
			last = f; n++; rw.landed();
		}
		if (rw.entries() == 0 && a == RW_STAND) break;
	}
	CHECK(n > 10);
}

TEST(rewind_quiet_game_keeps_the_whole_minute_in_a_small_budget)
{
	const size_t N = 145 * 1024;               /* the Mega Drive's state */
	Machine m(N, 3, 6);
	Rewind rw;
	CHECK(rw.open(cfg_for(N, 60, 12u << 20)));
	run(rw, m, 60 * 70, 0);                    /* 70 s */
	RewindStats s = rw.stats();
	CHECK(s.frames >= 60 * 60 - 4);            /* a full 60 s kept */
	CHECK(s.frames <= 60 * 60 + 64);
	CHECK(s.used_bytes < s.ring_bytes / 2);
	CHECK(s.interval == 2);
	/* the packed size per step and the ratio, for the report */
	fprintf(stderr, "    (145 KB state, 3 bytes/frame churn: %u steps, %.0f bytes a step, ring used %.0f KB of %.0f KB)\n", s.entries,
	        s.entries ? (double) s.used_bytes / s.entries : 0.0, s.used_bytes / 1024.0, s.ring_bytes / 1024.0);
}

TEST(rewind_ring_wraparound_stress)
{
	Rng r(7);
	for (int round = 0; round < 12; round++) {
		const size_t N = 3000 + r.below(9000);
		Machine m(N, 1 + r.below(60), 100 + (uint64_t) round);
		Rewind rw;
		CHECK(rw.open(cfg_for(N, 20, 2 * N + 140u * 1024 + r.below(80u * 1024), 1 + (int) r.below(3), 8 + (int) r.below(20))));
		std::vector<std::vector<uint8_t> > hist;
		for (int seg = 0; seg < 6; seg++) {
			run(rw, m, 200 + r.below(400), &hist);
			/* rewind a random part, let go, play on (a branch), over and over */
			const int hold = 1 + r.below(120);
			uint32_t last = 0xFFFFFFFFu;
			for (int i = 0; i < hold; i++) {
				RewindAct a = rw.refresh(true, false);
				if (a == RW_LAND) {
					const uint32_t f = frame_of(rw.cur());
					CHECK(f < last);
					CHECK(f < hist.size() && !hist[f].empty() && memcmp(rw.cur(), hist[f].data(), N) == 0);
					last = f;
					rw.landed();
					/* the player runs the landing frame: the machine is the landed state plus one frame, on the same timeline */
					memcpy(m.st.data(), rw.cur(), N);
					memcpy(&m.frame, rw.cur(), 4);
				}
			}
			rw.refresh(false, false);
			RewindStats s = rw.stats();
			CHECK(s.used_bytes <= s.ring_bytes);
			/* keep the timeline's numbering unique: jump the counter ahead so a frame number names one state only */
			m.frame += 10000 * (unsigned) (seg + 1);
		}
	}
}

TEST(rewind_async_helper_gives_the_same_history)
{
	const size_t N = 12000;
	std::vector<std::vector<uint8_t> > ha, hb;
	Machine ma(N, 8, 9), mb(N, 8, 9);
	Rewind sync_rw, async_rw;
	CHECK(sync_rw.open(cfg_for(N, 30, 3u << 20)));
	CHECK(async_rw.open(cfg_for(N, 30, 3u << 20)));
	std::atomic<bool> stop(false);
	std::atomic<int> wakes(0);
	auto wake = [](void *p) { ((std::atomic<int> *) p)->fetch_add(1); };
	async_rw.set_async(true, wake, &wakes);
	std::thread helper([&]() { while (!stop.load()) { if (!async_rw.pack_pending()) std::this_thread::sleep_for(std::chrono::microseconds(50)); } });
	/* the helper is slow on purpose: some captures are put off a frame, which changes the interval positions, so compare what is held
	 * against the machine's own history and not against the other ring */
	for (int i = 0; i < 1500; i++) {
		ma.step(); mb.step();
		std::this_thread::sleep_for(std::chrono::microseconds(150));
		uint8_t *b = sync_rw.capture_begin();
		if (b) { memcpy(b, ma.st.data(), N); sync_rw.capture_commit(N); }
		b = async_rw.capture_begin();
		if (b) {
			memcpy(b, mb.st.data(), N); async_rw.capture_commit(N);
			if (hb.size() <= mb.frame) hb.resize(mb.frame + 1);
			hb[mb.frame] = mb.st;
		}
	}
	stop = true;
	helper.join();
	async_rw.set_async(false, 0, 0);
	CHECK(wakes.load() > 0);
	uint32_t last = 0xFFFFFFFFu;
	unsigned n = 0;
	for (int i = 0; i < 4000; i++) {
		RewindAct a = async_rw.refresh(true, false);
		if (a == RW_LAND) {
			const uint32_t f = frame_of(async_rw.cur());
			CHECK(f < last);
			CHECK(f < hb.size() && !hb[f].empty() && memcmp(async_rw.cur(), hb[f].data(), N) == 0);
			last = f; n++; async_rw.landed();
		}
		if (async_rw.entries() == 0 && a == RW_STAND) break;
	}
	CHECK(n > 100);
}

TEST(rewind_locked_off_and_notices)
{
	const size_t N = 8000;
	Machine m(N, 4, 11);
	Rewind rw, off;
	CHECK(rw.open(cfg_for(N, 60, 2u << 20)));
	run(rw, m, 200, 0);
	/* a press while locked: a notice, the game plays */
	CHECK(rw.refresh(true, true) == RW_PLAY);
	CHECK_STR(rw.take_notice(), "Rewind is off while games are linked");
	CHECK(rw.refresh(false, true) == RW_PLAY);
	/* locked in the middle of a rewind ends it */
	CHECK(rw.refresh(true, false) == RW_LAND);
	rw.landed();
	CHECK(rw.active());
	CHECK(rw.refresh(true, true) == RW_PLAY);
	CHECK(!rw.active());
	/* never opened: says why, once per press */
	CHECK(off.refresh(true, false) == RW_PLAY);
	CHECK_STR(off.take_notice(), "Rewind is switched off (Settings)");
	/* a budget too small for the two states: not enough memory */
	Rewind tiny;
	CHECK(!tiny.open(cfg_for(N, 60, 2 * N)));
	CHECK(tiny.refresh(true, false) == RW_PLAY);
	CHECK_STR(tiny.take_notice(), "Rewind: not enough memory for it");
}

TEST(rewind_clear_forgets_the_history)
{
	const size_t N = 8000;
	Machine m(N, 4, 12);
	Rewind rw;
	CHECK(rw.open(cfg_for(N, 60, 2u << 20)));
	run(rw, m, 300, 0);
	CHECK(rw.entries() > 50);
	rw.clear();
	CHECK_EQ(rw.entries(), 0);
	CHECK_EQ(rw.history_frames(), 0);
	CHECK(rw.refresh(true, false) == RW_PLAY);         /* nothing to go back to */
	CHECK_STR(rw.take_notice(), "This is as far back as rewind goes");
	rw.refresh(false, false);
	run(rw, m, 10, 0);                                  /* the first state is taken at once after a clear */
	CHECK(rw.refresh(true, false) == RW_LAND);
}

TEST(rewind_speed_ramps_with_the_hold)
{
	const size_t N = 6000;
	Machine m(N, 2, 13);
	Rewind rw;
	CHECK(rw.open(cfg_for(N, 60, 8u << 20)));
	run(rw, m, 3600, 0);
	/* frames gone back per refresh: 2 for the first second, then 4, 8, 16 */
	uint32_t prev = frame_of(rw.cur());
	auto land = [&](int refreshes, uint32_t *gone) {
		*gone = 0;
		for (int i = 0; i < refreshes; i++) {
			if (rw.refresh(true, false) == RW_LAND) { uint32_t f = frame_of(rw.cur()); *gone += prev - f; prev = f; rw.landed(); }
		}
	};
	uint32_t g;
	rw.refresh(true, false); rw.landed(); prev = frame_of(rw.cur());
	land(58, &g);  CHECK(g >= 2 * 58 - 4 && g <= 2 * 58 + 4);
	land(60, &g);  CHECK(g >= 4 * 60 - 8 && g <= 4 * 60 + 8);
	land(60, &g);  CHECK(g >= 4 * 60 - 8 && g <= 4 * 60 + 8);
	land(100, &g); CHECK(g >= 8 * 99 + 4 - 24 && g <= 8 * 99 + 4 + 24);
}

/* ---- the capture off the frame's path (pk_speed: set_defer_capture / capture_idle) ---- */

namespace {
struct DeferHost : SpeedHost {
	Machine m;
	unsigned saves = 0;
	uint32_t saved_frame = 0;
	DeferHost() : m(4096, 2, 5) {}
	size_t state_size() override { return m.st.size(); }
	bool save_state(uint8_t *buf, size_t cap) override { if (cap != m.st.size()) return false; memcpy(buf, m.st.data(), cap); saves++; saved_frame = m.frame; return true; }
	bool load_state(const uint8_t *buf, size_t n) override { if (n != m.st.size()) return false; memcpy(m.st.data(), buf, n); memcpy(&m.frame, buf, 4); return true; }
	void run_frame(bool) override { m.step(); }
	void clear_pads() override {}
};
}

TEST(deferred_capture_takes_the_same_states_at_the_idle_point)
{
	SpeedSettings set;
	set.rewind_budget = 4u << 20;
	DeferHost a, b;
	Speed inl, def;
	inl.set_host(&a); def.set_host(&b);
	inl.start(60.0, 2, set); def.start(60.0, 2, set);
	def.set_defer_capture(true);
	for (int f = 0; f < 600; f++) {
		inl.run_frame(true);
		const unsigned before = b.saves;
		def.run_frame(true);
		CHECK_EQ(b.saves, before);                  /* nothing is written on the frame's path */
		CHECK(def.capture_pending());
		def.capture_idle(false);
		CHECK(!def.capture_pending());
		CHECK_EQ(a.saves, b.saves);                 /* the same frames are captured ... */
		CHECK_EQ(a.saved_frame, b.saved_frame);     /* ... with the same state */
	}
	CHECK(b.saves > 100);
	CHECK_EQ(inl.rewind().entries(), def.rewind().entries());
}

TEST(deferred_capture_is_put_off_on_late_passes_but_not_for_ever)
{
	SpeedSettings set;
	set.rewind_budget = 4u << 20;
	DeferHost h;
	Speed sp;
	sp.set_host(&h);
	sp.start(60.0, 2, set);
	sp.set_defer_capture(true);
	/* every pass is late: a capture still happens, after at most kPutOffMax passes */
	unsigned last = 0, worst = 0, gap = 0;
	for (int f = 0; f < 400; f++) {
		sp.run_frame(true);
		sp.capture_idle(true);
		if (h.saves != last) { last = h.saves; gap = 0; } else if (++gap > worst) worst = gap;
	}
	CHECK(h.saves >= 400 / (Speed::kPutOffMax + 2) - 2);
	CHECK(worst <= (unsigned) Speed::kPutOffMax + 2);
	CHECK(sp.captures_put_off() > 0);
	/* two frames in one pass (a catch-up) count as two towards the interval */
	const unsigned s0 = h.saves;
	for (int p = 0; p < 100; p++) { sp.run_frame(true); sp.run_frame(true); sp.capture_idle(false); }
	CHECK(h.saves - s0 >= 45);
	/* a rewind drops what was pending: the frames it ran are not history */
	sp.run_frame(true);
	CHECK(sp.capture_pending());
}

/* ---- fast forward ---- */

TEST(ff_hold_and_toggle)
{
	FastForward ff;
	ff.configure(2, true, false);
	CHECK(!ff.on());
	CHECK_EQ(ff.frames(1), 1);
	ff.update(true, true, false);
	CHECK(ff.on());
	CHECK_EQ(ff.frames(1), 2);
	CHECK_EQ(ff.frames(2), 4);
	CHECK_STR(ff.chip(), ">> 2x");
	ff.update(false, false, false);
	CHECK(!ff.on());
	CHECK(ff.take_ended());
	CHECK(!ff.take_ended());
	ff.configure(3, true, true);
	ff.update(true, true, false);              /* press */
	CHECK(ff.on());
	ff.update(true, false, false);             /* still held: stays on */
	ff.update(false, false, false);            /* released: toggle stays on */
	CHECK(ff.on());
	CHECK_EQ(ff.factor(), 3);
	CHECK_STR(ff.chip(), ">> 3x");
	ff.update(true, true, false);              /* press again: off */
	CHECK(!ff.on());
	/* two-player: never on */
	ff.configure(2, true, false);
	ff.update(true, true, true);
	CHECK(!ff.on());
}

/* Fast forward at normal pitch: a steady tone keeps its pitch, the output is one frame's worth per refresh, and the joins are smooth.
 * (A plain splice of every Nth chunk is measured too, so the test could tell if the crossfade were missing.) */
TEST(ff_normal_pitch_is_smooth_and_keeps_the_pitch)
{
	const double rate = 44100.0, hz = 440.0, amp = 10000.0, pi = 3.14159265358979;
	const double slope = 2 * pi * hz / rate * amp;            /* the steepest step of the tone itself */
	for (int factor = 2; factor <= 3; factor++) {
		FastForward ff;
		ff.configure(factor, FastForward::SOUND_NORMAL, false);
		ff.update(true, true, false);
		std::vector<int16_t> out, naive;
		size_t t = 0, in = 0, per_refresh_ok = 0;
		for (int refresh = 0; refresh < 200; refresh++) {
			size_t got = 0, last_len = 0;
			for (int f = 0; f < factor; f++) {
				const size_t n = 734 + (size_t) ((refresh + f) % 3);          /* a frame is not a whole number of samples */
				std::vector<int16_t> buf(n * 2);
				for (size_t i = 0; i < n; i++, t++) {
					int16_t v = (int16_t) (amp * sin(2 * pi * hz * (double) t / rate));
					buf[i * 2] = v; buf[i * 2 + 1] = (int16_t) -v;
				}
				in += n; last_len = n;
				if (f == factor - 1) naive.insert(naive.end(), buf.begin(), buf.end());
				const size_t k = ff.reduce(buf.data(), n);
				got += k;
				out.insert(out.end(), buf.begin(), buf.begin() + (long) (k * 2));
			}
			if (got == last_len) per_refresh_ok++;
		}
		CHECK_EQ((int) per_refresh_ok, 200);                                       /* one frame's worth every refresh, as at normal speed */
		CHECK(out.size() / 2 < in / factor + 800 && out.size() / 2 > in / factor - 800);
		/* no spike: the largest step between neighbouring samples (both channels) stays near the tone's own slope */
		double worst = 0, worst_naive = 0;
		for (size_t i = 1; i < out.size() / 2; i++) {
			worst = std::max(worst, (double) std::abs(out[i * 2] - out[(i - 1) * 2]));
			worst = std::max(worst, (double) std::abs(out[i * 2 + 1] - out[(i - 1) * 2 + 1]));
		}
		for (size_t i = 1; i < naive.size() / 2; i++) worst_naive = std::max(worst_naive, (double) std::abs(naive[i * 2] - naive[(i - 1) * 2]));
		CHECK(worst_naive > 5 * slope);                                            /* a bare splice does spike (so the check can fail) */
		CHECK(worst <= 1.5 * slope);                                               /* the fades do not */
		/* the pitch is the game's: zero crossings per second near 2 * 440, not 2 * 440 * factor */
		size_t crossings = 0;
		for (size_t i = 1; i < out.size() / 2; i++) if ((out[i * 2] < 0) != (out[(i - 1) * 2] < 0)) crossings++;
		const double secs = (double) (out.size() / 2) / rate, per_s = (double) crossings / secs;
		CHECK(per_s > 2 * hz * 0.85 && per_s < 2 * hz * 1.15);
	}
	/* what is dropped is dropped: the first chunks of a group return nothing */
	FastForward ff;
	ff.configure(3, FastForward::SOUND_NORMAL, false);
	ff.update(true, true, false);
	std::vector<int16_t> a(300 * 2, 5);
	CHECK_EQ((int) ff.reduce(a.data(), 300), 0);
	CHECK_EQ((int) ff.reduce(a.data(), 300), 0);
	CHECK_EQ((int) ff.reduce(a.data(), 300), 300);
	/* off again: the audio is untouched */
	ff.update(false, false, false);
	CHECK_EQ((int) ff.reduce(a.data(), 300), 300);
}

TEST(ff_sound_is_mixed_down_so_the_pacer_sees_real_time)
{
	for (int factor = 2; factor <= 3; factor++) {
		FastForward ff;
		ff.configure(factor, true, false);
		ff.update(true, true, false);
		/* 10 refreshes of `factor` emulated frames of 801 samples each (a frame is not a whole number of samples): the output is the same
		 * number of samples as at normal speed, within one sample */
		size_t out = 0, in = 0;
		for (int refresh = 0; refresh < 10; refresh++) {
			for (int f = 0; f < factor; f++) {
				std::vector<int16_t> buf(801 * 2);
				for (size_t i = 0; i < 801; i++) { buf[i * 2] = (int16_t) (1000 * factor); buf[i * 2 + 1] = (int16_t) (-1000 * factor); }
				in += 801;
				out += ff.reduce(buf.data(), 801);
				if (f == 0 && refresh == 0) { CHECK(buf[0] == 1000 * factor); }
			}
		}
		CHECK(out >= (in * 1) / factor - 1 && out <= in / factor + 1);
		CHECK(out >= 10 * 801 - 3 && out <= 10 * 801 + 3);
	}
	/* the value is the mean of the group */
	FastForward ff;
	ff.configure(3, true, false);
	ff.update(true, true, false);
	int16_t b[6] = { 3, 30, 6, 60, 9, 90 };
	CHECK_EQ(ff.reduce(b, 3), 1);
	CHECK_EQ(b[0], 6);
	CHECK_EQ(b[1], 60);
}

TEST(ff_without_sound_is_silence_and_normal_speed_is_untouched)
{
	FastForward ff;
	ff.configure(2, false, false);
	int16_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	CHECK_EQ(ff.reduce(b, 4), 4);              /* not fast forwarding: untouched */
	CHECK_EQ(b[0], 1);
	ff.update(true, true, false);
	CHECK_EQ(ff.reduce(b, 4), 2);              /* silence, at the real-time count */
	CHECK_EQ(b[0], 0);
	CHECK_EQ(b[3], 0);
}

/* ---- Unlimited and Unlimited Smooth fast forward (the main app's choices of the same names) ---- */
namespace {
struct ClockHost : SpeedHost {
	uint64_t clock = 0;
	uint32_t plain_us = 2000, drawn_us = 3000;
	unsigned ran = 0, drawn = 0;
	size_t state_size() override { return 64; }
	bool save_state(uint8_t *b, size_t) override { memset(b, 0, 64); return true; }
	bool load_state(const uint8_t *, size_t) override { return true; }
	void run_frame(bool render) override { ran++; if (render) drawn++; clock += render ? drawn_us : plain_us; }
	void clear_pads() override {}
	uint64_t now_us() override { return clock; }
};
}

TEST(ff_unlimited_counts_frames_from_their_cost)
{
	CHECK_EQ(Speed::unlimited_frames(14000, 2000, 3000, true), 1 + 11000 / 2000);
	CHECK_EQ(Speed::unlimited_frames(14000, 2000, 3000, false), 7);
	CHECK_EQ(Speed::unlimited_frames(14000, 0, 0, true), 1 + 10000 / 4000);        /* no clock: a guess */
	CHECK_EQ(Speed::unlimited_frames(14000, 20000, 20000, true), 1);               /* a game too heavy for it: still a frame */
	CHECK_EQ(Speed::unlimited_frames(14000, 100, 100, true), (int) Speed::kUnlimitedMax);
}

TEST(ff_unlimited_runs_more_than_3x_draws_one_picture_and_keeps_the_pacer_fed)
{
	for (int kind = 1; kind <= 2; kind++) {
		ClockHost h;
		Speed sp;
		sp.set_host(&h);
		SpeedSettings set;
		set.ff_unlimited = kind;
		set.rewind_on = false;
		sp.start(60.0, 2, set);
		sp.input(FIRE_FF_HELD | FIRE_FF_PRESS);
		CHECK(sp.ff_on());
		unsigned refreshes = 0, drawn_refreshes = 0, audio_out = 0;
		for (int r = 0; r < 120; r++) {
			const int n = sp.plan(1);
			CHECK(n >= 1);
			if (r > 6) CHECK(n > 3);                          /* the 2 ms frame leaves room for a good deal more than 3x */
			unsigned drawn_before = h.drawn;
			for (int i = 0; i < n; i++) {
				const bool render = sp.draw_this(i, n);
				sp.run_frame(render);
				int16_t lr[2 * 800];
				for (int k = 0; k < 2 * 800; k++) lr[k] = 1000;
				const size_t kept = sp.audio(lr, 800);
				audio_out += (unsigned) kept;
				for (size_t k = 0; k < kept * 2; k++) CHECK_EQ((int) lr[k], 0);       /* silent */
			}
			CHECK(h.drawn - drawn_before <= 1);               /* at most the last frame of the refresh is drawn */
			if (h.drawn != drawn_before) drawn_refreshes++;
			refreshes++;
		}
		CHECK_EQ(audio_out, 800u * refreshes);                    /* one frame's worth of silence per refresh, whatever the count */
		if (kind == 2) CHECK_EQ(drawn_refreshes, refreshes);     /* Smooth: a picture every refresh */
		else CHECK_EQ(drawn_refreshes, refreshes / 2);           /* Unlimited: every second one */
		CHECK_STR(sp.chip(), kind == 2 ? ">> Unlimited Smooth" : ">> Unlimited");
		sp.input(0);
		CHECK(!sp.ff_on());
		CHECK_EQ(sp.plan(1), 1);
	}
}

TEST(ff_unlimited_follows_a_heavier_scene)
{
	ClockHost h;
	Speed sp;
	sp.set_host(&h);
	SpeedSettings set;
	set.ff_unlimited = 2;
	set.rewind_on = false;
	sp.start(60.0, 2, set);
	sp.input(FIRE_FF_HELD | FIRE_FF_PRESS);
	int light = 0, heavy = 0;
	for (int r = 0; r < 80; r++) {
		if (r == 40) { h.plain_us = 6000; h.drawn_us = 7000; }
		const int n = sp.plan(1);
		for (int i = 0; i < n; i++) sp.run_frame(sp.draw_this(i, n));
		if (r == 39) light = n;
		if (r == 79) heavy = n;
	}
	CHECK(light >= 5 && heavy >= 2 && heavy < light);
}

/* a release in the middle of a refresh (the player's input() runs after plan() fixed the count): that refresh finishes as it began */
TEST(ff_unlimited_release_mid_refresh_does_not_dump_audio_or_pictures)
{
	for (int kind = 1; kind <= 2; kind++) {
		ClockHost h;
		Speed sp;
		sp.set_host(&h);
		SpeedSettings set;
		set.ff_unlimited = kind;
		set.rewind_on = false;
		sp.start(60.0, 2, set);
		sp.input(FIRE_FF_HELD | FIRE_FF_PRESS);
		for (int r = 0; r < 30; r++) { const int n = sp.plan(1); for (int i = 0; i < n; i++) sp.run_frame(sp.draw_this(i, n)); }
		const int n = sp.plan(1);
		CHECK(n > 3);
		const unsigned drawn0 = h.drawn;
		unsigned kept_total = 0;
		for (int i = 0; i < n; i++) {
			if (i == 1) sp.input(0);                          /* released while the refresh runs: input() is per emulated frame */
			sp.run_frame(sp.draw_this(i, n));
			int16_t lr[2 * 800];
			for (int k = 0; k < 2 * 800; k++) lr[k] = 1000;
			const size_t kept = sp.audio(lr, 800);
			kept_total += (unsigned) kept;
			for (size_t k = 0; k < kept * 2; k++) CHECK_EQ((int) lr[k], 0);   /* still silent */
		}
		CHECK(!sp.ff_on());
		CHECK_EQ(kept_total, 800u);                              /* one frame's worth, not n * 800 */
		CHECK(h.drawn - drawn0 <= 1);                            /* not a picture for each frame */
		CHECK_EQ(sp.plan(1), 1);                                 /* the next refresh is plain */
		int16_t lr[2 * 800];
		for (int k = 0; k < 2 * 800; k++) lr[k] = 1000;
		CHECK_EQ((int) sp.audio(lr, 800), 800);
		CHECK_EQ((int) lr[0], 1000);
	}
}

TEST(ff_2x_release_mid_refresh_keeps_the_refresh_whole)
{
	ClockHost h;
	Speed sp;
	sp.set_host(&h);
	SpeedSettings set;
	set.rewind_on = false;
	set.ff_sound = 1;
	sp.start(60.0, 2, set);
	sp.input(FIRE_FF_HELD | FIRE_FF_PRESS);
	const int n = sp.plan(1);
	CHECK_EQ(n, 2);
	size_t kept = 0;
	for (int i = 0; i < n; i++) {
		if (i == 1) sp.input(0);
		CHECK_EQ(sp.draw_this(i, n), i == n - 1);
		sp.run_frame(sp.draw_this(i, n));
		int16_t lr[2 * 800];
		for (int k = 0; k < 2 * 800; k++) lr[k] = 500;
		kept += sp.audio(lr, 800);
	}
	CHECK_EQ((int) kept, 800);                                   /* two frames' audio mixed to one frame's worth */
	CHECK_EQ(sp.plan(1), 1);
	CHECK(sp.draw_this(0, 1));
}

TEST(ff_unlimited_budget_takes_other_work_off_and_never_drops_below_2x)
{
	ClockHost h;
	Speed sp;
	sp.set_host(&h);
	SpeedSettings set;
	set.ff_unlimited = 2;
	set.rewind_on = false;
	sp.start(60.0, 2, set);
	CHECK_EQ(sp.unlimited_budget_us(), 14013u);                  /* 84 % of the 60 Hz refresh */
	sp.set_refresh_us(20000);
	CHECK_EQ(sp.unlimited_budget_us(), 16800u);                  /* the real period, not a fixed 14 ms */
	sp.set_refresh_us(0);
	sp.input(FIRE_FF_HELD | FIRE_FF_PRESS);
	int light = 0, loaded = 0;
	for (int r = 0; r < 40; r++) {
		const int n = sp.plan(1);
		for (int i = 0; i < n; i++) sp.run_frame(sp.draw_this(i, n));
		sp.note_other_us(r < 20 ? 0u : 6000u);                   /* the main thread's scaler and upload after the frames */
		if (r == 19) light = n;
		if (r == 39) loaded = n;
	}
	CHECK(sp.other_us() > 4000u);
	CHECK(loaded < light);
	/* a game too heavy for two frames (and a loaded main thread) still gets 2x, never 1x */
	h.plain_us = 12000; h.drawn_us = 13000;
	for (int r = 0; r < 40; r++) {
		const int n = sp.plan(1);
		CHECK(n >= 2);
		for (int i = 0; i < n; i++) sp.run_frame(sp.draw_this(i, n));
		sp.note_other_us(9000u);
	}
	CHECK_EQ(sp.plan(1), 2);
	/* a PAL refresh that runs nothing stays nothing */
	CHECK_EQ(sp.plan(0), 0);
}
