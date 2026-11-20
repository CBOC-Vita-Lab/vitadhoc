/*
 * "Quick response": the decision when a player runs one frame ahead (run-ahead by one frame).
 *
 * Run-ahead shows, at every refresh, the picture of the frame AFTER the one the game is at: the player runs the real frame without
 * drawing it, saves the machine, runs the next frame with the same buttons, shows that picture and puts the machine back. A button
 * press is then on screen one frame sooner. The game, its sound and its saves are untouched; the price is a second frame of emulation
 * and a state save and load in every refresh. This class only decides whether that price fits; the frames themselves are the host's.
 *
 * Modes: OFF never, ON always (when nothing blocks it), AUTO while the emulating thread has the time for it:
 *  - it starts after `settle_frames` of plain frames whose projected run-ahead cost (plain cost x ratio + overhead; the ratio is
 *    learned in the first frames after each start) stays at or under `on_pct` of a frame;
 *  - it stops when the measured cost averages over `off_pct` of a frame, or `late_limit` frames within `late_window` took longer
 *    than a whole frame; after a stop it waits (`cool_frames`, doubled up to `cool_max_frames` each time it had to stop soon after
 *    starting) before it tries again.
 * Blocked (the host says so: fast forward, rewind, linked play, half-rate pacing, an eased picture, a cartridge whose state cannot be
 * put back) means off at once and nothing is learned from those frames.
 *
 * Wrong cost figures cost speed for a moment at worst: the rule only ever moves between plain frames and run-ahead, and the steady
 * pacer and the look governor sit behind it (the host blocks run-ahead while either has had to act).
 *
 * Header only (no source file to add to a build). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_AHEAD_H
#define PK_AHEAD_H

#include <stdint.h>

namespace pk {

enum AheadMode { AHEAD_OFF = 0, AHEAD_AUTO = 1, AHEAD_ON = 2 };

struct AheadConfig {
	uint32_t frame_us = 16683;      /* one frame of the game */
	int on_pct = 50;                /* start at or under this share of a frame (projected) */
	int off_pct = 70;               /* stop over this share (measured average) */
	int settle_frames = 120;        /* plain frames in a row that must qualify before a start */
	int late_window = 120;          /* frames ... */
	int late_limit = 3;             /* ... in which this many longer than a whole frame stop it */
	int cool_frames = 300;          /* wait after a stop */
	int cool_max_frames = 7200;     /* the doubled wait's ceiling (2 minutes at 60 Hz) */
	int bounce_frames = 600;        /* a stop this soon after a start doubles the wait */
	int ratio_x100 = 230;           /* first guess: run-ahead costs this share of a plain frame (two frames, a save and a load) */
	uint32_t overhead_us = 600;     /* ... plus this (the state copy), until measured */
};

