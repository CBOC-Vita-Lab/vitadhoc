/*
 * pk_cores: the Vita's fourth ("system") CPU core, used only when it is proven usable.
 *
 * Background (measured by our other ports on a console, assumed here, not proven by this module):
 *   - a user app normally gets 3 cores, affinity masks USER_0..USER_2 = 0x10000, 0x20000, 0x40000;
 *   - the 4th core, mask 0x80000, is usable by an app only when a kernel plugin (CapUnlocker) is
 *     installed; without it the affinity call fails, or the thread gets almost no CPU time;
 *   - the system may take the core back at any time.
 * So: probe once at start-up, plan thread placement from the verdict, watch the threads that live
 * on that core, and fall back to the three-core layout without a hitch when it goes away.
 *
 * The module has three independent parts; all are plain C++11, no mutex, no thread_local, no
 * pthread, so the same file builds for the Vita and for the Linux unit tests:
 *
 *   1. Cores::probe()        spin-share probe with retry, cached, re-probe API
 *   2. make_plan()           the core layout planner (pure function)
 *   3. Watchdog              heartbeat counters for the threads on core 3
 *
 * ---- 1. The probe ---------------------------------------------------------------------------
 * A probe thread is pinned to the system core and spins for ProbeConfig::spin_us (10 ms) of wall
 * time; it measures the share of that time it actually ran (time lost to gaps longer than
 * 100 us, i.e. being preempted or not scheduled, does not count). The share is in permille.
 *
 *   share >= usable_permille (60 %)   decided: usable. A core that works gives ~100 %; the
 *                                      worst usable-looking real case is a busy system thread
 *                                      taking a few ms of a 10 ms spin, so 60 % leaves room
 *                                      without accepting a core that is mostly absent.
 *   share <= unusable_permille (20 %) bad luck or not available: RETRY (one port saw 3 % once
 *                                      by chance and then ran a whole session on 3 cores).
 *   in between                        RETRY too (neither proof of a working core nor of a
 *                                      missing one; a flapping core is not worth a thread).
 *   probe_once() < 0 (affinity refused, thread not created or started)   FINAL, no retry.
 *
 * At most max_tries (6) attempts, gap_us (40 ms) apart ("probe N of 6" in the log). When all
 * attempts end below the bar: unusable. The verdict is cached for the session. Hysteresis: after
 * a resume from suspend, a core that was usable stays usable on a share >= hold_permille (40 %)
 * (a Schmitt trigger: it takes 60 % to get in and only a clear drop to leave), so one noisy
 * re-probe does not move every thread. After the watchdog tripped, the bar is the full 60 %.
 * The probe blocks the calling thread for ~10 ms when the core works and up to
 * max_tries * (spin + gap) = 300 ms when it does not: call it at start-up (before the worker
 * threads exist, so the measurement is clean), on resume, and from a menu or pause, never from
 * inside a frame.
 *
 * ---- 2. The layout planner ------------------------------------------------------------------
 * make_plan(request, core3) returns, for every thread role the player has, an affinity mask and
 * a Vita priority. Rules, in the order they win:
 *   R1  The emulating main thread is alone on core 0: no pinned role except EMU_MAIN uses core 0.
 *       (Roles with mask 0 float: the system default, as today in the main app. A floating
 *       role is never placed on the system core.)
 *   R2  Priorities stay inside 0x40 (highest) .. 0xBF (lowest) always; the "default priority"
 *       0x10000100 some old call sites pass equals 160 = 0xA0 here, and Slot::kprio keeps the
 *       exact value the call site uses today, so nothing changes without the plugin.
 *   R3  "Whole-core" roles (EMU_MAIN, RENDER, APU, WIDE_COMPOSE: required work that runs long or
 *       spins) never share a core with each other in the 4-core layout.
 *   R4  Audio out never shares a core with a spinning role (a role whose Slot::spins is set:
 *       the SNES player's APU and PPU threads spin-wait before they sleep).
 *   R5  Optional helpers (UPSCALE, WIDE_SCOUT, WRITER, NET_IO) are the lowest priority on any
 *       core they share (>= 0xA0, numerically greater than every required role there). They are
 *       work-stealers: nothing waits for them.
 *   R6  The writer / io / net roles are light and may go anywhere that is free; they are the
 *       first to be put on the extra core, and never owners of time-critical work.
 *   R7  Without the extra core (core3 == false) the layout is exactly today's per profile (see
 *       the table in pk_cores.cpp, frozen by a golden test) and plan requests for a third
 *       upscale helper are clamped to two. No mask ever contains the system core.
 * The layout with the extra core (4-core), per profile:
 *   CP_SNES / CP_NES: PPU(RENDER) core 1, APU core 2 (each whole), AUDIO_OUT, DRAW, WRITER on core 3
 *       (audio is light and runs at 0x40; it no longer shares with the APU spinner), WIDE_COMPOSE on
 *       core 3 when a PPU or APU role exists, else on core 1; WIDE_SCOUT on core 3 when an APU
 *       exists, else on core 2.
 *   CP_APP (the main app): RENDER on core 3 (the three upscale helpers' cores 1 and 2 are then
 *       free of it), UPSCALE helpers 0, 1 on cores 1, 2 as today, helper 2 on core 3 (it only runs
 *       while the render thread, which outranks it, is idle); AUDIO_OUT, WRITER, NET_IO float.
 *
 * ---- 3. The watchdog -----------------------------------------------------------------------
 * Every thread placed on the extra core registers a slot and calls beat() once per unit of work
 * (audio grain, strip of an upscale, composed frame). A slot that is "busy" (it should be making
 * progress: set_busy(slot, true) while it has work, false while it legitimately waits on a
 * semaphore) and has not beaten for stall_us, while the reference clock (ref_beat(), the
 * emulation loop's frame counter) has moved at least ref_min times since, is dead: the system
 * took the core back. poll() (call it about once a frame from the emulating thread) then reports
 * the slot, Cores::poll_watchdog() marks core 3 LOST (decision "lost"), and the caller re-plans
 * with plan() (now the three-core layout) and moves every role whose mask changed
 * (apply_slot() on the Vita does the affinity and priority change on a live thread). Work that
 * was queued for a helper on the lost core is taken by the other threads, which is why helpers
 * must be optional work-stealers; the roles that cannot be (audio out, the PPU/render thread)
 * are moved, and cost one glitch at most. Recovery: try_recover() re-probes at a quiet point
 * after recover_after_us (30 s), at most max_recoveries (2) times a session, so a core that
 * flaps is not trusted again and again.
 *
 * Threading: Cores and make_plan are used from one thread (the emulating thread). Only
 * Watchdog::beat / set_busy / ref_beat may be called from any thread.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_CORES_H
#define PK_CORES_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace pk {

enum {
	CORE_U0 = 0x10000, CORE_U1 = 0x20000, CORE_U2 = 0x40000, CORE_SYS = 0x80000,
	CORE_USER_ALL = 0x70000   /* what mask 0 ("leave it to the system") means */
};
enum { PRIO_HIGHEST = 0x40, PRIO_LOWEST = 0xBF, KPRIO_DEFAULT_USER = 0x10000100 };

