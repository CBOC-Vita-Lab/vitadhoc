/*
 * pacing_model: prints the model's tables (see pacing_model.h). Plain C++, no console code.
 *
 *   pacing_model                 the standard tables (design today / hand-over depth 1..4, work 0.8..1.2 refreshes)
 *   pacing_model adaptive        the adaptive-depth checks: steps in cost, wrong cost guesses, flapping
 *   pacing_model all             both
 *
 * Every number is a MODEL number. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#include "pacing_model.h"
#include <stdio.h>
#include <string.h>

using namespace pkmodel;

static void head(const char *what)
{
	printf("\n%s\n", what);
	printf("%-13s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n", "design", "work", "fps", "rep/s", "unev/s", "speed%", "dly ms", "p95 ms", "half%", "drop/s");
}
static void row(const char *name, double work, const Result &r)
{
	printf("%-13s %6.2f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.0f %6.1f\n", name, work, r.fps, r.repeats_per_s, r.uneven_per_s,
	    r.speed_pct, r.delay_mean_ms, r.delay_p95_ms, r.half_pct, r.drops_per_s);
}

static Result avg_runs(Params p, int runs)
{
	Result a;
	for (int i = 0; i < runs; i++) {
		p.seed = 1 + i;
		Sim s(p);
		Result r = s.run();
		a.fps += r.fps; a.repeats_per_s += r.repeats_per_s; a.uneven_per_s += r.uneven_per_s; a.speed_pct += r.speed_pct;
		a.delay_mean_ms += r.delay_mean_ms; a.delay_p95_ms += r.delay_p95_ms; a.half_pct += r.half_pct;
		a.drops_per_s += r.drops_per_s; a.depth_avg += r.depth_avg; a.depth_changes += r.depth_changes; a.mode_changes += r.mode_changes;
	}
	a.fps /= runs; a.repeats_per_s /= runs; a.uneven_per_s /= runs; a.speed_pct /= runs; a.delay_mean_ms /= runs;
	a.delay_p95_ms /= runs; a.half_pct /= runs; a.drops_per_s /= runs; a.depth_avg /= runs;
	return a;
}

static void standard(double E, bool steady, const char *title)
{
	char t[200];
	snprintf(t, sizeof t, "%s  (emulation %.2f refresh, wobble +-8%%, 1%% spikes +35%%, steady pacing %s, 3 runs of 60 s)", title, E, steady ? "on" : "off");
	head(t);
	const double works[] = { 0.80, 0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.15, 1.20 };
	for (size_t w = 0; w < sizeof works / sizeof works[0]; w++) {
		Params p;
		p.E = E; p.D = works[w]; p.steady = steady; p.seconds = 60;
		row("today", works[w], avg_runs(p, 3));
		for (int d = 1; d <= 4; d++) {
			p.handoff = true; p.depth = d;
			char n[32]; snprintf(n, sizeof n, "handover d=%d", d);
			row(n, works[w], avg_runs(p, 3));
		}
		printf("\n");
	}
}

/* adaptive depth: a scene that gets heavy and light again, with right and wrong cost guesses */
static void adaptive()
{
	for (int steady = 1; steady >= 0; steady--) {
		printf("\nAdaptive depth, steady pacing %s. Draw work steps 0.70 -> 1.15 -> 0.70 -> 1.10 -> 0.70 (20 s each), emulation 0.30, wobble +-8%%.\n", steady ? "ON" : "OFF");
		printf("%-32s %6s %6s %6s %6s %6s %6s %6s %6s\n", "case", "fps", "rep/s", "unev/s", "speed%", "dly ms", "p95 ms", "d.chg", "dpth");
		struct Case { const char *name; bool adaptive; int depth; double bias; };
		const Case cases[] = {
			{ "fixed depth 3", false, 3, 1.0 },
			{ "fixed depth 2", false, 2, 1.0 },
			{ "fixed depth 1", false, 1, 1.0 },
			{ "adaptive 2->1, true costs", true, 2, 1.0 },
			{ "adaptive, costs read 30% low", true, 2, 0.70 },
			{ "adaptive, costs read 30% high", true, 2, 1.30 },
			{ "adaptive, costs read 10% low", true, 2, 0.90 },
			{ "adaptive, costs read 10% high", true, 2, 1.10 },
		};
		for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
			Params p;
			p.handoff = true; p.E = 0.30; p.seconds = 100; p.steady = steady != 0;
			p.adaptive = cases[i].adaptive; p.depth = cases[i].depth; p.bias = cases[i].bias;
			p.D_profile.push_back(std::make_pair(0.0, 0.70));
			p.D_profile.push_back(std::make_pair(20.0, 1.15));
			p.D_profile.push_back(std::make_pair(40.0, 0.70));
			p.D_profile.push_back(std::make_pair(60.0, 1.10));
			p.D_profile.push_back(std::make_pair(80.0, 0.70));
			Result r = avg_runs(p, 3);
			printf("%-32s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.2f\n", cases[i].name, r.fps, r.repeats_per_s, r.uneven_per_s,
			    r.speed_pct, r.delay_mean_ms, r.delay_p95_ms, (double) r.depth_changes / 3, r.depth_avg);
		}
	}
	printf("\nFlapping check: work sitting on the thresholds (draw 0.86 .. 1.02 refreshes, 100 s), adaptive depth changes per minute (steady pacing off, so only the depth rule acts):\n");
	for (double w = 0.84; w <= 1.041; w += 0.02) {
		Params p;
		p.handoff = true; p.E = 0.30; p.seconds = 100; p.adaptive = true; p.depth = 2; p.D = w; p.steady = false;
		Result r = avg_runs(p, 3);
		printf("  D=%.2f: depth changes %.1f/min, fps %.1f, delay %.1f ms, half %.0f%%\n", w, r.depth_changes / 3.0 * 60 / p.seconds, r.fps, r.delay_mean_ms, r.half_pct);
	}
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "standard";
	if (!strcmp(mode, "standard") || !strcmp(mode, "all")) {
		standard(0.30, true, "TABLE 1: SNES-like split");
		standard(0.05, false, "TABLE 2: drawing-only work (the Galaxy/Strikers shape, nothing else on the producer)");
	}
	if (!strcmp(mode, "adaptive") || !strcmp(mode, "all")) adaptive();
	return 0;
}