class AheadGovernor {
public:
	AheadGovernor() { reset(); }
	void configure(const AheadConfig &c) { cfg_ = c; reset(); }
	const AheadConfig &config() const { return cfg_; }
	void set_frame_us(uint32_t us) { if (us) cfg_.frame_us = us; }
	void set_mode(int m) { mode_ = m < AHEAD_OFF ? AHEAD_OFF : m > AHEAD_ON ? AHEAD_ON : m; if (mode_ != AHEAD_AUTO) { good_ = 0; } }
	int mode() const { return mode_; }
	void reset()
	{
		on_ = false; plain_ema_ = 0; ahead_ema_ = 0; good_ = 0; cool_ = 0; cool_cur_ = cfg_.cool_frames; since_start_ = 0;
		late_n_ = 0; late_age_ = 0; ratio_ = cfg_.ratio_x100; starts_ = stops_ = 0; frames_on_ = frames_all_ = 0; blocked_ = false;
		plain_at_start_ = 0; learn_sum_ = 0;
	}
	/* the host cannot run ahead now (see above); cleared by the next set_blocked(false) */
	void set_blocked(bool b)
	{
		if (b && !blocked_) { on_ = false; good_ = 0; }
		blocked_ = b;
	}
	bool blocked() const { return blocked_; }
	/* run the next frame ahead? */
	bool want() const { return !blocked_ && (mode_ == AHEAD_ON || (mode_ == AHEAD_AUTO && on_)); }
	/* after each frame at normal speed: the emulating thread's work for it, and whether it was run ahead. Returns want(). */
	bool add(uint32_t emu_us, bool ran_ahead)
	{
		frames_all_++;
		if (ran_ahead) frames_on_++;
		if (blocked_ || mode_ != AHEAD_AUTO) return want();
		const uint32_t f = cfg_.frame_us;
		if (ran_ahead) {
			ahead_ema_ = ahead_ema_ ? (ahead_ema_ * 15 + emu_us) / 16 : emu_us;
			since_start_++;
			/* the ratio is learned right after a start only, while the plain cost measured just before it still describes the game */
			if (since_start_ > kLearnSkip && since_start_ <= kLearnSkip + kLearnFrames) {
				learn_sum_ += emu_us;
				if (since_start_ == kLearnSkip + kLearnFrames && plain_at_start_ > 200) {
					const int r = (int) (learn_sum_ * 100 / kLearnFrames / plain_at_start_);
					ratio_ = r < 150 ? 150 : r > 400 ? 400 : r;
				}
			}
			if (late_age_ > 0 && --late_age_ == 0) late_n_ = 0;
			if (emu_us > f) { late_n_++; late_age_ = cfg_.late_window; }
			if (on_ && (ahead_ema_ > (uint64_t) f * (unsigned) cfg_.off_pct / 100 || late_n_ >= cfg_.late_limit)) {
				on_ = false; stops_++;
				if (since_start_ < cfg_.bounce_frames) { cool_cur_ *= 2; if (cool_cur_ > cfg_.cool_max_frames) cool_cur_ = cfg_.cool_max_frames; }
				else cool_cur_ = cfg_.cool_frames;
				cool_ = cool_cur_; good_ = 0; late_n_ = 0; late_age_ = 0;
				/* what a plain frame costs now is not known: start the average from the run-ahead cost */
				plain_ema_ = (uint32_t) ((uint64_t) ahead_ema_ * 100 / (unsigned) ratio_);
			}
		} else {
			plain_ema_ = plain_ema_ ? (plain_ema_ * 15 + emu_us) / 16 : emu_us;
			if (cool_ > 0) { cool_--; good_ = 0; return want(); }
			const uint64_t proj = (uint64_t) plain_ema_ * (unsigned) ratio_ / 100 + cfg_.overhead_us;
			const uint64_t one = (uint64_t) emu_us * (unsigned) ratio_ / 100 + cfg_.overhead_us;
			if (proj <= (uint64_t) f * (unsigned) cfg_.on_pct / 100 && one <= (uint64_t) f * (unsigned) cfg_.off_pct / 100) good_++; else good_ = 0;
			if (!on_ && good_ >= cfg_.settle_frames) { on_ = true; starts_++; since_start_ = 0; ahead_ema_ = (uint32_t) proj; late_n_ = 0; late_age_ = 0; plain_at_start_ = plain_ema_; learn_sum_ = 0; }
		}
		return want();
	}
	bool on() const { return on_; }
	uint32_t plain_us() const { return plain_ema_; }
	uint32_t ahead_us() const { return ahead_ema_; }
	int ratio_x100() const { return ratio_; }
	unsigned starts() const { return starts_; }
	unsigned stops() const { return stops_; }
	unsigned frames_ahead() const { return frames_on_; }
	unsigned frames() const { return frames_all_; }
	/* the census counters start over (the averages stay) */
	void census_reset() { frames_on_ = frames_all_ = 0; }
private:
	AheadConfig cfg_;
	int mode_ = AHEAD_AUTO;
	bool on_, blocked_;
	enum { kLearnSkip = 8, kLearnFrames = 32 };
	uint32_t plain_ema_, ahead_ema_, plain_at_start_;
	uint64_t learn_sum_;
	int good_, cool_, cool_cur_, since_start_, late_n_, late_age_, ratio_;
	unsigned starts_, stops_, frames_on_, frames_all_;
};

} // namespace pk
#endif
