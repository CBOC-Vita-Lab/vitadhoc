/* pk_cores tests: probe with a fake kernel, the planner tables and invariants, the watchdog, resume.
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_cores.h"

#include <initializer_list>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

using namespace pk;

namespace {

/* a kernel with a script of probe answers (permille, or -1 = refused); the last answer repeats */
struct FakeSys : CoreSys {
	std::vector<int> script;
	size_t next;
	uint64_t t;
	int calls, sleeps;
	uint64_t slept_total;
	uint32_t last_gap;
	FakeSys() : next(0), t(1000), calls(0), sleeps(0), slept_total(0), last_gap(0) {}
	uint64_t now_us() { return t; }
	void sleep_us(uint32_t us) { t += us; sleeps++; slept_total += us; last_gap = us; }
	int probe_once(uint32_t spin_us, int) {
		calls++;
		t += spin_us;
		int v = script.empty() ? 1000 : script[next < script.size() ? next : script.size() - 1];
		next++;
		return v;
	}
	void set(std::initializer_list<int> v) { script.assign(v.begin(), v.end()); next = 0; calls = 0; sleeps = 0; slept_total = 0; }
};

std::vector<std::string> g_log;
void log_fn(void *, const char *line) { g_log.push_back(line); }
bool logged(const char *needle) {
	for (size_t i = 0; i < g_log.size(); i++) if (g_log[i].find(needle) != std::string::npos) return true;
	return false;
}

struct Fx {
	FakeSys sys;
	Cores c;
	explicit Fx(std::initializer_list<int> v, Core4Mode m = CORE4_AUTO) : c(&sys, m) { sys.set(v); g_log.clear(); c.set_log(log_fn, 0); }
	std::string census() { char b[256]; c.census_line(b, sizeof b); return b; }
};

/* a clock for spin_share_permille: each call advances by step; at the numbered call an extra jump */
struct SpinClock {
	uint64_t t, step;
	std::vector<std::pair<int, uint64_t> > jumps;
	int calls;
	SpinClock() : t(0), step(2), calls(0) {}
	static uint64_t now(void *p) {
		SpinClock *c = (SpinClock *) p;
		c->t += c->step;
		for (size_t i = 0; i < c->jumps.size(); i++) if (c->jumps[i].first == c->calls) c->t += c->jumps[i].second;
		c->calls++;
		return c->t;
	}
};

uint32_t eff(uint32_t m) { return m ? m : (uint32_t) CORE_USER_ALL; }

Request snes_req() {
	Request r(CP_SNES);
	r.with(R_EMU_MAIN).with(R_RENDER).with(R_APU).with(R_AUDIO_OUT).with(R_DRAW).with(R_WRITER);
	return r;
}
Request app_req(int up) {
	Request r(CP_APP);
	r.with(R_EMU_MAIN).with(R_RENDER).with(R_AUDIO_OUT).with(R_WRITER).with(R_NET_IO).with(R_UPSCALE);
	r.upscale_n = up;
	return r;
}
Request nes_req() {
	Request r(CP_NES);
	r.with(R_EMU_MAIN).with(R_AUDIO_OUT).with(R_WRITER).with(R_WIDE_COMPOSE).with(R_WIDE_SCOUT).with(R_DRAW);
	return r;
}

void expect_slot(const Plan &p, Role r, int idx, uint32_t mask, int prio, int kprio, int line) {
	const Slot *s = p.find(r, idx);
	if (!s) { fprintf(stderr, "    FAIL line %d: role %d/%d missing\n", line, (int) r, idx); pktest::g_failed_checks++; return; }
	if (s->mask != mask || s->prio != prio || s->kprio != kprio) {
		fprintf(stderr, "    FAIL line %d: role %d/%d is mask %x prio %x kprio %x, wanted %x %x %x\n", line, (int) r, idx, s->mask, s->prio, s->kprio, mask, prio, kprio);
		pktest::g_failed_checks++;
	}
}
#define SLOT(p, r, i, m, pr, kp) expect_slot(p, r, i, m, pr, kp, __LINE__)

}  // namespace

/* ---------------- the probe ---------------- */

