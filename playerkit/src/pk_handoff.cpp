/* See pk_handoff.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_handoff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace pk {

/* ---- DepthGovernor ------------------------------------------------------------ */

void DepthGovernor::configure(const DepthConfig &c)
{
	cfg_ = c;
	if (cfg_.max_depth < 1) cfg_.max_depth = 1;
	if (cfg_.max_depth > HANDOFF_MAX_DEPTH) cfg_.max_depth = HANDOFF_MAX_DEPTH;
	if (cfg_.min_depth < 1) cfg_.min_depth = 1;
	if (cfg_.min_depth > cfg_.max_depth) cfg_.min_depth = cfg_.max_depth;
	if (cfg_.window < 4) cfg_.window = 4;
	if (cfg_.window > MAX_WINDOW) cfg_.window = MAX_WINDOW;
	if (cfg_.budget_us == 0) cfg_.budget_us = 16683;
	reset();
}

void DepthGovernor::reset()
{
	memset(ring_, 0, sizeof ring_);
	head_ = n_ = 0;
	sum_ = 0;
	depth_ = cfg_.max_depth;
	now_ms10_ = over_ms10_ = under_ms10_ = 0;
	last_shrink_ms10_ = last_restore_ms10_ = -1000000000;
	cool_cur_ms_ = cfg_.cool_ms;
	changes_ = 0;
}

int DepthGovernor::add(uint32_t cost_us, int span)
{
	if (span < 1) span = 1;
	if (cfg_.min_depth >= cfg_.max_depth) return depth_;
	/* ring of the last `window` costs */
	if (n_ == cfg_.window) sum_ -= ring_[head_];
	else n_++;
	ring_[head_] = cost_us;
	sum_ += cost_us;
	head_ = (head_ + 1) % cfg_.window;
	const int64_t step = (int64_t) cfg_.budget_us * span / 100;       /* tenths of a millisecond */
	now_ms10_ += step;
	if (n_ < cfg_.window) return depth_;
	const uint64_t mean = sum_ / (uint64_t) n_;
	const bool over = mean * 100 >= (uint64_t) cfg_.shrink_pct * cfg_.budget_us;
	const bool under = mean * 100 <= (uint64_t) cfg_.restore_pct * cfg_.budget_us;
	if (over) over_ms10_ += step; else over_ms10_ = 0;
	if (under) under_ms10_ += step; else under_ms10_ = 0;
	const int64_t hold = (int64_t) cfg_.hold_ms * 10;
	if (depth_ > cfg_.min_depth) {
		if (over_ms10_ >= hold) {
			/* a shrink soon after a restore: that try failed, wait longer before the next one */
			if (now_ms10_ - last_restore_ms10_ < (int64_t) cfg_.bounce_ms * 10) {
				cool_cur_ms_ *= 2;
				if (cool_cur_ms_ > cfg_.cool_max_ms) cool_cur_ms_ = cfg_.cool_max_ms;
			} else cool_cur_ms_ = cfg_.cool_ms;
			depth_ = cfg_.min_depth;
			last_shrink_ms10_ = now_ms10_;
			over_ms10_ = under_ms10_ = 0;
			changes_++;
		}
	} else if (depth_ < cfg_.max_depth) {
		if (under_ms10_ >= hold && now_ms10_ - last_shrink_ms10_ >= (int64_t) cool_cur_ms_ * 10) {
			depth_ = cfg_.max_depth;
			last_restore_ms10_ = now_ms10_;
			over_ms10_ = under_ms10_ = 0;
			changes_++;
		}
	}
	return depth_;
}

/* ---- Handoff -------------------------------------------------------------------- */

namespace {
enum { POOL = HANDOFF_MAX_DEPTH + 3, RING = HANDOFF_MAX_DEPTH + 2, COSTS = 16 };
}

