/*
 * wide_pipe_proof ROM [--system md|sms|gg|sg] [--frames N] [--margin M] [--strip S]
 *
 * Proof that the wide view survives the picture pipe the player uses (md/vita/main.cpp): the core draws beside the picture into a mailbox
 * buffer (direct) or into a canvas whose frame is copied with its strips (put), the Meta of the frame travels in a ring indexed by the frame
 * number, a second thread composes. Every wide picture that thread makes must be bit for bit the picture the same frame gives when it is composed
 * at once on the emulating thread (the way the player does with the hand-over off). Steady pacing is in full mode, so no picture is dropped.
 * Runs three passes: inline, pipe with direct drawing, pipe with copies (the gate closed / drop mode path).
 * Prints "WIDE-PIPE <rom> frames=N direct=D copies=C mismatches=X" and exits 1 on a mismatch.
 *
 * Copyright CBOC-Vita. MIT licence for this file; the core keeps its own licence.
 */
#include "host.h"
#include "wide.h"
#include "../core/thread_ops.h"
#include "../vita/pipe.h"
#include "pk_handoff_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static uint64_t fnv(const uint16_t *p, int w, int h, int pitch_px)
{
	uint64_t x = 1469598103934665603ull;
	for (int y = 0; y < h; y++)
		for (int i = 0; i < w; i++) { x ^= p[(size_t) y * pitch_px + i] & 255; x *= 1099511628211ull; x ^= p[(size_t) y * pitch_px + i] >> 8; x *= 1099511628211ull; }
	return x ^ ((uint64_t) w << 32) ^ (uint64_t) h;
}

static unsigned auto_mask(int f)
{
	unsigned s = (unsigned) (f / 24) * 2654435761u;
	s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
	unsigned m = 0;
	if (s & 0x1) m |= MD_RIGHT;
	if (s & 0x2) m |= MD_LEFT;
	if (s & 0x4) m |= MD_UP;
	if (s & 0x8) m |= MD_DOWN;
	if (s & 0x10) m |= MD_A;
	if (s & 0x20) m |= MD_B;
	if (s & 0x40) m |= MD_C;
	if (m & MD_LEFT && m & MD_RIGHT) m &= ~MD_LEFT;
	if (m & MD_UP && m & MD_DOWN) m &= ~MD_UP;
	if ((f % 150) >= 40 && (f % 150) < 46) m |= MD_START;
	return m;
}

/* threads for the VDP worker (the analysis of the wide view runs on it) */
static pthread_t g_wthr[MD_WORKERS];
struct SpawnArg { void (*fn)(void *); void *arg; };
static SpawnArg g_sarg[MD_WORKERS];
static void *thr_main(void *p) { SpawnArg *a = (SpawnArg *) p; a->fn(a->arg); return NULL; }
static void *t_sem_create(void *) { sem_t *s = new sem_t; sem_init(s, 0, 0); return s; }
static void t_sem_destroy(void *, void *s) { sem_destroy((sem_t *) s); delete (sem_t *) s; }
static void t_sem_post(void *, void *s) { sem_post((sem_t *) s); }
static void t_sem_wait(void *, void *s) { while (sem_wait((sem_t *) s) != 0) {} }
static int t_spawn(void *, int id, void (*fn)(void *), void *arg) { g_sarg[id].fn = fn; g_sarg[id].arg = arg; return pthread_create(&g_wthr[id], NULL, thr_main, &g_sarg[id]); }
static void t_wait_end(void *, int id) { pthread_join(g_wthr[id], NULL); }
static void t_yield(void *) { sched_yield(); }
static md_thread_ops g_tops = { NULL, t_sem_create, t_sem_destroy, t_sem_post, t_sem_wait, t_spawn, t_wait_end, t_yield, NULL, NULL };
static void yield_fn(void) { sched_yield(); }

struct Sess {
	std::vector<uint16_t> canvas;
	mdw::Wide wide;
	mdw::Meta ring[8];
	int system = 0, margin = 45, strip = 45;
};

