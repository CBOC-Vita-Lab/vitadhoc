/*
 * Audio pacing, see audio_pace.h. Plain C++11, no platform code.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#include "audio_pace.h"

#include <math.h>
#include <string.h>

namespace apace {

/* ---- constants of the controller ------------------------------------------------------------ */
static const double kEmaGain = 1.0 / 8.0;        /* smoothing of the fill measured at each grain start */
static const double kPclamp = 5000.0;            /* ... limited to +-5000 ppm */
static const double kIclamp = 4000.0;            /* ... limited to +-4000 ppm */
static const double kTargetMs0 = 70.0, kTargetMin = 50.0, kTargetMax = 140.0;
static const double kUnderrunUpMs = 20.0, kCalmDownMs = 5.0, kCalmSeconds = 30.0;
static const double kSlowEnter = 0.985, kSlowLeave = 0.995, kSlowFloor = 0.5;
static const double kTrimFactor = 3.0;           /* fill above this many targets is trimmed back to one */
static const double kLegacyTargetMs = 60.0;

void Pacer::Window::reset()
{
	out_frames = grains = 0;
	fill_min = 1e300; fill_max = -1e300; fill_sum = 0;
	ratio_min = 1e300; ratio_max = -1e300; ratio_sum = 0;
	ur = trims = 0;
}

Pacer::Pacer()
    : mask_(0), w_(0), r_(0), paused_(false), speed_(1.0), legacy_ratio_(1.0), in_total_(0), dropped_total_(0),
      primed_(false), in_pause_(false), frac_(0), ema_(0), integ_ppm_(0), base_(1.0), ratio_(1.0), slow_(false),
      target_ms_(kTargetMs0), calm_frames_(0), fadein_left_(0), tail_left_(0), xfade_left_(0), pause_left_(0),
      xa_pos_(0), xa_frac_(0), out_total_(0), ur_total_(0), trims_total_(0), in_underrun_legacy_(false),
      req_(false), ready_(false), ur_total_pub_(0), ev_index_(0), ev_ema_(0), ev_target_(0), ev_ratio_(0),
      ev_pos_(0), last_in_(0), last_dropped_(0), last_trims_total_(0), last_ur_total_(0)
{
	last_[0] = last_[1] = 0;
	tail_from_[0] = tail_from_[1] = 0;
	memset(&pub_, 0, sizeof pub_);
	win_.reset();
}

void Pacer::init(const Config &cfg)
{
	cfg_ = cfg;
	uint32_t cap = cfg.legacy ? 32768 : RING;
	ring_.assign((size_t) cap * 2, 0);
	mask_ = cap - 1;
	w_.store(0); r_.store(0);
	paused_.store(false); speed_.store(1.0); legacy_ratio_.store(1.0);
	in_total_.store(0); dropped_total_.store(0);
	primed_ = in_pause_ = false;
	frac_ = ema_ = integ_ppm_ = 0;
	base_ = ratio_ = 1.0;
	slow_ = false;
	target_ms_ = cfg.legacy ? kLegacyTargetMs : kTargetMs0;
	calm_frames_ = 0;
	fadein_left_ = tail_left_ = xfade_left_ = pause_left_ = 0;
	last_[0] = last_[1] = 0;
	out_total_ = 0; ur_total_ = trims_total_ = 0;
	in_underrun_legacy_ = false;
	win_.reset();
	req_.store(false); ready_.store(false);
	ur_total_pub_.store(0);
	last_in_ = last_dropped_ = last_trims_total_ = last_ur_total_ = 0;
}

/* ---- emulation thread ----------------------------------------------------------------------- */