/* ---- the platform: tiny, so a fake kernel drives the tests ------------------------------------ */
class CoreSys {
public:
	virtual ~CoreSys() {}
	virtual uint64_t now_us() = 0;                 /* monotonic clock, microseconds */
	virtual void sleep_us(uint32_t us) = 0;
	/* Create a thread pinned to the system core at priority prio, let it spin for spin_us of wall time and
	 * return the share of that time it ran, in permille (0..1000). Negative: the thread could not be created,
	 * pinned or started (affinity refused): a final answer. A thread that never gets any CPU returns 0 (the
	 * implementation must not wait for it for ever). Blocks until done. */
	virtual int probe_once(uint32_t spin_us, int prio) = 0;
};

/* The spin itself, portable: runs on the calling thread for spin_us of wall time and returns the share
 * (permille) of it spent running; a gap between two clock reads longer than gap_us counts as lost. */
int spin_share_permille(uint64_t (*now)(void *), void *ctx, uint32_t spin_us, uint32_t gap_us);

/* ---- 1. the probe ---------------------------------------------------------------------------- */
enum Core4Mode { CORE4_AUTO = 0, CORE4_OFF = 1 };            /* vita.ini / threads.ini: core4 = auto | off */
enum Decision {
	DEC_UNKNOWN = 0,
	DEC_USABLE,            /* a probe reached the bar */
	DEC_UNUSABLE_SHARE,    /* every attempt stayed below the bar (not there, or flapping) */
	DEC_REFUSED,           /* the affinity call or the thread was refused: final */
	DEC_DISABLED           /* core4 = off: nothing was probed */
};

