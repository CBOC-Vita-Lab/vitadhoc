/*
 * Rewind: hold a button and the game runs backwards.
 *
 * HOW IT IS KEPT. Every few emulated frames the player takes the machine's save state (the container the core writes: about 145 KB
 * for a Mega Drive, a few hundred KB for a Super NES). Only the NEWEST state is kept whole. Every older one exists as the difference
 * to the one after it, the two states XORed, which is nearly all zeros, with the zero runs squeezed out (delta_encode), in one ring of
 * bytes of a fixed size. Going back one step is XORing the newest difference into the newest state, which turns it into the one before.
 * When the ring is full the oldest difference is dropped: only the oldest moment is lost.
 *
 * HOW FAR BACK. The player asks for a length in seconds (60 at most in the shipped players). The ring is a budget, not a promise, so
 * the INTERVAL between states follows from it: as short as `interval_min` frames while the differences are small, longer (up to
 * `interval_max`) when a busy game makes them big, so that the length asked for fits. A game too busy even for that has less history;
 * history_frames() says how far back the player really is.
 *
 * WHAT A STEP COSTS THE GAME. The emulation thread asks capture_begin() after every emulated frame: usually it gets NULL (not due),
 * once per interval it gets a buffer, writes the state into it and calls capture_commit(). That is all it does: the XOR and the
 * squeeze run in pack_pending(), which the platform calls on a helper thread when it has one (set_async(true) + wake callback), or
 * which capture_commit() runs itself when it has not. A state that is still waiting for the helper when the next one is due is not
 * queued twice: the capture is put off by a frame (counted in `skipped`).
 *
 * THE SESSION (refresh()). While the control is held the game does not run: once per display refresh refresh() says what to do. The
 * first landing is the newest stored state (up to an interval ago), then the game goes back faster the longer the control is held
 * (2 frames per refresh for the first second, 4, 8, then 16: the same ramp the main app has). Each landing is: load cur(), run ONE
 * frame with no buttons and no sound, show it. When the control is let go the game carries on from the frame on screen; the stored
 * history after that point is gone (it was another timeline). At the oldest stored moment a notice says so.
 *
 * Portable C++ with no console calls: the Vita players and the Linux twins run this same code.
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_REWIND_H
#define PK_REWIND_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace pk {

/* ---- the delta codec ----------------------------------------------------------------------------------------------------
 * The stream is a list of records: [skip][count][count XOR bytes] with skip and count as LEB128 numbers (7 bits a byte, low first),
 * where `skip` zero (equal) bytes come first, then `count` bytes of XOR. Runs of equal bytes shorter than 4 inside a changed stretch
 * stay in the literal (a record costs two bytes at least). The tail of equal bytes is not written. Two equal buffers encode to nothing
 * (length 0). Never longer than delta_bound(n). The same stream turns either buffer into the other (XOR is its own inverse). */
size_t delta_bound(size_t n);
/* Returns the length written (0 for two equal buffers), or (size_t) -1 when `cap` is too small (never when cap >= delta_bound(n)). */
size_t delta_encode(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *out, size_t cap);
/* buf ^= delta. false when the stream is damaged or reaches past n (buf may then be half changed). */
bool delta_apply(uint8_t *buf, size_t n, const uint8_t *d, size_t len);

struct RewindConfig {
	size_t state_bytes = 0;        /* one whole state (every state of a game has this size) */
	int seconds = 60;              /* history asked for */
	size_t budget_bytes = 12u << 20;   /* everything allocated: two whole states, the ring and the index */
	double fps = 60.0;             /* the game's frame rate (seconds -> frames) */
	int interval_min = 2;          /* frames between states at best */
	int interval_max = 30;         /* never further apart than this: where the game continues from is that coarse */
};

struct RewindStats {
	bool on = false;
	unsigned seconds = 0;          /* asked for */
	size_t ring_bytes = 0, total_bytes = 0, state_bytes = 0;
	unsigned interval = 0;         /* frames between states, now */
	unsigned entries = 0;          /* steps held */
	unsigned frames = 0;           /* emulated frames they span */
	size_t used_bytes = 0;         /* ring bytes they occupy */
	unsigned captures = 0;         /* states handed over */
	unsigned skipped = 0;          /* captures put off a frame: the helper was still on the one before */
	unsigned dropped = 0;          /* steps that fell off the old end */
	uint64_t raw_bytes = 0, packed_bytes = 0;
	unsigned rewinds = 0, steps_back = 0, landings = 0;
	uint64_t pack_us = 0;          /* total time packing (when a clock was given) */
	unsigned pack_max_us = 0;
};

enum RewindAct {
	RW_PLAY = 0,       /* not rewinding: run the game */
	RW_LAND,           /* load cur(), run ONE frame with no buttons and no sound, show it */
	RW_STAND           /* between two steps (or at the oldest moment): the picture on screen stands, no frame runs */
};

class Rewind {
public:
	Rewind();
	~Rewind();

