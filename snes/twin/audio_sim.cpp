/*
 * snes_twin --audio-sim SCENARIO: the audio pacing code (frontend/audio_pace.cpp, the same file the
 * Vita player links) run in virtual time against a simulated 59.94 Hz display, a device audio clock
 * with an offset, and per-pass cost scenarios. The "emulator" runs one frame per display refresh like
 * the Vita player's main loop; its audio is either the real core's (a ROM) or a synthetic tone.
 * Nothing here sleeps: ten virtual minutes take a second or two.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "host.h"
#include "audio_pace.h"
#include "audio_sim.h"

namespace {

const double kNtscHz = 60.0988;     /* the console's frame rate (what the player's frame_hz is) */
const double kDisplayHz = 59.94;    /* the Vita's display */
const int OUT_RATE = 48000;

/* ---- click detector: an output step far larger than the steps around it ---------------------- */
struct ClickDet {
	int pl = 0, pr = 0;
	int d[9];
	uint64_t idx = 0;
	int clicks = 0, hold = 0;
	std::vector<uint64_t> where;
	ClickDet() { memset(d, 0, sizeof d); }
	void push(int l, int r)
	{
		int s = std::max(abs(l - pl), abs(r - pr));
		pl = l; pr = r;
		for (int i = 0; i < 8; i++) d[i] = d[i + 1];
		d[8] = s;
		idx++;
		if (idx < 9) return;
		int ref = 0;
		for (int i = 0; i < 9; i++) if (i != 4 && d[i] > ref) ref = d[i];
		if (hold > 0) { hold--; return; }
		if (d[4] > 3 * ref + 300) { clicks++; if (where.size() < 32) where.push_back(idx - 5); hold = 8; }
	}
};

/* ---- the sound source -------------------------------------------------------------------------- */
struct Source {
	bool tone = true;
	double acc = 0, phase_l = 0, phase_r = 0;
	std::vector<int16_t> buf;
	Source() : buf(8192 * 2) {}
	/* one emulated frame of audio -> (pointer, frames) */
	int frame(const int16_t **out)
	{
		if (!tone) {
			int n = snes_host_audio_take(buf.data(), 4096);
			*out = buf.data();
			return n;
		}
		acc += (double) SNES_AUDIO_RATE / kNtscHz;
		int n = (int) acc;
		acc -= n;
		for (int i = 0; i < n; i++) {
			buf[i * 2] = (int16_t) (8000 * sin(phase_l));
			buf[i * 2 + 1] = (int16_t) (8000 * sin(phase_r));
			phase_l += 2 * M_PI * 440.0 / SNES_AUDIO_RATE;
			phase_r += 2 * M_PI * 660.0 / SNES_AUDIO_RATE;
		}
		*out = buf.data();
		return n;
	}
};

struct Rec { double t; apace::Counters c; };

struct Scen {
	std::string name;       /* S1, S2+, ... */
	double dur;             /* virtual seconds */
	double audio_ppm;       /* device clock offset */
	double step_at;         /* S3: display switches from exact production to 59.94 at this time (0 = off) */
	int kind;               /* cost model */
};

void write_wav(const char *path, const std::vector<int16_t> &pcm)
{
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint32_t data = (uint32_t) (pcm.size() * 2), rate = OUT_RATE, br = rate * 4, riff = 36 + data, sixteen = 16;
	uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&sixteen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
	fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
	fwrite(pcm.data(), 2, pcm.size(), f);
	fclose(f);
}

const int64_t NS = 1000000000ll;

enum { K_STEADY, K_STALL, K_SLOW185, K_BURST, K_PAUSE };

bool make_scen(const std::string &n, Scen *s)
{
	if (n == "S1") *s = { "S1", 60, 0, 0, K_STEADY };
	else if (n == "S2+") *s = { "S2+", 600, +500, 0, K_STEADY };
	else if (n == "S2-") *s = { "S2-", 600, -500, 0, K_STEADY };
	else if (n == "S3") *s = { "S3", 45, 0, 15, K_STEADY };
	else if (n == "S4") *s = { "S4", 120, 0, 0, K_STALL };
	else if (n == "S5") *s = { "S5", 60, 0, 0, K_SLOW185 };
	else if (n == "S6") *s = { "S6", 60, 0, 0, K_BURST };
	else if (n == "S7") *s = { "S7", 30, 0, 0, K_PAUSE };
	else return false;
	return true;
}

