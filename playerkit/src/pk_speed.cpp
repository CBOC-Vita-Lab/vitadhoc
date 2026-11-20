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
	act_ = rw_.refresh(want_rw_, locked_, why_);
	std::string n = rw_.take_notice();
	if (!n.empty()) notice_ = n;
	if (!locked_ && ff_.on() && rw_.active()) ff_.reset();      /* rewinding wins */
	const bool b = busy();
	if (was_busy_ && !b) ended_ = true;
	was_busy_ = b;
	if (act_ != RW_PLAY) return 1;
	return ff_.frames(normal);
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
	host_->run_frame(render);
	if (uint8_t *b = rw_.capture_begin()) {
		if (host_->save_state(b, rw_.cur_size())) rw_.capture_commit(rw_.cur_size());
	}
	return true;
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
