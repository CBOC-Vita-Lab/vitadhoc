/*
 * Fast forward, as a player needs it: 2x or 3x, hold or toggle, with the game's sound sped up or without sound.
 *
 * The game is emulated `factor` frames per display refresh; only the last of them is drawn (the others are not rendered, which the
 * cores allow). With sound, the audio of `factor` emulated frames is mixed down to one frame's worth (a box filter over `factor` samples
 * of each channel: the sound plays at `factor` times its pitch, as on a console running fast, and the audio pacer sees the same number
 * of samples per refresh as at normal speed). Without sound the audio of those frames is dropped.
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

namespace pk {

class FastForward {
public:
	FastForward() { configure(2, true, false); }
	/* factor 2 or 3 (anything else is 2); sound on/off; toggle (a press switches it) or hold */
	void configure(int factor, bool sound, bool toggle);
	/* One update per emulated frame's input: `held` = the control is down, `pressed` = it went down on this frame. */
	void update(bool held, bool pressed, bool locked);
	bool on() const { return on_; }
	int factor() const { return on_ ? factor_ : 1; }
	int configured_factor() const { return factor_; }
	bool sound() const { return sound_; }
	bool toggle() const { return toggle_; }
	/* frames to emulate this refresh when `normal` would be run at normal speed */
	int frames(int normal) const { return on_ ? normal * factor_ : normal; }
	/* Reduce `frames` stereo frames (L,R int16 pairs) in place; returns the number kept. Without FF the audio is untouched. With FF and no
	 * sound it returns 0. The remainder of a group of `factor` input frames is carried to the next call. */
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
	bool sound_ = true, toggle_ = false;
	bool on_ = false, locked_ = false, ended_ = false, started_ = false;
	int32_t acc_[2] = { 0, 0 };
	int acc_n_ = 0;
};

}  // namespace pk

#endif
