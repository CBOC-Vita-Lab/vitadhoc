/* pk_cores: see pk_cores.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_cores.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#endif

namespace pk {

/* ---- the spin ---------------------------------------------------------------------------------- */

int spin_share_permille(uint64_t (*now)(void *), void *ctx, uint32_t spin_us, uint32_t gap_us)
{
	const uint64_t t0 = now(ctx);
	uint64_t last = t0, t = t0, lost = 0;
	do {
		t = now(ctx);
		const uint64_t d = t - last;
		if (d > gap_us) lost += d;
		last = t;
	} while (t - t0 < spin_us);
	const uint64_t total = t - t0;
	if (total == 0) return 0;
	if (lost > total) lost = total;
	return (int) (((total - lost) * 1000u) / total);
}

/* ---- the probe ---------------------------------------------------------------------------------- */

void Cores::logf(const char *fmt, ...)
{
	if (!log_) return;
	char line[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	log_(log_ctx_, line);
}

void Cores::run_probe(bool hold)
{
	res_ = ProbeResult();
	probed_ = true;
	if (mode_ == CORE4_OFF) {
		res_.decision = DEC_DISABLED;
		logf("cores: extra core off (setting core4 = off), not probed");
		return;
	}
	const int bar = hold ? cfg_.hold_permille : cfg_.usable_permille;
	Decision d = DEC_UNKNOWN;
	for (int t = 1; t <= cfg_.max_tries; t++) {
		const int s = sys_->probe_once(cfg_.spin_us, cfg_.probe_prio);
		res_.tries = t;
		res_.shares[t - 1] = s < 0 ? -1 : s;
		res_.final_permille = s < 0 ? -1 : s;
		if (s < 0) {
			logf("cores: probe %d of %d: affinity refused (rc %d), final", t, cfg_.max_tries, s);
			d = DEC_REFUSED;
			break;
		}
		logf("cores: probe %d of %d: share %d %% of a %u ms spin%s", t, cfg_.max_tries, s / 10, (unsigned) (cfg_.spin_us / 1000),
		    s >= bar ? " (usable)" : s <= cfg_.unusable_permille ? " (too low, retry)" : " (in between, retry)");
		if (s >= bar) { d = DEC_USABLE; break; }
		if (t < cfg_.max_tries) sys_->sleep_us(cfg_.gap_us);
	}
	res_.decision = d != DEC_UNKNOWN ? d : DEC_UNUSABLE_SHARE;
}

const ProbeResult &Cores::probe()
{
	if (!probed_) {
		run_probe(false);
		if (core3_usable()) gen_++;
	}
	return res_;
}

bool Cores::reprobe()
{
	const bool before = core3_usable();
	run_probe(false);
	lost_ = false;                       /* a fresh verdict replaces the watchdog's */
	const bool now = core3_usable();
	if (before != now) gen_++;
	return before != now;
}

bool Cores::on_resume()
{
	if (mode_ == CORE4_OFF) return false;
	if (!probed_) { probe(); return core3_usable(); }
	const bool before = core3_usable();
	const bool hold = before;
	const bool was_lost = lost_;
	run_probe(hold);
	if (res_.decision == DEC_USABLE) lost_ = false;
	else if (was_lost) lost_ = true;
	const bool now = core3_usable();
	if (before != now) gen_++;
	return before != now;
}

bool Cores::poll_watchdog(uint64_t now_us)
{
	if (!core3_usable()) return false;
	const int s = wd_.poll(now_us);
	if (s < 0) return false;
	char why[48];
	snprintf(why, sizeof why, "watchdog: '%s' made no progress", wd_.name(s));
	return mark_lost(why, now_us);
}

bool Cores::mark_lost(const char *why, uint64_t now_us)
{
	if (!core3_usable()) return false;
	lost_ = true;
	trips_++;
	gen_++;
	lost_at_ = now_us;
	wd_.unwatch_all();
	logf("cores: %s; core 3 marked lost, back to three cores (trip %d)", why ? why : "lost", trips_);
	return true;
}

bool Cores::try_recover(uint64_t now_us)
{
	if (!lost_ || recoveries_ >= cfg_.max_recoveries) return false;
	if (now_us - lost_at_ < cfg_.recover_after_us) return false;
	recoveries_++;
	run_probe(false);
	if (res_.decision == DEC_USABLE) {
		lost_ = false;
		gen_++;
		logf("cores: recovered: core 3 usable again (recovery %d of %d)", recoveries_, cfg_.max_recoveries);
		return true;
	}
	lost_ = true;
	lost_at_ = now_us;
	return false;
}

const char *Cores::decision_text() const
{
	if (lost_) return "lost (watchdog)";
	switch (res_.decision) {
	case DEC_USABLE: return "usable";
	case DEC_UNUSABLE_SHARE: return "unusable (share too low)";
	case DEC_REFUSED: return "unusable (affinity refused)";
	case DEC_DISABLED: return "off (setting)";
	default: return "not probed";
	}
}

void Cores::census_line(char *buf, size_t n) const
{
	char sh[96];
	sh[0] = 0;
	size_t o = 0;
	for (int i = 0; i < res_.tries && o + 8 < sizeof sh; i++) {
		if (res_.shares[i] < 0) o += snprintf(sh + o, sizeof sh - o, "%srefused", i ? "," : "");
		else o += snprintf(sh + o, sizeof sh - o, "%s%d", i ? "," : "", res_.shares[i] / 10);
	}
	snprintf(buf, n, "cores: probe %d of %d, share %s%s, decision %s, layout %s, watchdog trips %d, recoveries %d",
	    res_.tries, cfg_.max_tries, res_.tries ? sh : "-", res_.tries ? " %" : "", decision_text(),
	    core3_usable() ? "4-core" : "3-core", trips_, recoveries_);
}

const char *Cores::status_text() const
{
	if (core3_usable()) return "Extra CPU core: in use";
	if (mode_ == CORE4_OFF) return "Extra CPU core: off (setting)";
	if (lost_) return "Extra CPU core: not available (the system took it back)";
	return "Extra CPU core: not available (needs the CapUnlocker plugin)";
}

/* ---- the watchdog -------------------------------------------------------------------------------- */

void Watchdog::reset(const WatchdogConfig &c)
{
	cfg_ = c;
	n_ = 0;
	ref_.store(0, std::memory_order_relaxed);
	for (int i = 0; i < MAX_SLOTS; i++) {
		s_[i].beat.store(0, std::memory_order_relaxed);
		s_[i].busy.store(false, std::memory_order_relaxed);
		s_[i].used = s_[i].watched = false;
		s_[i].fresh = true;
		s_[i].name[0] = 0;
	}
}

int Watchdog::add(const char *name, uint32_t stall_us)
{
	if (n_ >= MAX_SLOTS) return -1;
	S &s = s_[n_];
	strncpy(s.name, name ? name : "", sizeof s.name - 1);
	s.name[sizeof s.name - 1] = 0;
	s.stall = stall_us;
	s.used = true;
	s.watched = false;
	s.fresh = true;
	return n_++;
}

void Watchdog::watch(int slot, bool on_core3)
{
	if (slot < 0 || slot >= n_) return;
	s_[slot].watched = on_core3;
	s_[slot].fresh = true;
}

void Watchdog::unwatch_all()
{
	for (int i = 0; i < n_; i++) s_[i].watched = false;
}

int Watchdog::poll(uint64_t now_us)
{
	const uint32_t ref = ref_.load(std::memory_order_relaxed);
	for (int i = 0; i < n_; i++) {
		S &s = s_[i];
		if (!s.used || !s.watched) continue;
		const uint32_t cur = s.beat.load(std::memory_order_relaxed);
		const bool busy = s.busy.load(std::memory_order_relaxed);
		if (s.fresh || cur != s.seen || !busy) {      /* progress, or nothing owed: restart the clock */
			s.fresh = false;
			s.seen = cur;
			s.since = now_us;
			s.ref_seen = ref;
			continue;
		}
		const uint32_t stall = s.stall ? s.stall : cfg_.stall_us;
		if (now_us - s.since >= stall && (uint32_t) (ref - s.ref_seen) >= cfg_.ref_min) {
			s.watched = false;
			return i;
		}
	}
	return -1;
}

/* ---- the planner ----------------------------------------------------------------------------------- */

static bool is_whole(Role r) { return r == R_EMU_MAIN || r == R_RENDER || r == R_APU || r == R_WIDE_COMPOSE; }
static bool is_optional(Role r) { return r == R_UPSCALE || r == R_WIDE_SCOUT || r == R_WRITER || r == R_NET_IO; }

struct Place { uint32_t mask; int prio; int kprio; };

/* The three-core layout: today's, per profile. Values marked (new) belong to roles no player has yet. */
static Place place3(CoreProfile p, Role r, int idx)
{
	Place o = { 0, 0xA0, 0xA0 };
	if (p != CP_APP) {                  /* snes/vita/main.cpp, nes/vita/main.cpp */
		switch (r) {
		case R_EMU_MAIN: { Place x = { CORE_U0, 0x60, 0x60 }; return x; }          /* main_prio default, core 0 */
		case R_RENDER: { Place x = { CORE_U1, 0x60, 0x60 }; return x; }            /* snes_host_thread_config(2, 0x60, 1, 0x60) */
		case R_APU: { Place x = { CORE_U2, 0x60, 0x60 }; return x; }
		case R_AUDIO_OUT: { Place x = { CORE_U2, 0x40, 0x40 }; return x; }         /* PRIO_AUDIO_DEFAULT, USER_2 */
		case R_DRAW: { Place x = { CORE_U1, 0x60, 0x60 }; return x; }              /* draw_core 1, draw_prio 0x60 */
		case R_WRITER: { Place x = { CORE_U1, 0xA0, 0xA0 }; return x; }            /* writer_start */
		case R_WIDE_COMPOSE: { Place x = { CORE_U1, 0x68, 0x68 }; return x; }      /* (new) */
		case R_WIDE_SCOUT: { Place x = { CORE_U2, 0xA0, 0xA0 }; return x; }        /* (new) */
		case R_UPSCALE: { Place x = { idx ? CORE_U2 : CORE_U1, 0xA0, 0xA0 }; return x; }   /* (new) */
		case R_NET_IO: { Place x = { 0, 0xB8, 0xB8 }; return x; }                  /* (new) */
		default: return o;
		}
	}
	switch (r) {                       /* gpsp/vita: plat_vita.c, upscale_vita.c, render_thread.c */
	case R_EMU_MAIN: { Place x = { CORE_U0, 0xA0, 0 }; return x; }                 /* plat_thread_pin(main_cpu); priority left alone */
	case R_RENDER: { Place x = { CORE_U1 | CORE_U2, 0x60, 0x60 }; return x; }
	case R_UPSCALE: { Place x = { idx ? CORE_U2 : CORE_U1, 0xA0, KPRIO_DEFAULT_USER }; return x; }   /* 0x10000100 == 160 */
	case R_AUDIO_OUT: { Place x = { 0, 0x50, 0x50 }; return x; }                   /* PRIO_AUDIO, floats */
	case R_WRITER: { Place x = { 0, 0xB0, 0xB0 }; return x; }                      /* PRIO_WORKER, floats */
	case R_NET_IO: { Place x = { 0, 0xB8, 0xB8 }; return x; }                      /* PRIO_BG, floats */
	case R_DRAW: { Place x = { CORE_U2, 0x68, 0x68 }; return x; }                  /* (new) hand-over draw thread */
	case R_WIDE_COMPOSE: { Place x = { CORE_U1, 0x68, 0x68 }; return x; }          /* (new) */
	case R_WIDE_SCOUT: { Place x = { CORE_U2, 0xA0, 0xA0 }; return x; }            /* (new) */
	case R_APU: { Place x = { CORE_U2, 0x60, 0x60 }; return x; }                   /* (new) */
	default: return o;
	}
}

/* The four-core layout: the three-core place, moved only where the rules say. */
static Place place4(CoreProfile p, Role r, int idx, const Request &req)
{
	Place o = place3(p, r, idx);
	if (p != CP_APP) {
		const bool cores_taken = req.has[R_RENDER] || req.has[R_APU];
		switch (r) {
		case R_AUDIO_OUT: o.mask = CORE_SYS; break;
		case R_DRAW: o.mask = CORE_SYS; break;
		case R_WRITER: o.mask = CORE_SYS; break;
		case R_WIDE_COMPOSE: o.mask = cores_taken ? CORE_SYS : CORE_U1; break;
		case R_WIDE_SCOUT: o.mask = req.has[R_APU] ? CORE_SYS : CORE_U2; break;
		case R_UPSCALE: if (idx == 2) o.mask = CORE_SYS; break;
		default: break;
		}
		return o;
	}
	switch (r) {
	case R_RENDER: o.mask = CORE_SYS; break;
	case R_UPSCALE: if (idx == 2) o.mask = CORE_SYS; break;
	default: break;
	}
	return o;
}

Plan make_plan(const Request &req, bool core3)
{
	Plan pl;
	pl.core3 = core3;
	pl.n = 0;
	for (int r = 0; r < R_COUNT; r++) {
		if (!req.has[r]) continue;
		int count = 1;
		if (r == R_UPSCALE) {
			count = req.upscale_n < 0 ? 0 : req.upscale_n;
			const int cap = core3 ? 3 : 2;
			if (count > cap) count = cap;
		}
		for (int i = 0; i < count && pl.n < PLAN_MAX_SLOTS; i++) {
			const Place pc = core3 ? place4(req.profile, (Role) r, i, req) : place3(req.profile, (Role) r, i);
			Slot &s = pl.slots[pl.n++];
			s.role = (Role) r;
			s.index = i;
			s.mask = pc.mask;
			s.prio = pc.prio < PRIO_HIGHEST ? PRIO_HIGHEST : pc.prio > PRIO_LOWEST ? PRIO_LOWEST : pc.prio;
			s.kprio = pc.kprio;
			s.on_core3 = (pc.mask & CORE_SYS) != 0;
			s.whole_core = is_whole((Role) r);
			s.spins = req.profile != CP_APP && (r == R_RENDER || r == R_APU);
			s.optional = is_optional((Role) r);
		}
	}
	return pl;
}

/* ---- the Vita --------------------------------------------------------------------------------------- */
#ifdef __vita__

namespace {
uint64_t vita_now(void *) { return sceKernelGetProcessTimeWide(); }

struct ProbeArg { uint32_t spin_us, gap_us; };

int probe_entry(SceSize, void *argp)
{
	const ProbeArg a = *(const ProbeArg *) argp;
	const int share = spin_share_permille(vita_now, 0, a.spin_us, 100);
	return sceKernelExitThread(share);
}

class VitaCoreSys : public CoreSys {
public:
	uint64_t now_us() { return sceKernelGetProcessTimeWide(); }
	void sleep_us(uint32_t us) { sceKernelDelayThread(us); }
	int probe_once(uint32_t spin_us, int prio)
	{
		SceUID th = sceKernelCreateThread("pk_core_probe", probe_entry, prio, 0x4000, 0, SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
		if (th < 0) return -1;
		ProbeArg a;
		a.spin_us = spin_us;
		a.gap_us = 100;
		if (sceKernelStartThread(th, sizeof a, &a) < 0) { sceKernelDeleteThread(th); return -1; }
		int status = 0;
		SceUInt timeout = spin_us * 6 + 100000;      /* a thread that gets (almost) no CPU must not hold us */
		int rc = sceKernelWaitThreadEnd(th, &status, &timeout);
		if (rc < 0) {
			/* it never finished: give it a normal core so it can end, count it as 0 % */
			sceKernelChangeThreadCpuAffinityMask(th, SCE_KERNEL_CPU_MASK_USER_2);
			SceUInt t2 = 1000000;
			if (sceKernelWaitThreadEnd(th, &status, &t2) < 0) return 0;     /* leaked, harmless: it ends by itself */
			sceKernelDeleteThread(th);
			return 0;
		}
		sceKernelDeleteThread(th);
		return status < 0 ? 0 : status > 1000 ? 1000 : status;
	}
};
VitaCoreSys g_vita_sys;      /* a namespace-scope object: no function-local static, so no guard calls */
}  // namespace

CoreSys *vita_core_sys() { return &g_vita_sys; }

int apply_slot(int thid, const Slot &s)
{
	int bad = 0;
	const int rc1 = sceKernelChangeThreadCpuAffinityMask(thid, s.mask ? (int) s.mask : SCE_KERNEL_CPU_MASK_USER_ALL);
	if (rc1 < 0) bad = rc1;
	const int rc2 = sceKernelChangeThreadPriority(thid, s.prio);
	if (rc2 < 0 && !bad) bad = rc2;
	return bad;
}

#endif  /* __vita__ */

}  // namespace pk