/* one pass: mode 0 inline, 1 pipe + direct, 2 pipe + copies; hashes[seq] of every composed picture */
static bool run_pass(const std::vector<uint8_t> &rom, const char *name, int system, int frames, int margin, int strip, int mode, std::vector<uint64_t> *hashes,
                     int *ndirect, int *ncopy, bool worker)
{
	/* the pipe passes run the way the player does: the VDP on its worker thread, the analysis of the wide view on that thread while the emulating
	 * thread goes on (frame(), not analyse()), the compose on a third thread that waits for the analysis if it has to */
	const bool async = worker && mode > 0;
	if (async) md_host_set_threads(&g_tops, MD_THREAD_VDP | MD_THREAD_VDP_SMS);
	mdw::Wide::set_yield(yield_fn);
	if (md_host_init()) return false;
	md_host_set_seed(1);
	char err[128];
	if (md_host_load_mem(rom.data(), rom.size(), name, system, err, sizeof err)) { printf("LOADFAIL %s\n", err); return false; }
	Sess *S = new Sess;
	S->canvas.assign((size_t) MD_WIDE_PITCH * MD_MAX_HEIGHT, 0);
	S->system = md_host_system();
	S->margin = S->system == MD_SYS_GG ? 48 : margin;
	S->strip = strip;
	if (S->system == MD_SYS_GG) md_host_set_option("gg_band", 1);
	S->wide.set_real_cap_default(S->system == MD_SYS_MD ? strip : (1 << 20));
	hashes->assign((size_t) frames + 2, 0);

	pk::PosixHandoffSys sys;
	mdp::PicturePipe pipe;
	std::thread consumer;
	if (mode > 0) {
		if (!pipe.start(&sys, 2, false, 16683, 16639)) return false;      /* full mode: the producer waits at the gate, nothing is dropped */
		consumer = std::thread([&]() {
			for (;;) {
				pk::HandoffFrame *f = pipe.box().take();
				if (!f) break;
				mdw::Meta &m = S->ring[f->seq & 7];
				const uint16_t *v0 = (const uint16_t *) (f->data + f->offset) + m.view_off;
				int w = 0, h = 0;
				/* the lines shared between this thread and two more, the way the player's scaler helpers take them */
				const uint16_t *wp = NULL;
				const int nl = S->wide.compose_begin(m, v0, f->pitch / 2, &w, &h);
				if (nl > 0) {
					std::atomic<int> next(0);
					auto work = [&](int who) { for (;;) { const int c = next.fetch_add(1); if (c * 32 >= nl) break; S->wide.compose_lines(c * 32, (c + 1) * 32 < nl ? (c + 1) * 32 : nl, who); } };
					std::thread t1(work, 1), t2(work, 2);
					work(0);
					t1.join(); t2.join();
					wp = S->wide.compose_end();
				}
				(*hashes)[f->seq] = wp ? fnv(wp, w, h, w) : fnv(v0, m.vw, m.vh, f->pitch / 2);
				pipe.box().release(f, 4000);
			}
		});
	}
	for (int f = 1; f <= frames; f++) {
		md_host_set_pad(0, auto_mask(f));
		uint16_t *dfb = NULL;
		if (mode == 1) { int dp = 0; dfb = pipe.direct_begin(&dp); if (dfb) md_host_set_framebuffer(dfb + MD_WIDE_MAX, dp); }
		if (!dfb) md_host_set_framebuffer(S->canvas.data() + MD_WIDE_MAX, MD_WIDE_PITCH);
		md_host_set_wide_margin(S->system == MD_SYS_MD ? strip : margin);
		md_host_run_frame();
		int w = 0, h = 0, pp = 0;
		const uint16_t *pix = md_host_video(&w, &h, &pp);
		md_host_set_framebuffer(NULL, 0);
		int16_t ab[4096 * 2];
		while (md_host_audio_take(ab, 4096) > 0) {}
		md_video_info vi;
		md_host_video_info(&vi);
		mdw::Meta *m = &S->ring[(unsigned) f & 7];
		if (async) S->wide.frame((uint32_t) f, vi, S->margin, strip, m);
		else S->wide.analyse((uint32_t) f, vi, S->margin, strip, m);
		int off = vi.system == MD_SYS_GG ? 48 : 0;
		const uint16_t *src = pix;
		int sw = w;
		if (mode > 0 && !dfb && vi.system == MD_SYS_MD && strip > 0) { src = pix - strip; sw = w + 2 * strip; off += strip; }
		m->view_off = off;
		if (mode == 0) {
			int ow = 0, oh = 0;
			const uint16_t *wp = S->wide.compose(*m, pix + off, pp / 2, &ow, &oh);
			(*hashes)[(size_t) f] = wp ? fnv(wp, ow, oh, ow) : fnv(pix + off, m->vw, m->vh, pp / 2);
		} else {
			int r = dfb ? pipe.direct_commit(pix, w, h, pp, 0, (uint32_t) f, 4000) : pipe.put(src, sw, h, pp, 0, (uint32_t) f, 4000);
			if (r <= 0) { fprintf(stderr, "frame %d not handed over (%d)\n", f, r); }
			(dfb ? *ndirect : *ncopy)++;
			if (dfb && pipe.direct_open()) pipe.direct_abandon();
		}
	}
	if (mode > 0) {
		pipe.box().flush(5000000);
		pipe.stop();
		consumer.join();
	}
	S->wide.sync();
	delete S;
	md_host_unload();
	md_host_shutdown();
	return true;
}