int Pacer::push(const int16_t *lr, int frames)
{
	if (frames <= 0) return 0;
	const uint32_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
	const uint32_t cap = mask_ + 1, fill = w - r;
	/* full: drop the new audio (never block the emulation) */
	uint32_t space = fill >= cap - 1 ? 0 : cap - 1 - fill;
	uint32_t n = (uint32_t) frames < space ? (uint32_t) frames : space;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t p = (w + i) & mask_;
		ring_[(size_t) p * 2] = lr[i * 2];
		ring_[(size_t) p * 2 + 1] = lr[i * 2 + 1];
	}
	w_.store(w + n, std::memory_order_release);
	in_total_.store(in_total_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
	if (n < (uint32_t) frames)
		dropped_total_.store(dropped_total_.load(std::memory_order_relaxed) + (frames - n), std::memory_order_relaxed);
	return (int) n;
}

void Pacer::report_speed(double speed) { speed_.store(speed, std::memory_order_relaxed); }

void Pacer::legacy_update()
{
	if (!cfg_.legacy) return;
	/* audio rate control of the first player: keep the queue near 60 ms, P only, on the raw fill */
	double target = 0.06 * cfg_.in_rate, fill = (double) (w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire));
	double e = (fill - target) / target; /* >0: too full, consume faster */
	if (e > 1) e = 1;
	if (e < -1) e = -1;
	legacy_ratio_.store(1.0 + 0.01 * e, std::memory_order_relaxed);
}

/* ---- audio thread --------------------------------------------------------------------------- */

void Pacer::publish_if_asked()
{
	if (!req_.load(std::memory_order_acquire) || ready_.load(std::memory_order_acquire)) return;
	const double inr = cfg_.in_rate;
	Counters &c = pub_;
	memset(&c, 0, sizeof c);
	c.out_frames = win_.out_frames;
	if (win_.grains) {
		c.fill_ms_min = win_.fill_min * 1000.0 / inr;
		c.fill_ms_max = win_.fill_max * 1000.0 / inr;
		c.fill_ms_avg = win_.fill_sum / win_.grains * 1000.0 / inr;
		c.ratio_ppm_min = win_.ratio_min;
		c.ratio_ppm_max = win_.ratio_max;
		c.ratio_ppm_avg = win_.ratio_sum / win_.grains;
	}
	c.ema_ms = ema_ * 1000.0 / inr;
	c.target_ms = target_ms_;
	c.base_speed = cfg_.legacy ? 1.0 : base_;
	c.underruns_sec = win_.ur;
	c.underruns_total = ur_total_;
	c.trims_sec = win_.trims;
	c.trims_total = trims_total_;
	win_.reset();
	req_.store(false, std::memory_order_relaxed);
	ready_.store(true, std::memory_order_release);
}

bool Pacer::poll(Counters *c)
{
	if (!ready_.load(std::memory_order_acquire)) {
		req_.store(true, std::memory_order_release);    /* the audio thread publishes at its next grain */
		return false;
	}
	*c = pub_;
	const uint32_t in = in_total_.load(std::memory_order_relaxed), dr = dropped_total_.load(std::memory_order_relaxed);
	c->in_frames = in - last_in_;
	c->dropped_sec = dr - last_dropped_;
	c->dropped_total = dr;
	last_in_ = in;
	last_dropped_ = dr;
	ready_.store(false, std::memory_order_release);
	return true;
}

UnderrunEvent Pacer::last_underrun() const
{
	UnderrunEvent e;
	e.index = ev_index_.load(std::memory_order_acquire);
	e.ema_ms = ev_ema_.load(std::memory_order_relaxed);
	e.target_ms = ev_target_.load(std::memory_order_relaxed);
	e.ratio_ppm = ev_ratio_.load(std::memory_order_relaxed);
	e.out_frames = ev_pos_.load(std::memory_order_relaxed);
	return e;
}

void Pacer::note_grain(double fill, double ratio)
{
	win_.grains++;
	win_.fill_sum += fill;
	if (fill < win_.fill_min) win_.fill_min = fill;
	if (fill > win_.fill_max) win_.fill_max = fill;
	const double ppm = (ratio - 1.0) * 1e6;
	win_.ratio_sum += ppm;
	if (ppm < win_.ratio_min) win_.ratio_min = ppm;
	if (ppm > win_.ratio_max) win_.ratio_max = ppm;
}

