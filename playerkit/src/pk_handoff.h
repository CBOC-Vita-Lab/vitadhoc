/*
 * Frame hand-over: a small bounded mailbox between the thread that EMULATES (and plays the sound, reads
 * the pad) and a thread that FILTERS, COMPOSES and PRESENTS pictures.
 *
 * Why: when the per-picture drawing work (a scaler such as xBRZ, the wide-view compose, later anti
 * aliasing) comes close to one screen refresh, doing emulation and drawing on one thread makes every
 * pass longer than a refresh. With a second thread the two overlap and a few pictures "in flight" soak
 * up the wobble in either one. The game itself is never slowed: emulation and sound stay at true speed.
 *
 * Contract, in short:
 *  - The emulating thread (the "producer") asks for a buffer with begin_write(), fills it, and commit()s
 *    it. A buffer is owned by exactly one side at a time (producer filling it, the queue, the drawing
 *    thread, or the free pool): nothing is ever read while it is written.
 *  - The drawing thread (the "consumer") take()s the oldest picture (the newest when dropping is on),
 *    draws and presents it, and release()s it with the time its work took.
 *  - IN FLIGHT = committed and not yet released. The gate: in the normal ("full") mode begin_write waits
 *    while in-flight >= depth, so no picture is ever dropped and none is built needlessly early. When
 *    steady pacing (pk_pace) is in its half mode, or fast-forward / rewind is on, nothing waits and the
 *    NEWEST picture wins: older queued pictures are dropped (counted).
 *  - Depth is adaptive (DepthGovernor): the full depth absorbs wobble; when the work stays over budget the
 *    queue only adds delay, so it shrinks, and it comes back after sustained headroom (sticky).
 *  - flush() / discard() give the producer a quiet drawing thread (pause, menu, thumbnails, loading a
 *    state, rewind, leaving); set_enabled(false) turns the whole thing off (two-player lockstep): then
 *    begin_write() returns NULL and the host draws inline as it did before.
 *  - census_line() gives the once-a-second log line proving the feature acted, with emulated time against
 *    wall time so lost time is visible.
 *
 * Portable: no threads, mutexes or thread-local storage of its own. Waiting uses the small HandoffSys
 * interface (semaphores and a clock); on the console it is sceKernel semaphores, on Linux pk_handoff_posix.h.
 * Short critical sections use pk::SpinLock (std::mutex and pthread TLS are unreliable on the console).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_HANDOFF_H
#define PK_HANDOFF_H

#include "pk_pace.h"
#include "pk_util.h"
#include <stdint.h>
#include <stddef.h>

namespace pk {

/* ---- the platform's part ---------------------------------------------------- */
class HandoffSys {
public:
	virtual ~HandoffSys() {}
	virtual uint64_t now_us() = 0;
	/* a counting semaphore starting at 0 */
	virtual void *sem_create() = 0;
	virtual void sem_destroy(void *s) = 0;
	virtual void sem_post(void *s) = 0;
	/* true when taken; false on timeout. timeout_us == 0xFFFFFFFF waits without limit */
	virtual bool sem_wait(void *s, uint32_t timeout_us) = 0;
};

/* ---- adaptive depth ----------------------------------------------------------- */
struct DepthConfig {
	int max_depth = 2;           /* pictures in flight when there is headroom (1..HANDOFF_MAX_DEPTH): the model's choice */
	int min_depth = 1;           /* ... and when the work stays over budget */
	uint32_t budget_us = 16683;  /* one refresh */
	int window = 24;             /* pictures averaged */
	int shrink_pct = 100;        /* mean cost at or over this share of the budget ... */
	int restore_pct = 88;        /* ... and back only at or under this share (a gap between: no change) */
	int hold_ms = 1000;          /* either condition must last this long (in refreshes of game time) */
	int cool_ms = 4000;          /* least time at the small depth before a restore ... */
	int cool_max_ms = 32000;     /* ... doubled up to this each time a restore is followed by a shrink within bounce_ms */
	int bounce_ms = 6000;
};

/* Pure bookkeeping: feed the cost of each picture (the larger of the emulating thread's work and the
 * drawing thread's work for it) and how many refreshes it stood for (1 full, 2 half). */
class DepthGovernor {
public:
	enum { MAX_WINDOW = 64 };
	DepthGovernor() { configure(DepthConfig()); }
	void configure(const DepthConfig &c);
	const DepthConfig &config() const { return cfg_; }
	void reset();                       /* back to max_depth, history forgotten */
	int add(uint32_t cost_us, int span);   /* returns the depth now in force */
	int depth() const { return depth_; }
	int changes() const { return changes_; }
	/* wrong cost figures cost speed at worst: the rule only ever moves between min and max */
private:
	DepthConfig cfg_;
	uint32_t ring_[MAX_WINDOW];
	int head_, n_;
	uint64_t sum_;
	int depth_;
	int64_t now_ms10_;                  /* game time, tenths of a millisecond */
	int64_t over_ms10_, under_ms10_;
	int64_t last_shrink_ms10_, last_restore_ms10_;
	int cool_cur_ms_;
	int changes_;
};