TEST(cores_probe_100_first_try)
{
	Fx f({ 1000 });
	const ProbeResult &r = f.c.probe();
	CHECK_EQ(r.decision, DEC_USABLE);
	CHECK_EQ(r.tries, 1);
	CHECK_EQ(f.sys.calls, 1);
	CHECK_EQ(f.sys.sleeps, 0);
	CHECK(f.c.core3_usable());
	CHECK_EQ(f.c.generation(), 1);
	CHECK(logged("probe 1 of 6: share 100 %"));
	CHECK_STR(f.c.status_text(), "Extra CPU core: in use");
}

TEST(cores_probe_bad_luck_then_good)
{
	Fx f({ 30, 970 });
	const ProbeResult &r = f.c.probe();
	CHECK_EQ(r.decision, DEC_USABLE);
	CHECK_EQ(r.tries, 2);
	CHECK_EQ(f.sys.sleeps, 1);
	CHECK_EQ(f.sys.last_gap, 40000);
	CHECK_EQ(r.shares[0], 30);
	CHECK_EQ(r.shares[1], 970);
	CHECK(logged("probe 1 of 6: share 3 %"));
	CHECK(logged("probe 2 of 6: share 97 %"));
	const std::string c = f.census();
	CHECK(c.find("probe 2 of 6") != std::string::npos);
	CHECK(c.find("share 3,97 %") != std::string::npos);
	CHECK(c.find("decision usable") != std::string::npos);
	CHECK(c.find("layout 4-core") != std::string::npos);
	CHECK(c.find("watchdog trips 0") != std::string::npos);
}

TEST(cores_probe_six_bad_is_unusable)
{
	Fx f({ 30 });
	const ProbeResult &r = f.c.probe();
	CHECK_EQ(r.decision, DEC_UNUSABLE_SHARE);
	CHECK_EQ(r.tries, 6);
	CHECK_EQ(f.sys.calls, 6);
	CHECK_EQ(f.sys.sleeps, 5);
	CHECK_EQ(f.sys.slept_total, 5 * 40000);
	CHECK(!f.c.core3_usable());
	CHECK_EQ(f.c.generation(), 0);
	CHECK(logged("probe 6 of 6"));
	CHECK_STR(f.c.status_text(), "Extra CPU core: not available (needs the CapUnlocker plugin)");
	const std::string c = f.census();
	CHECK(c.find("share 3,3,3,3,3,3 %") != std::string::npos);
	CHECK(c.find("layout 3-core") != std::string::npos);
}

TEST(cores_probe_refused_is_final)
{
	Fx f({ -1, 1000 });
	const ProbeResult &r = f.c.probe();
	CHECK_EQ(r.decision, DEC_REFUSED);
	CHECK_EQ(r.tries, 1);
	CHECK_EQ(f.sys.calls, 1);
	CHECK_EQ(f.sys.sleeps, 0);
	CHECK(!f.c.core3_usable());
	CHECK(f.census().find("refused") != std::string::npos);
	CHECK(f.census().find("affinity refused") != std::string::npos);
}

TEST(cores_probe_flapping_middle_never_accepted)
{
	Fx f({ 500, 100, 500, 100, 500, 100 });
	CHECK_EQ(f.c.probe().decision, DEC_UNUSABLE_SHARE);
	CHECK_EQ(f.c.result().tries, 6);
	CHECK(logged("in between, retry"));
	CHECK(logged("too low, retry"));
}

TEST(cores_probe_middle_then_usable)
{
	Fx f({ 500, 700 });
	CHECK_EQ(f.c.probe().decision, DEC_USABLE);
	CHECK_EQ(f.c.result().tries, 2);
}

TEST(cores_probe_threshold_edges)
{
	{ Fx f({ 599, 599, 599, 599, 599, 599 }); CHECK_EQ(f.c.probe().decision, DEC_UNUSABLE_SHARE); }
	{ Fx f({ 600 }); CHECK_EQ(f.c.probe().decision, DEC_USABLE); }
	{ Fx f({ 0 }); CHECK_EQ(f.c.probe().decision, DEC_UNUSABLE_SHARE); }
}