	/* Allocate for a game. false: the budget is too small for two states and a useful ring, or no memory (then every call below does
	 * nothing and refresh() says why). Call close() or open() again to change the settings (the history is then gone). */
	bool open(const RewindConfig &c);
	void close();
	bool is_open() const { return open_; }
	/* Forget the history (a state was loaded, the game was reset, a link began): "a moment ago" is another timeline. */
	void clear();
	/* The helper thread takes the packing: capture_commit() then only queues the state and calls `wake` (may be NULL: the thread polls).
	 * Set it once the thread runs; clear it (false) AFTER the thread has stopped: what it had not finished is finished by the caller. */
	void set_async(bool on, void (*wake)(void *), void *wake_arg);
	/* A clock in microseconds (stats only; may be NULL). */
	void set_clock(uint64_t (*now_us)(void)) { clock_ = now_us; }

	/* ---- the emulation thread, after every emulated frame of ordinary play (fast forward included) ---- */
	/* NULL when no state is due; otherwise a buffer of state_bytes to write the state into, then call capture_commit(). */
	uint8_t *capture_begin();
	/* `bytes` = what was written (must be state_bytes; anything else abandons the capture). */
	void capture_commit(size_t bytes);

	/* ---- the helper thread (or the emulation thread when there is none) ---- */
	/* Packs the queued state into the ring. true = it did. */
	bool pack_pending();
	/* a state is queued and not yet packed */
	bool pending() const { return pend_state_.load(std::memory_order_acquire) != 0; }
	/* Emulation thread: returns when nothing is queued or being packed (it packs a queued one itself). */
	void wait_idle();

	/* ---- the session (emulation thread), once per display refresh while the game would run ---- */
	/*   want   the rewind control is held
	 *   locked rewinding is not possible now (a link session, two players, Hardcore): a press then only produces a notice() */
	RewindAct refresh(bool want, bool locked, const char *lock_notice = 0);
	bool active() const { return active_; }
	/* The state to load for a RW_LAND (valid until the next refresh()). */
	const uint8_t *cur() const { return cur_; }
	size_t cur_size() const { return cfg_.state_bytes; }
	/* the player ran the landing frame: the history ends at the state just loaded, `1` frame has passed since */
	void landed();
	/* Something to tell the player, once ("Rewind is off while games are linked", "This is as far back as rewind goes", ...); "" = none */
	std::string take_notice();
	/* The chip text while rewinding ("<< REWIND 12 s" or "<< REWIND 12 s (oldest)"); "" when not rewinding. */
	std::string chip() const;
	/* seconds the game is behind now (while rewinding) */
	unsigned back_seconds() const;

	/* ---- numbers ---- */
	unsigned history_frames() const { return frames_total_; }
	unsigned entries() const { return count_; }
	unsigned interval() const { return interval_; }
	size_t state_bytes() const { return cfg_.state_bytes; }
	RewindStats stats() const;
	const RewindConfig &config() const { return cfg_; }

	/* the length of a delta that goes back (tests): oldest first */
	unsigned entry_frames(unsigned i) const;

private:
	struct Ent { uint32_t off, len, frames; };
	bool ring_reserve(size_t len, size_t *off);
	void ring_push(const uint8_t *d, size_t len, unsigned frames);
	void pop_newest();
	void drop_oldest();
	void tune_interval();
	void end_session(const char *why);
	bool step_back();
	unsigned speed() const;

	RewindConfig cfg_;
	std::atomic<bool> open_;
	bool nomem_ = false;
	uint8_t *cur_ = 0;             /* the newest packed state, whole */
	uint8_t *pend_ = 0;            /* the state written by the game thread, waiting to be packed */
	uint8_t *ring_ = 0;
	size_t ring_cap_ = 0;
	std::vector<Ent> ents_;        /* circular index, oldest at first_ */
	unsigned first_ = 0, count_ = 0;
	size_t head_ = 0;              /* where the next delta goes */
	unsigned frames_total_ = 0;
	std::vector<uint8_t> tmp_;     /* encoder scratch (delta_bound) */
	bool have_cur_ = false;
	std::atomic<int> pend_state_;  /* 0 empty, 1 queued, 2 being packed */
	bool async_ = false;
	void (*wake_)(void *) = 0;
	void *wake_arg_ = 0;
	uint64_t (*clock_)(void) = 0;
	unsigned interval_ = 2;
	unsigned since_ = 0;           /* frames since the newest stored state */
	unsigned pend_frames_ = 0;     /* frames from the state before to the queued one */
	double ema_pack_ = 0;          /* average bytes of a stored step */
	/* session */
	bool active_ = false, prev_want_ = false, first_land_ = false, at_oldest_ = false, told_oldest_ = false;
	bool locked_ = false;
	unsigned held_ = 0, credit_ = 0, tl_back_frames_ = 0;
	std::string notice_;
	RewindStats st_;
};

}  // namespace pk

#endif
