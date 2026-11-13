/*
 * The look governor: every game starts with the best look; when THIS game cannot hold a locked 60 with it, the
 * governor takes the least visible sacrifices first, remembers the result for the game, and does not flap.
 *
 * What it owns and what it does not:
 *  - It owns "Auto" rows only. A row the player set by hand is never touched (set_user_row).
 *  - It never changes the picture shape, the resolution of the game itself, or the speed of the game, and it never
 *    skips frames: that stays pk_pace's last resort. A governed look is a list of RUNGS ordered by how visible the
 *    sacrifice is; the player's frontend decides what a rung means (xBRZ 4x -> 3x, wide-view scout off, ...).
 *  - It does no file I/O and makes no thread of its own: the frontend feeds it one LookSecond per second of play,
 *    tells it about safe points, reads the result, and reads/writes the per-game memory text through its own safe
 *    write path (FORMATS.md, "Look memory").
 *
 * How it decides (all numbers are defaults in LookConfig):
 *  - A second is BAD when its load is at or over bad_load of a refresh, or when bad_miss of its refreshes showed an
 *    unplanned repeat, or the steady pacer is in half mode with little headroom. A step down is WANTED when
 *    down_s of the last 8 seconds were bad (seconds, never frames: a 70 ms hitch is one missed refresh and changes
 *    nothing). In the first start_grace_s seconds of a game down_start_s bad seconds in a row are enough, and the step is
 *    applied at once: the game is loading anyway, so the picture is steady from the first scene on.
 *  - A step is APPLIED at a safe point (game start, scene change, blank frames, menu close), at the next tick when
 *    the frontend can cross-fade (crossfade), or, for a step down only, after force_s with no safe point (a few
 *    seconds of misses cost more than a switch). A step up is never forced.
 *  - A step UP is tried only after the cool-down, only with no pacing in half mode, and only when the load the
 *    last seconds showed plus the learned cost of the rung (times a margin) stays under up_load. The cost of a
 *    rung is learned from when it was last in force (the load before and after it), starting from the guess the
 *    frontend gives. A climb that is followed by a step down within bounce_s is a failed climb: the cool-down
 *    doubles (up to cool_max_s). A climb that holds settle_s resets it.
 *  - Per game memory: the mask that cured the last overload, the learned costs and the cool-down. A game that held
 *    60 at the full look stores nothing.
 *
 * Portable: no platform calls, C++11, deterministic (no clock of its own: a "second" is one call to second()).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_LOOK_H
#define PK_LOOK_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace pk {

enum { LOOK_MAX_RUNGS = 12, LOOK_WINDOW = 8, LOOK_MIN_REFRESHES = 30 };

/* One sacrifice the governor may make. The ladder is the list of rungs, least visible first. */
struct LookRung {
	const char *name;     /* "xBRZ 4x -> 3x": for the log and the summary */
	int row;              /* the frontend's settings row this rung belongs to; a row set by hand protects all its rungs */
	float est_saving;     /* a guess: the share of a refresh this rung frees (0.12 = 12 %). Learned over time. */
	int visibility;       /* 1..100: how visible the sacrifice is. Must not decrease down the ladder. */
};

enum LookSafe { LOOK_SAFE_START = 0, LOOK_SAFE_SCENE, LOOK_SAFE_BLANK, LOOK_SAFE_MENU };