void Pacer::record_underrun(double ratio)
{
	ur_total_++;
	win_.ur++;
	ev_ema_.store(ema_ * 1000.0 / cfg_.in_rate, std::memory_order_relaxed);
	target_ms_ = clampd(target_ms_ + kUnderrunUpMs, kTargetMin, kTargetMax);
	calm_frames_ = 0;
	ev_target_.store(target_ms_, std::memory_order_relaxed);
	ev_ratio_.store((ratio - 1.0) * 1e6, std::memory_order_relaxed);
	ev_pos_.store(out_total_, std::memory_order_relaxed);
	ev_index_.store(ur_total_, std::memory_order_release);
	ur_total_pub_.store(ur_total_, std::memory_order_release);
}

void Pacer::render(int16_t *out)
{
	if (cfg_.legacy) render_legacy(out);
	else render_new(out);
}

/* The first player's audio thread body, one grain, unchanged in behaviour. */
void Pacer::render_legacy(int16_t *out)
{
	publish_if_asked();
	const int G = GRAIN_LEGACY;
	uint32_t r = r_.load(std::memory_order_relaxed);
	const bool paused = paused_.load(std::memory_order_acquire);
	const double ratio = legacy_ratio_.load(std::memory_order_relaxed);
	const double fill0 = (double) (w_.load(std::memory_order_acquire) - r);
	/* counters only (no effect on the sound) */
	ema_ += (fill0 - ema_) * kEmaGain;
	note_grain(fill0, ratio);
	win_.out_frames += G;
	out_total_ += G;
	const double step = (double) cfg_.in_rate / cfg_.out_rate * ratio;
	for (int i = 0; i < G; i++) {
		const uint32_t fill = w_.load(std::memory_order_acquire) - r;
		if (paused || fill < 3) {
			out[i * 2] = last_[0]; out[i * 2 + 1] = last_[1];
			if (!paused && primed_ && !in_underrun_legacy_) {   /* statistics: one underrun per dry spell */
				in_underrun_legacy_ = true;
				ur_total_++; win_.ur++;
				ev_ema_.store(ema_ * 1000.0 / cfg_.in_rate, std::memory_order_relaxed);
				ev_target_.store(target_ms_, std::memory_order_relaxed);
				ev_ratio_.store((ratio - 1.0) * 1e6, std::memory_order_relaxed);
				ev_pos_.store(out_total_, std::memory_order_relaxed);
				ev_index_.store(ur_total_, std::memory_order_release);
				ur_total_pub_.store(ur_total_, std::memory_order_release);
			}
			continue;
		}
		primed_ = true;
		in_underrun_legacy_ = false;
		const uint32_t a = r & mask_, b = (r + 1) & mask_;
		const double f = frac_;
		out[i * 2] = (int16_t) (ring_[(size_t) a * 2] * (1 - f) + ring_[(size_t) b * 2] * f);
		out[i * 2 + 1] = (int16_t) (ring_[(size_t) a * 2 + 1] * (1 - f) + ring_[(size_t) b * 2 + 1] * f);
		last_[0] = out[i * 2]; last_[1] = out[i * 2 + 1];
		frac_ += step;
		const unsigned adv = (unsigned) frac_;
		frac_ -= adv;
		r += adv;
		r_.store(r, std::memory_order_release);
	}
}

