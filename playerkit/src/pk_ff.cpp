/* Fast forward: see pk_ff.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_ff.h"

namespace pk {

void FastForward::configure(int factor, bool sound, bool toggle)
{
	factor_ = factor == 3 ? 3 : 2;
	sound_ = sound;
	if (toggle_ != toggle && on_) { on_ = false; ended_ = true; }
	toggle_ = toggle;
	acc_[0] = acc_[1] = 0;
	acc_n_ = 0;
}

void FastForward::update(bool held, bool pressed, bool locked)
{
	const bool before = on_;
	locked_ = locked;
	if (locked) on_ = false;
	else if (toggle_) { if (pressed) on_ = !on_; }
	else on_ = held;
	if (on_ != before) {
		if (on_) started_ = true; else ended_ = true;
		acc_[0] = acc_[1] = 0;
		acc_n_ = 0;
	}
}

void FastForward::reset()
{
	if (on_) ended_ = true;
	on_ = false;
	acc_[0] = acc_[1] = 0;
	acc_n_ = 0;
}

size_t FastForward::reduce(int16_t *lr, size_t frames)
{
	if (!on_) return frames;
	/* Without sound the same number of frames comes out as silence: the audio pacer is fed at the real-time rate and never runs dry */
	size_t k = 0;
	for (size_t i = 0; i < frames; i++) {
		acc_[0] += lr[i * 2 + 0];
		acc_[1] += lr[i * 2 + 1];
		if (++acc_n_ < factor_) continue;
		lr[k * 2 + 0] = sound_ ? (int16_t) (acc_[0] / factor_) : 0;
		lr[k * 2 + 1] = sound_ ? (int16_t) (acc_[1] / factor_) : 0;
		k++;
		acc_[0] = acc_[1] = 0;
		acc_n_ = 0;
	}
	return k;
}

std::string FastForward::chip() const
{
	return on_ ? (factor_ == 3 ? ">> 3x" : ">> 2x") : std::string();
}

}  // namespace pk