int main(int argc, char **argv)
{
	const char *rom = NULL;
	int system = 0, frames = 300, margin = 45, strip = 45, worker = 1, one = -1;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--system" && i + 1 < argc) { std::string r = argv[++i]; system = r == "md" ? MD_SYS_MD : r == "sms" ? MD_SYS_SMS : r == "gg" ? MD_SYS_GG : r == "sg" ? MD_SYS_SG : 0; }
		else if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]);
		else if (a == "--margin" && i + 1 < argc) margin = atoi(argv[++i]);
		else if (a == "--strip" && i + 1 < argc) strip = atoi(argv[++i]);
		else if (a == "--no-worker") worker = 0;
		else if (a == "--one" && i + 1 < argc) one = atoi(argv[++i]);
		else if (a[0] != '-' && !rom) rom = argv[i];
		else { fprintf(stderr, "usage: wide_pipe_proof ROM [--system md|sms|gg|sg] [--frames N] [--margin M] [--strip S]\n"); return 2; }
	}
	if (!rom) return 2;
	FILE *f = fopen(rom, "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> data((size_t) n);
	if (fread(data.data(), 1, data.size(), f) != data.size()) { fclose(f); return 1; }
	fclose(f);
	const char *bn = strrchr(rom, '/'); bn = bn ? bn + 1 : rom;
	if (one >= 0) {
		/* one pass in this process (for a thread sanitizer run): prints a hash of the composed pictures' hashes */
		std::vector<uint64_t> hv;
		int d = 0, c = 0;
		if (!run_pass(data, bn, system, frames, margin, strip, one, &hv, &d, &c, worker != 0)) return 1;
		uint64_t hh = 1469598103934665603ull;
		for (size_t i = 0; i < hv.size(); i++) { hh ^= hv[i]; hh *= 1099511628211ull; }
		printf("WIDE-PIPE-ONE %s mode=%d frames=%d direct=%d copies=%d hash=%016llx\n", bn, one, frames, d, c, (unsigned long long) hh);
		return 0;
	}
	/* every pass in a process of its own: the core keeps some memory across a load in the same process (not a wide view matter, the player is
	 * started per game), and a pass must start from the same nothing */
	struct Out { uint64_t h[2048]; int direct, copies, ok; };
	if (frames > 2040) frames = 2040;
	Out *o[3];
	for (int k = 0; k < 3; k++) {
		o[k] = (Out *) mmap(NULL, sizeof(Out), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		memset(o[k], 0, sizeof(Out));
		fflush(stdout);
		const pid_t pid = fork();
		if (pid == 0) {
			std::vector<uint64_t> hv;
			int d = 0, c = 0;
			const bool ok = run_pass(data, bn, system, frames, margin, strip, k, &hv, &d, &c, worker != 0);
			for (size_t i = 0; i < hv.size() && i < 2048; i++) o[k]->h[i] = hv[i];
			o[k]->direct = d; o[k]->copies = c; o[k]->ok = ok ? 1 : 0;
			_exit(0);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (!o[k]->ok) return 1;
	}
	std::vector<uint64_t> h0(o[0]->h, o[0]->h + 2048), h1(o[1]->h, o[1]->h + 2048), h2(o[2]->h, o[2]->h + 2048);
	const int d1 = o[1]->direct, c2 = o[2]->copies;
	int bad = 0, zero = 0, b1 = 0, b2 = 0, first = 0;
	for (int i = 1; i <= frames; i++) {
		if (h0[(size_t) i] != h1[(size_t) i]) { b1++; if (!first) first = i; }
		if (h0[(size_t) i] != h2[(size_t) i]) b2++;
		if (h0[(size_t) i] != h1[(size_t) i] || h0[(size_t) i] != h2[(size_t) i]) bad++;
		if (!h0[(size_t) i]) zero++;
	}
	printf("WIDE-PIPE %s frames=%d direct=%d copies=%d worker=%d mismatches=%d (direct %d, copies %d, first at %d) empty=%d\n", bn, frames, d1, c2, worker, bad, b1, b2, first, zero);
	return bad || zero ? 1 : 0;
}
