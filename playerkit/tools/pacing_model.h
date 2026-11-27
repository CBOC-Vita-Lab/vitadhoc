/*
 * A model of how a finished emulated picture reaches the screen, in virtual time (no threads, no clock),
 * for two designs:
 *   (a) one thread does everything per pass: read pad, emulate, filter / compose / draw, present (today's
 *       SNES player and main app);
 *   (b) frame hand-over: the emulating thread passes the picture to a drawing thread through a mailbox of
 *       `depth` pictures in flight (pk_handoff).
 * The display refreshes at a fixed rate and holds a flip queue of two pictures; a present waits when it is full.
 * Steady pacing is the real pk::Pacer, and the adaptive depth is the real pk::DepthGovernor, so the model and
 * the library cannot drift apart. Every number it produces is a MODEL number, not a measurement on a console.
 *
 * Header only, so the model tool and the agreement test share one engine.
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_PACING_MODEL_H
#define PK_PACING_MODEL_H

#include "pk_pace.h"
#include "pk_handoff.h"
#include <stdint.h>
#include <math.h>
#include <deque>
#include <vector>
#include <algorithm>

namespace pkmodel {

struct Params {
	double R = 16683.0;          /* one refresh, us */
	double E = 0.30;             /* emulating one picture's frame, in refreshes */
	double E_skip = 0.70;        /* a frame that is not rendered costs this share of E */
	double D = 1.00;             /* filter / compose / draw of one picture, in refreshes */
	double copy = 0.02;          /* handing the picture over (copy into the mailbox), in refreshes */
	double wobble = 0.08;        /* each cost varies uniformly by +-this share */
	double spike_p = 0.01;       /* a rare slow picture ... */
	double spike = 0.35;         /* ... costs this much more (share) */
	double seconds = 60;
	double warmup = 2;           /* seconds not measured */
	int flipq = 2;               /* pictures the display can hold waiting */
	bool handoff = false;
	int depth = 3;               /* pictures in flight (handoff); the full depth when adaptive */
	bool adaptive = false;
	pk::DepthConfig gov;
	bool steady = true;          /* pk::Pacer on */
	int pace_over_pct = 10;      /* the Pacer's enter_over_pct (10 = as shipped) */
	int pace_avg_pct = 97;       /* ... and enter_avg_pct */
	double bias = 1.0;           /* the cost the controllers (pacer, governor) are TOLD is the true cost times this */
	unsigned seed = 1;
	/* D changes over time: (from second, D in refreshes) */
	std::vector<std::pair<double, double> > D_profile;
};

struct Result {
	double fps = 0;              /* distinct pictures first shown per second */
	double repeats_per_s = 0;    /* refreshes on which the picture on screen did not change */
	double uneven_per_s = 0;     /* picture changes whose gap differs from the previous gap (judder) */
	double speed_pct = 0;        /* emulated game time against wall time */
	double delay_mean_ms = 0;    /* a button press to the first picture that shows it (pad read once per emulated frame) */
	double delay_p95_ms = 0;
	double half_pct = 0;         /* share of the time steady pacing was in half mode */
	double depth_avg = 0;
	double drops_per_s = 0;
	int mode_changes = 0, depth_changes = 0;
	long shown = 0, emulated = 0, pictures = 0;
	double seconds = 0;
};

static inline uint64_t mix(uint64_t x)
{
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}
static inline double unit(uint64_t x) { return (double) (mix(x) >> 11) / 9007199254740992.0; }


/* The mailbox between the emulating thread and the drawing thread, as the simulation uses it. ModelBox is the
 * model's own arithmetic; the agreement test plugs in a box backed by pk::Handoff and expects identical runs. */
struct Box {
	virtual ~Box() {}
	/* the gate: may a picture be built now? In drop mode it never says no (older queued pictures make room) */
	virtual bool acquire(bool drop) = 0;
	virtual void commit(uint32_t seq, uint32_t emu_us, bool drop) = 0;
	/* the drawing thread takes the next picture (the newest in drop mode) */
	virtual bool take(bool drop, uint32_t *seq, uint32_t *emu_us, int *span) = 0;
	virtual void release(uint32_t draw_us) = 0;       /* drawn and handed to the display */
	virtual bool poll_cost(uint32_t *c) = 0;
	virtual int depth() = 0;
	virtual long dropped() = 0;
	virtual int depth_changes() = 0;
};