struct ProbeConfig {
	uint32_t spin_us = 10000;
	int max_tries = 6;                 /* 1..8 */
	uint32_t gap_us = 40000;
	int usable_permille = 600;         /* >= : usable */
	int unusable_permille = 200;       /* <= : surely not there (still retried; used by tests and the log) */
	int hold_permille = 400;           /* hysteresis: the bar for a core that was usable (resume re-probe) */
	int probe_prio = 0x60;
	uint32_t gap_lost_us = 100;        /* spin_share_permille's gap threshold */
	uint32_t recover_after_us = 30u * 1000000u;
	int max_recoveries = 2;
};

enum { PROBE_MAX_TRIES = 8 };
struct ProbeResult {
	Decision decision = DEC_UNKNOWN;
	int tries = 0;
	int shares[PROBE_MAX_TRIES] = { 0 };   /* permille, per attempt (-1 = refused) */
	int final_permille = -1;               /* the last attempt's share */
};

/* ---- 2. the planner --------------------------------------------------------------------------- */
enum Role {
	R_EMU_MAIN = 0,    /* the emulating thread */
	R_RENDER,          /* render / PPU thread (SNES PPU worker, GBA line render thread) */
	R_APU,             /* sound chip thread */
	R_AUDIO_OUT,       /* the thread that feeds sceAudioOutOutput */
	R_UPSCALE,         /* xBRZ / EPX helpers, index 0..2 */
	R_WIDE_COMPOSE,    /* wide view composer */
	R_WIDE_SCOUT,      /* wide view look-ahead (second emulation) */
	R_DRAW,            /* frame hand-over draw / present thread */
	R_WRITER,          /* writer / io worker */
	R_NET_IO,          /* network io */
	R_COUNT
};
enum CoreProfile { CP_SNES = 0, CP_NES, CP_APP };

struct Request {
	CoreProfile profile;
	bool has[R_COUNT];
	int upscale_n;     /* helpers wanted (0..3); in the 3-core layout at most 2 are planned */
	explicit Request(CoreProfile p = CP_SNES) : profile(p), upscale_n(0) { for (int i = 0; i < R_COUNT; i++) has[i] = false; }
	Request &with(Role r) { has[r] = true; return *this; }
};

struct Slot {
	Role role;
	int index;          /* upscale helper number, else 0 */
	uint32_t mask;      /* affinity mask; 0 = leave to the system (USER_ALL) */
	int prio;           /* 0x40..0xBF */
	int kprio;          /* the exact value today's call site passes (0x10000100 for the default); 0 = leave unchanged */
	bool on_core3;      /* mask contains the system core */
	bool whole_core;    /* required work that runs long or spins (rule R3) */
	bool spins;         /* spin-waits before sleeping (rule R4) */
	bool optional;      /* a work-stealer nothing waits for (rule R5) */
};
enum { PLAN_MAX_SLOTS = 16 };
struct Plan {
	bool core3 = false;            /* the 4-core layout */
	int n = 0;
	Slot slots[PLAN_MAX_SLOTS];
	const Slot *find(Role r, int index = 0) const {
		for (int i = 0; i < n; i++) if (slots[i].role == r && slots[i].index == index) return &slots[i];
		return NULL;
	}
};
Plan make_plan(const Request &req, bool core3);

/* ---- 3. the watchdog -------------------------------------------------------------------------- */
struct WatchdogConfig {
	uint32_t stall_us = 150000;    /* a busy slot with no beat for this long, while the reference moves */
	uint32_t ref_min = 3;          /* the reference clock must have moved this many beats meanwhile */
};

class Watchdog {
public:
	enum { MAX_SLOTS = 8 };
	Watchdog() : n_(0) { reset(WatchdogConfig()); }
	void reset(const WatchdogConfig &c);
	/* register a thread; stall_us 0 = the config's. Returns the slot, or -1 when full. Not watched until watch(). */
	int add(const char *name, uint32_t stall_us = 0);
	void watch(int slot, bool on_core3);         /* only slots on the extra core are judged; starts a clean clock */
	void unwatch_all();
	void beat(int slot) { if (slot >= 0 && slot < MAX_SLOTS) s_[slot].beat.fetch_add(1, std::memory_order_relaxed); }
	void set_busy(int slot, bool busy) { if (slot >= 0 && slot < MAX_SLOTS) s_[slot].busy.store(busy, std::memory_order_relaxed); }
	void ref_beat() { ref_.fetch_add(1, std::memory_order_relaxed); }
	/* Judge every watched slot at time now_us; returns the first dead slot (it stops being watched), else -1. */
	int poll(uint64_t now_us);
	const char *name(int slot) const { return slot >= 0 && slot < n_ ? s_[slot].name : ""; }
private:
	struct S {
		std::atomic<uint32_t> beat;
		std::atomic<bool> busy;
		char name[16];
		uint32_t stall;
		bool used, watched;
		bool fresh;
		uint32_t seen, ref_seen;     /* poll state (polling thread only) */
		uint64_t since;
		S() : beat(0), busy(false), stall(0), used(false), watched(false), fresh(true), seen(0), ref_seen(0), since(0) { name[0] = 0; }
	};
	S s_[MAX_SLOTS];
	std::atomic<uint32_t> ref_;
	int n_;
	WatchdogConfig cfg_;
};