struct Handoff::Impl {
	HandoffConfig cfg;
	HandoffSys *sys;
	SpinLock lk;
	void *gate_sem, *frame_sem, *idle_sem;
	HandoffFrame pool[POOL];
	uint8_t *mem;
	HandoffFrame *freel[POOL];
	int nfree;
	HandoffFrame *q[RING];
	int qhead, queued;
	HandoffFrame *drawing;      /* with the consumer (NULL = none) */
	HandoffFrame *shown;        /* the last picture released, kept for last_shown() */
	bool enabled, stop;
	bool half, force_drop;
	bool gate_waiting, cons_waiting, idle_waiting;
	DepthGovernor gov;
	int depth;
	uint32_t costs[COSTS];
	int cost_head, cost_n;
	/* counters (this window / since start) */
	uint32_t produced, shown_n, dropped, gate_waits, gate_timeouts;
	uint32_t w_produced, w_shown, w_dropped, w_gate_waits;
	bool w_half_seen;
	uint64_t emu_us, base_us, win_start_us;
	uint32_t depth_changes;
	uint32_t repeated_total;

	bool drop_mode() const { return half || force_drop; }
	/* The gate, lock held. In drop mode nothing waits: the oldest queued pictures make room (newest wins).
	 * Otherwise NULL while in-flight (queued + being drawn) has reached the depth. */
	HandoffFrame *acquire_locked(bool force = false)
	{
		if (!enabled || stop) return 0;
		if (force || drop_mode()) {
			int keep = depth - (drawing ? 1 : 0);
			if (keep < 1) keep = 1;
			while (queued >= keep) evict_oldest();
		} else if (queued + (drawing ? 1 : 0) >= depth) {
			return 0;
		}
		if (nfree == 0) return 0;      /* cannot happen with the pool's size; refuse rather than corrupt */
		HandoffFrame *f = freel[--nfree];
		f->w = f->h = f->pitch = 0; f->seq = 0; f->emu_us = 0; f->user = 0; f->span = 1;
		return f;
	}
	/* evict the oldest queued picture: its buffer goes back to the pool */
	void evict_oldest()
	{
		HandoffFrame *f = q[qhead];
		qhead = (qhead + 1) % RING;
		queued--;
		freel[nfree++] = f;
		dropped++;
		w_dropped++;
	}
};

Handoff::Handoff() : p_(0), inited_(false) {}

Handoff::~Handoff()
{
	if (!p_) return;
	shutdown();
	Impl &m = *p_;
	if (inited_) {
		m.sys->sem_destroy(m.gate_sem);
		m.sys->sem_destroy(m.frame_sem);
		m.sys->sem_destroy(m.idle_sem);
	}
	free(m.mem);
	delete p_;
	p_ = 0;
}

bool Handoff::init(const HandoffConfig &c, HandoffSys *sys)
{
	if (inited_ || !sys || c.frame_bytes == 0) return false;
	p_ = new Impl();
	Impl &m = *p_;
	m.cfg = c;
	m.sys = sys;
	m.mem = (uint8_t *) malloc((size_t) POOL * c.frame_bytes);
	m.gate_sem = sys->sem_create();
	m.frame_sem = sys->sem_create();
	m.idle_sem = sys->sem_create();
	if (!m.mem || !m.gate_sem || !m.frame_sem || !m.idle_sem) {
		if (m.gate_sem) sys->sem_destroy(m.gate_sem);
		if (m.frame_sem) sys->sem_destroy(m.frame_sem);
		if (m.idle_sem) sys->sem_destroy(m.idle_sem);
		free(m.mem);
		delete p_;
		p_ = 0;
		return false;
	}
	for (int i = 0; i < POOL; i++) {
		memset(&m.pool[i], 0, sizeof m.pool[i]);
		m.pool[i].data = m.mem + (size_t) i * c.frame_bytes;
		m.pool[i].cap = c.frame_bytes;
		m.freel[i] = &m.pool[i];
	}
	m.nfree = POOL;
	m.qhead = m.queued = 0;
	m.drawing = m.shown = 0;
	m.enabled = true; m.stop = false;
	m.half = m.force_drop = false;
	m.gate_waiting = m.cons_waiting = m.idle_waiting = false;
	m.cfg.depth.budget_us = c.refresh_us;
	m.gov.configure(m.cfg.depth);
	m.depth = m.cfg.adaptive ? m.gov.depth() : m.gov.config().max_depth;
	m.cost_head = m.cost_n = 0;
	m.produced = m.shown_n = m.dropped = m.gate_waits = m.gate_timeouts = 0;
	m.w_produced = m.w_shown = m.w_dropped = m.w_gate_waits = 0;
	m.w_half_seen = false;
	m.emu_us = 0; m.base_us = sys->now_us(); m.win_start_us = 0;
	m.depth_changes = 0;
	m.repeated_total = 0;
	inited_ = true;
	return true;
}

