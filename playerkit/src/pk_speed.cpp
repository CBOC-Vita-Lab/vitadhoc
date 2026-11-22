/* Fast forward and rewind for a player: see pk_speed.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_speed.h"

#include <stdio.h>

namespace pk {

void Speed::start(double fps, int interval_min, const SpeedSettings &s)
{
	fps_ = fps;
	imin_ = interval_min;
	set_ = s;
	ff_.configure(s.ff_factor, s.ff_sound, s.ff_toggle);
	ff_.set_unlimited(s.ff_unlimited);
	rw_.close();
	if (s.rewind_on && host_ && host_->state_size()) {
		RewindConfig c;
		c.state_bytes = host_->state_size();
		c.seconds = s.rewind_seconds;
		c.budget_bytes = s.rewind_budget;
		c.fps = fps;
		c.interval_min = interval_min;
		rw_.open(c);        /* a refusal is told to the player at the first press */
	}
}

void Speed::apply(const SpeedSettings &s)
{
	const bool rebuild = s.rewind_on != set_.rewind_on || s.rewind_seconds != set_.rewind_seconds || s.rewind_budget != set_.rewind_budget;
	set_ = s;
	ff_.configure(s.ff_factor, s.ff_sound, s.ff_toggle);
	ff_.set_unlimited(s.ff_unlimited);
	if (rebuild) {
		/* the helper thread keeps its callback across this: close() waits for it to be idle */
		start(fps_, imin_, s);
	}
}

void Speed::stop()
{
	rw_.close();
}

void Speed::input(uint32_t fired)
{
	ff_.update((fired & FIRE_FF_HELD) != 0, (fired & FIRE_FF_PRESS) != 0, locked_);
	want_rw_ = (fired & FIRE_REWIND_HELD) != 0;
}

int Speed::plan(int normal)
{
	act_ = RW_PLAY;
	act_ = rw_.refresh(want_rw_, locked_ || rw_locked_, locked_ ? why_ : rw_why_);
	std::string n = rw_.take_notice();
	if (!n.empty()) notice_ = n;
	if (!locked_ && ff_.on() && rw_.active()) ff_.reset();      /* rewinding wins */
	ff_.latch();
	const bool b = busy();
	if (was_busy_ && !b) ended_ = true;
	was_busy_ = b;
	run_in_refresh_ = 0;
	if (act_ != RW_PLAY) return 1;
	if (ff_.on() && ff_.unlimited() && normal > 0) {
		/* never slower than 2x: Unlimited that fits fewer than two frames in a refresh would be slower than the plain choice */
		int n = unlimited_frames(unlimited_budget_us(), cost_plain_, cost_drawn_, ff_.next_refresh_drawn());
		ff_.begin_refresh(n < (int) kUnlimitedMin ? (int) kUnlimitedMin : n);
	}
	return ff_.frames(normal);
}

int Speed::unlimited_frames(uint32_t budget_us, uint32_t plain, uint32_t drawn, bool is_drawn)
{
	if (!drawn) drawn = plain ? plain : (uint32_t) kGuessUs;
	if (!plain) plain = drawn;
	if (plain < 200) plain = 200;
	if (drawn < plain) drawn = plain;
	int n;
	if (is_drawn) n = 1 + (budget_us > drawn ? (int) ((budget_us - drawn) / plain) : 0);
	else n = (int) (budget_us / plain);
	return n < 1 ? 1 : n > (int) kUnlimitedMax ? (int) kUnlimitedMax : n;
}

bool Speed::run_frame(bool render)
{
	if (!host_) return false;
	if (act_ == RW_LAND) {
		if (!host_->load_state(rw_.cur(), rw_.cur_size())) {
			rw_.clear();
			notice_ = "Rewind failed";
			act_ = RW_PLAY;
			return false;
		}
		host_->clear_pads();
		host_->run_frame(true);
		rw_.landed();
		return true;
	}
	if (act_ == RW_STAND) return false;
	const uint64_t t0 = host_->now_us();
	host_->run_frame(render);
	if (!defer_) {
		if (uint8_t *b = rw_.capture_begin()) {
			if (host_->save_state(b, rw_.cur_size())) rw_.capture_commit(rw_.cur_size());
		}
	} else pend_++;
	if (ff_.latched() && ff_.unlimited()) {
		/* the whole frame's work (a rewind capture included) is what a refresh has to hold */
		const uint64_t t1 = host_->now_us();
		if (t1 > t0) {
			const uint32_t us = (uint32_t) (t1 - t0 > 100000 ? 100000 : t1 - t0);
			uint32_t &avg = render ? cost_drawn_ : cost_plain_;
			avg = avg ? (avg * 7 + us) / 8 : us;
		}
		ff_.set_audio_frame(run_in_refresh_);
	}
	run_in_refresh_++;
	return true;
}

void Speed::capture_idle(bool late)
{
	if (pend_ <= 0) return;
	if (!host_ || act_ != RW_PLAY) { pend_ = 0; put_off_ = 0; return; }     /* rewinding: the frames that ran are not history */
	if (late && put_off_ < kPutOffMax) { put_off_++; put_off_total_++; return; }
	put_off_ = 0;
	/* one request per frame that ran, as run_frame() would have made them; at most one of them is due */
	for (; pend_ > 0; pend_--)
		if (uint8_t *b = rw_.capture_begin()) {
			if (host_->save_state(b, rw_.cur_size())) rw_.capture_commit(rw_.cur_size());
			pend_ = 0;
			break;
		}
}

size_t Speed::audio(int16_t *lr, size_t frames)
{
	if (rw_.active()) return 0;
	return ff_.reduce(lr, frames);
}

bool Speed::take_ended()
{
	bool e = ended_;
	ended_ = false;
	if (ff_.take_ended()) e = true;
	return e;
}

std::string Speed::chip() const
{
	if (rw_.active()) return rw_.chip();
	return ff_.chip();
}

std::string Speed::take_notice()
{
	std::string s;
	s.swap(notice_);
	return s;
}

void Speed::state_changed()
{
	rw_.clear();
	ff_.reset();
}

std::string Speed::stats_line() const
{
	const RewindStats s = rw_.stats();
	char b[200];
	snprintf(b, sizeof b, "rewind: %s steps=%u frames=%u interval=%u ring=%uK/%uK avg=%uB captures=%u skipped=%u dropped=%u pack_max=%uus",
	         rw_.is_open() ? "on" : "off", s.entries, s.frames, s.interval, (unsigned) (s.used_bytes >> 10), (unsigned) (s.ring_bytes >> 10),
	         s.entries ? (unsigned) (s.used_bytes / s.entries) : 0u, s.captures, s.skipped, s.dropped, s.pack_max_us);
	return b;
}

}  // namespace pk
