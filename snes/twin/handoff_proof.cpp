/*
 * player_twin handoff ROM WORK: proof that "Smooth hand-over" changes nothing about the pictures.
 *
 * The same game and the same scripted pad run twice with the player's own code (snes/vita/player.cpp): once drawing
 * the way the player did before (each new picture hashed straight from the core), once through the picture pipe
 * (snes/vita/pipe.cpp, playerkit's pk_handoff mailbox) with a real second thread taking the pictures and a random
 * slow-down on that thread, so the gate and the dropping are exercised. Steady pacing is driven into its half mode in
 * the middle of the run, the mailbox is flushed once, and switched off and on again once (the two-player lockstep case).
 * Every picture the pipe delivers must be bit for bit the picture the plain run made for that frame, in order,
 * and in the stretches that are not in half mode every picture must arrive.
 *
 * Also the setting row: default on, saved and read back.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#include "player.h"
#include "pipe.h"
#include "pk_handoff_posix.h"

#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <vector>

using namespace pk;
#undef W_OK

namespace {

int g_fail = 0, g_checks = 0;
#define HEXPECT(c) do { g_checks++; if (!(c)) { fprintf(stderr, "handoff proof: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct Plat : snesp::Platform {
	uint64_t clock_us = 1000000;
	std::string log_path;
	uint64_t now_us() { return clock_us; }
	void pause_us(unsigned) {}
	bool wake_writer() { return false; }
	bool current_frame(Frame *) { return false; }
	void ask_cheat_text() {}
	void log_write(const std::string &chunk)
	{
		FILE *f = fopen(log_path.c_str(), "ab");
		if (f) { fwrite(chunk.data(), 1, chunk.size(), f); fclose(f); }
	}
};

uint64_t fnv(const uint8_t *p, int w, int h, int pitch)
{
	uint64_t x = 1469598103934665603ull;
	for (int y = 0; y < h; y++)
		for (int i = 0; i < w * 2; i++) { x ^= p[(size_t) y * pitch + i]; x *= 1099511628211ull; }
	return x ^ ((uint64_t) w << 32) ^ (uint64_t) h;
}

struct Rec { uint32_t seq; uint64_t hash; };

struct PassOut {
	std::vector<Rec> shown;
	uint32_t produced = 0, dropped = 0, gate_waits = 0;
	int depth_end = 0;
	std::string census;
	bool started = false;
	std::vector<char> half;          /* steady pacing's mode when each frame was handed over */
};

const int FRAMES = 700;
const int HALF_FROM = 150, HALF_TO = 300;          /* the pacer is driven into half mode in here */
const int OFF_FROM = 400, OFF_TO = 440;            /* the mailbox is off in here (lockstep) */
const int FLUSH_AT = 100;

uint32_t pad_script(int f)
{
	/* a few taps and a held direction so the picture changes */
	if (f % 90 >= 10 && f % 90 < 14) return PH_CROSS;
	if (f % 200 >= 120 && f % 200 < 180) return PH_RIGHT;
	return 0;
}