void Pacer::render_new(int16_t *out)
{
	publish_if_asked();
	const int G = GRAIN;
	const double inr = cfg_.in_rate, outr = cfg_.out_rate;
	uint32_t w = w_.load(std::memory_order_acquire);
	uint32_t r = r_.load(std::memory_order_relaxed);
	const bool paused = paused_.load(std::memory_order_acquire);
	int pos = 0;
	win_.out_frames += G;
	out_total_ += G;

	/* the decaying tail of an earlier underrun fade-out */
	while (tail_left_ > 0 && pos < G) {
		const int k = FADE - tail_left_;
		out[pos * 2] = (int16_t) (tail_from_[0] * (FADE - k) / FADE);
		out[pos * 2 + 1] = (int16_t) (tail_from_[1] * (FADE - k) / FADE);
		last_[0] = out[pos * 2]; last_[1] = out[pos * 2 + 1];
		tail_left_--; pos++;
	}

	/* pause / resume: fade the audio out, flush the ring, prime again when audio comes back */
	if (paused) {
		if (primed_ && !in_pause_) { in_pause_ = true; pause_left_ = FADE; }
		if (!(primed_ && pause_left_ > 0)) {
			for (; pos < G; pos++) out[pos * 2] = out[pos * 2 + 1] = 0;
			last_[0] = last_[1] = 0;
			in_pause_ = true;
			primed_ = false;
			fadein_left_ = xfade_left_ = 0;
			r_.store(w, std::memory_order_release);    /* stale audio is not played after the resume */
			return;
		}
	} else if (in_pause_ && !primed_) {
		in_pause_ = false;
		pause_left_ = 0;
	}

	const double inframes_per_ms = inr / 1000.0;
	double tgt = target_ms_ * inframes_per_ms;

	if (!primed_) {
		/* not playing: priming waits for half the cushion */
		const double fill = (double) (w - r);
		if (fill >= tgt * 0.5) {
			primed_ = true;
			fadein_left_ = FADE;
			ema_ = fill;
			frac_ = 0;
			xfade_left_ = 0;
		} else {
			for (; pos < G; pos++) out[pos * 2] = out[pos * 2 + 1] = 0;
			last_[0] = last_[1] = 0;
			return;
		}
	} else {
		/* ---- rate control, once per grain ---- */
		const double fill = (double) (w - r);
		ema_ += (fill - ema_) * kEmaGain;
		calm_frames_ += G;
		if (calm_frames_ >= (uint64_t) (kCalmSeconds * outr)) {
			calm_frames_ = 0;
			target_ms_ = clampd(target_ms_ - kCalmDownMs, kTargetMin, kTargetMax);
			tgt = target_ms_ * inframes_per_ms;
		}
		if (fill > kTrimFactor * tgt) {
			/* far too full (the producer ran ahead, a pause was missed...): drop the oldest audio down to
			 * the target; the cut is hidden by a crossfade between the old and the new position */
			xa_pos_ = r; xa_frac_ = frac_;
			r += (uint32_t) (fill - tgt);
			xfade_left_ = FADE;
			ema_ = tgt;
			trims_total_++; win_.trims++;
		}
		double sp = speed_.load(std::memory_order_relaxed);
		if (!slow_ && sp < kSlowEnter) slow_ = true;
		else if (slow_ && sp > kSlowLeave) slow_ = false;
		base_ = slow_ ? (sp > kSlowFloor ? sp : kSlowFloor) : 1.0;
		const double err = (ema_ - tgt) / tgt;     /* >0: too full, consume faster */
		const double p = clampd(err * cfg_.kp_ppm, -kPclamp, kPclamp);
		integ_ppm_ = clampd(integ_ppm_ + err * cfg_.ki_ppm_per_grain, -kIclamp, kIclamp);
		ratio_ = base_ * (1.0 + (p + integ_ppm_) * 1e-6);
		note_grain(fill, ratio_);
	}

	const double step = inr / outr * ratio_;
	for (; pos < G; pos++) {
		if (in_pause_ && pause_left_ <= 0) {
			/* pause fade finished */
			for (; pos < G; pos++) out[pos * 2] = out[pos * 2 + 1] = 0;
			last_[0] = last_[1] = 0;
			primed_ = false;
			fadein_left_ = xfade_left_ = 0;
			r_.store(w_.load(std::memory_order_acquire), std::memory_order_release);
			return;
		}
		if (w - r < 2) {
			w = w_.load(std::memory_order_acquire);
			if (w - r < 2) {
				/* underrun: decay from the last sample over FADE frames, stop, prime again */
				for (int k = 0; pos + k < G; k++) {
					const int g = k < FADE ? FADE - k : 0;
					out[(pos + k) * 2] = (int16_t) (last_[0] * g / FADE);
					out[(pos + k) * 2 + 1] = (int16_t) (last_[1] * g / FADE);
				}
				if (G - pos < FADE) {
					tail_left_ = FADE - (G - pos);
					tail_from_[0] = last_[0]; tail_from_[1] = last_[1];
				}
				const bool counts = !in_pause_;
				if (G - pos < FADE) {
					last_[0] = (int16_t) (last_[0] * (FADE - (G - pos)) / FADE);
					last_[1] = (int16_t) (last_[1] * (FADE - (G - pos)) / FADE);
				} else {
					last_[0] = last_[1] = 0;
				}
				primed_ = false;
				fadein_left_ = xfade_left_ = 0;
				if (counts) record_underrun(ratio_);
				else r = w;
				r_.store(r, std::memory_order_release);
				return;
			}
		}
		const uint32_t a = r & mask_, b = (r + 1) & mask_;
		const double f = frac_;
		double l = ring_[(size_t) a * 2] * (1 - f) + ring_[(size_t) b * 2] * f;
		double rr = ring_[(size_t) a * 2 + 1] * (1 - f) + ring_[(size_t) b * 2 + 1] * f;
		if (xfade_left_ > 0) {
			const uint32_t xa = xa_pos_ & mask_, xb = (xa_pos_ + 1) & mask_;
			const double xf = xa_frac_;
			const double ol = ring_[(size_t) xa * 2] * (1 - xf) + ring_[(size_t) xb * 2] * xf;
			const double orr = ring_[(size_t) xa * 2 + 1] * (1 - xf) + ring_[(size_t) xb * 2 + 1] * xf;
			const double t = (double) (FADE - xfade_left_ + 1) / FADE;
			l = ol * (1 - t) + l * t;
			rr = orr * (1 - t) + rr * t;
			xa_frac_ += step;
			const unsigned xadv = (unsigned) xa_frac_;
			xa_frac_ -= xadv;
			xa_pos_ += xadv;
			xfade_left_--;
		}
		if (fadein_left_ > 0) {
			const double g = (double) (FADE - fadein_left_) / FADE;
			l *= g; rr *= g;
			fadein_left_--;
		}
		if (in_pause_ && pause_left_ > 0) {
			const double g = (double) pause_left_ / FADE;
			l *= g; rr *= g;
			pause_left_--;
		}
		out[pos * 2] = (int16_t) lrint(l);
		out[pos * 2 + 1] = (int16_t) lrint(rr);
		last_[0] = out[pos * 2]; last_[1] = out[pos * 2 + 1];
		frac_ += step;
		const unsigned adv = (unsigned) frac_;
		frac_ -= adv;
		r += adv;
	}
	r_.store(r, std::memory_order_release);
}

/* ---- SpeedMeter ----------------------------------------------------------------------------- */

SpeedMeter::SpeedMeter(int window) : window_(window < 1 ? 1 : (window > MAXW ? MAXW : window)) { reset(); }

void SpeedMeter::reset()
{
	n_ = head_ = seen_ = 0;
	for (int i = 0; i < MAXW; i++) fr_[i] = sec_[i] = 0;
}

double SpeedMeter::update(unsigned frames, double secs, double nominal_fps, int skip)
{
	if (++seen_ <= skip || secs <= 0 || nominal_fps <= 0) return -1.0;
	fr_[head_] = frames; sec_[head_] = secs;
	head_ = (head_ + 1) % window_;
	if (n_ < window_) n_++;
	double f = 0, s = 0;
	for (int i = 0; i < n_; i++) { f += fr_[i]; s += sec_[i]; }
	return s > 0 ? f / s / nominal_fps : -1.0;
}

} /* namespace apace */