void Handoff::shutdown()
{
	if (!inited_) return;
	Impl &m = *p_;
	{ SpinGuard g(m.lk); m.stop = true; }
	/* everything that can be waiting is woken (the semaphores count, so a post is never lost); they stay
	 * allocated until the destructor, because a thread that is just leaving a wait still looks at one */
	for (int i = 0; i < 2; i++) {
		m.sys->sem_post(m.gate_sem);
		m.sys->sem_post(m.frame_sem);
		m.sys->sem_post(m.idle_sem);
	}
}

int Handoff::depth() const
{
	if (!inited_) return 0;
	SpinGuard g(p_->lk);
	return p_->depth;
}

/* ---- producer ---- */


HandoffFrame *Handoff::try_begin_write()
{
	if (!inited_) return 0;
	SpinGuard g(p_->lk);
	return p_->acquire_locked();
}

HandoffFrame *Handoff::begin_write()
{
	if (!inited_) return 0;
	Impl &m = *p_;
	const uint64_t t0 = m.sys->now_us();
	for (;;) {
		{
			SpinGuard g(m.lk);
			if (!m.enabled || m.stop) return 0;
			HandoffFrame *f = m.acquire_locked();
			if (f) return f;
			/* the flag is set under the same lock the check was made under: a release cannot slip between */
			m.gate_waiting = true;
			m.gate_waits++;
			m.w_gate_waits++;
		}
		m.sys->sem_wait(m.gate_sem, m.cfg.gate_timeout_us);
		if (m.sys->now_us() - t0 >= m.cfg.gate_timeout_us) {
			/* the drawing thread is not answering: do not hold the game up for it. Newest wins this once. */
			SpinGuard g(m.lk);
			m.gate_waiting = false;
			m.gate_timeouts++;
			if (!m.enabled || m.stop) return 0;
			return m.acquire_locked(true);
		}
	}
}

void Handoff::commit(HandoffFrame *f)
{
	if (!inited_ || !f) return;
	Impl &m = *p_;
	bool wake = false;
	{
		SpinGuard g(m.lk);
		f->span = (uint8_t) (m.half ? 2 : 1);
		f->made_us = m.sys->now_us();
		m.q[(m.qhead + m.queued) % RING] = f;
		m.queued++;
		m.produced++;
		m.w_produced++;
		if (m.half) m.w_half_seen = true;
		if (m.cons_waiting) { m.cons_waiting = false; wake = true; }
	}
	if (wake) m.sys->sem_post(m.frame_sem);
}

void Handoff::abandon(HandoffFrame *f)
{
	if (!inited_ || !f) return;
	Impl &m = *p_;
	SpinGuard g(m.lk);
	m.freel[m.nfree++] = f;
}

void Handoff::note_emulated(uint32_t n)
{
	if (!inited_) return;
	SpinGuard g(p_->lk);
	p_->emu_us += (uint64_t) n * p_->cfg.frame_us;
}

void Handoff::set_pace_mode(PaceMode mode)
{
	if (!inited_) return;
	SpinGuard g(p_->lk);
	p_->half = mode == PACE_HALF;
}