/* ---- the whole: probe + verdict + watchdog + census --------------------------------------------- */
typedef void (*CoresLogFn)(void *ctx, const char *line);

class Cores {
public:
	Cores(CoreSys *sys, Core4Mode mode = CORE4_AUTO, const ProbeConfig &cfg = ProbeConfig())
	    : sys_(sys), cfg_(cfg), mode_(mode), probed_(false), lost_(false), gen_(0), trips_(0), recoveries_(0),
	      lost_at_(0), log_(NULL), log_ctx_(NULL) {
		if (cfg_.max_tries < 1) cfg_.max_tries = 1;
		if (cfg_.max_tries > PROBE_MAX_TRIES) cfg_.max_tries = PROBE_MAX_TRIES;
	}
	void set_log(CoresLogFn fn, void *ctx) { log_ = fn; log_ctx_ = ctx; }

	/* The verdict: the first call probes (with retry), every later call returns the cached result. */
	const ProbeResult &probe();
	/* Probe again (forced). After a resume from suspend, see on_resume(). Returns whether core3_usable() changed. */
	bool reprobe();
	/* After a resume: re-probe with the hysteresis bar. Returns true when the layout must be re-planned and applied. */
	bool on_resume();

	/* usable now: proven by the probe and not lost to the watchdog; false in core4 = off */
	bool core3_usable() const { return probed_ && res_.decision == DEC_USABLE && !lost_; }
	bool lost() const { return lost_; }
	uint32_t generation() const { return gen_; }          /* +1 whenever core3_usable() changes: re-plan when it moved */
	const ProbeResult &result() const { return res_; }
	Core4Mode mode() const { return mode_; }

	Plan plan(const Request &req) const { return make_plan(req, core3_usable()); }

	/* watchdog: register slots, then per frame ref_beat() and poll_watchdog(now). */
	Watchdog &watchdog() { return wd_; }
	/* Returns true when a thread on the extra core died and core 3 is now marked lost: re-plan and move the roles. */
	bool poll_watchdog(uint64_t now_us);
	/* Outside evidence that the extra core is gone (a wait on a helper that never ends, a missed audio deadline):
	 * the same as a trip. Returns true when it changed the state (core 3 was in use). */
	bool mark_lost(const char *why, uint64_t now_us);
	/* At a quiet point (menu, pause): when lost, long enough ago and not too often, probe again.
	 * Returns true when core 3 is usable again (re-plan and apply). */
	bool try_recover(uint64_t now_us);

	int trips() const { return trips_; }
	int recoveries() const { return recoveries_; }
	const char *decision_text() const;
	/* One line for the log: "cores: probe 2/6 shares 3,97 % decision usable layout 4-core watchdog trips 0 recoveries 0" */
	void census_line(char *buf, size_t n) const;
	/* Settings / About: "Extra CPU core: in use" or "Extra CPU core: not available (needs the CapUnlocker plugin)" */
	const char *status_text() const;

private:
	void run_probe(bool hold);
	void logf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
	CoreSys *sys_;
	ProbeConfig cfg_;
	Core4Mode mode_;
	bool probed_, lost_;
	uint32_t gen_;
	int trips_, recoveries_;
	uint64_t lost_at_;
	ProbeResult res_;
	Watchdog wd_;
	CoresLogFn log_;
	void *log_ctx_;
};

/* ---- the Vita ---------------------------------------------------------------------------------- */
#ifdef __vita__
CoreSys *vita_core_sys();      /* the real kernel behind CoreSys (a static object) */
/* Put a live thread on its planned core and priority (mask 0 = USER_ALL). Returns the first negative kernel rc, else 0. */
int apply_slot(int thid, const Slot &s);
#endif

}  // namespace pk

#endif
