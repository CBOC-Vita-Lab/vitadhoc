/* See pk_pace.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_pace.h"
#include <string.h>

namespace pk {

static int span_ms(const PaceConfig &c, const PaceSample &s)
{
	return (int) ((uint64_t) c.budget_us * (s.span ? s.span : 1) / 1000);
}

PaceMode pace_decide(const PaceConfig &c, PaceMode current, const PaceSample *samples, int n, int ms_in_mode)
{
	if (!samples || n <= 0 || c.budget_us == 0) return current;
	if (current == PACE_FULL) {
		/* the last window: how many drawn frames missed their refresh, and the average */
		int ms = 0, cnt = 0, over = 0;
		uint64_t sum = 0;
		for (int i = n - 1; i >= 0 && ms < c.window_ms; i--) {
			const PaceSample &s = samples[i];
			ms += span_ms(c, s);
			cnt++;
			sum += s.cost_us;
			if (s.cost_us > c.budget_us) over++;
		}
		if (cnt < c.min_samples || ms * 10 < c.window_ms * 9) return PACE_FULL;   /* judge a whole second, not its start */
		bool many_over = over * 100 >= c.enter_over_pct * cnt;
		bool avg_high = sum * 100 >= (uint64_t) c.enter_avg_pct * c.budget_us * cnt;
		return (many_over || avg_high) ? PACE_HALF : PACE_FULL;
	}
	/* half: back to every frame only after a long enough stretch in which the drawn frames (what a full-rate
	 * frame costs) all had real headroom */
	if (ms_in_mode < c.min_half_ms || ms_in_mode < c.recover_ms) return PACE_HALF;
	int ms = 0, slips = 0;
	const uint64_t limit = (uint64_t) c.budget_us * c.recover_pct / 100;
	for (int i = n - 1; i >= 0 && ms < c.recover_ms; i--) {
		const PaceSample &s = samples[i];
		ms += span_ms(c, s);
		if (s.cost_us > limit && ++slips > c.recover_slips) return PACE_HALF;
	}
	if (ms < c.recover_ms) return PACE_HALF;      /* not enough history yet */
	return PACE_FULL;
}

void Pacer::reset(PaceMode m)
{
	head_ = count_ = 0;
	mode_ = m;
	ms_in_mode_ = 0;
	memset(&counts_, 0, sizeof counts_);
	memset(hist_, 0, sizeof hist_);
}

PaceMode Pacer::add(uint32_t cost_us, bool *changed)
{
	if (changed) *changed = false;
	PaceSample s;
	s.cost_us = cost_us;
	s.span = (uint8_t) (mode_ == PACE_HALF ? 2 : 1);
	hist_[head_] = s;
	head_ = (head_ + 1) % HISTORY;
	if (count_ < HISTORY) count_++;
	ms_in_mode_ += span_ms(cfg_, s);
	counts_.drawn++;
	if (cost_us > cfg_.budget_us) counts_.over++;
	if (cost_us > counts_.max_us) counts_.max_us = cost_us;
	if (!enabled_) return mode_ = PACE_FULL;
	/* oldest first, as pace_decide wants it */
	PaceSample lin[HISTORY];
	int start = (head_ - count_ + HISTORY) % HISTORY;
	for (int i = 0; i < count_; i++) lin[i] = hist_[(start + i) % HISTORY];
	PaceMode m = pace_decide(cfg_, mode_, lin, count_, ms_in_mode_);
	if (m != mode_) {
		mode_ = m;
		ms_in_mode_ = 0;
		/* the samples so far were measured in the other mode: a fresh history for the new one */
		head_ = count_ = 0;
		counts_.changes++;
		if (changed) *changed = true;
	}
	return mode_;
}

Pacer::Counts Pacer::take_counts()
{
	Counts c = counts_;
	memset(&counts_, 0, sizeof counts_);
	return c;
}

const char *pace_mode_name(PaceMode m) { return m == PACE_HALF ? "half" : "full"; }

}  // namespace pk
