/* Rewind: see pk_rewind.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_rewind.h"
#include "pk_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#elif !defined(_WIN32)
#include <sched.h>
#endif

namespace pk {

/* ---- the delta codec ---------------------------------------------------------------------------------------------------- */

size_t delta_bound(size_t n)
{
	/* worst case: every other byte differs, so a record per two bytes costs 2 header bytes: n + n / 2 + 16 is safe; with the
	 * fold-in rule below (gaps under 4 equal bytes stay in the literal) the true worst case is a record per 4 bytes at 2 bytes each */
	return n + n / 3 + 16;
}

static inline size_t put_leb(uint8_t *o, size_t v)
{
	size_t k = 0;
	while (v >= 0x80) { o[k++] = (uint8_t) (v | 0x80); v >>= 7; }
	o[k++] = (uint8_t) v;
	return k;
}

static inline bool get_leb(const uint8_t *&p, const uint8_t *e, size_t *v)
{
	size_t r = 0;
	unsigned sh = 0;
	for (;;) {
		if (p >= e || sh > 28) return false;
		uint8_t b = *p++;
		r |= (size_t) (b & 0x7F) << sh;
		if (!(b & 0x80)) break;
		sh += 7;
	}
	*v = r;
	return true;
}

static inline uint64_t ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

size_t delta_encode(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *out, size_t cap)
{
	size_t i = 0, o = 0;
	const size_t FOLD = 4;                 /* an equal run shorter than this stays inside the literal */
	for (;;) {
		/* skip equal bytes: eight at a time */
		size_t s = i;
		while (i + 8 <= n && ld64(a + i) == ld64(b + i)) i += 8;
		while (i < n && a[i] == b[i]) i++;
		if (i >= n) break;                 /* the tail of equal bytes is not written */
		const size_t skip = i - s;
		/* the literal: until FOLD equal bytes in a row (or the end) */
		size_t j = i + 1, last = i + 1;
		while (j < n) {
			if (a[j] != b[j]) { last = j + 1; j++; continue; }
			size_t k = j;
			while (k < n && k - j < FOLD && a[k] == b[k]) k++;
			if (k - j >= FOLD || k >= n) break;
			j = k;
		}
		const size_t cnt = last - i;
		if (o + 10 + cnt > cap) {
			/* the exact test: the two numbers are at most 5 bytes each */
			uint8_t h[10];
			size_t hl = put_leb(h, skip);
			hl += put_leb(h + hl, cnt);
			if (o + hl + cnt > cap) return (size_t) -1;
		}
		o += put_leb(out + o, skip);
		o += put_leb(out + o, cnt);
		for (size_t k = 0; k < cnt; k++) out[o + k] = (uint8_t) (a[i + k] ^ b[i + k]);
		o += cnt;
		i = last;
	}
	return o;
}

bool delta_apply(uint8_t *buf, size_t n, const uint8_t *d, size_t len)
{
	const uint8_t *p = d, *e = d + len;
	size_t pos = 0;
	while (p < e) {
		size_t skip, cnt;
		if (!get_leb(p, e, &skip) || !get_leb(p, e, &cnt)) return false;
		if (skip > n - pos) return false;
		pos += skip;
		if (cnt > n - pos || cnt > (size_t) (e - p)) return false;
		uint8_t *w = buf + pos;
		const uint8_t *r = p;
		size_t k = 0;
		for (; k + 8 <= cnt; k += 8) {
			uint64_t x, y;
			memcpy(&x, w + k, 8);
			memcpy(&y, r + k, 8);
			x ^= y;
			memcpy(w + k, &x, 8);
		}
		for (; k < cnt; k++) w[k] ^= r[k];
		p += cnt;
		pos += cnt;
	}
	return true;
}

/* ---- the ring ----------------------------------------------------------------------------------------------------------- */

Rewind::Rewind() : pend_state_(0) {}
Rewind::~Rewind() { close(); }