bool run_pass(const char *rom, const std::string &work, bool use_pipe, PassOut *out)
{
	const std::string root = work + (use_pipe ? "/root_on" : "/root_off");
	make_dirs(root);
	Plat pf;
	pf.log_path = root + "/snes/log.txt";
	snesp::SnesPlayer *pl = new snesp::SnesPlayer(&pf);
	char a0[] = "app0:snes.bin";
	std::string arg = rom;
	char *av[] = { a0, &arg[0], 0 };
	snesp::StartResult sr = pl->start(2, av, root, true);
	HEXPECT(sr.ok);
	if (!sr.ok) { delete pl; return false; }
	pl->apply_start_mode();

	PosixHandoffSys sys;
	snesp::PicturePipe pipe;
	std::thread consumer;
	std::atomic<int> busy(0), bad(0);
	if (use_pipe) {
		out->started = pipe.start(&sys, 2, true, 16683, 16639);
		HEXPECT(out->started);
		consumer = std::thread([&]() {
			unsigned rs = 12345;
			for (;;) {
				HandoffFrame *f = pipe.box().take();
				if (!f) break;
				busy = 1;
				Rec r; r.seq = f->seq; r.hash = fnv(f->data, f->w, f->h, f->pitch);
				if (f->pitch != f->w * 2) bad++;
				out->shown.push_back(r);
				rs = rs * 1664525u + 1013904223u;
				std::this_thread::sleep_for(std::chrono::microseconds(rs % 1500));      /* a drawing thread that is slow now and then */
				busy = 0;
				pipe.box().release(f, 4000 + rs % 9000);
			}
		});
	}

	unsigned inline_serial = 0;
	for (int f = 0; f < FRAMES; f++) {
		PadSample p[MAX_PLAYERS];
		for (int i = 0; i < MAX_PLAYERS; i++) { p[i] = PadSample(); p[i].connected = i < 2; }
		p[0].buttons = pad_script(f);
		pf.clock_us += 16683;
		pl->input(p, false, pf.clock_us / 1000, (uint32_t) f);
		snes_host_set_render(pl->want_render((uint32_t) f) ? 1 : 0);
		snes_host_run_frame();
		int16_t ab[4096 * 2];
		while (snes_host_audio_take(ab, 4096) > 0) {}
		bool off = f >= OFF_FROM && f < OFF_TO;
		if (use_pipe) {
			if (f == OFF_FROM) pipe.box().set_enabled(false);
			if (f == OFF_TO) pipe.box().set_enabled(true);
			if (f == FLUSH_AT) {
				/* a pause or a thumbnail: afterwards nothing is queued or in the drawing thread's hands */
				HEXPECT(pipe.box().flush());
				HandoffCensus k = pipe.box().census();
				HEXPECT(busy.load() == 0);
				HEXPECT(k.shown + k.dropped == k.produced);
			}
			pipe.box().set_pace_mode(pl->pace_mode());
			pipe.box().note_emulated(1);
		}
		if (!use_pipe || off) {
			/* the way it was: the newest finished picture, once per new picture, straight from the core */
			int w = 0, h = 0, pitch = 0; unsigned serial = 0;
			const uint16_t *v = snes_host_video_nowait(&w, &h, &pitch, &serial);
			if (v && serial != inline_serial) {
				inline_serial = serial;
				Rec r; r.seq = (uint32_t) f; r.hash = fnv((const uint8_t *) v, w, h, pitch);
				out->shown.push_back(r);
			}
			if (use_pipe && off) HEXPECT(pipe.stage((uint32_t) f, 4000) == -1);       /* off: the host draws inline */
		} else {
			int r = pipe.stage((uint32_t) f, 4000);
			HEXPECT(r >= 0);
		}
		/* the same cost script in both runs, so steady pacing decides the same way in both */
		out->half.push_back(pl->pace_mode() == PACE_HALF ? 1 : 0);
		pl->drawn_cost(f >= HALF_FROM && f < HALF_TO ? 60000u : 9000u);
		pl->after_frame(pf.clock_us);
		uint32_t c;
		if (use_pipe) while (pipe.box().poll_cost(&c)) {}
		}
	if (use_pipe) {
		HEXPECT(pipe.box().flush(5000000));
		HandoffCensus k = pipe.box().census();
		out->produced = k.produced; out->dropped = k.dropped; out->gate_waits = k.gate_waits; out->depth_end = k.depth;
		HEXPECT(k.shown + k.dropped == k.produced);
		char line[512];
		const uint64_t tnow = sys.now_us();                         /* the census clock is the mailbox's own */
		pipe.box().census_line(tnow, line, sizeof line);            /* opens the window */
		pipe.box().census_line(tnow + 2000000, line, sizeof line);
		out->census = line;
		pipe.stop();
		consumer.join();
		HEXPECT(bad.load() == 0);
	}
	pl->finish(EXIT_NORMAL);
	delete pl;
	snes_host_unload();
	return true;
}

}  // namespace