struct ModelBox : Box {
	struct Pic { uint32_t seq, emu_us; int span; };
	std::deque<Pic> q;
	std::deque<uint32_t> costs;
	pk::DepthGovernor gov;
	int depth_;
	bool adaptive_, busy_;
	long dropped_;
	Pic cur;
	ModelBox(int depth, bool adaptive, const pk::DepthConfig &g, uint32_t budget_us) : depth_(depth), adaptive_(adaptive), busy_(false), dropped_(0)
	{
		pk::DepthConfig dc = g;
		dc.max_depth = depth; dc.budget_us = budget_us;
		gov.configure(dc);
		depth_ = adaptive ? gov.depth() : depth;
		cur.seq = cur.emu_us = 0; cur.span = 1;
	}
	bool acquire(bool drop)
	{
		int inflight = (int) q.size() + (busy_ ? 1 : 0);
		if (drop) {
			int keep = depth_ - (busy_ ? 1 : 0);
			if (keep < 1) keep = 1;
			while ((int) q.size() >= keep) { q.pop_front(); dropped_++; }
			return true;
		}
		return inflight < depth_;
	}
	void commit(uint32_t seq, uint32_t emu_us, bool drop) { Pic p; p.seq = seq; p.emu_us = emu_us; p.span = drop ? 2 : 1; q.push_back(p); }
	bool take(bool drop, uint32_t *seq, uint32_t *emu_us, int *span)
	{
		if (busy_ || q.empty()) return false;
		if (drop) while (q.size() > 1) { q.pop_front(); dropped_++; }
		cur = q.front(); q.pop_front();
		busy_ = true;
		*seq = cur.seq; *emu_us = cur.emu_us; *span = cur.span;
		return true;
	}
	void release(uint32_t draw_us)
	{
		busy_ = false;
		uint32_t cost = cur.emu_us > draw_us ? cur.emu_us : draw_us;
		costs.push_back(cost);
		if (costs.size() > 16) costs.pop_front();
		if (adaptive_) depth_ = gov.add(cost, cur.span);
	}
	bool poll_cost(uint32_t *c) { if (costs.empty()) return false; *c = costs.front(); costs.pop_front(); return true; }
	int depth() { return depth_; }
	long dropped() { return dropped_; }
	int depth_changes() { return gov.changes(); }
};

class Sim {
public:
	explicit Sim(const Params &p) : P(p) {}
	Result run(Box *box = 0);   /* box: a mailbox to use instead of the model's own (hand-over design only) */

	/* what the run did, for tests */
	struct Shown { uint32_t seq; double t; };
	std::vector<Shown> shown_list;
	std::vector<double> pad_t;       /* pad read time per emulated frame */
	long dropped_total = 0;

private:
	Params P;
	double noise(uint64_t idx, uint64_t salt) const
	{
		double n = 1.0 + P.wobble * (2.0 * unit(idx * 2 + salt + (uint64_t) P.seed * 7919u) - 1.0);
		if (unit(idx * 2 + salt + 1000003u + (uint64_t) P.seed * 104729u) < P.spike_p) n += P.spike;
		return n;
	}
	double dcost(double t_us) const
	{
		double d = P.D;
		for (size_t i = 0; i < P.D_profile.size(); i++)
			if (t_us / 1e6 >= P.D_profile[i].first) d = P.D_profile[i].second;
		return d;
	}
};