int run_one(const Scen &sc, const AudioSimArgs &a)
{
	using namespace apace;
	Config cfg;
	cfg.legacy = a.legacy;
	if (a.kp >= 0) cfg.kp_ppm = a.kp;
	if (a.ki >= 0) cfg.ki_ppm_per_grain = a.ki;
	Pacer pacer;
	pacer.init(cfg);
	SpeedMeter meter(8);
	Source src;
	src.tone = a.tone;
	ClickDet clicks;
	std::vector<int16_t> wav;
	std::vector<int16_t> gbuf((size_t) pacer.grain() * 2);
	const int G = pacer.grain();
	const int64_t aud_period = (int64_t) llround((double) G / (OUT_RATE * (1.0 + sc.audio_ppm * 1e-6)) * 1e9);
	const int64_t end = (int64_t) (sc.dur * 1e9);

	/* display: vblank edges, lazily generated */
	int64_t next_vb = (int64_t) (1e9 / kDisplayHz);
	unsigned vb_count = 0;
	auto display_hz = [&](int64_t t) { return (sc.step_at > 0 && t < (int64_t) (sc.step_at * 1e9)) ? kNtscHz : kDisplayHz; };
	auto advance_vb = [&](int64_t t) {
		while (next_vb <= t) { vb_count++; next_vb += (int64_t) (1e9 / display_hz(next_vb)); }
	};

	/* main loop state */
	unsigned last_vc = 0;
	int64_t t_main = 0, t_aud = 0;
	bool resuming = false;
	int pause_idx = 0;
	const int64_t pause_at[2] = { 10 * NS, 20 * NS }, pause_len[2] = { 2 * NS, NS / 4 };
	int64_t next_stall = 5 * NS;
	int64_t sec_next = NS, sec_last = 0;
	int sec_passes = 0;
	bool want_poll = false;
	uint64_t total_passes = 0, passes_last20 = 0;
	uint32_t seen_ur = 0;
	std::vector<Rec> recs;
	uint64_t silent_tail = 0, tail_frames = 0;   /* output energy in the last 5 s */
	const uint64_t out_total_frames = (uint64_t) (sc.dur * OUT_RATE);
	uint64_t out_frames = 0;

	auto cost_ns = [&](int64_t t) -> int64_t {
		switch (sc.kind) {
		case K_SLOW185: return 18500000;
		case K_BURST: return (t % NS) < 150000000 ? 25000000 : 8000000;
		case K_STALL: {
			int64_t c = 8000000;
			if (t >= next_stall) { c += 80000000; next_stall += 5 * NS; }
			return c;
		}
		default: return 8000000;
		}
	};
	/* the player's loop: wait for a refresh unless one has already passed, then do the pass */
	auto schedule = [&](int64_t t) {
		if (sc.kind == K_PAUSE && pause_idx < 2 && t >= pause_at[pause_idx]) {
			pacer.set_paused(true);
			meter.reset(); sec_passes = 0;
			t_main = t + pause_len[pause_idx];
			pause_idx++;
			resuming = true;
			return;
		}
		advance_vb(t);
		if (vb_count == last_vc) { t = next_vb; advance_vb(t); }
		last_vc = vb_count;
		t_main = t + cost_ns(t);
	};
	advance_vb(0);
	last_vc = vb_count;
	schedule(0);

	while (t_aud < end || t_main < end) {
		if (t_aud <= t_main) {
			pacer.render(gbuf.data());
			for (int i = 0; i < G; i++) {
				clicks.push(gbuf[i * 2], gbuf[i * 2 + 1]);
				if (out_frames + i >= out_total_frames - 5 * OUT_RATE) {
					tail_frames++;
					if (gbuf[i * 2] || gbuf[i * 2 + 1]) silent_tail++;
				}
			}
			out_frames += G;
			if (a.wav) wav.insert(wav.end(), gbuf.begin(), gbuf.end());
			t_aud += aud_period;
			continue;
		}
		const int64_t t = t_main;
		if (resuming) {
			resuming = false;
			pacer.set_paused(false);
			advance_vb(t);
			last_vc = vb_count;
			sec_next = t + NS;
			sec_last = t;
			schedule(t);
			continue;
		}
		/* a pass finished at time t: its audio goes in */
		if (!a.tone) snes_host_run_frame();
		const int16_t *p;
		int n = src.frame(&p);
		pacer.push(p, n);
		pacer.legacy_update();
		sec_passes++;
		total_passes++;
		if (t >= (sc.dur - 20) * 1e9) passes_last20++;
		if (t >= sec_next) {
			double sp = meter.update(sec_passes, (double) (t - sec_last) / 1e9, kNtscHz);
			sec_last = t;
			if (a.verbose) printf("  speed report %.4f (passes %d)\n", sp, sec_passes);
			if (sp >= 0 && !a.legacy) pacer.report_speed(sp);
			sec_passes = 0;
			sec_next = t + NS;
			want_poll = true;
		}
		if (want_poll) {
			Counters c;
			if (pacer.poll(&c)) {
				want_poll = false;
				recs.push_back({ (double) t / 1e9, c });
				if (a.verbose)
					printf("t=%6.1f in=%u out=%u fill=%.1f/%.1f/%.1f ema=%.1f tgt=%.0f ratio=%.0f/%.0f/%.0f base=%.3f ur=%u/%u drop=%u trim=%u\n",
					    (double) t / 1e9, c.in_frames, c.out_frames, c.fill_ms_min, c.fill_ms_avg, c.fill_ms_max, c.ema_ms, c.target_ms,
					    c.ratio_ppm_min, c.ratio_ppm_avg, c.ratio_ppm_max, c.base_speed, c.underruns_sec, c.underruns_total,
					    c.dropped_sec, c.trims_sec);
			}
		}
		uint32_t ur = pacer.underruns();
		if (ur != seen_ur) {
			seen_ur = ur;
			if (a.verbose) { UnderrunEvent e = pacer.last_underrun(); printf("  UNDERRUN #%u at t=%.3f ema=%.1f ms new target=%.0f ms\n", e.index, (double) t / 1e9, e.ema_ms, e.target_ms); }
		}
		schedule(t);
	}

	/* ---- numbers ---- */
	double fmin = 1e9, fmax = -1e9, fsum = 0, rmin = 1e9, rmax = -1e9, rsum = 0;
	int nrec = 0;
	uint32_t ur_total = 0, dropped = 0, trims = 0;
	for (auto &r : recs) {
		ur_total = r.c.underruns_total; dropped = r.c.dropped_total; trims = r.c.trims_total;
		if (r.t < 6) continue;      /* skip the start-up */
		fmin = std::min(fmin, r.c.fill_ms_min); fmax = std::max(fmax, r.c.fill_ms_max); fsum += r.c.fill_ms_avg;
		rmin = std::min(rmin, r.c.ratio_ppm_min); rmax = std::max(rmax, r.c.ratio_ppm_max); rsum += r.c.ratio_ppm_avg;
		nrec++;
	}
	if (!nrec) { fmin = fmax = rmin = rmax = 0; nrec = 1; }
	/* underruns after the last poll are not in the records: use the live total */
	ur_total = std::max(ur_total, pacer.underruns());

	bool pass = true;
	std::string why;
	const bool nonzero = silent_tail > tail_frames / 2;
	if (a.tone && !nonzero) { pass = false; why += "output silent at the end; "; }
	char tmp[160];
	if (sc.name == "S1" || sc.name == "S2+" || sc.name == "S2-") {
		if (ur_total != 0) { pass = false; snprintf(tmp, sizeof tmp, "%u underruns; ", ur_total); why += tmp; }
		if (clicks.clicks) { pass = false; snprintf(tmp, sizeof tmp, "%d clicks; ", clicks.clicks); why += tmp; }
	} else if (sc.name == "S3") {
		if (ur_total != 0) { pass = false; snprintf(tmp, sizeof tmp, "%u underruns; ", ur_total); why += tmp; }
		if (clicks.clicks) { pass = false; snprintf(tmp, sizeof tmp, "%d clicks; ", clicks.clicks); why += tmp; }
		double worst = 0, ema_end = 0, tgt_end = 0;
		for (auto &r : recs) {
			if (r.t < sc.step_at + 10) continue;
			double e = fabs(r.c.ema_ms - r.c.target_ms) / r.c.target_ms;
			worst = std::max(worst, e);
			ema_end = r.c.ema_ms; tgt_end = r.c.target_ms;
		}
		snprintf(tmp, sizeof tmp, "after step+10s: worst |ema-target|=%.1f%% (end %.1f vs %.1f ms)", worst * 100, ema_end, tgt_end);
		why += tmp;
		if (worst > 0.10) { pass = false; why = "fill not at target (>10%): " + why; }
	} else if (sc.name == "S4") {
		if (ur_total > 1) { pass = false; snprintf(tmp, sizeof tmp, "%u underruns (max 1); ", ur_total); why += tmp; }
		if (clicks.clicks) { pass = false; snprintf(tmp, sizeof tmp, "%d clicks; ", clicks.clicks); why += tmp; }
	} else if (sc.name == "S5") {
		double sum = 0; int k = 0; uint32_t late_ur = 0;
		for (auto &r : recs) {
			if (r.t >= sc.dur - 20) { sum += r.c.ratio_ppm_avg; k++; }
			if (r.t >= 20) late_ur += r.c.underruns_sec;
		}
		double ratio = 1 + (k ? sum / k : 0) * 1e-6;
		double speed = passes_last20 / 20.0 / kNtscHz;
		snprintf(tmp, sizeof tmp, "mean ratio %.4f vs speed %.4f (|diff| %.2f%%); ", ratio, speed, fabs(ratio - speed) * 100);
		why += tmp;
		if (fabs(ratio - speed) > 0.01) { pass = false; why = "pitch does not follow speed: " + why; }
		if (clicks.clicks) { pass = false; snprintf(tmp, sizeof tmp, "%d clicks; ", clicks.clicks); why += tmp; }
		if (late_ur) { pass = false; snprintf(tmp, sizeof tmp, "%u underruns after 20 s; ", late_ur); why += tmp; }
	} else if (sc.name == "S6") {
		/* no number was given for S6: no clicks, and after the first 10 s no underruns (the start-up, before
		 * the speed is known, may cost some) */
		uint32_t late_ur = 0;
		for (auto &r : recs) if (r.t >= 10) late_ur += r.c.underruns_sec;
		if (late_ur) { pass = false; snprintf(tmp, sizeof tmp, "%u underruns after 10 s; ", late_ur); why += tmp; }
		if (clicks.clicks) { pass = false; snprintf(tmp, sizeof tmp, "%d clicks; ", clicks.clicks); why += tmp; }
	} else if (sc.name == "S7") {
		if (ur_total != 0) { pass = false; snprintf(tmp, sizeof tmp, "%u underruns; ", ur_total); why += tmp; }
		if (clicks.clicks) { pass = false; snprintf(tmp, sizeof tmp, "%d clicks; ", clicks.clicks); why += tmp; }
	}
	if (!clicks.where.empty()) {
		why += "first clicks at ";
		for (size_t i = 0; i < clicks.where.size() && i < 4; i++) { snprintf(tmp, sizeof tmp, "%.3fs ", clicks.where[i] / (double) OUT_RATE); why += tmp; }
	}
	printf("SIM %s mode=%s src=%s dur=%.0fs result=%s underruns=%u clicks=%d fill_ms=%.1f/%.1f/%.1f ratio_ppm=%.0f/%.0f/%.0f trims=%u dropped=%u | %s\n",
	    sc.name.c_str(), a.legacy ? "legacy" : "new", a.tone ? "tone" : "core", sc.dur, pass ? "PASS" : "FAIL", ur_total,
	    clicks.clicks, fmin, fsum / nrec, fmax, rmin, rsum / nrec, rmax, trims, dropped, why.c_str());
	if (a.wav) write_wav(a.wav, wav);
	return pass ? 0 : 1;
}

} /* namespace */