int run_handoff_test(const char *rom, const std::string &work)
{
	make_dirs(work);
	PassOut off, on;
	/* ---- the setting row ---- */
	{
		const std::string root = work + "/root_set";
		make_dirs(root);
		Plat pf;
		pf.log_path = root + "/snes/log.txt";
		char a0[] = "app0:snes.bin";
		std::string arg = rom;
		char *av[] = { a0, &arg[0], 0 };
		snesp::SnesPlayer *pl = new snesp::SnesPlayer(&pf);
		HEXPECT(pl->start(2, av, root, true).ok);
		HEXPECT(pl->smooth_handoff());                                             /* default: on */
		HEXPECT(pl->menu().value(snesp::SnesPlayer::ROW_HANDOFF) == 1);
		pl->on_change(snesp::SnesPlayer::ROW_HANDOFF, 0);
		HEXPECT(!pl->smooth_handoff());
		pl->finish(EXIT_NORMAL);
		delete pl;
		snes_host_unload();
		snesp::SnesPlayer *p2 = new snesp::SnesPlayer(&pf);
		HEXPECT(p2->start(2, av, root, true).ok);
		HEXPECT(!p2->smooth_handoff());                                            /* kept */
		HEXPECT(p2->menu().value(snesp::SnesPlayer::ROW_HANDOFF) == 0);
		p2->on_change(snesp::SnesPlayer::ROW_HANDOFF, 1);
		p2->finish(EXIT_NORMAL);
		delete p2;
		snes_host_unload();
	}
	/* ---- the pictures ---- */
	if (!run_pass(rom, work, false, &off)) return 1;
	if (!run_pass(rom, work, true, &on)) return 1;
	std::map<uint32_t, uint64_t> ref;
	for (size_t i = 0; i < off.shown.size(); i++) ref[off.shown[i].seq] = off.shown[i].hash;
	HEXPECT(off.shown.size() > 100);
	HEXPECT(on.shown.size() > 100);
	size_t wrong = 0, unordered = 0, unknown = 0;
	uint32_t last = 0;
	bool have = false;
	std::map<uint32_t, bool> got;
	for (size_t i = 0; i < on.shown.size(); i++) {
		const Rec &r = on.shown[i];
		if (have && r.seq <= last) unordered++;
		have = true; last = r.seq;
		got[r.seq] = true;
		std::map<uint32_t, uint64_t>::const_iterator it = ref.find(r.seq);
		if (it == ref.end()) unknown++;
		else if (it->second != r.hash) wrong++;
	}
	HEXPECT(wrong == 0);                 /* bit for bit */
	HEXPECT(unknown == 0);               /* nothing the plain run did not make */
	HEXPECT(unordered == 0);             /* in order, none twice */
	/* outside half mode nothing may be missing; in half mode older pictures may be dropped, newest wins */
	size_t missing_full = 0, missing_half = 0, in_half = 0;
	for (std::map<uint32_t, uint64_t>::const_iterator it = ref.begin(); it != ref.end(); ++it) {
		const size_t f = it->first;
		bool half_near = false;
		for (size_t k = f > 2 ? f - 2 : 0; k <= f + 4 && k < on.half.size(); k++) if (on.half[k]) half_near = true;
		if (half_near) in_half++;
		if (!got.count(it->first)) { if (half_near) missing_half++; else missing_full++; }
	}
	HEXPECT(missing_full == 0);
	HEXPECT(in_half > 100);               /* the script really spent a long stretch in half mode */
	fprintf(stderr, "handoff proof: plain run %zu pictures, hand-over run %zu pictures (in or next to half mode: %zu made, %zu not shown); "
	    "produced %u, dropped %u, gate waits %u, depth now %d\n", off.shown.size(), on.shown.size(), in_half, missing_half,
	    on.produced, on.dropped, on.gate_waits, on.depth_end);
	fprintf(stderr, "handoff proof: %s\n", on.census.c_str());
	HEXPECT(on.census.find("handoff:") != std::string::npos);
	HEXPECT(on.produced > 100);
	fprintf(stderr, "handoff proof: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