inline Result Sim::run(Box *ext)
{
	const double R = P.R, END = P.seconds * 1e6;
	const double INF = 1e30;
	pk::Pacer pacer;
	{ pk::PaceConfig pc; pc.budget_us = (uint32_t) R; pc.enter_over_pct = P.pace_over_pct; pc.enter_avg_pct = P.pace_avg_pct; pacer.configure(pc); pacer.set_enabled(P.steady); }
	ModelBox own(P.depth, P.adaptive, P.gov, (uint32_t) R);
	Box *box = ext ? ext : &own;

	/* display */
	std::deque<std::pair<uint32_t, uint64_t> > pending;      /* (seq, picture id) waiting for a refresh */
	long next_vb = 1;                                         /* next refresh index; its time is next_vb * R */
	long vblanks_measured = 0, changes = 0;
	double last_change_t = -1, prev_gap = -1;
	long uneven = 0;
	long pic_counter = 0;

	/* the one thread of design (a) / the producer of (b) */
	enum { P_START, P_EMU, P_DRAW, P_SWAP, P_GATE, P_COPY } ps = P_START;
	double p_wake = 0;
	long last_vc = 0;
	uint32_t emu_count = 0;
	double pass_t0 = 0, d0 = 0;
	int to_run = 1;
	bool half_pass = false;
	double render_emu_us = 0;
	bool rendered_in_pass = false;
	uint32_t rendered_seq = 0;

	/* consumer of (b) */
	enum { C_IDLE, C_DRAW, C_SWAP } cs = C_IDLE;
	double c_wake = INF;
	uint32_t c_seq = 0, c_emu = 0;
	int c_span = 1;
	uint32_t c_draw_us = 0;

	double half_time = 0;
	double depth_time = 0, last_depth_t = 0;
	pad_t.clear();
	shown_list.clear();
	double wall_start = P.warmup * 1e6;
	long first_emu = -1;

	double t = 0;
	while (t < END) {
		/* the next moment anything can happen */
		double vb_t = next_vb * R;
		double next = vb_t;
		if (ps != P_GATE && ps != P_SWAP) next = std::min(next, p_wake);
		if (P.handoff && cs == C_DRAW) next = std::min(next, c_wake);
		if (next > END) break;
		t = next;

		if (vb_t <= t) {
			/* a refresh: the display takes the oldest waiting picture */
			if (!pending.empty()) {
				std::pair<uint32_t, uint64_t> pp = pending.front();
				pending.pop_front();
				if (t >= wall_start) {
					changes++;
					if (last_change_t >= 0) {
						double gap = t - last_change_t;
						if (prev_gap >= 0 && fabs(gap - prev_gap) > R * 0.5) uneven++;
						prev_gap = gap;
					}
					last_change_t = t;
				}
				Shown s; s.seq = pp.first; s.t = t;
				shown_list.push_back(s);
			}
			if (t >= wall_start) { vblanks_measured++; if (pacer.mode() == pk::PACE_HALF) half_time += R; }
			next_vb++;
		}
		if (t >= wall_start) depth_time += (t - last_depth_t) * (P.handoff ? box->depth() : 1);
		last_depth_t = t;

		/* --- the consumer (b): takes, draws, hands to the display, releases --- */
		if (P.handoff) {
			bool again = true;
			while (again) {
				again = false;
				if (cs == C_IDLE && box->take(pacer.mode() == pk::PACE_HALF, &c_seq, &c_emu, &c_span)) {
					c_draw_us = (uint32_t) (R * dcost(t) * noise((uint64_t) c_seq, 1));
					c_wake = t + c_draw_us;
					cs = C_DRAW;
					again = true;
				}
				if (cs == C_DRAW && c_wake <= t) { cs = C_SWAP; again = true; }
				if (cs == C_SWAP && (int) pending.size() < P.flipq) {
					pending.push_back(std::make_pair(c_seq, (uint64_t) ++pic_counter));
					box->release(c_draw_us);
					cs = C_IDLE;
					c_wake = INF;
					again = true;
				}
			}
		}

		/* --- the one thread / the producer --- */
		bool again = true;
		while (again) {
			again = false;
			switch (ps) {
			case P_START: {
				long vc = (long) floor(t / R);
				if (vc == last_vc) { p_wake = (last_vc + 1) * R; break; }
				int elapsed = (int) std::min<long>(2, vc - last_vc);
				last_vc = vc;
				pass_t0 = t;
				/* the pacer is fed the finished pictures' costs on this thread */
				if (P.handoff) {
					uint32_t c;
					while (box->poll_cost(&c)) pacer.add((uint32_t) (c * P.bias));
				}
				half_pass = pacer.mode() == pk::PACE_HALF;
				to_run = P.handoff ? elapsed : (half_pass ? elapsed : 1);
				double total = 0;
				rendered_in_pass = false;
				render_emu_us = 0;
				for (int i = 0; i < to_run; i++) {
					uint32_t e = emu_count++;
					bool render;
					if (P.handoff) render = half_pass ? ((e & 1) == 0) : (i == to_run - 1);
					else render = !P.steady || !half_pass || ((e & 1) == 0);
					double c = R * P.E * noise(e, 0) * (render ? 1.0 : P.E_skip);
					total += c;
					if (render) { rendered_in_pass = true; render_emu_us = c; rendered_seq = e; }
					pad_t.push_back(pass_t0);
					if (first_emu < 0 && t >= wall_start) first_emu = (long) e;
				}
				p_wake = t + total;
				ps = P_EMU;
				break;
			}
			case P_EMU:
				if (p_wake > t) break;
				if (P.handoff) {
					if (!rendered_in_pass) { ps = P_START; again = true; break; }
					ps = P_GATE; again = true;
				} else {
					if (half_pass && !rendered_in_pass) { ps = P_START; again = true; break; }
					d0 = t;
					p_wake = t + R * dcost(t) * noise((uint64_t) rendered_seq, 1);
					ps = P_DRAW;
				}
				break;
			case P_DRAW:
				if (p_wake > t) break;
				ps = P_SWAP; again = true;
				break;
			case P_SWAP:
				if ((int) pending.size() < P.flipq) {
					pending.push_back(std::make_pair(rendered_seq, (uint64_t) ++pic_counter));
					/* the cost for the pacer: the rendered frame's emulation plus the show (draw and the wait in present) */
					pacer.add((uint32_t) ((render_emu_us + (t - d0)) * P.bias));
					ps = P_START; again = true;
				}
				break;
			case P_GATE:
				if (!box->acquire(pacer.mode() == pk::PACE_HALF)) break;     /* wait for a release */
				p_wake = t + R * P.copy;
				ps = P_COPY; again = true;
				break;
			case P_COPY:
				if (p_wake > t) break;
				box->commit(rendered_seq, (uint32_t) (render_emu_us + R * P.copy), pacer.mode() == pk::PACE_HALF);
				ps = P_START; again = true;
				break;
			}
		}
		/* a consumer that was idle starts as soon as something was queued (same instant) */
		if (P.handoff && cs == C_IDLE && box->take(pacer.mode() == pk::PACE_HALF, &c_seq, &c_emu, &c_span)) {
			c_draw_us = (uint32_t) (R * dcost(t) * noise((uint64_t) c_seq, 1));
			c_wake = t + c_draw_us;
			cs = C_DRAW;
		}
	}

	/* ---- results ---- */
	Result r;
	const double secs = (END - wall_start) / 1e6;
	r.seconds = secs;
	long shown_n = 0;
	for (size_t i = 0; i < shown_list.size(); i++) if (shown_list[i].t >= wall_start) shown_n++;
	r.shown = shown_n;
	r.fps = shown_n / secs;
	r.repeats_per_s = (vblanks_measured - changes) / secs;
	r.uneven_per_s = uneven / secs;
	long emu_measured = first_emu >= 0 ? (long) emu_count - first_emu : 0;
	r.emulated = emu_measured;
	r.speed_pct = 100.0 * emu_measured * R / (END - wall_start);
	r.half_pct = 100.0 * half_time / (END - wall_start);
	r.depth_avg = depth_time / (END - wall_start);
	r.pictures = pic_counter;
	r.drops_per_s = box->dropped() / (P.seconds);
	dropped_total = box->dropped();
	r.mode_changes = pacer.take_counts().changes;
	r.depth_changes = box->depth_changes();
	/* press-to-picture delay: for emulated frame e the press is somewhere in (pad_t[e-1], pad_t[e]]; its first
	 * picture is the first shown one with seq >= e */
	std::vector<double> d_hi;
	double sum = 0;
	size_t si = 0;
	for (size_t e = 1; e < pad_t.size(); e++) {
		if (pad_t[e] < wall_start) continue;
		while (si < shown_list.size() && shown_list[si].seq < e) si++;
		if (si >= shown_list.size()) break;
		double st = shown_list[si].t;
		double hi = st - pad_t[e - 1], lo = st - pad_t[e];
		sum += (hi + lo) / 2;
		d_hi.push_back(hi);
	}
	if (!d_hi.empty()) {
		r.delay_mean_ms = sum / d_hi.size() / 1000.0;
		std::sort(d_hi.begin(), d_hi.end());
		r.delay_p95_ms = d_hi[(size_t) (d_hi.size() * 0.95)] / 1000.0;
	}
	return r;
}

}  // namespace pkmodel

#endif