bool Rewind::open(const RewindConfig &c)
{
	close();
	nomem_ = false;
	if (c.state_bytes == 0 || c.seconds <= 0 || c.fps <= 1.0 || c.interval_min < 1) return false;
	cfg_ = c;
	if (cfg_.interval_max < cfg_.interval_min) cfg_.interval_max = cfg_.interval_min;
	size_t ents = (size_t) (cfg_.seconds * cfg_.fps / cfg_.interval_min) + 16;
	if (ents > 65536) ents = 65536;
	const size_t tmp_cap = delta_bound(cfg_.state_bytes);
	const size_t fixed = 2 * cfg_.state_bytes + tmp_cap + ents * sizeof(Ent);
	if (cfg_.budget_bytes <= fixed + (128u << 10)) { nomem_ = true; return false; }
	ring_cap_ = cfg_.budget_bytes - fixed;
	cur_ = (uint8_t *) malloc(cfg_.state_bytes);
	pend_ = (uint8_t *) malloc(cfg_.state_bytes);
	ring_ = (uint8_t *) malloc(ring_cap_);
	if (!cur_ || !pend_ || !ring_) { close(); nomem_ = true; return false; }
	ents_.assign(ents, Ent());
	tmp_.assign(tmp_cap, 0);
	open_ = true;
	st_ = RewindStats();
	st_.on = true;
	st_.seconds = (unsigned) cfg_.seconds;
	st_.ring_bytes = ring_cap_;
	st_.total_bytes = cfg_.budget_bytes;
	st_.state_bytes = cfg_.state_bytes;
	ema_pack_ = 0;
	interval_ = (unsigned) cfg_.interval_min;
	clear();
	return true;
}

void Rewind::close()
{
	if (open_) wait_idle();
	open_ = false;
	free(cur_); free(pend_); free(ring_);
	cur_ = pend_ = ring_ = 0;
	ring_cap_ = 0;
	ents_.clear();
	std::vector<Ent>().swap(ents_);
	std::vector<uint8_t>().swap(tmp_);
	first_ = count_ = 0;
	head_ = 0;
	frames_total_ = 0;
	have_cur_ = false;
	pend_state_.store(0);
	active_ = prev_want_ = false;
}

void Rewind::clear()
{
	if (!open_) return;
	wait_idle();
	first_ = count_ = 0;
	head_ = 0;
	frames_total_ = 0;
	have_cur_ = false;
	since_ = interval_;          /* the first state is due at once */
	pend_frames_ = 0;
	ema_pack_ = 0;
	interval_ = (unsigned) cfg_.interval_min;
	since_ = interval_;
	end_session(0);
	notice_.clear();
}

void Rewind::set_async(bool on, void (*wake)(void *), void *wake_arg)
{
	if (!on) {
		async_ = false;
		wake_ = 0;
		wake_arg_ = 0;
		while (pack_pending()) {}
		return;
	}
	wake_ = wake;
	wake_arg_ = wake_arg;
	async_ = true;
}

/* ---- capture ------------------------------------------------------------------------------------------------------------ */

uint8_t *Rewind::capture_begin()
{
	if (!open_ || active_) return 0;
	since_++;
	if (since_ < interval_) return 0;
	if (pend_state_.load(std::memory_order_acquire) != 0) { st_.skipped++; return 0; }
	return pend_;
}

void Rewind::capture_commit(size_t bytes)
{
	if (!open_ || bytes != cfg_.state_bytes) return;
	pend_frames_ = since_;
	since_ = 0;
	st_.captures++;
	pend_state_.store(1, std::memory_order_release);
	if (async_) { if (wake_) wake_(wake_arg_); }
	else pack_pending();
}