TEST(cores_probe_cached_and_reprobe)
{
	Fx f({ 1000 });
	f.c.probe();
	f.c.probe();
	f.c.probe();
	CHECK_EQ(f.sys.calls, 1);
	f.sys.set({ 20 });
	CHECK(f.c.reprobe());                        /* usable -> unusable: the layout must change */
	CHECK(!f.c.core3_usable());
	CHECK_EQ(f.sys.calls, 6);
	CHECK_EQ(f.c.generation(), 2);
	f.sys.set({ 1000 });
	CHECK(f.c.reprobe());
	CHECK(f.c.core3_usable());
	CHECK_EQ(f.c.generation(), 3);
	CHECK(!f.c.reprobe());                       /* same verdict: no change reported */
}

TEST(cores_probe_off_never_touches_the_kernel)
{
	Fx f({ 1000 }, CORE4_OFF);
	CHECK_EQ(f.c.probe().decision, DEC_DISABLED);
	CHECK_EQ(f.sys.calls, 0);
	CHECK(!f.c.core3_usable());
	CHECK(!f.c.on_resume());
	CHECK_STR(f.c.status_text(), "Extra CPU core: off (setting)");
	CHECK(f.census().find("decision off") != std::string::npos);
}

TEST(cores_probe_max_tries_configurable)
{
	FakeSys sys;
	sys.set({ 10 });
	ProbeConfig pc;
	pc.max_tries = 3;
	pc.gap_us = 5000;
	pc.spin_us = 2000;
	Cores c(&sys, CORE4_AUTO, pc);
	CHECK_EQ(c.probe().decision, DEC_UNUSABLE_SHARE);
	CHECK_EQ(sys.calls, 3);
	CHECK_EQ(sys.last_gap, 5000);
	pc.max_tries = 99;
	Cores d(&sys, CORE4_AUTO, pc);
	sys.set({ 10 });
	d.probe();
	CHECK_EQ(sys.calls, PROBE_MAX_TRIES);       /* clamped */
}

TEST(cores_spin_share_clean_and_starved)
{
	{
		SpinClock c;
		CHECK_EQ(spin_share_permille(SpinClock::now, &c, 10000, 100), 1000);
	}
	{   /* half the spin lost to one preemption */
		SpinClock c;
		c.jumps.push_back(std::make_pair(100, (uint64_t) 5000));
		const int s = spin_share_permille(SpinClock::now, &c, 10000, 100);
		CHECK(s > 450 && s < 550);
	}
	{   /* ran 2 ms, then nothing for 30 ms */
		SpinClock c;
		c.jumps.push_back(std::make_pair(1000, (uint64_t) 30000));
		const int s = spin_share_permille(SpinClock::now, &c, 10000, 100);
		CHECK(s < 100);
	}
	{   /* small preemptions below the gap threshold do not count */
		SpinClock c;
		c.jumps.push_back(std::make_pair(50, (uint64_t) 60));
		CHECK_EQ(spin_share_permille(SpinClock::now, &c, 10000, 100), 1000);
	}
}

/* ---------------- suspend / resume ---------------- */

TEST(cores_resume_hysteresis_keeps_a_usable_core)
{
	Fx f({ 1000 });
	f.c.probe();
	f.sys.set({ 450 });
	CHECK(!f.c.on_resume());                     /* 45 % >= hold bar 40 %: stays */
	CHECK(f.c.core3_usable());
	CHECK_EQ(f.sys.calls, 1);
	CHECK_EQ(f.c.generation(), 1);
}

TEST(cores_resume_drops_a_core_that_went_away)
{
	Fx f({ 1000 });
	f.c.probe();
	f.sys.set({ 350 });
	CHECK(f.c.on_resume());
	CHECK(!f.c.core3_usable());
	CHECK_EQ(f.sys.calls, 6);
	CHECK_EQ(f.c.generation(), 2);
	/* a plan made now is the three-core one */
	const Plan p = f.c.plan(snes_req());
	CHECK(!p.core3);
	CHECK(!p.find(R_AUDIO_OUT)->on_core3);
}

TEST(cores_resume_does_not_use_hold_bar_from_unusable)
{
	Fx f({ 20 });
	f.c.probe();
	f.sys.set({ 450 });
	CHECK(!f.c.on_resume());                     /* 45 % is not enough to get in */
	CHECK(!f.c.core3_usable());
	f.sys.set({ 970 });
	CHECK(f.c.on_resume());                      /* but a working core after resume is picked up */
	CHECK(f.c.core3_usable());
	CHECK(f.c.plan(snes_req()).find(R_AUDIO_OUT)->on_core3);
}

/* ---------------- the planner ---------------- */

