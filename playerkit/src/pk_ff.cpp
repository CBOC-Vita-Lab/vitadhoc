/* Fast forward: see pk_ff.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_ff.h"
#include "pk_i18n.h"

namespace pk {

void FastForward::configure(int factor, int sound, bool toggle)
{
	factor_ = factor == 3 ? 3 : 2;
	sound_ = sound < SOUND_OFF ? SOUND_OFF : sound > SOUND_NORMAL ? SOUND_NORMAL : sound;
	pk_ffnorm_reset(&norm_, factor_);
	if (toggle_ != toggle && on_) { on_ = false; ended_ = true; if (!frozen_) lat_ = false; }
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
	if (!frozen_) lat_ = on_;
	if (on_ != before) {
		if (on_) started_ = true; else ended_ = true;
		acc_[0] = acc_[1] = 0;
		acc_n_ = 0;
		pk_ffnorm_reset(&norm_, factor_);
	}
}

void FastForward::reset()
{
	if (on_) ended_ = true;
	if (!frozen_) lat_ = false;
	on_ = false;     /* with a latch, lat_ stays: the refresh that is running finishes as it began */
	acc_[0] = acc_[1] = 0;
	acc_n_ = 0;
	pk_ffnorm_reset(&norm_, factor_);
}

size_t FastForward::reduce(int16_t *lr, size_t frames)
{
	if (!lat_) return frames;
	if (unl_) {
		/* silence, one frame's worth per refresh: the chunks of the first frame come out zeroed, the others of the refresh not at all */
		if (!unl_emit_) return 0;
		for (size_t i = 0; i < frames * 2; i++) lr[i] = 0;
		return frames;
	}
	if (sound_ == SOUND_NORMAL) return pk_ffnorm_chunk(&norm_, lr, frames);
	/* Without sound the same number of frames comes out as silence: the audio pacer is fed at the real-time rate and never runs dry */
	size_t k = 0;
	for (size_t i = 0; i < frames; i++) {
		acc_[0] += lr[i * 2 + 0];
		acc_[1] += lr[i * 2 + 1];
		if (++acc_n_ < factor_) continue;
		lr[k * 2 + 0] = sound_ != SOUND_OFF ? (int16_t) (acc_[0] / factor_) : 0;
		lr[k * 2 + 1] = sound_ != SOUND_OFF ? (int16_t) (acc_[1] / factor_) : 0;
		k++;
		acc_[0] = acc_[1] = 0;
		acc_n_ = 0;
	}
	return k;
}

std::string FastForward::chip() const
{
	if (!on_) return std::string();
	if (unl_) return unl_ == 2 ? tr("speed.unlimited_smooth") : tr("speed.unlimited");
	return factor_ == 3 ? ">> 3x" : ">> 2x";
}

}  // namespace pk