struct LookConfig {
	int down_s = 4;                 /* bad seconds out of the last LOOK_WINDOW that want a step down */
	int start_grace_s = 10;          /* after start(): this many seconds count as the start of the game ... */
	int down_start_s = 3;           /* ... and this many bad ones in a row apply a step down at once */
	int up_hold_s = 10;             /* the headroom for a climb must have held this long */
	int cool_s = 30;                /* least time since the last change before a climb is tried */
	int cool_max_s = 960;           /* the cool-down doubles after a failed climb, up to this */
	int bounce_s = 90;              /* a step down this soon after a climb is a failed climb */
	int settle_s = 180;             /* a climb that holds this long resets the cool-down */
	int force_s = 12;               /* a wanted step down with no safe point is applied after this */
	int forget_s = 600;             /* a remembered look that was not needed for this long at the full look is dropped */
	bool crossfade = false;         /* the frontend cross-fades a change: apply at the next tick, no safe point needed */
	float bad_load = 0.97f;         /* mean load (fraction of a refresh) at or over this: a bad second */
	float bad_miss = 0.10f;         /* share of the second's refreshes that missed: a bad second */
	float up_load = 0.80f;          /* a climb is tried only when the predicted load stays under this */
	float pace_recover = 0.80f;     /* pk_pace goes back to every frame only below this share of a refresh (PaceConfig::recover_pct):
	                                 * with pacing in half mode, a load at or over it is bad, so the look keeps coming down until pacing can recover */
	float up_margin = 1.15f;        /* the learned cost of the rung is multiplied by this before it is compared */
	float down_target = 0.80f;      /* a step down takes as many rungs as the learned costs say bring the load under this */
	int peak_hold_s = 600;          /* the worst load seen at a level keeps a climb out for this long */
};

/* One second of play, from the frontend's own counters. All costs are mean fractions of one refresh (1.0 = 16.68 ms). */
struct LookSecond {
	float emu = 0;          /* the emulating thread's work per emulated frame */
	float draw = 0;         /* drawing, composing and presenting (without the filter) */
	float filter = 0;       /* the scaling filter's critical-path time (xBRZ ...) */
	int refreshes = 60;     /* refreshes in this second (a menu, a pause or a load gives fewer: below
	                         * LOOK_MIN_REFRESHES the second is ignored) */
	int missed = 0;         /* refreshes that showed a repeated picture because the new one was NOT ready.
	                         * Planned repeats (steady half mode, a 50 Hz game) are not misses. */
	bool parallel = false;  /* hand-over active: emulation and drawing overlap (load = the larger) */
	bool pace_half = false; /* pk_pace is in half mode */
};

enum LookMemAction { LOOK_MEM_NONE = 0, LOOK_MEM_WRITE, LOOK_MEM_DELETE };

class LookGovernor {
public:
	LookGovernor();
	LookGovernor(const LookGovernor &) = delete;
	LookGovernor &operator=(const LookGovernor &) = delete;

	/* ---- set up, before start() ---- */
	void configure(const LookConfig &c) { cfg_ = c; }
	const LookConfig &config() const { return cfg_; }
	/* false (and no ladder) when n is out of range or the visibility ever decreases down the ladder */
	bool set_ladder(const LookRung *r, int n);
	int rungs() const { return n_; }
	const LookRung &rung(int i) const { return rung_[i]; }
	/* the memory text read from the game's file. false: rejected (another ladder, damaged) and the governor starts
	 * from the guesses. mhz is the CPU clock now; a memory made at another clock gives its costs, not its mask. */
	bool load_memory(const char *text, size_t n, int mhz);
	/* a settings row set by hand (true) or back on Auto (false); safe at any time, also in play */
	void set_user_row(int row, bool user);
	bool user_row(int row) const;
	/* the first frame of the game: the remembered look is applied (a change; poll generation()) */
	void start();

	/* ---- every second of play ---- */
	/* Returns true when the applied look changed. */
	bool second(const LookSecond &s);
	/* The frontend reports a safe point. Returns true when the applied look changed. */
	bool safe_point(LookSafe kind);
	/* the menu, a pause or a load stopped the clock: forget the last seconds, keep everything learned */
	void reset_window();

	/* ---- what to show ---- */
	uint32_t generation() const { return gen_; }          /* +1 for every change of the applied look */
	uint32_t mask() const { return applied_; }            /* bit i: rung i is applied */
	bool applied(int rung) const { return rung >= 0 && rung < n_ && ((applied_ >> rung) & 1u) != 0; }
	int level() const;                                    /* number of rungs applied */
	int steps_on_row(int row) const;                      /* rungs applied on this row: the row's value is its best minus this */
	bool eased(int row) const { return steps_on_row(row) > 0; }
	int pending() const { return pending_; }              /* -1 a step down waits for a safe point, +1 a step up, 0 none */