TEST(cores_plan_3core_snes_is_todays_layout)
{
	const Plan p = make_plan(snes_req(), false);
	CHECK(!p.core3);
	CHECK_EQ(p.n, 6);
	SLOT(p, R_EMU_MAIN, 0, CORE_U0, 0x60, 0x60);
	SLOT(p, R_RENDER, 0, CORE_U1, 0x60, 0x60);
	SLOT(p, R_APU, 0, CORE_U2, 0x60, 0x60);
	SLOT(p, R_AUDIO_OUT, 0, CORE_U2, 0x40, 0x40);
	SLOT(p, R_DRAW, 0, CORE_U1, 0x60, 0x60);
	SLOT(p, R_WRITER, 0, CORE_U1, 0xA0, 0xA0);
}

TEST(cores_plan_3core_app_is_todays_layout)
{
	const Plan p = make_plan(app_req(2), false);
	CHECK_EQ(p.n, 7);
	SLOT(p, R_EMU_MAIN, 0, CORE_U0, 0xA0, 0);
	SLOT(p, R_RENDER, 0, CORE_U1 | CORE_U2, 0x60, 0x60);
	SLOT(p, R_UPSCALE, 0, CORE_U1, 0xA0, KPRIO_DEFAULT_USER);
	SLOT(p, R_UPSCALE, 1, CORE_U2, 0xA0, KPRIO_DEFAULT_USER);
	SLOT(p, R_AUDIO_OUT, 0, 0, 0x50, 0x50);
	SLOT(p, R_WRITER, 0, 0, 0xB0, 0xB0);
	SLOT(p, R_NET_IO, 0, 0, 0xB8, 0xB8);
	CHECK(!p.find(R_UPSCALE, 2));
}

TEST(cores_plan_3core_clamps_the_third_helper)
{
	CHECK(!make_plan(app_req(3), false).find(R_UPSCALE, 2));
	CHECK(make_plan(app_req(3), true).find(R_UPSCALE, 2));
	CHECK(!make_plan(app_req(0), true).find(R_UPSCALE, 0));
}

TEST(cores_plan_4core_snes)
{
	const Plan p = make_plan(snes_req(), true);
	CHECK(p.core3);
	SLOT(p, R_EMU_MAIN, 0, CORE_U0, 0x60, 0x60);
	SLOT(p, R_RENDER, 0, CORE_U1, 0x60, 0x60);
	SLOT(p, R_APU, 0, CORE_U2, 0x60, 0x60);
	SLOT(p, R_AUDIO_OUT, 0, CORE_SYS, 0x40, 0x40);
	SLOT(p, R_DRAW, 0, CORE_SYS, 0x60, 0x60);
	SLOT(p, R_WRITER, 0, CORE_SYS, 0xA0, 0xA0);
}

TEST(cores_plan_4core_app)
{
	const Plan p = make_plan(app_req(3), true);
	SLOT(p, R_EMU_MAIN, 0, CORE_U0, 0xA0, 0);
	SLOT(p, R_RENDER, 0, CORE_SYS, 0x60, 0x60);
	SLOT(p, R_UPSCALE, 0, CORE_U1, 0xA0, KPRIO_DEFAULT_USER);
	SLOT(p, R_UPSCALE, 1, CORE_U2, 0xA0, KPRIO_DEFAULT_USER);
	SLOT(p, R_UPSCALE, 2, CORE_SYS, 0xA0, KPRIO_DEFAULT_USER);
	SLOT(p, R_AUDIO_OUT, 0, 0, 0x50, 0x50);
}