bool Rewind::pack_pending()
{
	if (!open_) return false;
	int expect = 1;
	if (!pend_state_.compare_exchange_strong(expect, 2, std::memory_order_acq_rel)) return false;
	const uint64_t t0 = clock_ ? clock_() : 0;
	const size_t n = cfg_.state_bytes;
	if (!have_cur_) {
		uint8_t *t = cur_; cur_ = pend_; pend_ = t;
		have_cur_ = true;
	} else {
		size_t len = delta_encode(cur_, pend_, n, tmp_.data(), tmp_.size());
		if (len != (size_t) -1) {
			ring_push(tmp_.data(), len, pend_frames_);
			st_.raw_bytes += n;
			st_.packed_bytes += len;
		} else {
			/* cannot happen with delta_bound; a state we cannot keep as a step is a break in the history */
			first_ = count_ = 0; head_ = 0; frames_total_ = 0;
		}
		uint8_t *t = cur_; cur_ = pend_; pend_ = t;
	}
	if (clock_) {
		const uint64_t us = clock_() - t0;
		st_.pack_us += us;
		if (us > st_.pack_max_us) st_.pack_max_us = (unsigned) (us > 0xFFFFFFFFull ? 0xFFFFFFFFull : us);
	}
	pend_state_.store(0, std::memory_order_release);
	return true;
}

void Rewind::wait_idle()
{
	if (!open_) return;
	for (;;) {
		int s = pend_state_.load(std::memory_order_acquire);
		if (s == 0) return;
		if (s == 1) { pack_pending(); continue; }
		/* being packed by the helper: yield / nap instead of busy-spinning the core */
#ifdef __vita__
		sceKernelDelayThread(100);
#elif !defined(_WIN32)
		sched_yield();
#else
		for (volatile int spin = 0; spin < 2000; spin = spin + 1) {}
#endif
	}
}

unsigned Rewind::entry_frames(unsigned i) const
{
	return i < count_ ? ents_[(first_ + i) % ents_.size()].frames : 0;
}

void Rewind::drop_oldest()
{
	if (!count_) return;
	frames_total_ -= ents_[first_].frames;
	first_ = (first_ + 1) % ents_.size();
	count_--;
	st_.dropped++;
	if (!count_) head_ = 0;
}

bool Rewind::ring_reserve(size_t len, size_t *off)
{
	if (len > ring_cap_) return false;
	for (;;) {
		if (!count_) { head_ = 0; *off = 0; return true; }
		const size_t tail = ents_[first_].off;
		const Ent &nw = ents_[(first_ + count_ - 1) % ents_.size()];
		const bool wrapped = count_ > 1 && nw.off < tail;
		if (!wrapped) {
			if (head_ + len <= ring_cap_) { *off = head_; return true; }
			if (len <= tail) { *off = 0; return true; }
		} else {
			if (head_ + len <= tail) { *off = head_; return true; }
		}
		drop_oldest();
	}
}

void Rewind::ring_push(const uint8_t *d, size_t len, unsigned frames)
{
	size_t off = 0;
	/* the index is full: the oldest step goes first */
	if (count_ == ents_.size()) drop_oldest();
	if (!ring_reserve(len, &off)) return;       /* a step bigger than the whole ring: the history restarts here */
	if (len) memcpy(ring_ + off, d, len);
	Ent &e = ents_[(first_ + count_) % ents_.size()];
	e.off = (uint32_t) off;
	e.len = (uint32_t) len;
	e.frames = frames;
	count_++;
	head_ = off + len;
	frames_total_ += frames;
	ema_pack_ = ema_pack_ == 0 ? (double) len : ema_pack_ * 0.9 + (double) len * 0.1;
	tune_interval();
}

void Rewind::pop_newest()
{
	if (!count_) return;
	const Ent e = ents_[(first_ + count_ - 1) % ents_.size()];
	count_--;
	frames_total_ -= e.frames;
	if (!count_) head_ = 0;
	else {
		const Ent &p = ents_[(first_ + count_ - 1) % ents_.size()];
		head_ = (size_t) p.off + p.len;
	}
}

void Rewind::tune_interval()
{
	/* entries needed for the history asked for, at the current interval, each about ema_pack_ bytes */
	const double frames_wanted = cfg_.seconds * cfg_.fps;
	double need = frames_wanted * ema_pack_ / (double) ring_cap_;      /* the interval at which the ring just holds it */
	unsigned t = (unsigned) (need + 0.999);
	if (t < (unsigned) cfg_.interval_min) t = (unsigned) cfg_.interval_min;
	if (t > (unsigned) cfg_.interval_max) t = (unsigned) cfg_.interval_max;
	if (t > interval_) interval_ = t;
	else if (t < interval_ && (st_.captures & 7) == 0) interval_--;
}

