/*
 * Fast forward, as a player needs it: 2x or 3x, hold or toggle, with the game's sound sped up or without sound.
 *
 * The game is emulated `factor` frames per display refresh; only the last of them is drawn (the others are not rendered, which the
 * cores allow). With sound, the audio of `factor` emulated frames is mixed down to one frame's worth (a box filter over `factor` samples
 * of each channel: the sound plays at `factor` times its pitch, as on a console running fast, and the audio pacer sees the same number
 * of samples per refresh as at normal speed). Without sound the same number of samples comes out as silence, so the pacer never runs dry.
 *
 * Unlimited and Unlimited Smooth (the main app's choices of the same names): as many frames as fit in a refresh (Speed::plan counts them from what
 * a frame costs), silent, and only the last frame of a refresh is drawn: every refresh with Unlimited Smooth (60 pictures a second), every
 * second one with Unlimited (30). A refresh still ends at the display's pace (a player cannot let the display run free), so "unlimited" means as
 * fast as one refresh holds. The audio pacer is fed one frame's worth of silence per refresh.
 *
 * Off in two-player and online play (`locked`): two machines that must stay equal cannot run ahead of each other.
 * It is also the player's job to leave the pacer (pk_pace) and the look governor alone while it runs: their costs are a multiple of the
 * game's and would only make them back off; reset them when it ends (Fast forward leaves *what the pacer decided* untouched, see
 * speed_ended()).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_FF_H
#define PK_FF_H

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "pk_ffnorm.h"

namespace pk {

class FastForward {
public:
	/* what the sound does while it runs: off (silence), sped up (the game's sound at `factor` times its pitch), or at normal pitch
	 * (every `factor`th chunk, joined with a short fade; pk_ffnorm.h) */
	enum { SOUND_OFF = 0, SOUND_FAST = 1, SOUND_NORMAL = 2 };
	FastForward() { configure(2, SOUND_FAST, false); }
	/* factor 2 or 3 (anything else is 2); sound SOUND_*; toggle (a press switches it) or hold */
	void configure(int factor, int sound, bool toggle);
	/* 0 = the factor above, 1 = Unlimited (30 pictures a second), 2 = Unlimited Smooth (60); both silent */
	void set_unlimited(int kind) { unl_ = kind < 0 ? 0 : kind > 2 ? 2 : kind; }
	int unlimited() const { return unl_; }
	/* Unlimited: a refresh begins and will run `frames` frames (Speed::plan); which refreshes are drawn alternates with Unlimited */
	void begin_refresh(int frames) { unl_n_ = frames < 1 ? 1 : frames; unl_refresh_++; }
	bool next_refresh_drawn() const { return unl_ == 2 || ((unl_refresh_ + 1) & 1) != 0; }
	/* Unlimited: the audio of the frame that just ran is the one whose silence is kept (the first of the refresh) */
	void set_audio_frame(int index_in_refresh) { unl_emit_ = index_in_refresh == 0; }
	/* is the last frame of this refresh drawn? (everything else is `index == count - 1`) */
	bool draw_last() const { return unl_ == 0 || unl_ == 2 || (unl_refresh_ & 1) != 0; }
	/* One update per emulated frame's input: `held` = the control is down, `pressed` = it went down on this frame. */
	void update(bool held, bool pressed, bool locked);
	bool on() const { return on_; }
	/* The state for one whole refresh: the player latches it once, in Speed::plan, before the frames; draw_last(), frames() are asked of it and
	 * reduce() follows it (a user that never latches has it follow on() live), so a release (or a press) in the middle of a refresh cannot change what that refresh does with its frames and audio. */
	void latch() { lat_ = on_; frozen_ = true; }
	bool latched() const { return lat_; }
	int factor() const { return on_ ? factor_ : 1; }
	int configured_factor() const { return factor_; }
	bool sound() const { return sound_ != SOUND_OFF && !unl_; }
	int sound_mode() const { return sound_; }
	bool toggle() const { return toggle_; }
	/* frames to emulate this refresh when `normal` would be run at normal speed */
	int frames(int normal) const { return on_ ? (unl_ ? (normal > 0 ? unl_n_ : 0) : normal * factor_) : normal; }
	/* Reduce `frames` stereo frames (L,R int16 pairs) in place; returns the number kept. Without FF the audio is untouched. With FF and no
	 * sound the same count comes out, zeroed. The remainder of a group of `factor` input frames is carried to the next call. At normal pitch
	 * one call is one emulated frame's audio: the first factor-1 calls of a group return 0 and the last returns its whole chunk. */
	size_t reduce(int16_t *lr, size_t frames);
	/* a state load, the menu, a pause: forget the half-built group and a toggled-on state */
	void reset();
	/* the chip text (">> 2x"), "" when off */
	std::string chip() const;
	/* true once, on the update that switched it off (the player resets its pacing then) */
	bool take_ended() { bool e = ended_; ended_ = false; return e; }
	bool take_started() { bool e = started_; started_ = false; return e; }
private:
	int factor_ = 2;
	int unl_ = 0, unl_n_ = 1, unl_refresh_ = 0;
	bool unl_emit_ = true;
	int sound_ = SOUND_FAST;
	bool toggle_ = false;
	bool on_ = false, lat_ = false, frozen_ = false, locked_ = false, ended_ = false, started_ = false;
	int32_t acc_[2] = { 0, 0 };
	int acc_n_ = 0;
	pk_ffnorm norm_;
};

}  // namespace pk

#endif