TEST(cores_plan_4core_nes_and_wide)
{
	const Plan p3 = make_plan(nes_req(), false);
	SLOT(p3, R_AUDIO_OUT, 0, CORE_U2, 0x40, 0x40);
	SLOT(p3, R_WIDE_COMPOSE, 0, CORE_U1, 0x68, 0x68);
	SLOT(p3, R_WIDE_SCOUT, 0, CORE_U2, 0xA0, 0xA0);
	const Plan p = make_plan(nes_req(), true);
	SLOT(p, R_WIDE_COMPOSE, 0, CORE_U1, 0x68, 0x68);       /* nothing else wants core 1: it keeps a core of its own */
	SLOT(p, R_WIDE_SCOUT, 0, CORE_U2, 0xA0, 0xA0);         /* and the look-ahead has core 2 to itself now audio left it */
	SLOT(p, R_AUDIO_OUT, 0, CORE_SYS, 0x40, 0x40);
	SLOT(p, R_DRAW, 0, CORE_SYS, 0x60, 0x60);
	/* SNES with a PPU and APU thread and the wide composer: the composer takes core 3 with the audio thread */
	Request r = snes_req();
	r.with(R_WIDE_COMPOSE);
	const Plan s = make_plan(r, true);
	SLOT(s, R_WIDE_COMPOSE, 0, CORE_SYS, 0x68, 0x68);
	SLOT(s, R_RENDER, 0, CORE_U1, 0x60, 0x60);
	SLOT(s, R_APU, 0, CORE_U2, 0x60, 0x60);
}

TEST(cores_plan_invariants_every_role_combination)
{
	long combos = 0;
	for (int prof = 0; prof < 3; prof++) {
		for (unsigned bits = 0; bits < (1u << R_COUNT); bits++) {
			for (int up = 0; up <= 3; up++) {
				if (!(bits & (1u << R_UPSCALE)) && up) continue;
				Request req((CoreProfile) prof);
				for (int r = 0; r < R_COUNT; r++) req.has[r] = (bits >> r) & 1;
				req.has[R_EMU_MAIN] = true;
				req.upscale_n = up;
				const Plan p3 = make_plan(req, false);
				const Plan p4 = make_plan(req, true);
				combos++;
				for (int core3 = 0; core3 < 2; core3++) {
					const Plan &p = core3 ? p4 : p3;
					for (int i = 0; i < p.n; i++) {
						const Slot &a = p.slots[i];
						/* priorities inside the user range */
						CHECK(a.prio >= PRIO_HIGHEST && a.prio <= PRIO_LOWEST);
						CHECK(a.kprio == 0 || a.kprio == KPRIO_DEFAULT_USER || (a.kprio >= 0x40 && a.kprio <= 0xBF));
						/* on_core3 is the mask's system bit; no system core in a 3-core plan; a floating role never gets it */
						CHECK_EQ(a.on_core3, (a.mask & CORE_SYS) != 0);
						if (!core3) CHECK(!a.on_core3);
						if (a.mask == 0) CHECK(!a.on_core3);
						/* R1: core 0 belongs to the emulating thread alone (pinned roles) */
						if (a.role == R_EMU_MAIN) CHECK_EQ(a.mask, CORE_U0);
						else if (a.mask) CHECK_EQ(a.mask & CORE_U0, 0);
						/* R5: optional helpers are at the bottom */
						if (a.optional) CHECK(a.prio >= 0xA0);
						for (int j = 0; j < p.n; j++) {
							const Slot &b = p.slots[j];
							if (i == j || !(eff(a.mask) & eff(b.mask))) continue;
							if (a.optional && !b.optional) CHECK(a.prio > b.prio);
							if (core3) {
								/* R3: whole-core roles never share */
								if (a.whole_core && b.whole_core) {
									fprintf(stderr, "    whole-core roles %d and %d share (profile %d bits %x)\n", (int) a.role, (int) b.role, prof, bits);
									CHECK(false);
								}
								/* R4: audio out never with a spinner */
								if (a.role == R_AUDIO_OUT && b.spins) CHECK(false);
							}
						}
					}
				}
				/* what moves with the extra core is a short list */
				for (int i = 0; i < p4.n; i++) {
					const Slot &a = p4.slots[i];
					const Slot *b = p3.find(a.role, a.index);
					if (!b) { CHECK(a.role == R_UPSCALE && a.index == 2); continue; }
					if (a.mask == b->mask) { CHECK_EQ(a.prio, b->prio); continue; }
					if (prof == CP_APP) CHECK(a.role == R_RENDER || a.role == R_UPSCALE);
					else CHECK(a.role == R_AUDIO_OUT || a.role == R_DRAW || a.role == R_WRITER || a.role == R_WIDE_COMPOSE || a.role == R_WIDE_SCOUT || a.role == R_UPSCALE);
					CHECK_EQ(a.prio, b->prio);                     /* only the core changes, never the priority */
				}
			}
		}
	}
	CHECK(combos > 1000);
}