void Handoff::set_force_drop(bool on)
{
	if (!inited_) return;
	SpinGuard g(p_->lk);
	p_->force_drop = on;
}

bool Handoff::poll_cost(uint32_t *cost_us)
{
	if (!inited_) return false;
	Impl &m = *p_;
	SpinGuard g(m.lk);
	if (m.cost_n == 0) return false;
	*cost_us = m.costs[(m.cost_head + COSTS - m.cost_n) % COSTS];
	m.cost_n--;
	return true;
}

/* quiet = nothing queued and nothing in the drawing thread's hands */
bool Handoff::flush(uint32_t timeout_us)
{
	if (!inited_) return false;
	Impl &m = *p_;
	const uint64_t t0 = m.sys->now_us();
	for (;;) {
		{
			SpinGuard g(m.lk);
			if (m.stop) return false;
			if (m.queued == 0 && !m.drawing) return true;
			m.idle_waiting = true;
		}
		const uint64_t used = m.sys->now_us() - t0;
		if (timeout_us != 0xFFFFFFFFu && used >= timeout_us) {
			SpinGuard g(m.lk);
			m.idle_waiting = false;
			return false;
		}
		const uint32_t left = timeout_us == 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) (timeout_us - used);
		m.sys->sem_wait(m.idle_sem, left);
	}
}

bool Handoff::discard(uint32_t timeout_us)
{
	if (!inited_) return false;
	Impl &m = *p_;
	{
		SpinGuard g(m.lk);
		while (m.queued > 0) m.evict_oldest();
	}
	return flush(timeout_us);
}

void Handoff::set_enabled(bool on)
{
	if (!inited_) return;
	Impl &m = *p_;
	if (!on) {
		flush();
		SpinGuard g(m.lk);
		m.enabled = false;
	} else {
		SpinGuard g(m.lk);
		m.enabled = true;
	}
	/* a producer that was waiting at the gate must see the new state */
	m.sys->sem_post(m.gate_sem);
}

bool Handoff::enabled() const
{
	if (!inited_) return false;
	SpinGuard g(p_->lk);
	return p_->enabled && !p_->stop;
}

const HandoffFrame *Handoff::last_shown() const
{
	if (!inited_) return 0;
	SpinGuard g(p_->lk);
	return p_->shown;
}

void Handoff::time_baseline()
{
	if (!inited_) return;
	Impl &m = *p_;
	SpinGuard g(m.lk);
	m.emu_us = 0;
	m.base_us = m.sys->now_us();
}

/* ---- consumer ---- */

HandoffFrame *Handoff::try_take()
{
	if (!inited_) return 0;
	Impl &m = *p_;
	SpinGuard g(m.lk);
	if (m.stop || m.drawing || m.queued == 0) return 0;
	if (m.drop_mode()) while (m.queued > 1) m.evict_oldest();
	HandoffFrame *f = m.q[m.qhead];
	m.qhead = (m.qhead + 1) % RING;
	m.queued--;
	m.drawing = f;
	return f;
}

HandoffFrame *Handoff::take()
{
	if (!inited_) return 0;
	Impl &m = *p_;
	for (;;) {
		HandoffFrame *f = try_take();
		if (f) return f;
		bool wake_idle = false;
		{
			SpinGuard g(m.lk);
			if (m.stop || m.drawing) return 0;      /* (a host that forgot release() gets NULL, not a spin) */
			if (m.queued > 0) continue;       /* a commit slipped in between */
			m.cons_waiting = true;
			if (m.idle_waiting && !m.drawing) { m.idle_waiting = false; wake_idle = true; }
		}
		if (wake_idle) m.sys->sem_post(m.idle_sem);
		m.sys->sem_wait(m.frame_sem, 0xFFFFFFFFu);
	}
}

