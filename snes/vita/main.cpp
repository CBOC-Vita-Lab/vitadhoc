/*
 * snes.bin: the SNES program of VitAdhoc (Snes9x core, separate executable).
 *
 * Launched by VitAdhoc with sceAppMgrLoadExec("app0:snes.bin", {rom path}) and, from VitAdhoc 1.4, a launch
 * file (launch/snes.ini). Quitting launches app0:eboot.bin again. The player's behaviour (menu, states,
 * saves, cheats, launch protocol, pacing) is player.cpp, shared with the Linux twin; this file is the
 * console around it: screen, sound, pads, touch, keyboard, clocks and threads.
 *
 * This file and the other files in snes/frontend and snes/vita are new code (MIT); the Snes9x core in
 * snes/core keeps its own non-commercial licence. The menu library is playerkit (MIT). Only for a two-player
 * link (netplay.cpp) the files listed in netplay.mk are taken from gpsp/ (the licence note there).
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <atomic>
#include <stdarg.h>
#include <math.h>
#include <string>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/display.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include "host.h"
#include "wide.h"
#include "picture.h"
#include "audio_pace.h"
#include "player.h"
#include "pipe.h"
#include "pal_pace.h"
#include "pk_palsmooth.h"
#include "pk_gfx_vita.h"
#include "pk_cores.h"
#include "pk_scale.h"
#include "netplay.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "s9x_prof.h"

extern "C" {
unsigned int sceUserMainThreadStackSize = 1024 * 1024;
unsigned int _newlib_heap_size_user = 72 * 1024 * 1024;     /* 56 MB before rewind: its 12 MB ring is on top */
}

#define APP_ROOT "ux0:data/GBAdhoc"
#define DATA_ROOT APP_ROOT "/snes"
#define SCREEN_W 960
#define SCREEN_H 544

static snesp::SnesPlayer *g_pl;
static snesp::PicturePipe g_pipe;      /* frame hand-over to the drawing thread (pipe.h); idle unless started */
#define logf_(...) g_pl->logf(__VA_ARGS__)
static FILE *g_log;

/* ---- audio ------------------------------------------------------------- */
/* The pacing itself (ring, resampler, rate control, underrun handling) is frontend/audio_pace.cpp, shared
 * with the Linux twin's simulation. This is only the thread around it. */

static const int OUT_RATE = 48000;
static const int PRIO_AUDIO_DEFAULT = 0x40, PRIO_MAIN_DEFAULT = 0x60;   /* threads.ini: audio_prio, main_prio (0 = leave) */
static int g_aport = -1;
static SceUID g_athread = -1;
static volatile int g_quit;
static apace::Pacer g_pace;
static std::atomic<unsigned> g_agap_max_us(0);   /* longest gap between two sceAudioOutOutput returns */

/* The fourth core (playerkit pk_cores, docs/FOURTH-CORE.md): probed once at start-up; only when it is proven usable
 * (CapUnlocker plugin) the audio thread moves to it, so the sound-chip thread has core 2 to itself. Without it
 * nothing below changes: the plan puts the audio thread on core 2 exactly as before. */
static pk::Cores *g_cores;
static SceUID g_help_thread[2] = { -1, -1 };   /* the pixel art scaler's helpers */
static pk::Plan g_core_plan;
static int g_wd_audio = -1;              /* watchdog slot of the audio thread; only registered when it runs on core 3 */

static int audio_thread(SceSize, void *)
{
	static int16_t out[apace::Pacer::GRAIN_LEGACY * 2];
	uint64_t prev = 0;
	while (!g_quit) {
		g_pace.render(out);
		sceAudioOutOutput(g_aport, out);
		if (g_wd_audio >= 0) g_cores->watchdog().beat(g_wd_audio);
		const uint64_t t = sceKernelGetProcessTimeWide();
		if (prev) {
			const unsigned gap = (unsigned) (t - prev);
			if (gap > g_agap_max_us.load(std::memory_order_relaxed)) g_agap_max_us.store(gap, std::memory_order_relaxed);
		}
		prev = t;
	}
	return 0;
}