TEST(cores_plan_follows_the_verdict)
{
	Fx f({ 1000 });
	f.c.probe();
	const Plan a = f.c.plan(snes_req());
	CHECK(a.core3);
	CHECK(a.find(R_AUDIO_OUT)->on_core3);
	Fx g({ 10 });
	g.c.probe();
	const Plan b = g.c.plan(snes_req());
	const Plan c3 = make_plan(snes_req(), false);
	CHECK_EQ(b.n, c3.n);
	for (int i = 0; i < b.n; i++) CHECK(b.slots[i].mask == c3.slots[i].mask && b.slots[i].prio == c3.slots[i].prio);
}

/* ---------------- the watchdog ---------------- */

namespace {
struct Wd {
	Fx f;
	int audio, up;
	uint64_t t;
	Wd() : f({ 1000 }), audio(-1), up(-1), t(5000000) {
		f.c.probe();
		audio = f.c.watchdog().add("audio");
		up = f.c.watchdog().add("upscale");
		f.c.watchdog().watch(audio, true);
		f.c.watchdog().watch(up, true);
		f.c.watchdog().set_busy(audio, true);
	}
	/* one 16 ms frame; the audio thread beats when alive */
	bool frame(bool audio_alive) {
		t += 16683;
		f.c.watchdog().ref_beat();
		if (audio_alive) f.c.watchdog().beat(audio);
		return f.c.poll_watchdog(t);
	}
};
}  // namespace

TEST(cores_watchdog_quiet_while_alive)
{
	Wd w;
	for (int i = 0; i < 600; i++) CHECK(!w.frame(true));
	CHECK(w.f.c.core3_usable());
	CHECK_EQ(w.f.c.trips(), 0);
}

TEST(cores_watchdog_trips_on_a_dead_thread)
{
	Wd w;
	for (int i = 0; i < 30; i++) w.frame(true);
	int frames = 0;
	bool tripped = false;
	while (!tripped && frames < 100) { tripped = w.frame(false); frames++; }
	CHECK(tripped);
	const uint64_t ms = (uint64_t) frames * 16683 / 1000;
	CHECK(ms >= 150 && ms <= 175);                         /* the stall limit, plus at most the poll period */
	CHECK(w.f.c.lost());
	CHECK(!w.f.c.core3_usable());
	CHECK_EQ(w.f.c.trips(), 1);
	CHECK_EQ(w.f.c.generation(), 2);
	CHECK(logged("watchdog: 'audio'"));
	CHECK(w.f.census().find("watchdog trips 1") != std::string::npos);
	CHECK(w.f.census().find("decision lost (watchdog)") != std::string::npos);
	CHECK_STR(w.f.c.status_text(), "Extra CPU core: not available (the system took it back)");
	/* the roles move back: the plan is the three-core one, and the poll is quiet from now on */
	const Plan p = w.f.c.plan(snes_req());
	CHECK(!p.core3);
	CHECK_EQ(p.find(R_AUDIO_OUT)->mask, CORE_U2);
	for (int i = 0; i < 50; i++) CHECK(!w.frame(false));
	CHECK_EQ(w.f.c.trips(), 1);
}

TEST(cores_watchdog_needs_the_others_to_be_running)
{
	Wd w;
	for (int i = 0; i < 10; i++) w.frame(true);
	/* the reference clock stands still (menu, pause): the audio thread's silence proves nothing */
	for (int i = 0; i < 400; i++) { w.t += 16683; CHECK(!w.f.c.poll_watchdog(w.t)); }
	CHECK(w.f.c.core3_usable());
}

TEST(cores_watchdog_idle_thread_is_not_dead)
{
	Wd w;
	w.f.c.watchdog().set_busy(w.audio, false);            /* legitimately waiting */
	for (int i = 0; i < 600; i++) CHECK(!w.frame(false));
	CHECK(w.f.c.core3_usable());
	/* it gets work and does not finish it */
	w.f.c.watchdog().set_busy(w.audio, true);
	bool tripped = false;
	for (int i = 0; i < 60 && !tripped; i++) tripped = w.frame(false);
	CHECK(tripped);
}

TEST(cores_watchdog_ignores_slots_not_on_core3)
{
	Wd w;
	w.f.c.watchdog().watch(w.audio, false);
	for (int i = 0; i < 100; i++) CHECK(!w.frame(false));
	CHECK(w.f.c.core3_usable());
}