/* The click detector must stay quiet on a clean tone and see a step. */
static int detector_selftest()
{
	ClickDet clean, stepped, faded;
	for (int i = 0; i < 48000; i++) {
		int v = (int) (8000 * sin(2 * M_PI * 440.0 * i / OUT_RATE));
		clean.push(v, v);
		stepped.push(i >= 20000 ? v + 3000 : v, v);               /* a DC step of 3000 */
		double g = i < 20000 ? 1.0 : (i < 20064 ? (20064 - i) / 64.0 : 0.0);   /* 64 frame fade to silence */
		faded.push((int) (v * g), (int) (v * g));
	}
	bool ok = clean.clicks == 0 && stepped.clicks == 1 && faded.clicks == 0;
	printf("SIM SD detector self-test result=%s clean=%d step=%d fade=%d\n", ok ? "PASS" : "FAIL", clean.clicks, stepped.clicks, faded.clicks);
	return ok ? 0 : 1;
}

int audio_sim_run(const AudioSimArgs &a)
{
	if (a.scenario == "SD") return detector_selftest();
	std::vector<std::string> names;
	if (a.scenario == "all") names = { "S1", "S2+", "S2-", "S3", "S4", "S5", "S6", "S7" };
	else if (a.scenario == "S2") names = { "S2+", "S2-" };
	else names.push_back(a.scenario);
	int worst = 0;
	for (auto &n : names) {
		Scen sc;
		if (!make_scen(n, &sc)) { fprintf(stderr, "unknown scenario %s (S1..S7, S2+, S2-, all)\n", n.c_str()); return 2; }
		if (a.minutes > 0) sc.dur = a.minutes * 60;
		int r = run_one(sc, a);
		if (r > worst) worst = r;
	}
	return worst;
}
