/* A threaded stress run of pk::Handoff, shared by the unit tests (AddressSanitizer) and the ThreadSanitizer binary.
 * No dependence on the unit-test harness: failures are counted and returned. Linux only (std::thread).
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#ifndef PK_HANDOFF_STRESS_H
#define PK_HANDOFF_STRESS_H

#include "pk_handoff.h"
#include "pk_handoff_posix.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>

namespace pkstress {

struct Cfg {
	int frames = 2000;
	int depth = 2;
	bool adaptive = true;
	bool modes = false;            /* toggle half mode, fast-forward drop, flush, discard, disable/enable while running */
	int consumer_delay_us = 150;   /* a random delay up to this per picture (the drawing work) */
	int producer_delay_us = 50;
	unsigned seed = 1;
};

struct Out {
	int failures = 0;
	uint32_t produced = 0, shown = 0, dropped = 0;
	uint32_t seen = 0;            /* pictures the consumer verified */
};

static const uint32_t BYTES = 2048;

inline uint8_t pat(uint32_t seq, uint32_t i) { return (uint8_t) (seq * 131u + i * 7u + (seq >> 8)); }

inline Out run(const Cfg &c)
{
	Out out;
	pk::PosixHandoffSys sys;
	pk::Handoff h;
	pk::HandoffConfig hc;
	hc.frame_bytes = BYTES;
	hc.depth.max_depth = c.depth;
	hc.adaptive = c.adaptive;
	hc.gate_timeout_us = 20000000;   /* a starved consumer on a busy host must not look like a lost picture */
	if (!h.init(hc, &sys)) { out.failures++; return out; }

	std::atomic<int> failures(0), busy(0);
	std::atomic<uint32_t> seen(0);
	std::thread consumer([&]() {
		unsigned rs = c.seed * 2654435761u + 1;
		bool have = false;
		uint32_t last = 0;
		for (;;) {
			pk::HandoffFrame *f = h.take();
			if (!f) break;
			busy.store(1);
			/* ordering: strictly increasing (older pictures may be dropped, never reordered or shown twice) */
			if (have && f->seq <= last) failures++;
			have = true; last = f->seq;
			/* no torn picture: every byte is this frame's pattern */
			if (f->w != 64 || f->h != 32 || f->pitch != 64) failures++;
			for (uint32_t i = 0; i < BYTES; i++)
				if (f->data[i] != pat(f->seq, i)) { failures++; break; }
			rs = rs * 1664525u + 1013904223u;
			if (c.consumer_delay_us) std::this_thread::sleep_for(std::chrono::microseconds(rs % (unsigned) (c.consumer_delay_us + 1)));
			seen++;
			busy.store(0);
			h.release(f, 100 + rs % 1000);
		}
	});

	unsigned rs = c.seed * 40503u + 7;
	bool half = false, ff = false;
	int mode_left = 0;
	for (int n = 0; n < c.frames; n++) {
		if (c.modes) {
			rs = rs * 1664525u + 1013904223u;
			const unsigned r = (rs >> 8) % 400;
			if (r == 0) { half = !half; h.set_pace_mode(half ? pk::PACE_HALF : pk::PACE_FULL); }
			else if (r == 1) { ff = !ff; h.set_force_drop(ff); }
			else if (r == 2) {
				/* a pause / menu: afterwards nothing is queued, nothing is being drawn, and every picture is accounted for */
				bool ok = h.flush();
				pk::HandoffCensus k = h.census();
				if (!ok || busy.load() || k.shown + k.dropped != k.produced) failures++;
			} else if (r == 3) {
				bool ok = h.discard();
				pk::HandoffCensus k = h.census();
				if (!ok || busy.load() || k.shown + k.dropped != k.produced) failures++;
			} else if (r == 4 && mode_left == 0) {
				/* link lockstep: off, the host draws inline; then on again */
				h.set_enabled(false);
				if (h.begin_write() != 0) failures++;
				if (busy.load()) failures++;
				h.set_enabled(true);
			}
		}
		pk::HandoffFrame *f = h.begin_write();
		if (!f) { failures++; break; }
		for (uint32_t i = 0; i < BYTES; i++) f->data[i] = pat((uint32_t) n, i);
		f->w = 64; f->h = 32; f->pitch = 64; f->seq = (uint32_t) n; f->emu_us = 2000;
		h.commit(f);
		h.note_emulated(1);
		uint32_t cost;
		while (h.poll_cost(&cost)) {}
		if (c.producer_delay_us) {
			rs = rs * 1664525u + 1013904223u;
			std::this_thread::sleep_for(std::chrono::microseconds((rs >> 4) % (unsigned) (c.producer_delay_us + 1)));
		}
	}
	h.set_force_drop(false);
	h.set_pace_mode(pk::PACE_FULL);
	if (!h.flush(5000000)) failures++;
	pk::HandoffCensus k = h.census();
	if (k.shown + k.dropped != k.produced) failures++;
	h.shutdown();
	consumer.join();
	out.failures = failures.load();
	out.produced = k.produced; out.shown = k.shown; out.dropped = k.dropped;
	out.seen = seen.load();
	if (out.seen != k.shown) out.failures++;
	if (!c.modes && k.dropped != 0) out.failures++;       /* the full mode never drops */
	if (!c.modes && k.produced != (uint32_t) c.frames) out.failures++;
	return out;
}

}  // namespace pkstress

#endif
