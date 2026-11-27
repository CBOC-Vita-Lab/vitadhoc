/*
 * Steady frame pacing: when the drawn frames cannot keep up, show every second
 * emulated frame on an exact even cadence instead of dropping frames at random;
 * go back to every frame only after a sustained stretch with headroom.
 *
 * The game itself always runs at its true rate (every frame is emulated, sound
 * and input are untouched); only which frames are drawn changes. The decision
 * is a pure function of recent frame costs, so it is unit-tested on Linux and
 * any player (or the main app) can use the same logic.
 *
 * What a "cost" is: the time one drawn frame needs from the frame's start to
 * its picture being handed to the display, in microseconds, as the player
 * measures it (for a player with a picture thread, the larger of the emulation
 * thread's work and the picture thread's work for that frame). In half mode
 * the player still measures the drawn frames, so the same numbers say whether
 * every frame would fit again.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_PACE_H
#define PK_PACE_H

#include <stdint.h>

namespace pk {

enum PaceMode { PACE_FULL = 0, PACE_HALF = 1 };

struct PaceConfig {
	uint32_t budget_us = 16683;     /* one display refresh (59.94 Hz) */
	int window_ms = 1000;           /* "the last second" looked at to go to half */
	int enter_over_pct = 10;        /* go to half when at least this share of the window's drawn frames were over budget... */
	int enter_avg_pct = 97;         /* ...or their average cost is at least this share of the budget */
	int min_samples = 20;           /* never decide on fewer drawn frames than this */
	int recover_ms = 3000;          /* back to every frame only after this long... */
	int recover_pct = 80;           /* ...with every drawn frame under this share of the budget */
	int recover_slips = 1;          /* frames over recover_pct tolerated in that stretch (one stray spike) */
	int min_half_ms = 2000;         /* stay in half at least this long once there */
};

/* one drawn frame: its cost and how many display refreshes it stands for (1 in full mode, 2 in half) */
struct PaceSample {
	uint32_t cost_us;
	uint8_t span;
};

/* The decision. `samples` are the most recent drawn frames, oldest first (the caller keeps at least
 * recover_ms worth). `ms_in_mode` is how long the current mode has lasted. Returns the mode for the
 * frames that follow. Pure: same input, same answer. */
PaceMode pace_decide(const PaceConfig &c, PaceMode current, const PaceSample *samples, int n, int ms_in_mode);

/* A small keeper of the history around pace_decide, for a player's main loop. */
class Pacer {
public:
	enum { HISTORY = 256 };
	Pacer() { reset(PACE_FULL); }
	void configure(const PaceConfig &c) { cfg_ = c; }
	const PaceConfig &config() const { return cfg_; }
	/* forget the history (after the menu, a load, a pause) and start in `m` */
	void reset(PaceMode m);
	/* Steady pacing switched off: always PACE_FULL. */
	void set_enabled(bool on) { enabled_ = on; if (!on) reset(PACE_FULL); }
	bool enabled() const { return enabled_; }
	/* Feed one drawn frame. Returns the mode now in force; *changed is set when it just changed. */
	PaceMode add(uint32_t cost_us, bool *changed = 0);
	PaceMode mode() const { return mode_; }
	int ms_in_mode() const { return ms_in_mode_; }
	/* counters for the log, cleared by take_counts() */
	struct Counts { int drawn, over, changes; uint32_t max_us; };
	Counts take_counts();
private:
	PaceConfig cfg_;
	PaceSample hist_[HISTORY];
	int head_, count_;
	PaceMode mode_;
	int ms_in_mode_;
	bool enabled_ = true;
	Counts counts_;
};

const char *pace_mode_name(PaceMode m);

}  // namespace pk

#endif