/* ---- the session -------------------------------------------------------------------------------------------------------- */

unsigned Rewind::speed() const
{
	return held_ < 60 ? 2 : held_ < 180 ? 4 : held_ < 360 ? 8 : 16;
}

void Rewind::end_session(const char *)
{
	active_ = false;
	first_land_ = at_oldest_ = told_oldest_ = false;
	held_ = credit_ = 0;
	tl_back_frames_ = 0;
}

bool Rewind::step_back()
{
	if (!count_) return false;
	const Ent &e = ents_[(first_ + count_ - 1) % ents_.size()];
	if (!delta_apply(cur_, cfg_.state_bytes, ring_ + e.off, e.len)) {
		/* a damaged step: the history before it cannot be trusted */
		first_ = count_ = 0; head_ = 0; frames_total_ = 0;
		return false;
	}
	tl_back_frames_ += e.frames;
	pop_newest();
	st_.steps_back++;
	return true;
}

RewindAct Rewind::refresh(bool want, bool locked, const char *lock_notice)
{
	const bool pressed = want && !prev_want_;
	prev_want_ = want;
	locked_ = locked;
	if (!open_) {
		if (pressed) notice_ = nomem_ ? tr("rewind.rewind_not_enough_memory_for_it") : tr("rewind.rewind_is_switched_off_settings");
		return RW_PLAY;
	}
	if (locked && active_) end_session("locked");
	if (!active_) {
		if (!pressed) return RW_PLAY;
		if (locked) { notice_ = lock_notice ? tr(lock_notice) : tr("rewind.rewind_is_off_while_games_are"); return RW_PLAY; }
		wait_idle();
		if (!have_cur_) { notice_ = tr("rewind.this_is_as_far_back_as"); return RW_PLAY; }
		active_ = first_land_ = true;
		at_oldest_ = told_oldest_ = false;
		held_ = credit_ = 0;
		tl_back_frames_ = since_;
		st_.rewinds++;
	} else if (!want) {
		end_session("released");
		return RW_PLAY;
	}
	bool landed = false;
	if (first_land_) {
		first_land_ = false;
		landed = true;
	} else {
		credit_ += speed();
		while (count_ && credit_ >= ents_[(first_ + count_ - 1) % ents_.size()].frames) {
			credit_ -= ents_[(first_ + count_ - 1) % ents_.size()].frames;
			if (!step_back()) break;
			landed = true;
		}
	}
	held_++;
	if (!count_) { credit_ = 0; at_oldest_ = true; }
	if (at_oldest_ && !told_oldest_) { told_oldest_ = true; notice_ = tr("rewind.this_is_as_far_back_as"); }
	if (landed) st_.landings++;
	return landed ? RW_LAND : RW_STAND;
}

void Rewind::landed()
{
	since_ = 1;
}

std::string Rewind::take_notice()
{
	std::string s;
	s.swap(notice_);
	return s;
}

unsigned Rewind::back_seconds() const
{
	return cfg_.fps > 0 ? (unsigned) (tl_back_frames_ / cfg_.fps + 0.5) : 0;
}

std::string Rewind::chip() const
{
	if (!active_) return std::string();
	char b[48];
	snprintf(b, sizeof b, tr(at_oldest_ ? "rewind.chip_oldest" : "rewind.chip"), back_seconds());
	return b;
}

RewindStats Rewind::stats() const
{
	RewindStats s = st_;
	s.interval = interval_;
	s.entries = count_;
	s.frames = frames_total_;
	s.used_bytes = 0;
	for (unsigned i = 0; i < count_; i++) s.used_bytes += ents_[(first_ + i) % ents_.size()].len;
	return s;
}

}  // namespace pk