void Handoff::release(HandoffFrame *f, uint32_t draw_us)
{
	if (!inited_ || !f) return;
	Impl &m = *p_;
	bool wake_gate = false, wake_idle = false;
	{
		SpinGuard g(m.lk);
		if (m.drawing == f) m.drawing = 0;
		if (m.shown) m.freel[m.nfree++] = m.shown;
		m.shown = f;
		m.shown_n++;
		m.w_shown++;
		const uint32_t cost = f->emu_us > draw_us ? f->emu_us : draw_us;
		m.costs[m.cost_head] = cost;
		m.cost_head = (m.cost_head + 1) % COSTS;
		if (m.cost_n < COSTS) m.cost_n++;
		if (m.cfg.adaptive) {
			const int d = m.gov.add(cost, f->span);
			if (d != m.depth) { m.depth = d; m.depth_changes++; }
		}
		if (m.gate_waiting) { m.gate_waiting = false; wake_gate = true; }
		if (m.idle_waiting && m.queued == 0) { m.idle_waiting = false; wake_idle = true; }
	}
	if (wake_gate) m.sys->sem_post(m.gate_sem);
	if (wake_idle) m.sys->sem_post(m.idle_sem);
}

/* ---- census ---- */

HandoffCensus Handoff::census() const
{
	HandoffCensus c;
	memset(&c, 0, sizeof c);
	if (!inited_) return c;
	Impl &m = *p_;
	SpinGuard g(m.lk);
	c.produced = m.produced; c.shown = m.shown_n; c.dropped = m.dropped;
	c.repeated = m.repeated_total; c.gate_waits = m.gate_waits; c.gate_timeouts = m.gate_timeouts;
	c.depth = m.depth; c.depth_max = m.gov.config().max_depth;
	c.half = m.half; c.enabled = m.enabled && !m.stop;
	c.emu_ms = (int64_t) (m.emu_us / 1000);
	c.wall_ms = (int64_t) ((m.sys->now_us() - m.base_us) / 1000);
	c.depth_changes = m.depth_changes;
	return c;
}

bool Handoff::census_line(uint64_t now_us, char *buf, size_t n)
{
	if (!inited_ || !buf || n == 0) return false;
	Impl &m = *p_;
	SpinGuard g(m.lk);
	if (m.win_start_us == 0) { m.win_start_us = now_us; return false; }
	if (now_us - m.win_start_us < 1000000) return false;
	const uint64_t win = now_us - m.win_start_us;
	m.win_start_us = now_us;
	/* pictures a steady 60 would have shown in this window and did not: repeats. Only for a window that was
	 * entirely in full mode (in half mode every second refresh repeats by design). */
	uint32_t rep = 0;
	if (!m.w_half_seen && !m.half && !m.force_drop && m.enabled) {
		const uint32_t due = (uint32_t) (win / m.cfg.refresh_us);
		rep = due > m.w_shown ? due - m.w_shown : 0;
	}
	m.repeated_total += rep;
	const int64_t emu_ms = (int64_t) (m.emu_us / 1000);
	const int64_t wall_ms = (int64_t) ((now_us - m.base_us) / 1000);
	snprintf(buf, n, "handoff: on=%d depth=%d/%d mode=%s produced=%u shown=%u repeated=%u dropped=%u gate_waits=%u timeouts=%u "
	    "emu_ms=%lld wall_ms=%lld drift_ms=%+lld%s",
	    m.enabled && !m.stop ? 1 : 0, m.depth, m.gov.config().max_depth,
	    m.force_drop ? "drop" : (m.half ? "half" : "full"),
	    m.w_produced, m.w_shown, rep, m.w_dropped, m.w_gate_waits, m.gate_timeouts,
	    (long long) emu_ms, (long long) wall_ms, (long long) (emu_ms - wall_ms),
	    m.force_drop ? " (fast-forward or rewind: drift not meaningful)" : "");
	m.w_produced = m.w_shown = m.w_dropped = m.w_gate_waits = 0;
	m.w_half_seen = false;
	return true;
}

}  // namespace pk
