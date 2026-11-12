/*
 * Audio pacing for emulator players: a lock-free single producer / single consumer
 * ring of stereo int16 frames at the emulator's rate, and a renderer that turns it
 * into fixed-size grains at the device rate. A PI controller on the (smoothed) ring
 * fill adjusts the resampling ratio so the ring stays at a target level; an adaptive
 * cushion, a slow-emulation mode, and click-free underrun / pause / overfill handling
 * sit around it.
 *
 * Portable plain C++11: no Vita, no Snes9x. Written so that a NES player can use it.
 *
 * Threads: push(), legacy_update() and report_speed() belong to the emulation thread,
 * render() to the audio thread, poll() / set_paused() to any one control thread.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#ifndef AUDIO_PACE_H
#define AUDIO_PACE_H

#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <vector>

namespace apace {

struct Config {
	int in_rate = 32040;     /* emulator audio rate, frames per second */
	int out_rate = 48000;    /* device rate */
	/* run-time A/B switch: true reproduces the first SNES player's audio exactly (1024-frame grains,
	 * linear interpolation, P-only rate control on the post-push fill with a 60 ms target, hold the
	 * last sample on underrun, 32768-frame queue). */
	bool legacy = false;
	/* PI controller gains (normal mode): ppm of ratio per unit of relative fill error, and ppm added to
	 * the integral per grain per unit of error. The output limits (+-5000 / +-4000 ppm) are fixed. */
	double kp_ppm = 40000.0;
	double ki_ppm_per_grain = 150.0;
};

/* Counters of one window (about a second of output). */
struct Counters {
	uint32_t in_frames, out_frames;          /* frames pushed / rendered in the window */
	double fill_ms_min, fill_ms_avg, fill_ms_max;   /* ring fill at grain starts, before consuming */
	double ema_ms, target_ms;                /* smoothed fill, cushion target at the window's end */
	double ratio_ppm_min, ratio_ppm_avg, ratio_ppm_max;   /* (ratio - 1) * 1e6, base speed included */
	double base_speed;                       /* base ratio (1.0, or the slow-emulation speed) */
	uint32_t underruns_sec, underruns_total;
	uint32_t dropped_sec, dropped_total;     /* frames refused because the ring was full */
	uint32_t trims_sec, trims_total;         /* overfill trims */
};

/* The most recent underrun (for a log line of its own). */
struct UnderrunEvent {
	uint32_t index;         /* underruns_total after this one */
	double ema_ms;          /* smoothed fill just before it */
	double target_ms;       /* the new (raised) target */
	double ratio_ppm;       /* ratio in force */
	uint64_t out_frames;    /* output frames rendered before it (position in the stream) */
};

class Pacer {
public:
	static const int GRAIN = 512;          /* output frames per grain in the normal mode */
	static const int GRAIN_LEGACY = 1024;
	static const int RING = 16384;         /* input frames (legacy: 32768) */
	static const int FADE = 64;            /* frames of every fade / crossfade */

	Pacer();
	/* before the audio thread starts (allocates) */
	void init(const Config &cfg);
	const Config &config() const { return cfg_; }
	int grain() const { return cfg_.legacy ? GRAIN_LEGACY : GRAIN; }

	/* ---- emulation thread ---- */
	/* Returns frames accepted; the rest is dropped (counted) when the ring is full. */
	int push(const int16_t *lr, int frames);
	/* Measured emulation speed (1.0 = real time). Slow mode below 0.985, back to 1.0 above 0.995. */
	void report_speed(double speed);
	/* Legacy mode only: the old rate control ran here, once per emulated pass, on the fill right
	 * after the push. A no-op in the normal mode. */
	void legacy_update();

	/* ---- audio thread ---- */
	/* Fills grain() stereo frames. Never blocks, never allocates. */
	void render(int16_t *out);

	/* ---- control thread ---- */
	/* Pause / resume: a fade out, the ring is flushed, and playing starts again through the
	 * priming path once audio arrives. */
	void set_paused(bool p) { paused_.store(p, std::memory_order_release); }
	/* Counters of the window since the previous successful poll. Call at a one second tick, then on
	 * every pass until it returns true (the audio thread publishes at its next grain, about 10 ms
	 * after the first call). Returns false while waiting. */
	bool poll(Counters *c);
	uint32_t underruns() const { return ur_total_pub_.load(std::memory_order_relaxed); }
	UnderrunEvent last_underrun() const;
	uint32_t fill_frames() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }

private:
	struct Window {
		uint32_t out_frames, grains;
		double fill_min, fill_sum, fill_max;
		double ratio_min, ratio_sum, ratio_max;
		uint32_t ur, trims;
		void reset();
	};

	void render_new(int16_t *out);
	void render_legacy(int16_t *out);
	void note_grain(double fill_frames, double ratio);
	void publish_if_asked();
	void record_underrun(double ratio);
	static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

	Config cfg_;
	std::vector<int16_t> ring_;
	uint32_t mask_;
	std::atomic<uint32_t> w_, r_;            /* frame counters, wrap at 2^32 */
	std::atomic<bool> paused_;
	std::atomic<double> speed_;
	std::atomic<double> legacy_ratio_;
	/* producer side totals, read by poll() */
	std::atomic<uint32_t> in_total_, dropped_total_;

	/* audio thread state */
	bool primed_, in_pause_;
	double frac_, ema_, integ_ppm_, base_, ratio_;
	bool slow_;
	double target_ms_;
	uint64_t calm_frames_;
	int fadein_left_, tail_left_, xfade_left_, pause_left_;
	uint32_t xa_pos_; double xa_frac_;
	int16_t last_[2];
	int16_t tail_from_[2];
	uint64_t out_total_;
	uint32_t ur_total_, trims_total_;
	bool in_underrun_legacy_;
	Window win_;

	/* stats hand-off: poll() sets req_, the audio thread publishes at its next grain start (before consuming) */
	std::atomic<bool> req_, ready_;
	Counters pub_;
	std::atomic<uint32_t> ur_total_pub_;
	std::atomic<uint32_t> ev_index_;
	std::atomic<double> ev_ema_, ev_target_, ev_ratio_;
	std::atomic<uint64_t> ev_pos_;
	/* poll-side bookkeeping */
	uint32_t last_in_, last_dropped_, last_trims_total_, last_ur_total_;
};

/* Emulation speed over a sliding window of the last N one-second reports. Feed it what the
 * emulation thread did each second; reset it after a pause. */
class SpeedMeter {
public:
	explicit SpeedMeter(int window = 8);
	void reset();
	/* frames emulated in `secs` seconds of wall time; nominal_fps is the console's frame rate.
	 * Returns the speed averaged over the window, or a negative number while warming up
	 * (the first `skip` reports are not trusted). */
	double update(unsigned frames, double secs, double nominal_fps, int skip = 2);
private:
	enum { MAXW = 32 };
	int window_, n_, head_, seen_;
	double fr_[MAXW], sec_[MAXW];
};

} /* namespace apace */

#endif
