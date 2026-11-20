/* Unit tests: pk_rewind (the delta codec, the ring, the session) and pk_ff. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_rewind.h"
#include "pk_ff.h"

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

TEST(ff_without_sound_drops_it_and_normal_speed_is_untouched)
{
	FastForward ff;
	ff.configure(2, false, false);
	int16_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	CHECK_EQ(ff.reduce(b, 4), 4);              /* not fast forwarding: untouched */
	CHECK_EQ(b[0], 1);
	ff.update(true, true, false);
	CHECK_EQ(ff.reduce(b, 4), 0);
}