TEST(cores_watchdog_per_slot_limit)
{
	Fx f({ 1000 });
	f.c.probe();
	const int s = f.c.watchdog().add("slow", 1000000);    /* a helper that may be 1 s on a long job */
	f.c.watchdog().watch(s, true);
	f.c.watchdog().set_busy(s, true);
	uint64_t t = 1000;
	int frames = 0;
	bool tripped = false;
	while (!tripped && frames < 200) { t += 16683; f.c.watchdog().ref_beat(); tripped = f.c.poll_watchdog(t); frames++; }
	CHECK(tripped);
	CHECK(frames >= 60 && frames <= 62);
}

TEST(cores_watchdog_does_nothing_without_the_core)
{
	Fx f({ 10 });
	f.c.probe();
	const int s = f.c.watchdog().add("audio");
	f.c.watchdog().watch(s, true);
	f.c.watchdog().set_busy(s, true);
	uint64_t t = 0;
	for (int i = 0; i < 100; i++) { t += 16683; f.c.watchdog().ref_beat(); CHECK(!f.c.poll_watchdog(t)); }
	CHECK_EQ(f.c.trips(), 0);
}

TEST(cores_watchdog_recovery)
{
	Wd w;
	for (int i = 0; i < 10; i++) w.frame(true);
	bool tripped = false;
	for (int i = 0; i < 40 && !tripped; i++) tripped = w.frame(false);
	CHECK(tripped);
	CHECK(!w.f.c.try_recover(w.t + 1000000));             /* too soon */
	CHECK_EQ(w.f.sys.calls, 1);                            /* no probe was spent */
	w.f.sys.set({ 20 });
	CHECK(!w.f.c.try_recover(w.t + 31000000));            /* due, but the core is still not there */
	CHECK(w.f.c.lost());
	CHECK_EQ(w.f.c.recoveries(), 1);
	w.f.sys.set({ 980 });
	CHECK(!w.f.c.try_recover(w.t + 32000000));            /* a failed recovery restarts the cool-down */
	CHECK(w.f.c.try_recover(w.t + 62000000));
	CHECK(w.f.c.core3_usable());
	CHECK(!w.f.c.lost());
	CHECK_EQ(w.f.c.recoveries(), 2);
	CHECK(w.f.c.plan(snes_req()).core3);
	CHECK(logged("recovered"));
}

TEST(cores_watchdog_recovery_is_capped)
{
	Wd w;
	uint64_t base = w.t;
	for (int round = 0; round < 4; round++) {
		w.f.c.watchdog().watch(w.audio, true);
		w.f.c.watchdog().set_busy(w.audio, true);
		bool tripped = false;
		for (int i = 0; i < 40 && !tripped; i++) tripped = w.frame(false);
		CHECK(tripped);
		base = w.t + 31000000;
		w.t = base;
		const bool back = w.f.c.try_recover(base);
		CHECK_EQ(back, round < 2);                           /* max_recoveries = 2 per session */
		if (!back) break;
	}
	CHECK(w.f.c.lost());
	CHECK_EQ(w.f.c.trips(), 3);
	CHECK(!w.f.c.try_recover(base + 3600000000ull));
}

TEST(cores_watchdog_explicit_reprobe_clears_lost)
{
	Wd w;
	for (int i = 0; i < 10; i++) w.frame(true);
	for (int i = 0; i < 40; i++) w.frame(false);
	CHECK(w.f.c.lost());
	w.f.sys.set({ 1000 });
	CHECK(w.f.c.reprobe());
	CHECK(w.f.c.core3_usable());
}

TEST(cores_mark_lost_by_outside_evidence)
{
	Fx f({ 1000 });
	f.c.probe();
	CHECK(f.c.mark_lost("slot_wait stalled 50 ms", 7000000));
	CHECK(f.c.lost());
	CHECK_EQ(f.c.trips(), 1);
	CHECK(logged("slot_wait stalled 50 ms"));
	CHECK(!f.c.mark_lost("again", 7000001));             /* already lost: no second trip */
	CHECK_EQ(f.c.trips(), 1);
	CHECK(!f.c.plan(app_req(3)).core3);
	Fx g({ 20 });
	g.c.probe();
	CHECK(!g.c.mark_lost("x", 1));                       /* never had the core: nothing to lose */
}