	/* ---- the log ---- */
	/* One line per second of play (call after second()); false when there is nothing yet. */
	bool census_line(char *buf, size_t n) const;
	/* One line at the end of the game: time at each level, steps taken and why. */
	std::string summary() const;
	/* the reasons, one line per change (oldest first, the last 32) */
	const std::vector<std::string> &events() const { return events_; }
	int downs() const { return downs_; }
	int ups() const { return ups_; }
	int rungs_down() const { return rungs_down_; }
	int changes() const { return downs_ + ups_; }
	int forced() const { return forced_; }
	int failed_climbs() const { return failed_climbs_; }
	int seconds() const { return now_; }
	int seconds_at_level(int l) const { return l >= 0 && l <= LOOK_MAX_RUNGS ? sec_at_[l] : 0; }
	float saving(int rung) const { return rung >= 0 && rung < n_ ? saving_[rung] : 0.f; }
	int cool_now_s() const { return cool_cur_; }
	float last_load() const { return last_load_; }

	/* ---- the game's memory ---- */
	LookMemAction memory_action() const;
	/* the text to write when memory_action() is LOOK_MEM_WRITE */
	std::string serialize(int mhz) const;

	static float load_of(const LookSecond &s);

private:
	void apply_down(const char *why, bool forced);
	void apply_up(const char *why);
	int next_down_rung() const;
	int next_up_rung() const;
	void learn_tick(float load);
	void event(const char *what, const char *why, float load);
	void clear_window();
	uint32_t ladder_hash() const;

	LookConfig cfg_;
	LookRung rung_[LOOK_MAX_RUNGS];
	int n_ = 0;
	float saving_[LOOK_MAX_RUNGS];
	int user_row_ids_[16];           /* settings rows that are by hand */
	int n_user_ = 0;
	std::string names_[LOOK_MAX_RUNGS];
	uint32_t applied_ = 0;
	uint32_t hold_mask_ = 0;         /* the look that cured the last overload: what the memory stores */
	uint32_t gen_ = 0;
	int now_ = 0;                    /* seconds of play counted */
	int start_at_ = 0;
	bool started_ = false;
	/* window */
	float wload_[LOOK_WINDOW];
	uint8_t wbad_[LOOK_WINDOW];
	int wn_ = 0, whead_ = 0;
	int fit_run_ = 0;                /* consecutive seconds the next climb would fit */
	int bad_run_ = 0;
	int good_run_ = 0;
	/* decisions */
	int pending_ = 0;
	int pending_since_ = 0;
	int since_change_ = 0;
	int cool_cur_ = 0;
	int last_up_at_ = -100000;
	bool last_change_up_ = false;
	int up_rung_last_ = -1;
	int probe_since_ = -1;           /* when the last climb happened, while it has not settled */
	/* learning */
	uint32_t learn_mask_ = 0;
	int learn_dir_ = 0;
	int learn_wait_ = 0;
	float learn_pre_ = 0, learn_acc_ = 0;
	int learn_n_ = 0;
	/* the counts */
	int downs_ = 0, ups_ = 0, forced_ = 0, failed_climbs_ = 0, rungs_down_ = 0;
	float peak_[LOOK_MAX_RUNGS + 1];       /* the worst 4 s load seen at each level */
	int peak_at_[LOOK_MAX_RUNGS + 1];      /* when */
	int sec_at_[LOOK_MAX_RUNGS + 1];
	std::vector<std::string> events_;
	float last_load_ = 0, last_emu_ = 0, last_draw_ = 0, last_filter_ = 0, last_miss_ = 0;
	bool last_parallel_ = false, last_half_ = false;
	/* memory */
	bool mem_loaded_ = false;
	bool mem_needed_ = false;        /* a step down cured an overload this session (or the memory said so) */
	int mem_cool_ = 0;
	bool mem_mismatch_ = false;      /* the memory was made at another CPU clock: its mask is not applied */
	bool mem_dirty_ = false;         /* this session learned something worth writing */
	uint32_t mem_mask_ = 0;
	float mem_peak_ = 0;
};

/* "Auto (xBRZ 4x)" / "Auto (xBRZ 3x, eased)": the label of a governed row, with the current choice visible. */
std::string look_auto_label(const std::string &current, bool eased);

}  // namespace pk

#endif