static void audio_init(bool legacy, int prio)
{
	apace::Config cfg;
	cfg.in_rate = SNES_AUDIO_RATE;
	cfg.out_rate = OUT_RATE;
	cfg.legacy = legacy;
	g_pace.init(cfg);
	g_aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, g_pace.grain(), OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
	if (g_aport < 0) { logf_("audio open failed %08x\n", g_aport); return; }
	int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume(g_aport, (SceAudioOutChannelFlag) (SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
	const pk::Slot *aslot = g_core_plan.find(pk::R_AUDIO_OUT);
	const unsigned amask = aslot ? aslot->mask : (unsigned) SCE_KERNEL_CPU_MASK_USER_2;   /* USER_2 unless core 3 is proven */
	if (aslot && aslot->on_core3) {
		g_wd_audio = g_cores->watchdog().add("snes_audio");
		g_cores->watchdog().set_busy(g_wd_audio, true);
		g_cores->watchdog().watch(g_wd_audio, true);
		logf_("audio thread on the extra core (core 3)\n");
	}
	g_athread = sceKernelCreateThread("snes_audio", audio_thread, prio, 0x4000, 0, amask, NULL);
	if (g_athread >= 0) sceKernelStartThread(g_athread, 0, NULL);
	logf_("audio: %s, grain %d frames at %d Hz, thread priority 0x%x (%s)\n", legacy ? "LEGACY (first player's pacing)" : "pacer v2",
	    g_pace.grain(), OUT_RATE, prio, g_athread >= 0 ? "started" : "NOT started");
}

static pk::Request core4_request(void)
{
	pk::Request rq(pk::CP_SNES);
	rq.with(pk::R_EMU_MAIN).with(pk::R_RENDER).with(pk::R_APU).with(pk::R_AUDIO_OUT).with(pk::R_UPSCALE);
	rq.upscale_n = 2;     /* two helpers (this player uses two): core 0 and core 2, in the gaps of the emulating and the sound chip threads; core 1 holds the composing draw thread and the PPU thread */
	return rq;
}

/* threads.ini: core4=auto (default) or core4=off; the probe runs before any worker thread exists */
static void core4_start(const char *path)
{
	pk::Core4Mode mode = pk::CORE4_AUTO;
	if (FILE *f = fopen(path, "rb")) {
		char line[96];
		while (fgets(line, sizeof line, f))
			if (!strncmp(line, "core4=", 6) && (!strncasecmp(line + 6, "off", 3) || line[6] == '0')) mode = pk::CORE4_OFF;
		fclose(f);
	}
	g_cores = new pk::Cores(pk::vita_core_sys(), mode);
	g_cores->set_log([](void *, const char *l) { logf_("%s\n", l); }, NULL);
	g_cores->probe();
	g_core_plan = g_cores->plan(core4_request());
	char c[220];
	g_cores->census_line(c, sizeof c);
	logf_("%s\n%s\n", c, g_cores->status_text());
}

/* the watchdog found the audio thread dead on core 3: back to core 2 (the three-core layout) */
static void core4_fallback(void)
{
	g_core_plan = g_cores->plan(core4_request());
	const pk::Slot *a = g_core_plan.find(pk::R_AUDIO_OUT);
	const int rc = a && g_athread >= 0 ? sceKernelChangeThreadCpuAffinityMask(g_athread, a->mask) : -1;
	for (int i = 0; i < 2; i++) {
		const pk::Slot *h = g_core_plan.find(pk::R_UPSCALE, i);
		if (h && g_help_thread[i] >= 0) sceKernelChangeThreadCpuAffinityMask(g_help_thread[i], h->mask);
	}
	g_wd_audio = -1;
	char c[220];
	g_cores->census_line(c, sizeof c);
	logf_("audio thread back on core 2 rc=%08x\n%s\n", rc, c);
}

/* ---- video ------------------------------------------------------------- */
/* The picture goes through the GPU only: the core's frame is copied into a texture, then
 *   1. (Sharp)  drawn with nearest sampling at an integer scale into a render target,
 *   2. (Glow)   every few frames shrunk twice (64x64, 16x16) into two tiny render targets,
 *   3. the screen: glow stretched over everything and darkened, the picture on top
 *      (bilinear from the prescaled target), scanline / grid bands, the bezel PNG.
 * Nothing here runs on an emulation core except the one copy into the texture. A texture or target
 * is only written again once the GPU has finished reading it (vita2d fragment fences). */

#define RT_SIZE 1024

struct GTex {
	vita2d_texture *t;
	vita2d_fragment_fence rd;
};

static GTex g_src[4];            /* the core's picture, 512x512 RGB565 */
#define SC_W 1024              /* the scaler's output texture: 896x480 (xBR 2x of the widest wide picture, 448x240) is the largest */
#define SC_H 512
static GTex g_scaled[3];         /* the scaler's picture */
static int g_sc_i = -1, g_sc_f = 0;       /* newest scaled texture and its factor (0: the picture is shown unscaled) */
static std::atomic<unsigned> g_scale_us(0), g_scale_pics(0), g_scale_nonflat(0), g_scale_helped(0), g_scale_waits(0), g_scale_blocks(0), g_scale_blocks_done(0), g_scale_blocks_moved(0);   /* the scaler, per second */
static std::atomic<int> g_scale_last(0);     /* the scaler of the last picture (pk::ScalerKind), for the log */
static GTex g_rt, g_glowA, g_glowB, g_blend;   /* g_blend: 512x512, made when a blend option is first used */
static unsigned g_blend_serial;
static std::atomic<bool> g_pic_dirty(false);     /* the picture settings changed: frame_draw resets its caches (set by any thread) */
static int g_blend_w, g_blend_h;
static GTex g_bezel;
static int g_bezel_w, g_bezel_h;
static int g_src_i = -1;         /* newest uploaded texture */
static int g_src_w = 256, g_src_h = 224;
static unsigned g_serial_seen;      /* counts uploaded pictures */
static unsigned g_norm_seen, g_wide_seen;   /* the core's and the wide view's own picture serials */
static bool g_wide_split;           /* the wide view captures in the frame hook and composes on the drawing thread */
static int g_src_wide;              /* the uploaded picture is a wide view frame (256 + 2 margin columns) */
static int g_src_margin;
static unsigned g_up_serial;
static int g_crop_top, g_crop_bot, g_crop_cand_top, g_crop_cand_bot, g_crop_votes;
static int g_have_picture;
static unsigned g_fence_skips;

static bool gtex_writable(GTex &g) { return g.t && vita2d_fragment_fence_done(&g.rd); }
static void gtex_read(GTex &g) { if (g.t) vita2d_fragment_fence_record(&g.rd); }

static void video_init(void)
{
	for (int i = 0; i < 4; i++) {
		memset(&g_src[i], 0, sizeof g_src[i]);
		g_src[i].t = vita2d_create_empty_texture_format(512, 512, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
		if (g_src[i].t) vita2d_texture_set_filters(g_src[i].t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	}
	for (int i = 0; i < 3; i++) {
		memset(&g_scaled[i], 0, sizeof g_scaled[i]);
		g_scaled[i].t = vita2d_create_empty_texture_format(SC_W, SC_H, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
		if (g_scaled[i].t) vita2d_texture_set_filters(g_scaled[i].t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	}
	memset(&g_rt, 0, sizeof g_rt);
	g_rt.t = vita2d_create_empty_texture_rendertarget(RT_SIZE, RT_SIZE, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
	memset(&g_glowA, 0, sizeof g_glowA);
	g_glowA.t = vita2d_create_empty_texture_rendertarget(64, 64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
	memset(&g_glowB, 0, sizeof g_glowB);
	g_glowB.t = vita2d_create_empty_texture_rendertarget(16, 16, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
	GTex *all[3] = { &g_rt, &g_glowA, &g_glowB };
	for (int i = 0; i < 3; i++)
		if (all[i]->t) vita2d_texture_set_filters(all[i]->t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	logf_("render targets: sharp=%d glow=%d/%d scaled textures=%d\n", g_rt.t != NULL, g_glowA.t != NULL, g_glowB.t != NULL,
	    (int) (g_scaled[0].t != NULL) + (int) (g_scaled[1].t != NULL) + (int) (g_scaled[2].t != NULL));
}

/* Bezel / overlay PNG: overlays/<game>.png, else overlays/snes.png. Loaded once. */
static bool read_whole(const char *path, std::string &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 8 * 1024 * 1024) { fclose(f); return false; }
	out.resize(n);
	size_t got = fread(&out[0], 1, n, f);
	fclose(f);
	return got == (size_t) n;
}

static void bezel_load(const std::string &game)
{
	std::string path = std::string("ux0:data/GBAdhoc/overlays/") + game + ".png", data;
	if (!read_whole(path.c_str(), data)) {
		path = "ux0:data/GBAdhoc/overlays/snes.png";
		if (!read_whole(path.c_str(), data)) return;
	}
	int w, h, n;
	unsigned char *px = stbi_load_from_memory((const unsigned char *) data.data(), (int) data.size(), &w, &h, &n, 4);
	if (!px) { logf_("overlay %s: not a PNG\n", path.c_str()); return; }
	if (w > 2048 || h > 2048 || w < 1 || h < 1) { stbi_image_free(px); logf_("overlay too large\n"); return; }
	memset(&g_bezel, 0, sizeof g_bezel);
	g_bezel.t = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
	if (g_bezel.t) {
		uint8_t *dst = (uint8_t *) vita2d_texture_get_datap(g_bezel.t);
		unsigned stride = vita2d_texture_get_stride(g_bezel.t);
		for (int y = 0; y < h; y++) memcpy(dst + (size_t) y * stride, px + (size_t) y * w * 4, (size_t) w * 4);
		vita2d_texture_set_filters(g_bezel.t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
		g_bezel_w = w; g_bezel_h = h;
		logf_("overlay %s %dx%d\n", path.c_str(), w, h);
	}
	stbi_image_free(px);
}

/* Takes the newest finished picture from the core without waiting and copies it into a texture the GPU is
 * done with. Returns 1 when the picture changed. */
/* ---- the scaler's helpers ------------------------------------------------------------------------------------------------------------
 * One job at a time, cut into chunks of rows. The drawing thread publishes the job (fields first, then `next` with the job's number in
 * its upper half), takes chunks itself, and waits until every chunk has been finished; a helper that is awake takes chunks the same way
 * (compare-and-swap on `next`, so a late helper can never take a chunk of a job it did not read). A helper that is not scheduled costs
 * nothing: the drawing thread has done its chunks. */
struct ScaleJob {
	pk::ScaleCache *cache;
	int nchunks;
	std::atomic<uint64_t> next;      /* (job << 32) | next chunk */
	std::atomic<int> done;
	std::atomic<unsigned> nonflat;
	std::atomic<int> by_helpers;
	ScaleJob() : cache(0), nchunks(0), next(0), done(0), nonflat(0), by_helpers(0) {}
};
static ScaleJob g_job;
static uint32_t g_job_no;
static SceUID g_help_sema[2] = { -1, -1 };
static volatile int g_help_stop;
static int g_help_n;
static pk::ScaleScratch g_draw_scratch, g_help_scratch[2];

/* take and run chunks of the current job; returns how many this thread ran */
static int job_work(pk::ScaleScratch &sc, bool helper)
{
	int ran = 0;
	for (;;) {
		uint64_t n = g_job.next.load(std::memory_order_acquire);
		pk::ScaleCache *cache = g_job.cache;
		const int nch = g_job.nchunks;
		const uint32_t c = (uint32_t) (n & 0xFFFFFFFFu);
		if (!cache || (int) c >= nch) return ran;
		if (!g_job.next.compare_exchange_strong(n, n + 1, std::memory_order_acq_rel)) continue;
		const unsigned nf = cache->run_chunk((int) c, sc);
		g_job.nonflat.fetch_add(nf, std::memory_order_relaxed);
		if (helper) g_job.by_helpers.fetch_add(1, std::memory_order_relaxed);
		g_job.done.fetch_add(1, std::memory_order_acq_rel);
		ran++;
	}
}

/* The same helpers share the lines of the wide view's compose (snes_wide_parallel, snes/frontend/wide.h): the composing thread publishes the job,
 * wakes the helpers and works itself (worker 0) until no line is left; a helper that is awake in time takes lines as worker 1 or 2. `open` is
 * cleared before the composing thread waits for `active` to fall to zero, and a helper counts itself in before it looks at `open`, so a helper that
 * wakes late never touches a job that is over. */
struct LineJob {
	void (*fn)(void *, int);
	void *arg;
	std::atomic<int> open, active;
	LineJob() : fn(0), arg(0), open(0), active(0) {}
};
static LineJob g_lines;
static std::atomic<unsigned> g_lines_jobs(0), g_lines_helped(0);

static void wide_pfor(void *, void (*job)(void *, int), void *arg, int)
{
	g_lines.fn = job; g_lines.arg = arg;
	g_lines.open.store(1);
	for (int i = 0; i < g_help_n; i++) sceKernelSignalSema(g_help_sema[i], 1);
	job(arg, 0);
	g_lines.open.store(0);
	while (g_lines.active.load()) sceKernelDelayThread(20);
	g_lines_jobs.fetch_add(1, std::memory_order_relaxed);
}

static int help_thread(SceSize, void *argp)
{
	const int idx = *(int *) argp;
	while (!g_help_stop) {
		sceKernelWaitSema(g_help_sema[idx], 1, NULL);
		if (g_help_stop) break;
		job_work(g_help_scratch[idx], true);
		g_lines.active.fetch_add(1);
		if (g_lines.open.load()) { g_lines.fn(g_lines.arg, idx + 1); g_lines_helped.fetch_add(1, std::memory_order_relaxed); }
		g_lines.active.fetch_sub(1);
	}
	return sceKernelExitThread(0);
}

static void help_start(int n_wanted)
{
	static int idx[2] = { 0, 1 };
	for (int i = 0; i < n_wanted && i < 2; i++) {
		const pk::Slot *s = g_core_plan.find(pk::R_UPSCALE, i);
		if (!s) break;
		char nm[16];
		snprintf(nm, sizeof nm, "snes_scale%d", i);
		g_help_sema[i] = sceKernelCreateSema(nm, 0, 0, 1 << 16, NULL);
		if (g_help_sema[i] < 0) break;
		g_help_thread[i] = sceKernelCreateThread(nm, help_thread, s->prio, 0x10000, 0, s->mask, NULL);
		if (g_help_thread[i] >= 0 && sceKernelStartThread(g_help_thread[i], sizeof(int), &idx[i]) < 0) { sceKernelDeleteThread(g_help_thread[i]); g_help_thread[i] = -1; }
		if (g_help_thread[i] < 0) break;
		g_help_n++;
		logf_("scaler helper %d: core mask %05x priority 0x%x\n", i, s->mask, s->prio);
	}
	logf_("scaler helpers: %d\n", g_help_n);
	snes_wide_parallel(g_help_n > 0 ? wide_pfor : NULL, NULL, 1 + g_help_n);     /* the wide view's compose shares its lines with them */
}

static void help_stop(void)
{
	snes_wide_parallel(NULL, NULL, 1);       /* waits for a compose that is running */
	g_help_stop = 1;
	for (int i = 0; i < g_help_n; i++) {
		sceKernelSignalSema(g_help_sema[i], 1);
		sceKernelWaitThreadEnd(g_help_thread[i], NULL, NULL);
		sceKernelDeleteThread(g_help_thread[i]);
		sceKernelDeleteSema(g_help_sema[i]);
		g_help_thread[i] = -1;
	}
	g_help_n = 0;
}

/* The incremental scaler (pk::ScaleCache): the chunks of rows / columns that changed since the last picture are computed by this thread and the
 * helpers (work stealing), the rest of the picture comes from the kept scaled picture. Then the changed rows go into the texture. */
static pk::ScaleCache g_scache;
static pk::TexSync g_scaled_sync[3];

static unsigned scale_picture(pk::ScalerKind kind, const uint16_t *src, int w, int h, int spitch_px, uint16_t *dst, int dpitch_px, pk::TexSync &ts)
{
	const int nch = g_scache.plan(kind, src, w, h, spitch_px);
	unsigned nf = 0;
	if (nch > 0) {
		g_job.nchunks = 0;
		g_job.cache = &g_scache;
		g_job.done.store(0); g_job.nonflat.store(0); g_job.by_helpers.store(0);
		g_job.nchunks = nch;
		g_job.next.store((uint64_t) (++g_job_no) << 32, std::memory_order_release);
		for (int i = 0; i < g_help_n && nch > 1; i++) sceKernelSignalSema(g_help_sema[i], 1);
		job_work(g_draw_scratch, false);
		unsigned waits = 0;
		while (g_job.done.load(std::memory_order_acquire) < g_job.nchunks) { sceKernelDelayThread(20); waits++; }
		g_scale_waits.fetch_add(waits);
		g_scale_helped.fetch_add((unsigned) g_job.by_helpers.load());
		nf = g_job.nonflat.load();
		g_job.cache = 0;          /* a helper that wakes late finds nothing to take */
		g_job.nchunks = 0;
	}
	const pk::ScaleStats &st = g_scache.stats();
	g_scale_blocks.fetch_add((unsigned) st.blocks);
	g_scale_blocks_done.fetch_add((unsigned) st.computed);
	g_scale_blocks_moved.fetch_add((unsigned) st.shifted);
	g_scache.blit(dst, dpitch_px, ts);
	return nf;
}

struct PicSrc { const uint16_t *p; int w, h, pitch; unsigned serial; bool is_wide; bool mix; };
/* Smooth 50 Hz (pk_palsmooth.h): the drawing side keeps the previous picture and, for a picture marked `mix`, shows the mean of the two */
static std::atomic<int> g_pal_mix_on(0);           /* the loop sets it while the mode is on: the previous picture is then kept */
static uint16_t *g_mix_prev, *g_mix_out;           /* 512 x 512 each, allocated at the first use */
static int g_mix_w, g_mix_h, g_mix_wide, g_mix_valid;
static bool g_next_mix;                            /* the inline fetch: the next picture is a mixed one */

/* Where the next picture comes from (the wide view's frame or the core's own); false when there is nothing new. The three
 * "seen" values are the caller's memory of what it took last: the drawing side's globals when the picture is fetched
 * inline, the emulating thread's own copies when it goes through the hand-over mailbox. */
static bool video_source(PicSrc *s, unsigned norm_seen, unsigned wide_seen, int shown_wide)
{
	int w = 256, h = 224, pitch = 0;
	unsigned serial = 0;
	const uint16_t *src = NULL;
	bool is_wide = false;
	if (g_pl->wide_active()) {
		/* the wide view's newest finished frame (composed off this thread); when it has none for this picture (hi-res,
		 * nothing widenable) the core's own picture is shown */
		snes_wide_stats st;
		const uint16_t *wp = NULL;
		int ww = 0, wh = 0, wpp = 0;
		if (snes_wide_frame(&wp, &ww, &wh, &wpp, &st) && wp) {
			if (st.serial == wide_seen && shown_wide) return false;
			src = wp; w = ww; h = wh; pitch = wpp * 2; serial = st.serial; is_wide = true;
		}
	}
	if (!is_wide) {
		src = snes_host_video_nowait(&w, &h, &pitch, &serial);
		if (!src || (serial == norm_seen && !shown_wide)) return false;
	}
	s->p = src; s->w = w; s->h = h; s->pitch = pitch; s->serial = serial; s->is_wide = is_wide; s->mix = false;
	return true;
}

static int video_upload(const pic_settings &ps, int frame_no, const PicSrc &ps_src);

/* The picture that was last shown mixed, as itself (it is what the mix kept): the refresh after the early frame shows it */
static bool pal_pure_src(PicSrc *s)
{
	if (!g_mix_prev || !g_mix_valid) return false;
	s->p = g_mix_prev; s->w = g_mix_w; s->h = g_mix_h; s->pitch = g_mix_w * 2; s->serial = 0; s->is_wide = g_mix_wide != 0; s->mix = false;
	return true;
}

static int video_fetch(const pic_settings &ps, int frame_no)
{
	PicSrc s;
	if (!video_source(&s, g_norm_seen, g_wide_seen, g_src_wide)) return 0;
	s.mix = g_next_mix; g_next_mix = false;
	return video_upload(ps, frame_no, s);
}

/* The second half: copies the picture into a texture the GPU is done with (on the drawing side). Returns 1 when the picture changed. */
static int video_upload(const pic_settings &ps, int frame_no, const PicSrc &ps_src)
{
	int w = ps_src.w, h = ps_src.h, pitch = ps_src.pitch;
	unsigned serial = ps_src.serial;
	const uint16_t *src = ps_src.p;
	const bool is_wide = ps_src.is_wide;
	int idx = -1;
	for (int n = 1; n <= 4; n++) {
		int c = (g_src_i + n) % 4;
		if (gtex_writable(g_src[c])) { idx = c; break; }
	}
	if (idx < 0) { g_fence_skips++; return 0; }          /* the GPU is behind: show the old picture again */
	g_serial_seen = ++g_up_serial;
	if (is_wide) g_wide_seen = serial; else g_norm_seen = serial;
	if (w > 512) w = 512;
	if (h > 512) h = 512;
	if (g_pal_mix_on.load(std::memory_order_relaxed)) {
		/* Smooth 50 Hz: keep this picture for the next mix; a picture marked mix is shown as the mean of the last one and itself */
		if (!g_mix_prev) { g_mix_prev = (uint16_t *) malloc(512 * 512 * 2); g_mix_out = (uint16_t *) malloc(512 * 512 * 2); g_mix_valid = 0; }
		if (g_mix_prev && g_mix_out) {
			const bool same = g_mix_valid && g_mix_w == w && g_mix_h == h && g_mix_wide == (is_wide ? 1 : 0);
			if (ps_src.mix && same) { pk::mix16_half((uint8_t *) g_mix_out, w * 2, (const uint8_t *) g_mix_prev, w * 2, (const uint8_t *) src, pitch, w, h); }
			for (int y = 0; y < h; y++) memcpy(g_mix_prev + (size_t) y * w, (const uint8_t *) src + (size_t) y * pitch, (size_t) w * 2);
			g_mix_w = w; g_mix_h = h; g_mix_wide = is_wide ? 1 : 0; g_mix_valid = 1;
			if (ps_src.mix && same) { src = g_mix_out; pitch = w * 2; }
		}
	} else g_mix_valid = 0;
	if (ps.shape == PIC_ZOOM && frame_no % 20 == 0) {
		int mx = h / 8;
		int t = pic_blank_rows(src, pitch, w, h, 1, mx), b = pic_blank_rows(src, pitch, w, h, 0, mx);
		if (t == g_crop_cand_top && b == g_crop_cand_bot) g_crop_votes++; else { g_crop_cand_top = t; g_crop_cand_bot = b; g_crop_votes = 0; }
		if (g_crop_votes >= 2) { g_crop_top = t; g_crop_bot = b; }
		else { if (t < g_crop_top) g_crop_top = t; if (b < g_crop_bot) g_crop_bot = b; }
	}
	vita2d_texture *tx = g_src[idx].t;
	uint8_t *dst = (uint8_t *) vita2d_texture_get_datap(tx);
	unsigned dpitch = vita2d_texture_get_stride(tx);
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t) y * dpitch, (const uint8_t *) src + (size_t) y * pitch, (size_t) w * 2);
	if (w != g_src_w || h != g_src_h || is_wide != (g_src_wide != 0)) { g_crop_top = g_crop_bot = g_crop_cand_top = g_crop_cand_bot = 0; }
	g_src_i = idx; g_src_w = w; g_src_h = h;
	g_src_wide = is_wide ? 1 : 0; g_src_margin = is_wide ? (w - 256) / 2 : 0;
	g_have_picture = 1;

	/* the pixel art scaler: the look's choice for this picture (xBR 2x for 256 .. 448 wide pictures of 224 / 239 rows, the wide view's
	 * included; never for hi-res or interlaced ones), into a scaled texture the GPU is done with. It reads CPU memory (the mailbox's copy
	 * or the wide view's frame), never the texture. */
	g_sc_f = 0;
	pk::ScalerKind kind = pk::SC_NONE;
	if (g_pl->look_mode() != 2 && pic_scaler_ceiling(w, h) <= 1) {     /* a hand-set scaler is held, but not on pictures it cannot take */
		switch (pic_scaler_effective(&ps, pic_scaler_ceiling(w, h), ps.tier_steps)) {
		case SCALER_MMPX2: kind = pk::SC_MMPX2; break;
		case SCALER_XBR2: kind = pk::SC_XBR2; break;
		case SCALER_XBR3: kind = pk::SC_XBR3; break;
		default: break;
		}
	}
	g_scale_last.store((int) kind, std::memory_order_relaxed);
	if (kind != pk::SC_NONE) {
		const int f = pk::scaler_factor(kind);
		int si = -1;
		for (int n = 1; n <= 3; n++) {
			int c = (g_sc_i + n + 3) % 3;
			if (gtex_writable(g_scaled[c])) { si = c; break; }
		}
		if (si >= 0 && w * f <= SC_W && h * f <= SC_H) {
			vita2d_texture *st = g_scaled[si].t;
			const uint64_t t0 = sceKernelGetProcessTimeWide();
			const unsigned nf = scale_picture(kind, src, w, h, pitch / 2, (uint16_t *) vita2d_texture_get_datap(st),
			    (int) (vita2d_texture_get_stride(st) / 2), g_scaled_sync[si]);
			g_scale_us.fetch_add((unsigned) (sceKernelGetProcessTimeWide() - t0));
			g_scale_pics.fetch_add(1);
			g_scale_nonflat.fetch_add(nf);
			g_sc_i = si; g_sc_f = f;
		} else g_fence_skips++;
	}
	return 1;
}

static void filt(vita2d_texture *t, SceGxmTextureFilter f) { vita2d_texture_set_filters(t, f, f); }

/* Draws part of `t` into a render target of rtw x rth pixels (libvita2d's projection is a fixed 960x544
 * whatever the target is, so sizes are scaled by 960/rtw and 544/rth). dst_w/dst_h are in target pixels. */
static void draw_into_rt(vita2d_texture *t, int rtw, int rth, int sx, int sy, int sw, int sh, int dst_w, int dst_h)
{
	vita2d_draw_texture_part_scale(t, 0, 0, (float) sx, (float) sy, (float) sw, (float) sh,
	    (dst_w * 960.0f / rtw) / sw, (dst_h * 544.0f / rth) / sh);
}

static bool rt_scene(GTex &rt)
{
	if (!rt.t || !gtex_writable(rt)) return false;
	if (!vita2d_try_start_drawing_advanced(rt.t, 0)) return false;
	return true;
}

struct RtKey { int idx; unsigned serial; int kx, ky; float sx, sy, sw, sh; };
static RtKey g_rtkey;
static bool g_rtkey_valid;
static unsigned g_rt_reused;

static int g_glow_age = 99;
static unsigned g_glow_serial;


/* Many same-coloured rectangles in one GPU draw (a triangle list from the vita2d temp pool). If the pool is
 * full nothing is drawn for that band set. */
static vita2d_color_vertex *g_rb_v;
static int g_rb_n, g_rb_cap;
static void rect_batch_begin(int max_rects)
{
	g_rb_n = 0; g_rb_cap = max_rects;
	g_rb_v = (vita2d_color_vertex *) vita2d_pool_memalign(6 * max_rects * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
}
static void rect_batch_add(float x, float y, float w, float h)
{
	if (g_rb_n >= g_rb_cap) return;
	if (g_rb_v) {
		vita2d_color_vertex *v = g_rb_v + 6 * g_rb_n;
		const float xs[6] = { x, x + w, x, x + w, x + w, x };
		const float ys[6] = { y, y, y + h, y, y + h, y + h };
		for (int k = 0; k < 6; k++) { v[k].x = xs[k]; v[k].y = ys[k]; v[k].z = 0.5f; }
	}
	g_rb_n++;
}
static void rect_batch_draw(unsigned color)
{
	if (g_rb_n <= 0) return;
	if (g_rb_v) {
		for (int i = 0; i < 6 * g_rb_n; i++) g_rb_v[i].color = color;
		vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, g_rb_v, 6 * g_rb_n);
	}
}

static unsigned band_alpha(int strength) { unsigned a = 20 + 19 * strength; return a > 210 ? 210 : a; }

/* Opens the frame's last scene and draws the picture. Returns false when no scene could be started
 * (the display queue is full): the caller then skips this frame. `extras` = 0 draws without the picture
 * options (not used by the game loop: the governor never drops them). */
static bool frame_draw(const pic_settings &ps, int extras, int frame_no)
{
	if (g_pic_dirty.exchange(false)) { g_rtkey_valid = false; g_glow_serial = 0; g_glow_age = 99; g_blend_serial = 0; }
	vita2d_pool_reset();
	pic_rect r;                  /* in the core's own picture pixels: the layout, the scanlines, the grid, the glow */
	pic_rect rs;                 /* the same in the pixels of the picture that is drawn (the scaler's output is g_sc_f times bigger) */
	GTex *src = g_src_i >= 0 ? &g_src[g_src_i] : NULL;
	int eff_w = g_src_w, eff_h = g_src_h, crop_top = g_crop_top, crop_bot = g_crop_bot;
	/* Optional blends (Picture menu), done by the GPU at no cost to the emulation: 512-wide pictures (hi-res
	 * and pseudo-hi-res, where games alternate columns to fake transparency) shrunk to half width, and
	 * interlaced (400+ rows) pictures to half height, with bilinear sampling exactly between two source
	 * pixels, i.e. the plain average of each pair. The result is a 512x512 target used as the picture. */
	if (src && g_have_picture && extras && (ps.hires_blend || ps.interlace_blend)) {
		bool bx = ps.hires_blend && g_src_w >= 512, by = ps.interlace_blend && g_src_h >= 400;
		if ((bx || by) && !g_blend.t) {
			memset(&g_blend, 0, sizeof g_blend);
			g_blend.t = vita2d_create_empty_texture_rendertarget(512, 512, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
			if (g_blend.t) vita2d_texture_set_filters(g_blend.t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
			logf_("render target: blend=%d\n", g_blend.t != NULL);
		}
		if ((bx || by) && g_blend.t) {
			int bw = bx ? g_src_w / 2 : g_src_w, bh = by ? g_src_h / 2 : g_src_h;
			bool ready = g_blend_serial == g_serial_seen && g_blend_w == bw && g_blend_h == bh;
			if (!ready && rt_scene(g_blend)) {
				gtex_read(*src);
				filt(src->t, SCE_GXM_TEXTURE_FILTER_LINEAR);
				draw_into_rt(src->t, 512, 512, 0, 0, g_src_w, g_src_h, bw, bh);
				vita2d_end_drawing();
				g_blend_serial = g_serial_seen; g_blend_w = bw; g_blend_h = bh;
				ready = true;
			}
			if (ready) {
				src = &g_blend; eff_w = bw; eff_h = bh;
				if (by) { crop_top /= 2; crop_bot /= 2; }
			}
		}
	}
	if (src && g_have_picture) {
		if (g_src_wide && src == &g_src[g_src_i]) pic_geometry_wide(&ps, eff_w, eff_h, crop_top, crop_bot, SCREEN_W, SCREEN_H, &r);
		else pic_geometry(&ps, eff_w, eff_h, crop_top, crop_bot, SCREEN_W, SCREEN_H, &r);
	}
	/* the scaler's picture stands in for the core's own (never for the blend target: the scaler takes no hi-res or interlaced picture) */
	GTex *img = src;
	int F = 1;
	if (src && g_src_i >= 0 && src == &g_src[g_src_i] && g_have_picture && g_sc_f > 0 && g_sc_i >= 0 && g_scaled[g_sc_i].t) { img = &g_scaled[g_sc_i]; F = g_sc_f; }
	rs = r;
	rs.sx *= F; rs.sy *= F; rs.sw *= F; rs.sh *= F;
	bool bezel = g_bezel.t != NULL;
	int kx = 1, ky = 1;
	bool use_rt = false, exact = false;
	if (src && g_have_picture) {
		pic_prescale(&rs, &kx, &ky);
		exact = fabsf(rs.dw - rs.sw * kx) < 0.5f && fabsf(rs.dh - rs.sh * ky) < 0.5f && kx == ky;
		use_rt = extras && ps.quality == PIC_SHARP && !exact && g_rt.t != NULL && (kx > 1 || ky > 1);
	}

	/* scene 1: glow, a few times a second */
	bool glow_on = extras && src && g_have_picture && !bezel && ps.glow > 0 && ps.shape != PIC_FULL && g_glowA.t && g_glowB.t;
	if (glow_on && (++g_glow_age >= 4 || g_glow_serial == 0) && g_serial_seen != g_glow_serial &&
	    gtex_writable(g_glowA) && gtex_writable(g_glowB)) {
		filt(src->t, SCE_GXM_TEXTURE_FILTER_LINEAR);
		if (rt_scene(g_glowA)) {
			gtex_read(*src);
			draw_into_rt(src->t, 64, 64, r.sx, r.sy, r.sw, r.sh, 64, 64);
			vita2d_end_drawing();
			if (rt_scene(g_glowB)) {
				gtex_read(g_glowA);
				draw_into_rt(g_glowA.t, 16, 16, 0, 0, 64, 64, 16, 16);
				vita2d_end_drawing();
				g_glow_age = 0;
				g_glow_serial = g_serial_seen;
			}
		}
	}
	/* scene 2: the sharp-bilinear prescale */
	if (use_rt) {
		/* the prescaled target is only redrawn when its inputs changed: a new picture, another crop or
		 * another factor. A frame drawn again for the same picture (the picture is late, the pause menu
		 * is open) reuses it and saves a whole GPU pass. */
		RtKey key;
		memset(&key, 0, sizeof key);
		key.idx = (src == &g_blend) ? -2 : (img == src ? g_src_i : 8 + g_sc_i); key.serial = g_serial_seen; key.kx = kx; key.ky = ky;
		key.sx = rs.sx; key.sy = rs.sy; key.sw = rs.sw; key.sh = rs.sh;
		if (g_rtkey_valid && !memcmp(&key, &g_rtkey, sizeof key)) {
			g_rt_reused++;
		} else if (rt_scene(g_rt)) {
			gtex_read(*img);
			filt(img->t, SCE_GXM_TEXTURE_FILTER_POINT);
			draw_into_rt(img->t, RT_SIZE, RT_SIZE, rs.sx, rs.sy, rs.sw, rs.sh, rs.sw * kx, rs.sh * ky);
			vita2d_end_drawing();
			filt(img->t, SCE_GXM_TEXTURE_FILTER_LINEAR);
			g_rtkey = key; g_rtkey_valid = true;
		} else { use_rt = false; g_rtkey_valid = false; }
	}

	if (!vita2d_try_start_drawing_advanced(NULL, 0)) return false;
	vita2d_clear_screen();
	if (glow_on && g_glow_serial) {
		unsigned g = 16 + 9 * ps.glow;
		gtex_read(g_glowB);
		vita2d_draw_texture_tint_part_scale(g_glowB.t, 0, 0, 0, 0, 16, 16, SCREEN_W / 16.0f, SCREEN_H / 16.0f,
		    0xFF000000u | (g << 16) | (g << 8) | g);
	}
	if (src && g_have_picture) {
		if (use_rt) {
			gtex_read(g_rt);
			vita2d_draw_texture_part_scale(g_rt.t, rs.dx, rs.dy, 0, 0, (float) (rs.sw * kx), (float) (rs.sh * ky),
			    rs.dw / (rs.sw * kx), rs.dh / (rs.sh * ky));
		} else {
			gtex_read(*img);
			filt(img->t, (ps.quality == PIC_SHARP && exact) ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
			vita2d_draw_texture_part_scale(img->t, rs.dx, rs.dy, (float) rs.sx, (float) rs.sy, (float) rs.sw, (float) rs.sh,
			    rs.dw / rs.sw, rs.dh / rs.sh);
			filt(img->t, SCE_GXM_TEXTURE_FILTER_LINEAR);
		}
		/* CRT scanlines and LCD grid: dark bands between SNES lines / pixels, drawn as alpha rectangles */
		if (extras && (ps.scan > 0 || ps.grid > 0)) {
			float period = r.dh / r.logical_rows;
			int rows = r.logical_rows;
			int sstr = ps.scan > ps.grid ? ps.scan : ps.grid;
			if (ps.scan > 0 || ps.grid > 0) {
				float th = period * (0.18f + 0.03f * (ps.scan ? ps.scan : ps.grid));
				if (th > period * 0.5f) th = period * 0.5f;
				if (th < 1.0f) th = 1.0f;
				unsigned a = band_alpha(ps.scan ? ps.scan : ps.grid);
				unsigned col = a << 24;
				/* all bands of a kind in one draw call (a few hundred separate rectangles cost the main
				 * core more than the picture itself did); same vertices, same blending, same order */
				rect_batch_begin(rows);
				for (int i = 1; i < rows; i++) {
					float y = r.dy + i * period - th * 0.5f;
					rect_batch_add(r.dx, y, r.dw, th);
				}
				rect_batch_draw(col);
				(void) sstr;
			}
			if (ps.grid > 0) {
				float cp = r.dw / 256.0f;
				float th = cp * (0.15f + 0.02f * ps.grid);
				if (th < 1.0f) th = 1.0f;
				unsigned col = band_alpha(ps.grid) << 24;
				rect_batch_begin(256);
				for (int i = 1; i < 256; i++)
					rect_batch_add(r.dx + i * cp - th * 0.5f, r.dy, th, r.dh);
				rect_batch_draw(col);
			}
		}
	}
	if (bezel) {
		vita2d_draw_texture_scale(g_bezel.t, 0, 0, (float) SCREEN_W / g_bezel_w, (float) SCREEN_H / g_bezel_h);
	}
	return true;
}

static void frame_finish(void)
{
	vita2d_end_drawing();
	vita2d_swap_buffers();
}

/* ---- input ------------------------------------------------------------- */
/* Players. SNES pad 1 is the Vita's own controls (port 0). Pad 2 is port 2 when a controller is paired
 * there, else port 1 on a handheld (external pads through ds34vita and the like); on a PS TV port 1 is
 * the same pad as port 0, so it is not used. Pads 3 and 4 are ports 3 and 4; they reach the game only
 * through the Super Multitap (Settings > Controls > Multitap). The ports are looked at once a second, so
 * pads can be plugged in or removed while playing. Mapping, sticks, turbo, shortcuts and the menu chord
 * are playerkit's Mapper (player.cpp); here the controllers are only read. */

static int g_pad_port[4] = { 0, -1, -1, -1 };
static int g_port_age;

static void ports_update(void)
{
	if (g_port_age-- > 0) return;
	g_port_age = 60;
	SceCtrlPortInfo info;
	memset(&info, 0, sizeof info);
	int p2 = -1, p3 = -1, p4 = -1;
	if (sceCtrlGetControllerPortInfo(&info) >= 0) {
		if (info.port[2] != SCE_CTRL_TYPE_UNPAIRED) p2 = 2;
		else if (info.port[1] != SCE_CTRL_TYPE_UNPAIRED && !sceKernelIsPSVitaTV()) p2 = 1;
		if (info.port[3] != SCE_CTRL_TYPE_UNPAIRED) p3 = 3;
		if (info.port[4] != SCE_CTRL_TYPE_UNPAIRED) p4 = 4;
	}
	if (p2 != g_pad_port[1] || p3 != g_pad_port[2] || p4 != g_pad_port[3])
		logf_("pads: p2=port%d p3=port%d p4=port%d\n", p2, p3, p4);
	g_pad_port[1] = p2; g_pad_port[2] = p3; g_pad_port[3] = p4;
}

/* One port into a playerkit sample: the Vita's L/R arrive as L1/R1 (Ext2), a PS TV pad's L2/R2 as the
 * old L/R bits; both are the console's L/R. */
static void read_port(int port, pk::PadSample *s)
{
	*s = pk::PadSample();
	if (port < 0) return;
	SceCtrlData pad;
	memset(&pad, 0, sizeof pad);
	pad.lx = pad.ly = pad.rx = pad.ry = 128;
	if (sceCtrlPeekBufferPositiveExt2(port, &pad, 1) < 0) return;
	if (pad.lx == 128 && pad.ly == 128 && pad.rx == 128 && pad.ry == 128) {
		/* Ext2 left the sticks as they were set above: some systems (the Vita3K emulator) fill only the buttons in this call.
		 * The plain read has the same sticks on a real Vita, so take them from it when Ext2 shows both centred. */
		SceCtrlData q;
		memset(&q, 0, sizeof q);
		q.lx = q.ly = q.rx = q.ry = 128;
		if (sceCtrlPeekBufferPositive(port, &q, 1) > 0) { pad.lx = q.lx; pad.ly = q.ly; pad.rx = q.rx; pad.ry = q.ry; }
	}
	unsigned b = pad.buttons;
	uint32_t m = 0;
	if (b & SCE_CTRL_UP) m |= pk::PH_UP;
	if (b & SCE_CTRL_DOWN) m |= pk::PH_DOWN;
	if (b & SCE_CTRL_LEFT) m |= pk::PH_LEFT;
	if (b & SCE_CTRL_RIGHT) m |= pk::PH_RIGHT;
	if (b & SCE_CTRL_CROSS) m |= pk::PH_CROSS;
	if (b & SCE_CTRL_CIRCLE) m |= pk::PH_CIRCLE;
	if (b & SCE_CTRL_SQUARE) m |= pk::PH_SQUARE;
	if (b & SCE_CTRL_TRIANGLE) m |= pk::PH_TRIANGLE;
	if (b & (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)) m |= pk::PH_L;
	if (b & (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER)) m |= pk::PH_R;
	if (b & SCE_CTRL_START) m |= pk::PH_START;
	if (b & SCE_CTRL_SELECT) m |= pk::PH_SELECT;
	s->connected = true;
	s->buttons = m;
	s->lx = pad.lx; s->ly = pad.ly; s->rx = pad.rx; s->ry = pad.ry;
}

/* the touch panels: the front one (a tap on the picture opens the menu; the menu is touch-driven) and the
 * rear one (its four corners, for the Controls page's "Rear touch panel") */
struct TouchState { bool down; float x, y, x0, y0; uint64_t t0; };
static TouchState g_touch;

static bool touch_read(pk::TouchPoint *tp, uint8_t *back, uint64_t now_us, bool *tapped)
{
	SceTouchData d;
	*tapped = false;
	tp->down = false;
	if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &d, 1) > 0 && d.reportNum > 0) {
		tp->down = true;
		tp->x = d.report[0].x / 2.0f;     /* 1920 x 1088 -> 960 x 544 */
		tp->y = d.report[0].y / 2.0f;
	}
	if (tp->down && !g_touch.down) { g_touch.x0 = tp->x; g_touch.y0 = tp->y; g_touch.t0 = now_us; }
	if (tp->down) { g_touch.x = tp->x; g_touch.y = tp->y; }
	if (!tp->down && g_touch.down) {
		float dx = g_touch.x - g_touch.x0, dy = g_touch.y - g_touch.y0;
		*tapped = now_us - g_touch.t0 < 500000 && dx * dx + dy * dy < 40 * 40;
	}
	g_touch.down = tp->down;
	*back = 0;
	if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &d, 1) > 0)
		for (unsigned i = 0; i < d.reportNum && i < 4; i++) {
			bool left = d.report[i].x < 960, top = d.report[i].y < 445;
			*back |= (uint8_t) (top ? (left ? 1 : 2) : (left ? 4 : 8));
		}
	return true;
}

static void read_pads(pk::PadSample pads[pk::MAX_PLAYERS], uint8_t back)
{
	ports_update();
	for (int i = 0; i < pk::MAX_PLAYERS; i++) read_port(g_pad_port[i], &pads[i]);
	pads[0].back = back;
	pads[0].connected = true;
}

/* ---- the platform for the player --------------------------------------- */

static SceUID g_wsema = -1, g_wthread = -1;
static volatile int g_wstop;
static bool g_ime_wanted;

struct VitaPlatform : snesp::Platform {
	uint64_t now_us() { return sceKernelGetProcessTimeWide(); }
	void pause_us(unsigned us) { sceKernelDelayThread(us); }
	bool wake_writer()
	{
		if (g_wthread < 0) return false;
		sceKernelSignalSema(g_wsema, 1);
		return true;
	}
	/* the newest picture, straight from its texture (read only while no new picture is fetched) */
	bool current_frame(pk::Frame *f)
	{
		/* a state picture: the drawing thread finishes what it holds first, so the texture read is a whole picture */
		if (g_pipe.running()) g_pipe.box().flush(300000);
		if (!g_have_picture || g_src_i < 0 || !g_src[g_src_i].t) return false;
		vita2d_texture *t = g_src[g_src_i].t;
		/* a wide frame: the state picture is the 256 columns of the game's own picture in its middle */
		*f = pk::Frame((const uint16_t *) vita2d_texture_get_datap(t) + (g_src_wide ? g_src_margin : 0), g_src_wide ? 256 : g_src_w, g_src_h, (int) vita2d_texture_get_stride(t));
		return true;
	}
	void ask_cheat_text() { g_ime_wanted = true; }
	void picture_changed() { g_pic_dirty.store(true); }   /* the drawing code resets its caches itself, on its own thread */
	void log_write(const std::string &chunk)
	{
		if (!g_log) return;
		fwrite(chunk.data(), 1, chunk.size(), g_log);
		fflush(g_log);
	}
};
static VitaPlatform g_platform;

/* the writer thread: battery RAM, states, the resume point, settings and the log; low priority, on the
 * picture thread's core, never in the game thread's way */
static int writer_thread(SceSize, void *)
{
	for (;;) {
		sceKernelWaitSema(g_wsema, 1, NULL);
		if (g_wstop) break;
		while (g_pl->writer().work_one(g_pl)) {}
	}
	return sceKernelExitThread(0);
}

static void writer_start(void)
{
	g_wsema = sceKernelCreateSema("snes_writer", 0, 0, 1 << 20, NULL);
	if (g_wsema < 0) return;
	g_wthread = sceKernelCreateThread("snes_writer", writer_thread, 0xA0, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (g_wthread >= 0 && sceKernelStartThread(g_wthread, 0, NULL) < 0) { sceKernelDeleteThread(g_wthread); g_wthread = -1; }
	logf_("writer thread %s\n", g_wthread >= 0 ? "started" : "NOT started (writes run on the game thread)");
}

static void writer_stop(void)
{
	if (g_wthread < 0) return;
	g_wstop = 1;
	sceKernelSignalSema(g_wsema, 1);
	sceKernelWaitThreadEnd(g_wthread, NULL, NULL);
	sceKernelDeleteThread(g_wthread);
	g_wthread = -1;
}

/* ---- drawing helpers ------------------------------------------------------ */

static pk::VitaGfx *g_gfx;
static pk::Text *g_text;

static int ini_int(const char *path, const char *key, int def);
static int prio_ok(int p, int def);

/* ---- frame hand-over ------------------------------------------------------------------------------
 * "Smooth hand-over" (Speed page): the emulating thread (main) copies each finished picture into playerkit's
 * mailbox (pipe.h) and goes on; this thread uploads it, draws it (filters, bands, bezel, notices) and presents
 * it. The main thread owns the display only while this thread is idle: flush() before the menu, before a state
 * picture is read, and at the end. See playerkit/src/pk_handoff.h for the rules. */
struct VitaHandoffSys : pk::HandoffSys {
	uint64_t now_us() { return sceKernelGetProcessTimeWide(); }
	void *sem_create()
	{
		SceUID id = sceKernelCreateSema("snes_hd", 0, 0, 1 << 16, NULL);
		return id < 0 ? NULL : (void *) (intptr_t) id;
	}
	void sem_destroy(void *s) { sceKernelDeleteSema((SceUID) (intptr_t) s); }
	void sem_post(void *s) { sceKernelSignalSema((SceUID) (intptr_t) s, 1); }
	bool sem_wait(void *s, uint32_t timeout_us)
	{
		SceUInt t = timeout_us;
		return sceKernelWaitSema((SceUID) (intptr_t) s, 1, timeout_us == 0xFFFFFFFFu ? NULL : &t) >= 0;
	}
};
static VitaHandoffSys g_hd_sys;
static int clampi_(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static SceUID g_hd_thread = -1;
static int g_draw_core = 1;

/* CPU time a thread has used so far, in ms (the kernel's own count, SceKernelThreadInfo.runClocks), or -1 */
static double thread_cpu_ms(SceUID id)
{
	SceKernelThreadInfo ti;
	memset(&ti, 0, sizeof ti);
	ti.size = sizeof ti;
	if (id < 0 || sceKernelGetThreadInfo(id, &ti) < 0) return -1.0;
	return ti.runClocks / 1000.0;
}
static bool g_hd_ok;                                   /* the drawing thread runs */
static std::atomic<unsigned> g_hd_drawn(0), g_hd_failed(0), g_hd_draw_us(0);   /* this second, read and cleared by main */
static pk::SpinLock g_hd_lk;                           /* guards the notice snapshot */
static pk::Notices g_hd_notices;
static std::atomic<int> g_hd_notice_on(0);
static unsigned g_hd_norm_seen, g_hd_wide_seen;       /* what the emulating thread handed over last */
static int g_hd_src_wide;

/* the player's toast, as of the last emulated frame (the drawing thread must not touch the live one) */
static void hd_publish_notices(void)
{
	const bool on = g_pl->notices().active();
	if (!on && !g_hd_notice_on.load()) return;
	pk::SpinGuard g(g_hd_lk);
	g_hd_notices = g_pl->notices();
	g_hd_notice_on = on ? 1 : 0;
}

static int hd_draw_thread(SceSize, void *)
{
	for (;;) {
		pk::HandoffFrame *f = g_pipe.box().take();
		if (!f) break;
		const uint64_t t0 = sceKernelGetProcessTimeWide();
		PicSrc s;
		s.mix = false;
		bool have = true;
		if (f->user & snesp::PicturePipe::SNAP_FLAG) {
			/* wide view, capture / compose split: the frame is a captured snapshot; the compose runs here, off the emulating thread */
			snesp::PicturePipe::SnapDesc d;
			memcpy(&d, f->data, sizeof d);
			const uint16_t *wp = NULL; int ww = 0, wh = 0, wpp = 0;
			snes_wide_stats st;
			const int r = snes_wide_compose(d.snap, d.serial, &wp, &ww, &wh, &wpp, &st);
			if (r == 1 && wp) { s.p = wp; s.w = ww; s.h = wh; s.pitch = wpp * 2; s.serial = st.serial; s.is_wide = true; }
			else if (r == 0 && d.has_normal) {
				s.p = (const uint16_t *) (f->data + snesp::PicturePipe::SNAP_OFS); s.w = (int) d.w; s.h = (int) d.h; s.pitch = (int) d.w * 2;
				s.serial = (unsigned) (f->user & 0x1FFFFFFF); s.is_wide = false;
			} else have = false;     /* stale handle (the view was switched meanwhile) or a frame with no picture: the old one stays */
		} else {
			s.p = (const uint16_t *) f->data; s.w = f->w; s.h = f->h; s.pitch = f->pitch;
			s.serial = (unsigned) (f->user & 0x0FFFFFFF); s.is_wide = (f->user & 0x40000000) != 0; s.mix = (f->user & 0x10000000) != 0;
		}
		const pic_settings ps = g_pl->picture();     /* one copy for this picture: the game thread may ease the look meanwhile */
		if (have) video_upload(ps, (int) f->seq, s);
		bool drew = false;
		for (int tries = 0; tries < 4 && !drew; tries++) {
			if (tries) sceDisplayWaitVblankStart();       /* the display queue was full: the picture is not dropped for it */
			drew = frame_draw(ps, 1, (int) f->seq);
		}
		auto finish_pic = [&]() {
			if (g_hd_notice_on.load()) {
				pk::Notices n;
				{ pk::SpinGuard g(g_hd_lk); n = g_hd_notices; }
				if (n.active()) n.draw(g_gfx, g_text, PK_HEX(0x7B6CFF, 255), 26, true);
			}
			frame_finish();
			g_hd_drawn++;
		};
		if (drew) finish_pic(); else g_hd_failed++;
		const uint64_t dt = sceKernelGetProcessTimeWide() - t0;       /* the cost reported is this picture's own draw, not the repeat after it */
		g_hd_draw_us += (unsigned) dt;
		if (drew && have && s.mix && g_mix_valid) {
			/* Smooth 50 Hz: this picture was the early frame's, shown mixed for one refresh; the next refresh shows it alone */
				PicSrc pure;
				if (pal_pure_src(&pure) && video_upload(ps, (int) f->seq, pure)) {
					bool d2 = false;
					for (int tries = 0; tries < 4 && !d2; tries++) { sceDisplayWaitVblankStart(); d2 = frame_draw(ps, 1, (int) f->seq); }
					if (d2) finish_pic();
				}
		}
		g_pipe.box().release(f, (uint32_t) (dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : dt));
	}
	return sceKernelExitThread(0);
}

static bool hd_active(void) { return g_hd_ok && g_pipe.box().enabled() && g_pl->smooth_handoff(); }

/* Turn it on for the session (the setting is read again whenever the menu closes). */
static void hd_start(const char *tini, double frame_hz)
{
	if (!g_pl->smooth_handoff()) { logf_("handoff: off (setting)\n"); return; }
	if (!g_pipe.start(&g_hd_sys, 2, true, 16683, (uint32_t) (1e6 / frame_hz + 0.5))) { logf_("handoff: NOT started (mailbox)\n"); return; }
	/* threads.ini draw_core: 0, 1 (default) or 2, the core the drawing thread runs on */
	const int core = ini_int(tini, "draw_core", 1);
	g_draw_core = core == 0 ? 0 : core == 2 ? 2 : 1;
	const int prio = prio_ok(ini_int(tini, "draw_prio", 0x60), 0x60);
	const unsigned mask = core == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : core == 2 ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_1;
	g_hd_thread = sceKernelCreateThread("snes_draw", hd_draw_thread, prio ? prio : 0x60, 0x20000, 0, mask, NULL);
	if (g_hd_thread >= 0 && sceKernelStartThread(g_hd_thread, 0, NULL) < 0) { sceKernelDeleteThread(g_hd_thread); g_hd_thread = -1; }
	g_hd_ok = g_hd_thread >= 0;
	if (!g_hd_ok) g_pipe.stop();
	logf_("handoff: %s, draw thread on core %d priority 0x%x, depth 2 (adaptive 2/1)\n", g_hd_ok ? "on" : "NOT started (thread)", core, prio ? prio : 0x60);
}

/* The drawing thread is idle when this returns true: the main thread may use the display (menu, state picture, end). */
static bool hd_quiesce(void)
{
	if (!g_hd_ok) return true;
	const bool ok = g_pipe.box().flush(1000000);
	if (!ok) logf_("handoff: the drawing thread did not go idle in 1 s\n");
	return ok;
}

/* After the menu: the setting may have changed; the clocks start again. */
static void hd_after_menu(void)
{
	if (!g_hd_ok) return;
	g_pipe.box().set_enabled(g_pl->smooth_handoff());
	g_pipe.box().time_baseline();
	g_hd_norm_seen = g_hd_wide_seen = 0; g_hd_src_wide = 0;
}

static void hd_stop(void)
{
	if (!g_hd_ok) return;
	hd_quiesce();
	g_pipe.stop();
	sceKernelWaitThreadEnd(g_hd_thread, NULL, NULL);
	sceKernelDeleteThread(g_hd_thread);
	g_hd_thread = -1;
	g_hd_ok = false;
}

static void fatal_screen(const std::string &line1, const std::string &line2)
{
	/* About 5 s of frames. A scene can only start when the display queue has room and a pool bank is free (vita2d_try_start_drawing_advanced):
	 * a loop that does not wait for the refresh runs out of both, and drawing with no scene open is an error (sceGxmDraw NOT_WITHIN_SCENE, then a
	 * fault). So: draw only after a scene really started, otherwise wait for the next refresh and try again. */
	for (int shown = 0, tries = 0; shown < 300 && tries < 3000; tries++) {
		vita2d_pool_reset();          /* every frame, as frame_draw does: 300 frames of text outgrew the pool and the draws failed (a crash in the emulator) */
		if (!vita2d_try_start_drawing_advanced(NULL, 0)) { sceDisplayWaitVblankStart(); continue; }
		shown++;
		vita2d_clear_screen();
		g_text->draw(pk::F_SUB, 60, 180, line1.c_str(), PK_RGBA(255, 255, 255, 255));
		g_text->wrap(pk::F_BODY, 60, 240, 840, 3, 4, line2, PK_RGBA(192, 192, 192, 255));
		g_text->draw(pk::F_BODY, 60, 360, "Returning to VitAdhoc...", PK_RGBA(192, 192, 192, 255));
		vita2d_end_drawing();
		vita2d_swap_buffers();
	}
}

/* The system keyboard (an IME common dialog), drawn over the frozen game and the menu. ASCII only. */
static bool ime_ask(const char *title, const char *initial, std::string &out)
{
	static SceWChar16 title16[64], init16[96], buf16[96];
	memset(title16, 0, sizeof title16); memset(init16, 0, sizeof init16); memset(buf16, 0, sizeof buf16);
	for (int i = 0; title[i] && i < 63; i++) title16[i] = (unsigned char) title[i];
	for (int i = 0; initial[i] && i < 95; i++) init16[i] = (unsigned char) initial[i];
	SceImeDialogParam p;
	sceImeDialogParamInit(&p);
	p.supportedLanguages = 0;
	p.languagesForced = SCE_FALSE;
	p.type = SCE_IME_TYPE_BASIC_LATIN;
	p.option = SCE_IME_OPTION_NO_ASSISTANCE;
	p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
	p.title = title16;
	p.maxTextLength = 90;
	p.initialText = init16;
	p.inputTextBuffer = buf16;
	int rc = sceImeDialogInit(&p);
	if (rc < 0) { logf_("keyboard could not open: %08x\n", rc); return false; }
	bool ok = false;
	for (;;) {
		if (frame_draw(g_pl->picture(), 1, 0)) {
			g_pl->menu_draw(g_gfx, g_text);
			vita2d_end_drawing();
			vita2d_common_dialog_update();
			vita2d_swap_buffers();
		} else sceDisplayWaitVblankStart();
		SceCommonDialogStatus st = sceImeDialogGetStatus();
		if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
			SceImeDialogResult r;
			memset(&r, 0, sizeof r);
			sceImeDialogGetResult(&r);
			ok = r.button == SCE_IME_DIALOG_BUTTON_ENTER;
			break;
		}
		if (st == SCE_COMMON_DIALOG_STATUS_NONE) break;
	}
	sceImeDialogTerm();
	out.clear();
	for (int i = 0; buf16[i] && i < 95; i++) out += buf16[i] < 128 ? (char) buf16[i] : '?';
	return ok;
}

/* ---- launch / return --------------------------------------------------- */

static void return_to_vitadhoc(void)
{
	const char *argv[] = { "--from-snes", NULL };   /* arguments only: the system adds the path */
	if (g_log) { fputs("returning to VitAdhoc\n", g_log); fclose(g_log); g_log = NULL; }
	sceAppMgrLoadExec("app0:eboot.bin", (char *const *) argv, NULL);
	sceKernelExitProcess(0);
}

static volatile int g_suspend_flag;
static int power_cb(int, int info, int, void *)
{
	if (info & (SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND)) g_suspend_flag = 1;
	return 0;
}

static uint64_t process_us(void) { return sceKernelGetProcessTimeWide(); }

static unsigned arm_mhz_measured(void)
{
	const unsigned n = 2000000u;
	uint64_t best = ~0ull;
	for (int k = 0; k < 3; k++) {
		unsigned i = n;
		const uint64_t t0 = sceKernelGetProcessTimeWide();
		__asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(i) : : "cc");
		const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
		if (dt && dt < best) best = dt;
	}
	return best != ~0ull ? (unsigned) (n / best) : 0;
}

static int ini_int(const char *path, const char *key, int def)
{
	FILE *f = fopen(path, "rb");
	if (!f) return def;
	char line[96];
	size_t kl = strlen(key);
	int v = def;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, key, kl) && line[kl] == '=') v = atoi(line + kl + 1);
	fclose(f);
	return v;
}

/* thread priorities from threads.ini: only 0 (leave) or 0x40..0xBF are used; anything else is the default */
static int prio_ok(int p, int def) { return p == 0 || (p >= 0x40 && p <= 0xBF) ? p : def; }

/* ---- the two-player link (netplay.cpp) ---------------------------------------------------------------- */

static snesnet::Link *g_link;
static bool g_net_up, g_net_module, g_net_ours, g_netctl_ours;
static void *g_net_pool;

/* The system's network stack, as the main app brings it up (gpsp/vita/net_vita.c): the transport only opens sockets. */
static bool link_net_up(void)
{
	if (g_net_up) return true;
	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
		if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return false;
		g_net_module = true;
	}
	g_net_pool = malloc(1024 * 1024);
	if (!g_net_pool) return false;
	SceNetInitParam ip;
	ip.memory = g_net_pool;
	ip.size = 1024 * 1024;
	ip.flags = 0;
	int rc = sceNetInit(&ip);
	if (rc >= 0) g_net_ours = true;
	else { free(g_net_pool); g_net_pool = NULL; if ((unsigned) rc != (unsigned) SCE_NET_ERROR_EBUSY) return false; }
	rc = sceNetCtlInit();
	if (rc >= 0) g_netctl_ours = true;
	else if ((unsigned) rc != 0x80412102u) return false;     /* already up is fine */
	g_net_up = true;
	return true;
}

static void link_net_down(void)
{
	if (g_netctl_ours) sceNetCtlTerm();
	if (g_net_ours) sceNetTerm();
	free(g_net_pool);
	g_net_pool = NULL;
	if (g_net_module) sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
	g_netctl_ours = g_net_ours = g_net_module = g_net_up = false;
}

static void link_random(uint8_t *out, int n) { sceKernelGetRandomNumber(out, (SceSize) n); }
static void link_log(const char *line) { g_pl->logf("link: %s\n", line); }

/* Connecting, waiting, the session's set-up: a plain screen with two lines and the way out. Returns true when the
 * player asked to leave. */
static bool link_screen(const pk::PadSample &pad, bool enter_is_cross)
{
	std::string l1, l2;
	int p1 = -1, p2 = -1;
	g_link->status(&l1, &l2, &p1, &p2);
	vita2d_pool_reset();
	if (!vita2d_try_start_drawing_advanced(NULL, 0)) { sceDisplayWaitVblankStart(); return (pad.buttons & pk::phys_cancel(enter_is_cross)) != 0; }       /* no scene: nothing is drawn this time (see fatal_screen) */
	vita2d_clear_screen();
	g_text->draw(pk::F_SUB, 60, 170, "Two-player game", PK_RGBA(255, 255, 255, 255));
	g_text->draw(pk::F_SUB, 60, 230, l1.c_str(), PK_RGBA(255, 255, 255, 255));
	if (!l2.empty()) g_text->draw(pk::F_BODY, 60, 280, l2.c_str(), PK_RGBA(192, 192, 192, 255));
	const int bar = p1 >= 0 ? p1 : p2;
	if (bar >= 0) {
		vita2d_draw_rectangle(60, 330, 840, 12, RGBA8(60, 60, 60, 255));
		vita2d_draw_rectangle(60, 330, (float) (840 * bar / 1000), 12, RGBA8(123, 108, 255, 255));
	}
	g_text->draw(pk::F_BODY, 60, 470, enter_is_cross ? "Circle: cancel" : "Cross: cancel", PK_RGBA(160, 160, 160, 255));
	vita2d_end_drawing();
	vita2d_swap_buffers();
	return (pad.buttons & pk::phys_cancel(enter_is_cross)) != 0;
}

/* ---- the menu ---------------------------------------------------------------- */

/* Runs the control centre until it closes. Returns MENU_RESUME/RESET_DONE (play on), MENU_GAME_LIST or MENU_EXIT. */
static pk::MenuEvent menu_loop(int player)
{
	g_pace.set_paused(true);
	g_pl->open_menu(player);
	bool first = true;
	uint64_t prev = sceKernelGetProcessTimeWide();
	pk::MenuEvent ev = pk::MENU_NONE;
	for (;;) {
		/* draw first, so the menu is on screen before anything is written */
		if (frame_draw(g_pl->picture(), 1, 0)) {
			g_pl->menu_draw(g_gfx, g_text);
			frame_finish();
			if (first) { first = false; g_pl->menu_drawn(); }
		} else sceDisplayWaitVblankStart();
		uint64_t now = sceKernelGetProcessTimeWide();
		pk::MenuInput in;
		uint8_t back = 0;
		bool tapped;
		touch_read(&in.touch, &back, now, &tapped);
		read_pads(in.pads, back);
		in.dt_ms = (int) ((now - prev) / 1000);
		if (in.dt_ms <= 0 || in.dt_ms > 100) in.dt_ms = 16;
		prev = now;
		ev = g_pl->menu_update(in);
		if (g_ime_wanted) {
			g_ime_wanted = false;
			std::string code, name;
			bool ok = ime_ask("Cheat code (Game Genie, Pro Action Replay or address=value)", "", code);
			if (ok && !code.empty()) ime_ask("A name for it (you can leave it empty)", "", name);
			g_pl->cheat_entered(ok, code, name);
		}
		if (ev != pk::MENU_NONE && ev != pk::MENU_RESET_DONE && ev != pk::MENU_RESUME) break;
		if (!g_pl->menu_open()) break;
		if (g_suspend_flag) { g_suspend_flag = 0; if (g_link) g_link->suspend(); g_pl->suspend(); }
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
	}
	g_pace.set_paused(false);
	return ev;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(APP_ROOT, 0777);
	sceIoMkdir(DATA_ROOT, 0777);
	g_log = fopen(DATA_ROOT "/log.txt", "a");
	g_pl = new snesp::SnesPlayer(&g_platform);
	logf_("---- snes.bin start (argc=%d)\n", argc);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
	/* emulation on core 0; the workers start on cores 1 (picture) and 2 (sound chip) below */
	{
		int rc = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
		logf_("main thread to core 0 rc=%08x\n", rc);
	}
	{
		SceAppUtilInitParam ip;
		SceAppUtilBootParam bp;
		memset(&ip, 0, sizeof ip);
		memset(&bp, 0, sizeof bp);
		sceAppUtilInit(&ip, &bp);
	}
	SceCommonDialogConfigParam cdc;
	sceCommonDialogConfigParamInit(&cdc);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *) &cdc.language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *) &cdc.enterButtonAssign);
	sceCommonDialogSetConfigParam(&cdc);
	int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
	SceUID pcb = sceKernelCreateCallback("snes_power", 0, power_cb, NULL);
	if (pcb >= 0) scePowerRegisterCallback(pcb);

	vita2d_init();
	vita2d_set_clear_color(0xFF000000);
	g_gfx = new pk::VitaGfx();
	g_text = new pk::Text(g_gfx);
	pk::scale_init();
	video_init();

	/* worker threads: switches in threads.ini (apu_thread, ppu_thread, spin); on by default */
	const char *tini = DATA_ROOT "/threads.ini";
	core4_start(tini);
	int want_apu = ini_int(tini, "apu_thread", 1), want_ppu = ini_int(tini, "ppu_thread", 1);
	int spin = ini_int(tini, "spin", 300);
	snes_host_thread_config(2, 0x60, 1, 0x60, spin);
	/* The wide view's margin thread (snes/WIDE-INTEGRATION.md "Margin thread"): the core's wide passes and the capture, off the PPU thread.
	 * threads.ini: wide_thread=1 (default) / 0 (the PPU thread draws them, as before); wide_core=-1 (default: cores 0 and 2, in the gaps the
	 * emulating thread and the sound chip thread leave), 0, 1 or 2 (that core only); wide_prio (default 0x68: below those two threads, above
	 * the scaler's helpers). */
	const int want_wide_thr = ini_int(tini, "wide_thread", 1) != 0 ? 1 : 0;
	{
		const int wc = ini_int(tini, "wide_core", -1);
		const int wprio = prio_ok(ini_int(tini, "wide_prio", 0x68), 0x68);
		snes_host_wide_thread_config(wc >= 0 && wc <= 2 ? wc : (int) (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_2), wprio > 0 ? wprio : 0x68);
		logf_("wide margin thread: %s, core %s, priority 0x%x\n", want_wide_thr ? "on" : "off", wc == 0 ? "0" : wc == 1 ? "1" : wc == 2 ? "2" : "0+2", wprio > 0 ? wprio : 0x68);
	}
	const int audio_prio = prio_ok(ini_int(tini, "audio_prio", PRIO_AUDIO_DEFAULT), PRIO_AUDIO_DEFAULT);
	const int main_prio = prio_ok(ini_int(tini, "main_prio", PRIO_MAIN_DEFAULT), PRIO_MAIN_DEFAULT);
	const bool audio_legacy = ini_int(tini, "audio_legacy", 0) != 0;
	if (main_prio > 0) {
		int rc = sceKernelChangeThreadPriority(0, main_prio);
		logf_("main thread priority 0x%x rc=%08x now 0x%x\n", main_prio, rc, sceKernelGetThreadCurrentPriority());
	}
	snes_host_set_option("apu_thread", want_apu);
	snes_host_set_option("ppu_thread", want_ppu);
	snes_host_set_option("wide_thread", want_wide_thr);
	snes_host_set_log([](const char *line) { logf_("%s", line); });

	/* the game: launch file or argument, patch, saves, states, settings (player.cpp) */
	writer_start();
	snesp::StartResult sr = g_pl->start(argc, argv, APP_ROOT, enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS);
	if (!sr.ok) {
		logf_("cannot start: %s / %s\n", sr.fatal1.c_str(), sr.fatal2.c_str());
		g_pl->log_flush(true);
		fatal_screen(sr.fatal1, sr.fatal2);
		writer_stop();
		return_to_vitadhoc();
	}
	cdc.enterButtonAssign = sr.enter_is_cross ? SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS : SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
	sceCommonDialogSetConfigParam(&cdc);

	/* a two-player link: the connection is made again here (netplay.h); a link that cannot start returns to the app */
	if (g_pl->linked()) {
		snesnet::Env env;
		env.now_us = process_us;
		env.random = link_random;
		env.log = link_log;
		env.local_ip_be = 0;
		g_link = new snesnet::Link;
		bool ok = link_net_up();
		if (!ok) logf_("link: the network could not start\n");
		if (ok) ok = g_link->open(g_pl->request().link, g_pl->link_rom(), env);
		if (!ok) {
			const snesnet::Fail f = ok ? g_link->failure() : snesnet::F_NO_NETWORK;
			logf_("link: could not open (fail %d)\n", (int) f);
			g_link->close();
			g_pl->set_link_result("never", (int) f, "", 0);
			g_pl->finish(pk::EXIT_GAME_LIST);
			writer_stop();
			fatal_screen("The two-player game could not start.", "The connection could not be opened.");
			link_net_down();
			return_to_vitadhoc();
		}
	}

	/* clocks: the app's CPU speed request (0 = the player's default: 500 MHz, else 444) */
	{
		int want = sr.cpu_mhz == 444 ? 444 : 500;
		int clk = scePowerSetArmClockFrequency(want);
		if (clk < 0 && want == 500) scePowerSetArmClockFrequency(444);
		scePowerSetBusClockFrequency(222);
		scePowerSetGpuClockFrequency(222);
		scePowerSetGpuXbarClockFrequency(166);
		logf_("arm clock now %d MHz (asked %d%s, rc=%08x)\n", scePowerGetArmClockFrequency(), want, sr.cpu_mhz ? " by the app" : "", clk);
		logf_("clocks: the ARM core measures %u MHz\n", arm_mhz_measured());
	}
	bezel_load(g_pl->game_name());
	logf_("threads=%d (apu %d ppu %d spin %d)\n", snes_host_threads_active(), want_apu, want_ppu, spin);
	audio_init(audio_legacy, audio_prio > 0 ? audio_prio : PRIO_AUDIO_DEFAULT);
	g_pl->apply_start_mode();
	/* the wide view's cost ladder (threads.ini): wide_policy 1 (default) steps the composed margin down when the compose time is too high,
	 * 0 keeps the full margin; wide_cost_us (default 13000) is the smoothed compose time over which a rung is given up */
	g_pl->set_wide_policy_ini(ini_int(tini, "wide_policy", 1) != 0 ? 1 : 0);   /* the player sets it with the menu's Wide view row */
	snes_wide_option("cost_us", clampi_(ini_int(tini, "wide_cost_us", 13000), 500, 30000));
	logf_("wide ladder: policy=%d cost_us=%d\n", ini_int(tini, "wide_policy", 1) != 0, clampi_(ini_int(tini, "wide_cost_us", 13000), 500, 30000));
	g_pl->look_start(scePowerGetArmClockFrequency());   /* the look governor: the best look first, the game's memory of what it needed */

	const double frame_hz = snes_host_is_pal() ? 50.0 : 60.0988;
	const double vita_hz = 59.94;
	/* PAL: 5 frames in 6 refreshes (see pal_pace.h). The audio pacer measures the speed against frame_hz (50), so its base
	 * rate follows 50 Hz on its own once the frames come at that rate. NTSC does not use it (one per refresh, as before). */
	const bool pal = snes_host_is_pal() != 0;
	pk::PalSmooth pal_pace;
	bool pal_pure_pending = false;      /* Smooth 50 Hz: the early frame was shown mixed; the refresh after it shows the picture alone */
	unsigned pal_mixed = 0;
	if (pal) { pal_pace.set(50.0070, vita_hz); logf_("PAL: %s\n", g_pl->pal_fast() ? "Fast 60 Hz chosen: one frame per refresh" : g_pl->pal_smooth() ? "Smooth 50 Hz: 5 of 6 refreshes emulate a frame, the sixth's frame comes one refresh early and is mixed with the one before" : "True 50 Hz: 5 of 6 refreshes emulate a frame"); }
	hd_start(tini, frame_hz);
	help_start(clampi_(ini_int(tini, "helpers", 2), 0, 2));
	/* One emulated frame per display refresh: the console runs at 60.0988 Hz and the Vita shows 59.94, so
	 * the game's sound arrives 0.265% slower than real time; the audio rate control absorbs that. With
	 * steady pacing in half mode a pass that missed a refresh runs the missed frame too (undrawn), so the
	 * game keeps its true rate. */

	/* log state */
	uint64_t sec_start = sceKernelGetProcessTimeWide();
	int frames_run = 0, vblanks = 0, secs = 0;
	uint64_t emu_us = 0, main_us = 0, fetch_us = 0, draw_us = 0, finish_us = 0, vwait_us = 0;
	uint64_t prof_acc[S9XP_N] = { 0 };
	uint64_t prof_frames = 0;
	uint64_t last_frame_us = 0;
	snes_thread_stats st0, st1;
	snes_host_thread_stats(&st0);
	uint64_t ppu_busy_at_present = st0.ppu_busy_ns;
	unsigned drawn = 0, skipped = 0, pic_fresh = 0, not_rendered = 0;
	/* Super FX pacing: see below (re-clocked every 30 frames) */
	const bool superfx = snes_host_has_superfx() != 0;
	int fx_pct = 100;
	uint64_t fx_emu_us = 0, fx_chip_us = 0;
	int fx_frames = 0;
	if (superfx) snes_host_superfx_timer(process_us);
	unsigned last_vc = sceDisplayGetVcount();
	apace::SpeedMeter speed_meter(8);
	bool stats_pending = false;
	uint32_t ur_seen = 0;
	uint64_t prev_loop0 = 0;
	unsigned w16 = 0, w20 = 0, w25 = 0;
	double hd_draw_ms = 0;            /* the drawing thread's busy time and the repeats of the last hand-over census, for the look governor */
	unsigned hd_repeated = 0, missed_refr = 0, pal_planned = 0;   /* pal_planned: refreshes that owed no frame (the 6th of a PAL game) */
	uint64_t stall_max_us = 0, work_max_us = 0;
	unsigned s_w16 = 0, s_w20 = 0, s_w25 = 0;
	double s_stall = 0, s_work = 0, s_agap = 0;
	uint32_t emu_frame = 0;
	uint64_t render_emu_us = 0;
	pk::ExitReason why = pk::EXIT_NORMAL;
	g_pl->log_flush(false);

	g_pl->set_capture_deferred(true);
	for (;;) {
		/* rewind's state of the frame(s) of the last pass is written here, after the picture and the sound were handed on, in the time this
		 * thread would wait for the refresh; a pass that already missed its refresh puts it off (pk_speed) */
		g_pl->capture_idle(sceDisplayGetVcount() != last_vc);
		/* One pass per display refresh, counted by the display itself: a pass whose picture went into the
		 * queue without waiting must not start the next frame early (that ran games at twice their speed). */
		unsigned vc = sceDisplayGetVcount();
		if (vc == last_vc) {
			uint64_t w0 = sceKernelGetProcessTimeWide();
			sceDisplayWaitVblankStart();
			vc = sceDisplayGetVcount();
			vwait_us += sceKernelGetProcessTimeWide() - w0;
		}
		unsigned elapsed = vc - last_vc;
		last_vc = vc;
		if (elapsed > 1) missed_refr += elapsed - 1;   /* refreshes the loop was too late for */
		if (elapsed == 0) elapsed = 1;
		if (elapsed > 2) elapsed = 2;       /* a long stall (menu, suspend) is not caught up */
		uint64_t loop0 = sceKernelGetProcessTimeWide();
		const uint64_t emu_us0 = emu_us;       /* the refresh's emulation time is the difference at its end (Unlimited fast forward's budget) */
		if (prev_loop0 && loop0 - prev_loop0 > stall_max_us) stall_max_us = loop0 - prev_loop0;
		prev_loop0 = loop0;
		if (g_wd_audio >= 0) {
			g_cores->watchdog().ref_beat();
			if (g_cores->poll_watchdog(loop0)) core4_fallback();
		}

		pk::PadSample pads[pk::MAX_PLAYERS];
		pk::TouchPoint tp;
		uint8_t back = 0;
		bool tapped = false;
		touch_read(&tp, &back, loop0, &tapped);
		/* a linked game: the menu button means "end the session" for BOTH consoles; a brush of the screen must not (the held
		 * Start + Select chord still does) */
		if (g_link) tapped = false;
		read_pads(pads, back);
		/* a game being played keeps the console awake */
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

		/* a linked game that is not playing yet (reaching the other console, the session's set-up) or no longer: a plain
		 * screen; the session is pumped by step(). Cancel leaves; over (done or failed) leaves with the result. */
		if (g_link && !g_link->playing()) {
			const int lr = g_link->step(0);
			if (lr == snesnet::STEP_OVER) { why = pk::EXIT_GAME_LIST; break; }
			if (g_link->playing()) { g_pl->look_reset_window(); last_vc = sceDisplayGetVcount(); continue; }
			if (link_screen(pads[0], g_pl->request().enter_is_cross)) { logf_("link: cancelled by the player\n"); why = pk::EXIT_GAME_LIST; break; }
			continue;
		}
		/* a linked game never skips a picture and never widens it: the core's picture work leaves traces in the machine's own
		 * state (clip window counts, the sprite range and time flags a game can read), so two consoles must draw alike to stay
		 * identical (link_twin: a console that never draws diverges within 100 frames) */
		if (!g_link) g_pl->wide_apply(SCREEN_W, SCREEN_H);
		const bool half = g_pl->pace_mode() == pk::PACE_HALF;
		/* Smooth hand-over: the game never waits for a picture, so a pass that came late runs the frames it owes (only the
		 * last of them is drawn) and the game keeps its true speed; pk_handoff drops older pictures in half mode only. */
		const bool hd = hd_active();
		if (hd) g_pipe.box().set_pace_mode(g_pl->pace_mode());
		{
			/* the wide view composes on the drawing thread while the hand-over is on, inline (in the frame hook) when it is off */
			const bool want_split = hd && g_pl->wide_active();
			if (want_split != g_wide_split) {
				if (hd_quiesce()) { snes_wide_split(want_split ? 1 : 0); g_wide_split = want_split; }
			}
		}
		int to_run = (half || hd) ? (int) elapsed : 1;
		if (g_link) to_run = 1;           /* lockstep: one frame per refresh, a missed refresh is not caught up */
		bool mix_early = false;
		if (pal && !g_pl->pal_fast()) {
			const int refr = to_run;
			/* Smooth 50 Hz (pk_palsmooth.h) only for a plain single refresh: not linked, not in half mode, not with the wide view's capture split */
			pal_pace.smooth = g_pl->pal_smooth() && !g_link && !half && !g_wide_split;
			g_pal_mix_on.store(pal_pace.smooth ? 1 : 0, std::memory_order_relaxed);
			to_run = refr == 1 ? pal_pace.step(&mix_early) : pal_pace.owed(refr);
			pal_planned += (unsigned) (refr - to_run);       /* 0 on the 6th refresh (not with Smooth 50 Hz: that frame runs early) */
		}
		if (!g_link) {
			to_run = g_pl->speed_plan(to_run);       /* fast forward runs 2 or 3 frames a refresh, rewinding one landing */
			if (to_run != 1) mix_early = false;
			if (hd) g_pipe.box().set_force_drop(g_pl->speed_busy());
		}
		bool fresh = false, rendered_any = false, want_menu = false, link_over = false;
		uint64_t emu_pass_us = 0;
		if (pal && to_run == 0 && tapped && g_pl->input(pads, true, loop0 / 1000, emu_frame)) want_menu = true;   /* a tap on a refresh that runs no frame still counts */
		for (int i = 0; i < to_run; i++) {
			if (g_pl->input(pads, tapped && i == 0, loop0 / 1000, emu_frame)) { want_menu = true; break; }
			bool render = g_link ? true : g_pl->want_render(emu_frame);
			if (!g_link && hd && !half && i != to_run - 1) render = false;
			if (!g_link && !g_pl->speed_draws(i, to_run)) render = false;       /* fast forward draws the last frame of the refresh */
			snes_host_set_render(render ? 1 : 0);
			uint64_t t0 = sceKernelGetProcessTimeWide();
#ifdef S9X_PROF
			int prof_this = (secs & 3) == 0 && !snes_host_threads_active(); /* the hooks are not thread safe */
			if (prof_this) {
				memset(g_s9xp.acc, 0, sizeof g_s9xp.acc);
				g_s9xp.cur = S9XP_CPU; g_s9xp.sp = 0; g_s9xp.on = 1; g_s9xp.last = s9xp_now();
			}
#endif
			if (g_link) {
				/* both players' buttons are needed for a frame: a late partner is a stall (no frame this refresh) */
				const int lr = g_link->step((uint16_t) g_pl->link_pad());
				if (lr == snesnet::STEP_OVER) { why = pk::EXIT_GAME_LIST; link_over = true; break; }
				if (lr != snesnet::STEP_FRAME) break;
			} else g_pl->run_frame(render);
#ifdef S9X_PROF
			if (prof_this) {
				s9xp_enter(S9XP_FRONT); s9xp_leave();
				g_s9xp.on = 0;
				for (int c = 0; c < S9XP_N; c++) prof_acc[c] += g_s9xp.acc[c];
				prof_frames++;
			}
#endif
			uint64_t t1 = sceKernelGetProcessTimeWide();
			emu_us += t1 - t0;
			last_frame_us = t1 - t0;
			emu_pass_us += t1 - t0;
			frames_run++;
			emu_frame++;
			if (render) { render_emu_us = t1 - t0; rendered_any = true; } else not_rendered++;
			static int16_t abuf[2048 * 2];
			int n;
			while ((n = snes_host_audio_take(abuf, 2048)) > 0) { if (!g_link) n = (int) g_pl->audio_filter(abuf, (size_t) n); if (n > 0) g_pace.push(abuf, n); }
			{
				uint64_t f0 = sceKernelGetProcessTimeWide();
				if (hd) {
					/* the picture goes to the drawing thread by copy; this thread does not wait for it (except at the gate) */
					g_pipe.box().note_emulated(1);
					PicSrc ps_src;
					snes_wide_snap *wsnap = NULL; unsigned wser = 0; int whint = 0;
					if (g_wide_split && g_pl->wide_active()) {
						/* every captured frame is composed, in order (a frame in which the core's loop ended late has two captures, the next none) */
						for (int k = 0; k < 3 && snes_wide_snap_take(&wsnap, &wser, &whint); k++) {
							/* the capture goes to the drawing thread as it is; only a frame the view will not widen carries the normal picture */
							int nw = 0, nh = 0, np = 0; unsigned nser = 0;
							const uint16_t *np_ = whint ? NULL : snes_host_video_nowait(&nw, &nh, &np, &nser);
							if (g_pipe.put_snap(wsnap, wser, np_, nw, nh, np, (int) (wser & 0x1FFFFFFFu), (uint32_t) frames_run, (uint32_t) render_emu_us) > 0) fresh = true;
							else { snes_wide_snap_release(wsnap, wser); break; }
						}
					} else if (video_source(&ps_src, g_hd_norm_seen, g_hd_wide_seen, g_hd_src_wide)) {
						const int user = (ps_src.is_wide ? 0x40000000 : 0) | (mix_early && render ? 0x10000000 : 0) | (int) (ps_src.serial & 0x0FFFFFFFu);
						if (g_pipe.put(ps_src.p, ps_src.w, ps_src.h, ps_src.pitch, user, (uint32_t) frames_run, (uint32_t) render_emu_us) > 0) {
							if (ps_src.is_wide) g_hd_wide_seen = ps_src.serial; else g_hd_norm_seen = ps_src.serial;
							g_hd_src_wide = ps_src.is_wide ? 1 : 0;
							fresh = true;
						}
					}
				} else { g_next_mix = mix_early && render; if (video_fetch(g_pl->picture(), frames_run)) fresh = true; g_next_mix = false; }
				fetch_us += sceKernelGetProcessTimeWide() - f0;
			}
			g_pl->after_frame(sceKernelGetProcessTimeWide());
		}
		/* Smooth 50 Hz: the refresh after an early frame runs none; it shows that picture alone (the early refresh showed it mixed).
		 * With the hand-over the drawing thread does it by itself right after the mixed picture. */
		if (pal_pure_pending && to_run == 0 && !g_link && !hd) {
			PicSrc pure;
			if (pal_pure_src(&pure) && video_upload(g_pl->picture(), frames_run, pure)) fresh = true;
			if (fresh && pal_planned) pal_planned--;     /* a new picture was shown: this refresh did not repeat one */
		}
		pal_pure_pending = mix_early;
		if (mix_early) pal_mixed++;
		(void) pal_mixed;
		if (hd) {
			/* what the drawing thread measured goes to steady pacing, and its toast is the player's as of now */
			uint32_t hc;
			while (g_pipe.box().poll_cost(&hc)) g_pl->drawn_cost(hc);
			hd_publish_notices();
		}
		if (link_over) break;
		if (want_menu && g_link) {
			/* a linked game has no menu (a state, a cheat or a reset would change one console's machine): the menu button asks for the clean end */
			logf_("link: end asked from the menu button\n");
			g_link->request_end();
			want_menu = false;
		}
		if (want_menu) {
			hd_quiesce();
			pk::MenuEvent ev = menu_loop(0);
			if (ev == pk::MENU_GAME_LIST) { why = pk::EXIT_GAME_LIST; break; }
			if (ev == pk::MENU_EXIT) { why = pk::EXIT_NORMAL; break; }
			hd_after_menu();
			snes_host_set_multitap(g_pl->multitap_wanted());
			last_vc = sceDisplayGetVcount();
			speed_meter.reset();
			prev_loop0 = 0;
			sec_start = sceKernelGetProcessTimeWide();
			frames_run = vblanks = 0;
			continue;
		}
		if (fresh) pic_fresh++;
		(void) rendered_any;

		/* the old player's rate control ran here; the pacer's runs on the audio thread (a no-op unless audio_legacy=1) */
		g_pace.legacy_update();

		/* Draw: every pass in full mode (a late picture is shown again), in half mode only when a new
		 * picture came (every second pass, evenly). */
		bool drew = false;
		uint64_t d0 = sceKernelGetProcessTimeWide();
		if (!hd && (!half || fresh)) {
			drew = frame_draw(g_pl->picture(), 1, frames_run);
			if (drew && g_pl->notices().active()) g_pl->notices().draw(g_gfx, g_text, PK_HEX(0x7B6CFF, 255), 26, true);
		}
		draw_us += sceKernelGetProcessTimeWide() - d0;
		uint64_t work_end = sceKernelGetProcessTimeWide();
		if (!g_link && g_pl->speed_busy()) { const uint64_t w = work_end - loop0, e = emu_us - emu_us0; g_pl->speed_other_us((uint32_t) (w > e ? (w - e > 100000 ? 100000 : w - e) : 0)); }
		if (drew) {
			frame_finish();
			drawn++;
			finish_us += sceKernelGetProcessTimeWide() - work_end;
			/* the cost of a drawn frame, for steady pacing: the emulation of the frame that was rendered plus the
			 * fetch / GPU work of showing it, or the picture thread's work since the last picture, whichever is more */
			snes_host_thread_stats(&st1);
			uint64_t ppu_us = (st1.ppu_busy_ns - ppu_busy_at_present) / 1000;
			ppu_busy_at_present = st1.ppu_busy_ns;
			uint64_t show_us = sceKernelGetProcessTimeWide() - d0;
			uint64_t cost = render_emu_us + show_us;
			if (ppu_us > cost) cost = ppu_us;
			g_pl->drawn_cost((uint32_t) (cost > 0xFFFFFFFFull ? 0xFFFFFFFFu : cost));
		} else if (!hd) skipped++;
		vblanks += elapsed;
		main_us += work_end - loop0;
		/* Super FX pacing (see snes/README.md): the chip's speed is matched to what this Vita manages so the
		 * game, its music and its controls keep full speed. Only for Super FX carts. */
		if (superfx && !g_link) {           /* linked: both consoles run the chip at full speed (the adaptive clock would desync them) */
			fx_emu_us += emu_pass_us;
			fx_chip_us += snes_host_superfx_take_us();
			if ((fx_frames += to_run) >= 30) {
				const double avg = fx_emu_us / (double) fx_frames, chip = fx_chip_us / (double) fx_frames;
				const double rest = avg - chip, target = 1e6 / vita_hz * 0.90;
				int want = fx_pct;
				if (chip > 100.0) {
					double fit = fx_pct * (target - rest) / chip;
					if (avg > target) want = (int) fit;
					else if (avg < target * 0.9 && fx_pct < 100) want = (int) (fit < fx_pct + 10 ? fit : fx_pct + 10);
				} else if (fx_pct < 100) want = 100;
				if (want > 100) want = 100;
				if (want < 30) want = 30;
				if (want != fx_pct && (want < fx_pct || want >= fx_pct + 2)) {
					logf_("superfx clock %d%% -> %d%% (%.1f ms a frame, chip %.1f ms)\n", fx_pct, want, avg / 1000.0, chip / 1000.0);
					fx_pct = want;
					snes_host_set_superfx_clock(fx_pct);
				}
				fx_emu_us = fx_chip_us = 0;
				fx_frames = 0;
			}
		}
		uint64_t now = sceKernelGetProcessTimeWide();
		if (g_suspend_flag) { g_suspend_flag = 0; if (g_link) g_link->suspend(); g_pl->suspend(); }

		/* per pass bookkeeping for the pacing diagnostics */
		{
			const uint64_t work = now - loop0;
			if (work > 16700) w16++;
			if (work > 20000) w20++;
			if (work > 25000) w25++;
			if (work > work_max_us) work_max_us = work;
			const uint32_t u = g_pace.underruns();
			if (u != ur_seen) {
				const apace::UnderrunEvent e = g_pace.last_underrun();
				logf_("UNDERRUN #%u (%u new) at out=%.2fs ema=%.1fms target->%.0fms ratio=%+.0fppm queue_now=%ums work=%.1fms\n", e.index,
				    u - ur_seen, e.out_frames / (double) OUT_RATE, e.ema_ms, e.target_ms, e.ratio_ppm, g_pace.fill_frames() * 1000 / SNES_AUDIO_RATE,
				    work / 1000.0);
				ur_seen = u;
			}
		}

		if (g_hd_ok) {
			/* one census line a second while the hand-over is on: pictures made / shown / repeated / dropped, depth, and the game's
			 * time against the wall's (a game that lost time shows a negative drift) */
			char hl[512];
			if (g_pipe.box().census_line(now, hl, sizeof hl)) {
				hd_draw_ms = g_hd_draw_us.exchange(0) / 1000.0;
				const char *rp = strstr(hl, "repeated=");
				hd_repeated = rp ? (unsigned) atoi(rp + 9) : 0;
				logf_("%s draw_ms=%.1f\n", hl, hd_draw_ms);
			}
		}
		if (now - sec_start >= 1000000) {
			double dt = (now - sec_start) / 1e6;
			if (g_hd_ok) { drawn += g_hd_drawn.exchange(0); skipped += g_hd_failed.exchange(0); }
			{
				const double sp = speed_meter.update(frames_run, dt, frame_hz);
				if (sp >= 0) g_pace.report_speed(sp);
				s_w16 = w16; s_w20 = w20; s_w25 = w25; s_stall = stall_max_us / 1000.0; s_work = work_max_us / 1000.0;
				s_agap = g_agap_max_us.exchange(0) / 1000.0;
				w16 = w20 = w25 = 0; stall_max_us = work_max_us = 0;
				stats_pending = true;
			}
			const unsigned sc_us = g_scale_us.exchange(0), sc_pics = g_scale_pics.exchange(0), sc_nonflat = g_scale_nonflat.exchange(0),
			    sc_help = g_scale_helped.exchange(0), sc_waits = g_scale_waits.exchange(0);
			{
				/* the look governor's second (costs as fractions of a 60 Hz refresh): emulation per emulated frame, drawing per
				 * refresh (the drawing thread's busy time with the hand-over, else the main thread's drawing), and the refreshes that
				 * showed an old picture without the plan wanting it (the 6th refresh of a PAL game is planned) */
				const double budget = 16683.0, refr = vblanks > 0 ? vblanks : 1;
				pk::LookSecond ls;
				ls.refreshes = vblanks;
				ls.emu = frames_run ? (float) (emu_us / (double) frames_run / budget) : 0.f;
				const double all_draw_us = g_hd_ok ? hd_draw_ms * 1000.0 : (double) (draw_us + fetch_us + finish_us);
				ls.filter = (float) (sc_us / refr / budget);
				ls.draw = (float) ((all_draw_us > sc_us ? all_draw_us - sc_us : 0.0) / refr / budget);
				const unsigned planned = pal_planned;
				pal_planned = 0;
				/* a PAL game runs 5 frames in 6 refreshes: its cost per refresh is 50.007 / 59.94 of the cost per frame, so one that holds 50 is not overloaded */
				if (pal && !g_pl->pal_fast()) ls.emu *= (float) (50.007 / 59.94);
				const unsigned late = g_hd_ok ? (hd_repeated > planned ? hd_repeated - planned : 0) : missed_refr;
				ls.missed = (int) late;
				ls.parallel = hd;
				ls.pace_half = half;
				g_pl->look_second(ls);
				missed_refr = 0;
			}
			snes_host_thread_stats(&st1);
			{
				/* the per-thread CPU census: ms of CPU each thread used in this second (the kernel's run time). The PPU and APU workers
				 * are made inside the core, so for them the core's own busy time is given (marked b). 2 cores: the sum shows the load. */
				static SceUID s_main = sceKernelGetThreadId();
				static double prev[6] = { 0, 0, 0, 0, 0, 0 };
				const SceUID ids[6] = { s_main, g_athread, g_hd_ok ? g_hd_thread : -1, g_wthread, g_help_thread[0], g_help_thread[1] };
				double cur[6], d[6];
				for (int k = 0; k < 6; k++) { cur[k] = thread_cpu_ms(ids[k]); d[k] = cur[k] >= 0 && prev[k] >= 0 ? cur[k] - prev[k] : -1.0; prev[k] = cur[k]; }
				logf_("threads ms/s: main=%.0f audio=%.0f draw=%.0f writer=%.0f scale1=%.0f scale2=%.0f ppu_b=%.0f apu_b=%.0f wide_b=%.0f (wide frames %u, ppu waited for it %u) (draw core %d)\n", d[0], d[1], d[2], d[3], d[4], d[5],
				    (st1.ppu_busy_ns - st0.ppu_busy_ns) / 1e6, (st1.apu_busy_ns - st0.apu_busy_ns) / 1e6, (st1.wide_busy_ns - st0.wide_busy_ns) / 1e6,
				    (unsigned) (st1.wide_frames - st0.wide_frames), (unsigned) (st1.wide_centre_waits - st0.wide_centre_waits), g_draw_core);
			}
			double fr = frames_run ? frames_run : 1;
			logf_("t=%d fps=%.1f emu_fps=%.1f emu_ms=%.2f last_ms=%.2f aq=%u", secs, drawn / dt, frames_run / dt,
			    frames_run ? emu_us / 1000.0 / frames_run : 0.0, last_frame_us / 1000.0, g_pace.fill_frames());
			logf_(" | busy ms/fr: main=%.2f ppu=%.2f apu=%.2f | waits/s: ppu=%llu apu=%llu (%.1f ms) | main split ms/fr: emu=%.2f copy=%.2f gpu_submit=%.2f | drawn=%u skipped=%u not_rendered=%u new_pic=%u fence_skip=%u rt_reuse=%u",
			    main_us / 1000.0 / fr, (st1.ppu_busy_ns - st0.ppu_busy_ns) / 1e6 / fr, (st1.apu_busy_ns - st0.apu_busy_ns) / 1e6 / fr,
			    (unsigned long long) (st1.ppu_waits - st0.ppu_waits), (unsigned long long) (st1.apu_waits - st0.apu_waits),
			    ((st1.ppu_wait_ns - st0.ppu_wait_ns) + (st1.apu_wait_ns - st0.apu_wait_ns)) / 1e6,
			    emu_us / 1000.0 / fr, fetch_us / 1000.0 / fr, draw_us / 1000.0 / fr,
			    drawn, skipped, not_rendered, pic_fresh, g_fence_skips, g_rt_reused);
			const unsigned sc_blocks = g_scale_blocks.exchange(0), sc_bdone = g_scale_blocks_done.exchange(0), sc_bmoved = g_scale_blocks_moved.exchange(0);
			logf_(" | scaler=%s pics=%u ms/pic=%.2f nonflat/pic=%u helper_chunks=%u waits=%u computed=%u%% moved=%u%%", pk::scaler_name((pk::ScalerKind) g_scale_last.load()),
			    sc_pics, sc_pics ? sc_us / 1000.0 / sc_pics : 0.0, sc_pics ? sc_nonflat / sc_pics : 0, sc_help, sc_waits,
			    sc_blocks ? sc_bdone * 100 / sc_blocks : 0, sc_blocks ? sc_bmoved * 100 / sc_blocks : 0);
			{
				/* compose's line jobs (3 a frame), how many a helper joined, and the lines the helpers really did of all the jobs' lines */
				static unsigned la0, lh0;
				unsigned la = 0, lh = 0;
				snes_wide_line_counts(&la, &lh);
				logf_(" | wide_lines jobs=%u helped=%u lines=%u by_helpers=%u", g_lines_jobs.exchange(0), g_lines_helped.exchange(0), la - la0, lh - lh0);
				la0 = la; lh0 = lh;
			}
			logf_(" | per s: finish=%.1f ms vwait=%.1f ms | %s", finish_us / 1000.0, vwait_us / 1000.0, g_pl->pace_line().c_str());
			st0 = st1;
#ifdef S9X_PROF
			if (prof_frames) {
				double sum = 0;
				for (int c = 0; c < S9XP_N; c++) sum += prof_acc[c];
				logf_(" prof(%llu fr): cpu=%.0f%% apu=%.0f%% ppu=%.0f%% chip=%.0f%% front=%.0f%% ms/fr=%.2f",
				    (unsigned long long) prof_frames, 100 * prof_acc[0] / sum, 100 * prof_acc[1] / sum,
				    100 * prof_acc[2] / sum, 100 * prof_acc[3] / sum, 100 * prof_acc[4] / sum,
				    sum / 1e6 / prof_frames);
				memset(prof_acc, 0, sizeof prof_acc);
				prof_frames = 0;
			}
#endif
			logf_("\n");
			sec_start = now;
			vblanks = frames_run = 0;
			emu_us = main_us = fetch_us = draw_us = finish_us = vwait_us = 0;
			drawn = skipped = pic_fresh = not_rendered = 0;
			secs++;
		}

		/* one compact audio / pacing line a second, see snes/README.md "Reading the log" */
		if (stats_pending) {
			apace::Counters c;
			if (g_pace.poll(&c)) {
				stats_pending = false;
				logf_("A t=%d in=%u out=%u fill=%.1f/%.1f/%.1f ema=%.1f tgt=%.0f ratio=%+.0f/%+.0f/%+.0f base=%.4f ur=%u/%u drop=%u trim=%u"
				    " agap=%.1f w16=%u w20=%u w25=%u stall=%.1f wmax=%.1f %s\n", secs - 1, c.in_frames, c.out_frames, c.fill_ms_min, c.fill_ms_avg,
				    c.fill_ms_max, c.ema_ms, c.target_ms, c.ratio_ppm_min, c.ratio_ppm_avg, c.ratio_ppm_max, c.base_speed, c.underruns_sec,
				    c.underruns_total, c.dropped_sec, c.trims_sec, s_agap, s_w16, s_w20, s_w25, s_stall, s_work, audio_legacy ? "legacy" : "v2");
			}
		}
	}

	hd_stop();
	help_stop();
	g_quit = 1;
	{
		char c[220];
		g_cores->census_line(c, sizeof c);
		logf_("%s\n", c);
	}
	if (g_link) {
		/* how the link ended, before the session is destroyed (a guest's own game is put back then) */
		const bool done = g_link->ok();
		const int gerr = g_link->gblink_error();
		const snesnet::Fail f = g_link->failure();
		const uint32_t fr = g_link->frames();
		const std::string sum = g_link->summary();
		g_link->close();
		if (!g_link->restored_ok()) logf_("link: the machine could not be put back on this console's own game\n");
		logf_("%s\n", sum.c_str());
		g_pl->set_link_result(done ? "done" : "failed", gerr ? gerr : (int) f, g_link->gblink_error_name(), (int) fr);
		g_pl->set_link_peer_reason(g_link->peer_reason());
		link_net_down();
	}
	/* resume point, battery RAM, settings, log, launch/session.txt */
	g_pl->finish(why);
	writer_stop();
	if (g_athread >= 0) sceKernelWaitThreadEnd(g_athread, NULL, NULL);
	if (g_aport >= 0) sceAudioOutReleasePort(g_aport);
	snes_host_shutdown();
	if (why == pk::EXIT_NORMAL) {
		/* "Exit": close VitAdhoc, as the app's own Exit does */
		if (g_log) { fclose(g_log); g_log = NULL; }
		sceKernelExitProcess(0);
	}
	return_to_vitadhoc();
	return 0;
}
