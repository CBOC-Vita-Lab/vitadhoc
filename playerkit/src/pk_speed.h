/*
 * Fast forward and rewind for a player: the part that sits between the game loop and pk_ff / pk_rewind. The player gives it a
 * SpeedHost (save a state, load a state, run one frame) and calls, in this order:
 *
 *   input(fired, locked)       every emulated frame, with the Mapper's result (FIRE_FF_HELD / FIRE_FF_PRESS / FIRE_REWIND_HELD)
 *   n = plan(normal)           once per display refresh, before the frames: how many to run (normal * factor under fast forward; 1 while
 *                              rewinding, where the "frame" is a landing or a standing picture)
 *   run_frame(render)          for each of those frames, in place of the core's run: runs the frame, then hands the state to the ring
 *   audio(lr, n)               on what the core produced: mixed down under fast forward, dropped while rewinding or without sound
 *
 * While busy() (fast forward on, rewinding) the player leaves the steady pacer and the look governor alone and resets them when
 * take_ended() says it is over. Rewind and fast forward are switched off by `locked` (two players, online play, Hardcore): a press
 * then only gives the notice.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_SPEED_H
#define PK_SPEED_H

#include "pk_ff.h"
#include "pk_input.h"
#include "pk_rewind.h"

#include <string>

namespace pk {

class SpeedHost {
public:
	virtual ~SpeedHost() {}
	virtual size_t state_size() = 0;                           /* 0 = no game */
	virtual bool save_state(uint8_t *buf, size_t cap) = 0;     /* writes exactly state_size() bytes */
	virtual bool load_state(const uint8_t *buf, size_t n) = 0;
	virtual void run_frame(bool render) = 0;                   /* one emulated frame with the pads as they are */
	virtual void clear_pads() = 0;                             /* the next frame sees no button (a rewind landing) */
};

struct SpeedSettings {
	bool rewind_on = true;
	int rewind_seconds = 60;            /* 30 or 60 in the menu */
	size_t rewind_budget = 12u << 20;
	int ff_factor = 2;                  /* 2 or 3 */
	bool ff_sound = true;
	bool ff_toggle = false;
};

class Speed {
public:
	Speed() {}
	void set_host(SpeedHost *h) { host_ = h; }
	/* the game is loaded: opens the rewind ring when it is on. fps = the game's frame rate. */
	void start(double fps, int interval_min, const SpeedSettings &s);
	/* settings changed in the menu: fast forward changes at once, the rewind ring is rebuilt when its length or on/off changed (history gone) */
	void apply(const SpeedSettings &s);
	void stop();

	void set_locked(bool locked, const char *why = 0) { locked_ = locked; why_ = why; }
	/* Hardcore: rewinding is refused (a press gives `why`), fast forward stays */
	void set_rewind_locked(bool locked, const char *why = 0) { rw_locked_ = locked; rw_why_ = why; }
	bool rewind_locked() const { return rw_locked_; }
	bool locked() const { return locked_; }
	void input(uint32_t fired);
	int plan(int normal_frames);
	/* true when the core ran a frame (false: the picture on screen stands, rewinding between two steps) */
	bool run_frame(bool render);
	/* in place; returns the frames kept */
	size_t audio(int16_t *lr, size_t frames);

	bool ff_on() const { return ff_.on(); }
	bool rewinding() const { return rw_.active(); }
	bool busy() const { return ff_.on() || rw_.active(); }
	/* a frame of this refresh is the last one: draw it (fast forward draws one picture a refresh) */
	bool draw_this(int index, int count) const { return !ff_.on() || index == count - 1; }
	/* busy() went from true to false since the last call: reset the pacer and the look governor's window */
	bool take_ended();
	/* the chip text to show now (">> 2x", "<< REWIND 12 s"), "" when none */
	std::string chip() const;
	std::string take_notice();
	/* a state was loaded, the game was reset, a link began */
	void state_changed();

	/* the helper thread's step (nothing to do without one) */
	bool pack() { return rw_.pack_pending(); }
	Rewind &rewind() { return rw_; }
	const Rewind &rewind() const { return rw_; }
	FastForward &ff() { return ff_; }
	const SpeedSettings &settings() const { return set_; }
	std::string stats_line() const;

private:
	SpeedHost *host_ = 0;
	Rewind rw_;
	FastForward ff_;
	SpeedSettings set_;
	double fps_ = 60.0;
	int imin_ = 2;
	bool locked_ = false, rw_locked_ = false;
	const char *rw_why_ = 0;
	const char *why_ = 0;
	bool want_rw_ = false;
	RewindAct act_ = RW_PLAY;
	bool was_busy_ = false, ended_ = false;
	std::string notice_;
};

}  // namespace pk

#endif