/* ---- the mailbox ---------------------------------------------------------------- */
enum { HANDOFF_MAX_DEPTH = 4 };

struct HandoffFrame {
	uint8_t *data;          /* frame_bytes of storage, owned by the Handoff */
	uint32_t cap;
	int w, h, pitch;        /* the host's description of what it wrote */
	uint32_t seq;           /* the host's emulated frame number */
	uint32_t emu_us;        /* the emulating thread's work for this picture (set by the host before commit) */
	int user;               /* anything else the host wants to carry */
	/* set by the Handoff */
	uint8_t span;           /* refreshes this picture stands for (1 full, 2 half) */
	uint64_t made_us;
};

struct HandoffConfig {
	uint32_t frame_bytes = 0;      /* the largest picture */
	DepthConfig depth;
	bool adaptive = true;          /* false: depth stays at depth.max_depth */
	uint32_t refresh_us = 16683;
	uint32_t frame_us = 16683;     /* one emulated frame of the game, for the emulated-time counter */
	uint32_t gate_timeout_us = 250000;   /* a drawing thread that does not answer this long is not waited for */
};

struct HandoffCensus {
	uint32_t produced, shown, dropped, repeated, gate_waits, gate_timeouts;
	int depth, depth_max;
	bool half, enabled;
	int64_t emu_ms, wall_ms;       /* emulated game time and wall time since the baseline */
	uint32_t depth_changes;
};

class Handoff {
public:
	Handoff();
	~Handoff();
	/* allocates the buffers. false on bad input. */
	bool init(const HandoffConfig &c, HandoffSys *sys);
	/* wakes both sides, which then get NULL; safe to call twice */
	void shutdown();
	bool active() const { return inited_; }
	int depth() const;

	/* ---- producer (the emulating thread) ---- */
	/* A buffer to fill. Waits at the gate in full mode. NULL when disabled, shut down, or not initialised:
	 * then the host draws inline. */
	HandoffFrame *begin_write();
	HandoffFrame *try_begin_write();       /* never waits: NULL also when the gate is closed */
	void commit(HandoffFrame *f);          /* queue it (the host has set w, h, pitch, seq, emu_us) */
	void abandon(HandoffFrame *f);         /* give it back unused */
	/* n emulated frames just ran (all of them, drawn or not): feeds the emulated-vs-wall counter */
	void note_emulated(uint32_t n);
	/* the pace mode now in force (call each pass): half = newest wins, nothing waits */
	void set_pace_mode(PaceMode m);
	/* fast-forward and rewind: nothing waits, newest wins */
	void set_force_drop(bool on);
	/* the cost (us) of the next finished picture, for Pacer::add; false when none is new */
	bool poll_cost(uint32_t *cost_us);
	/* wait until nothing is queued or being drawn; the drawing thread is then idle (it touches nothing of the
	 * display) until the next commit. Use before a menu, a pause, a thumbnail, a state load, leaving.
	 * false after timeout_us (0xFFFFFFFF: no limit) or a shutdown. */
	bool flush(uint32_t timeout_us = 2000000);
	/* drop what is queued, wait for the picture in the drawing thread's hands, then it is idle (rewind, a state load) */
	bool discard(uint32_t timeout_us = 2000000);
	/* false: flush, then run without a mailbox (begin_write returns NULL). true: back. */
	void set_enabled(bool on);
	bool enabled() const;
	/* After flush(): the last picture shown (the one on screen), valid until the next release; NULL when none. */
	const HandoffFrame *last_shown() const;
	/* The game time lost or gained is counted from here on (after a menu, a pause, a load: time that was
	 * stopped on purpose is not lost time). */
	void time_baseline();

	/* ---- consumer (the drawing and presenting thread) ---- */
	/* The next picture (newest when dropping is on). Waits when none. NULL on shutdown. */
	HandoffFrame *take();
	HandoffFrame *try_take();
	/* done: drawn and handed to the display. draw_us = the drawing thread's work for it */
	void release(HandoffFrame *f, uint32_t draw_us);

	/* ---- census ---- */
	HandoffCensus census() const;
	/* Once a second while active: writes the log line into buf and returns true; otherwise false. */
	bool census_line(uint64_t now_us, char *buf, size_t n);

private:
	struct Impl;
	Impl *p_;
	bool inited_;
	Handoff(const Handoff &);
	Handoff &operator=(const Handoff &);
};

}  // namespace pk

#endif
