/*
 * md.bin: the Sega program of VitAdhoc (Genesis Plus GX core, separate executable): Mega Drive / Genesis, Master System, Game Gear and
 * SG-1000 in one binary (console ids md, sms, gg, sg).
 *
 * Launched by VitAdhoc with sceAppMgrLoadExec("app0:md.bin", {rom path}) and a launch file (launch/<console>.ini). Quitting launches
 * app0:eboot.bin again. The player's behaviour (menu, states, saves, cheats, launch protocol, pacing, look governor) is player.cpp, shared
 * with the Linux twin; this file is the console around it: screen, sound, pads, touch, keyboard, clocks and threads.
 *
 * Threads (playerkit pk_cores, profile CP_MD): the emulating thread (the core is one thread) on core 0; the drawing thread (picture upload,
 * pixel art scaler, bands, present) on core 1; the audio thread and the writer thread on core 2 (core 3 when it is proven usable), and one or
 * two scaler helpers that take stripes of the scaler's work whenever they are scheduled (they are work stealers: the drawing thread
 * finishes whatever they have not taken).
 *
 * Licence: links only md/, playerkit/ and widekit/ code (the core's licence is non-commercial: nothing from gpsp/ may be linked into md.bin).
 * Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <atomic>
#include <stdarg.h>
#include <math.h>
#include <string>
#include <vector>

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
#include <vita2d.h>

#include "host.h"
#include "thread_ops.h"
#include "picture.h"
#include "audio_pace.h"
#include "player.h"
#include "netplay.h"
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include "pipe.h"
#include "wide.h"
#include "pal_pace.h"
#include "pk_palsmooth.h"
#include "pk_gfx_vita.h"
#include "pk_cores.h"
#include "pk_scale.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

extern "C" {
unsigned int sceUserMainThreadStackSize = 1024 * 1024;
unsigned int _newlib_heap_size_user = 56 * 1024 * 1024;     /* 40 MB before rewind: its 12 MB ring is on top */
}

#define APP_ROOT "ux0:data/GBAdhoc"
#define SCREEN_W 960
#define SCREEN_H 544

static mdp::MdPlayer *g_pl;
static mdp::PicturePipe g_pipe;      /* frame hand-over to the drawing thread (pipe.h); idle unless started */
/* The wide view (md/frontend/wide.h): after a drawn frame the emulating thread hands the frame to the analysis (frame(): on the VDP worker
 * thread when it runs, so the emulating thread does not pay for it), compose() runs on the drawing thread (or inline when the hand-over is
 * off) and waits for the analysis if it has not finished. The Meta of a picture in flight is in g_meta[seq & 7] (at most 4 pictures are in
 * flight). */
static mdw::Wide *g_wide;
static mdw::Meta g_meta[8];
static uint16_t *g_canvas;           /* the core's picture buffer when it draws beside the mailbox: MD_WIDE_PITCH wide, the picture at column MD_WIDE_MAX */
#define USER_WIDE 0x40000000         /* HandoffFrame.user: the picture has a Meta in g_meta[seq & 7] */
#define USER_MIX  0x10000000         /* ... Smooth 50 Hz: show the mean of the previous picture and this one (the drawing thread then shows this one alone) */
static std::atomic<unsigned> g_wide_compose_us(0), g_wide_composed(0), g_wide_helped(0);
/* the wide view's cost ladder: three rungs, by the smoothed time of the emulating thread's frame against the frame time. That time is the
 * emulation and the wait for the VDP worker at the end of the frame, and the worker draws the strips and analyses the frame before: a worker
 * that is too busy shows here. After the frame the emulating thread only hands the picture over, so nine tenths of the frame time is the line
 * (it was four fifths while the analysis ran on this thread). Rung 0 draws the real strips in full, 1 draws half of them, 2 draws none and
 * the picture beside the console's is the soft extension only. "On" in the menu stays on rung 0; Auto steps down at once and up slowly. */
static int g_wide_S, g_wide_rung, g_wide_hold, g_wide_applied = -1;
static double g_wide_ema;
static unsigned long long g_wide_an_us, g_wide_an_n;

static void wide_yield(void) { sceKernelDelayThread(100); }       /* the drawing thread waiting for an analysis that is still running */

static void wide_cost_update(double cost_us, double budget_us)
{
	g_wide_ema = g_wide_ema <= 0.0 ? cost_us : g_wide_ema * 0.9 + cost_us * 0.1;
	if (g_wide_hold > 0) g_wide_hold--;
	if (g_pl->wide_mode() == 1) { g_wide_rung = 0; return; }
	if (g_wide_ema > 0.90 * budget_us && g_wide_rung < 2) { g_wide_rung++; g_wide_hold = 240; }
	else if (g_wide_ema < 0.62 * budget_us && g_wide_rung > 0 && g_wide_hold == 0) { g_wide_rung--; g_wide_hold = 240; }
}

/* what the core draws beside the picture this frame; run once per refresh, before the frames */
static void wide_core_apply(void)
{
	const bool on = g_wide && g_pl->wide_active();
	const int M = on ? g_pl->wide_margin() : 0;
	const int sys = md_host_system();
	int S = 0, want = 0;
	if (on) {
		if (sys == MD_SYS_MD) { S = g_wide_rung == 0 ? M : g_wide_rung == 1 ? M / 2 : 0; want = S; }
		else want = M;                    /* the 8-bit consoles keep the records the analysis needs; their picture has no strips */
	}
	g_wide_S = S;
	if (want != g_wide_applied) {
		md_host_set_wide_margin(want);
		g_wide_applied = want;
		if (g_wide) g_wide->set_real_cap_default(sys == MD_SYS_MD ? S : (1 << 20));
		g_pl->logf("wide view: %s margin=%d strips=%d rung=%d\n", on ? "on" : "off", M, S, g_wide_rung);
	}
}
#define logf_(...) g_pl->logf(__VA_ARGS__)
static FILE *g_log;
static std::string g_data_root;       /* ux0:data/GBAdhoc/<console>, known once the launch file is read */

/* ---- audio ------------------------------------------------------------- */
/* The pacing itself (ring, resampler, rate control, underrun handling) is frontend/audio_pace.cpp, shared with the Linux twin's simulation.
 * The core resamples to 48 kHz itself, so the pacer's input and output rates are equal and it only does the rate control. */

static const int OUT_RATE = 48000;
static const int PRIO_AUDIO_DEFAULT = 0x40, PRIO_MAIN_DEFAULT = 0x60;
static int g_aport = -1;
static SceUID g_athread = -1;
static volatile int g_quit;
static apace::Pacer g_pace;
static std::atomic<unsigned> g_agap_max_us(0);

static pk::Cores *g_cores;
static pk::Plan g_core_plan;
static int g_wd_audio = -1, g_wd_writer = -1;     /* watchdog slots of threads that run on core 3 */

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
	cfg.in_rate = MD_AUDIO_RATE;
	cfg.out_rate = OUT_RATE;
	cfg.legacy = legacy;
	g_pace.init(cfg);
	g_aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, g_pace.grain(), OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
	if (g_aport < 0) { logf_("audio open failed %08x\n", g_aport); return; }
	int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume(g_aport, (SceAudioOutChannelFlag) (SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
	const pk::Slot *aslot = g_core_plan.find(pk::R_AUDIO_OUT);
	const unsigned amask = aslot ? aslot->mask : (unsigned) SCE_KERNEL_CPU_MASK_USER_2;
	if (aslot && aslot->on_core3) {
		g_wd_audio = g_cores->watchdog().add("md_audio");
		g_cores->watchdog().set_busy(g_wd_audio, true);
		g_cores->watchdog().watch(g_wd_audio, true);
		logf_("audio thread on the extra core (core 3)\n");
	}
	g_athread = sceKernelCreateThread("md_audio", audio_thread, prio, 0x4000, 0, amask, NULL);
	if (g_athread >= 0) sceKernelStartThread(g_athread, 0, NULL);
	logf_("audio: %s, grain %d frames at %d Hz, thread priority 0x%x (%s)\n", legacy ? "LEGACY" : "pacer v2",
	    g_pace.grain(), OUT_RATE, prio, g_athread >= 0 ? "started" : "NOT started");
}

static int g_vdp_mode;      /* threads.ini vdp_thread: 0 off, 1 Mega Drive (default), 2 all systems */

/* the roles of the Sega player (pk_cores.h, CP_MD): the emulating thread, the drawing thread, the sound thread, the writer, and the scaler helpers */
static pk::Request core4_request(void)
{
	pk::Request rq(pk::CP_MD);
	rq.with(pk::R_EMU_MAIN).with(pk::R_DRAW).with(pk::R_AUDIO_OUT).with(pk::R_WRITER).with(pk::R_UPSCALE);
	rq.upscale_n = 3;     /* the plan gives two helpers on three cores (core 2, and core 0 below the emulating thread), three on four */
	if (g_vdp_mode) rq.with(pk::R_RENDER);      /* the VDP line renderer thread (md/core/vdp_thread.c) */
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

static SceUID g_wthread = -1, g_hd_thread = -1, g_help_thread[3] = { -1, -1, -1 };
static SceUID g_disc_thread = -1;            /* the Sega CD read-ahead helper (md/core/cd_hw/cdio.c), while a disc is open */
/* A helper on core 0 (below the emulating thread's priority) only claims chunks while the emulating thread is between frames: a chunk it
 * claimed and was then preempted in would hold the drawing thread up for the rest of the emulated frame. */
static std::atomic<int> g_emu_busy(0);
static std::atomic<int> g_help_on_core0[3];

static void core4_fallback(void)
{
	g_core_plan = g_cores->plan(core4_request());
	const pk::Slot *a = g_core_plan.find(pk::R_AUDIO_OUT);
	const pk::Slot *w = g_core_plan.find(pk::R_WRITER);
	const int rc = a && g_athread >= 0 ? sceKernelChangeThreadCpuAffinityMask(g_athread, a->mask) : -1;
	const int rc2 = w && g_wthread >= 0 ? sceKernelChangeThreadCpuAffinityMask(g_wthread, w->mask) : -1;
	for (int i = 0; i < 3; i++) {
		const pk::Slot *h = g_core_plan.find(pk::R_UPSCALE, i);
		if (h && g_help_thread[i] >= 0) { sceKernelChangeThreadCpuAffinityMask(g_help_thread[i], h->mask); g_help_on_core0[i] = h->mask == (unsigned) SCE_KERNEL_CPU_MASK_USER_0; }
	}
	if (w && g_disc_thread >= 0) sceKernelChangeThreadCpuAffinityMask(g_disc_thread, w->on_core3 ? (unsigned) SCE_KERNEL_CPU_MASK_USER_0 : w->mask);       /* the disc helper shares the writer's place, but never on the system core */
	g_wd_audio = g_wd_writer = -1;
	char c[220];
	g_cores->census_line(c, sizeof c);
	logf_("threads back on the three-core layout (audio rc=%08x, writer rc=%08x)\n%s\n", rc, rc2, c);
}

/* ---- video ------------------------------------------------------------- */
/* The picture goes through the GPU: the core's frame is copied into a texture (through the Game Gear's LCD colour table when that is on),
 * optionally scaled by the CPU pixel art scaler (MMPX / xBR, playerkit pk_scale, on the drawing thread with up to three helpers taking
 * stripes) into a second texture, then
 *   1. (Sharp)  drawn with nearest sampling at an integer scale into a render target,
 *   2. (Glow)   every few frames shrunk twice (64x64, 16x16) into two tiny render targets,
 *   3. the screen: glow stretched over everything and darkened, the picture on top (bilinear from the prescaled target), scanline / grid
 *      bands, the bezel PNG.
 * A texture or target is only written again once the GPU has finished reading it (vita2d fragment fences). */

#define RT_SIZE 1024
#define SC_W 1024              /* the scaler's output texture: 640x448 (MMPX of a 320x224 picture) is the largest */
#define SC_H 576               /* 576: xBR 3x of a 192 row picture (Master System, SG-1000) */

struct GTex {
	vita2d_texture *t;
	vita2d_fragment_fence rd;
};

static GTex g_src[4];            /* the core's picture, 512x512 RGB565 */
static GTex g_scaled[3];         /* the scaler's picture */
static GTex g_rt, g_glowA, g_glowB;
static std::atomic<bool> g_pic_dirty(false);
static GTex g_bezel;
static int g_bezel_w, g_bezel_h;
static int g_src_i = -1;
static int g_src_w = 320, g_src_h = 224;
static pic_info g_src_info;               /* the geometry of the uploaded picture (the console's own, without the wide view's margin) */
static int g_src_margin;                  /* the uploaded picture is a wide view picture with this many columns of margin per side (0: the plain picture) */
static int g_sc_i = -1, g_sc_f = 0;       /* newest scaled texture and its factor (0: the picture is shown unscaled) */
static unsigned g_serial_seen, g_up_serial;
static int g_crop_top, g_crop_bot, g_crop_cand_top, g_crop_cand_bot, g_crop_votes;
static int g_have_picture;
static unsigned g_fence_skips;
static std::atomic<unsigned> g_scale_us(0), g_scale_pics(0), g_scale_nonflat(0), g_scale_helped(0), g_scale_waits(0), g_scale_blocks(0), g_scale_blocks_done(0), g_scale_blocks_moved(0);   /* the drawing thread's scaler, per second */
static std::atomic<int> g_scale_last(0);     /* the scaler of the last picture (pk::ScalerKind), for the log */
static const unsigned short *g_lcd;          /* the Game Gear's LCD colour table, built once before any thread */
static bool g_is_gg;

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

/* Bezel / overlay PNG: overlays/<game>.png, else overlays/<console id>.png (md, sms, gg, sg). Loaded once. */
static void bezel_load(const std::string &game, const std::string &console)
{
	std::string path = std::string(APP_ROOT "/overlays/") + game + ".png", data;
	if (!read_whole(path.c_str(), data)) {
		path = std::string(APP_ROOT "/overlays/") + console + ".png";
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

/* ---- the core's worker threads (md/core/thread_ops.h): the VDP line renderer ------------------------------------------------------------ */
struct WorkerSlot { SceUID th; void (*fn)(void *); void *arg; };
static WorkerSlot g_wk[MD_WORKERS] = { { -1, NULL, NULL }, { -1, NULL, NULL }, { -1, NULL, NULL } };
static int worker_entry(SceSize, void *argp)
{
	WorkerSlot *w = *(WorkerSlot **) argp;
	w->fn(w->arg);
	return sceKernelExitThread(0);
}
static void *wk_sem_create(void *) { const SceUID s = sceKernelCreateSema("md_worker", 0, 0, 0x7FFFFFFF, NULL); return s < 0 ? NULL : (void *) (intptr_t) s; }
static void wk_sem_destroy(void *, void *s) { if (s) sceKernelDeleteSema((SceUID) (intptr_t) s); }
static void wk_sem_post(void *, void *s) { sceKernelSignalSema((SceUID) (intptr_t) s, 1); }
static void wk_sem_wait(void *, void *s) { sceKernelWaitSema((SceUID) (intptr_t) s, 1, NULL); }
static int wk_spawn(void *, int id, void (*fn)(void *), void *arg)
{
	/* the VDP line renderer has its own slot; the Sega CD's read-ahead (cdio.c) is light and works for the emulating thread, so it takes the
	 * writer's place in the plan: core 2 (or the extra core) at priority 0xA0, below every thread that has frame work (the emulating thread, the
	 * drawing thread, the sound thread) */
	const pk::Slot *sl = g_core_plan.find(id == MD_WORKER_VDP ? pk::R_RENDER : id == MD_WORKER_DISC ? pk::R_WRITER : pk::R_APU);
	const int prio = sl ? sl->prio : id == MD_WORKER_DISC ? 0xA0 : 0x60;
	/* the disc helper never runs on the system core (core 3: the system can keep it for long stretches and nothing watches this thread): it takes core 0's
	 * gaps below the emulating thread, and the emulating thread's wait for it is bounded anyway (cdio.c take_ready) */
	unsigned mask = sl ? sl->mask : (unsigned) SCE_KERNEL_CPU_MASK_USER_1;
	if (id == MD_WORKER_DISC && sl && sl->on_core3) mask = (unsigned) SCE_KERNEL_CPU_MASK_USER_0;
	g_wk[id].fn = fn; g_wk[id].arg = arg;
	g_wk[id].th = sceKernelCreateThread(id == MD_WORKER_VDP ? "md_vdp" : id == MD_WORKER_DISC ? "md_disc" : "md_fm", worker_entry, prio, id == MD_WORKER_DISC ? 0x20000 : 0x10000, 0, mask, NULL);
	if (g_wk[id].th < 0) return -1;
	WorkerSlot *self = &g_wk[id];
	if (sceKernelStartThread(g_wk[id].th, sizeof self, &self) < 0) { sceKernelDeleteThread(g_wk[id].th); g_wk[id].th = -1; return -2; }
	if (id == MD_WORKER_DISC) g_disc_thread = g_wk[id].th;
	logf_("worker %d on mask %x priority 0x%x\n", id, mask, prio);
	return 0;
}
static void wk_wait_end(void *, int id)
{
	if (g_wk[id].th < 0) return;
	sceKernelWaitThreadEnd(g_wk[id].th, NULL, NULL);
	sceKernelDeleteThread(g_wk[id].th);
	g_wk[id].th = -1;
	if (id == MD_WORKER_DISC) g_disc_thread = -1;
}
static void wk_yield(void *) { sceKernelDelayThread(0); }
/* a real sleep: a thread that waits for a lower priority worker on its own core must not spin (a spin never lets that worker run) */
static void wk_nap(void *) { sceKernelDelayThread(100); }
static unsigned long long wk_now(void *) { return sceKernelGetProcessTimeWide(); }
static const md_thread_ops g_wk_ops = { NULL, wk_sem_create, wk_sem_destroy, wk_sem_post, wk_sem_wait, wk_spawn, wk_wait_end, wk_yield, wk_now, wk_nap };

/* ---- the scaler's helpers ------------------------------------------------------------------------------------------------------------
 * One job at a time, cut into chunks of rows. The drawing thread publishes the job (fields first, then `next` with the job's number in
 * its upper half), takes chunks itself, and waits until every chunk has been finished; a helper that is awake takes chunks the same way
 * (compare-and-swap on `next`, so a late helper can never take a chunk of a job it did not read). A helper that is not scheduled costs
 * nothing: the drawing thread has done its chunks. */
struct ScaleJob {
	pk::ScaleCache *cache;
	int kind;                        /* 0: chunks of the scaler; 1: lines of the wide view's compose (WIDE_CHUNK lines a chunk) */
	int lines;
	int nchunks;
	std::atomic<uint64_t> next;      /* (job << 32) | next chunk */
	std::atomic<int> done;
	std::atomic<unsigned> nonflat;
	std::atomic<int> by_helpers;
	ScaleJob() : cache(0), kind(0), lines(0), nchunks(0), next(0), done(0), nonflat(0), by_helpers(0) {}
};
static ScaleJob g_job;
static uint32_t g_job_no;
static SceUID g_help_sema[3] = { -1, -1, -1 };
static volatile int g_help_stop;
static int g_help_n;
static pk::ScaleScratch g_draw_scratch, g_help_scratch[3];

enum { WIDE_CHUNK = 32 };
/* take and run chunks of the current job; returns how many this thread ran. who: 0 the thread that owns the job, 1.. the helpers */
static int job_work(pk::ScaleScratch &sc, bool helper, bool low = false, int who = 0)
{
	int ran = 0;
	for (;;) {
		if (low && g_emu_busy.load(std::memory_order_relaxed)) return ran;
		uint64_t n = g_job.next.load(std::memory_order_acquire);
		pk::ScaleCache *cache = g_job.cache;
		const int kind = g_job.kind, lines = g_job.lines;
		const int nch = g_job.nchunks;
		const uint32_t c = (uint32_t) (n & 0xFFFFFFFFu);
		if ((kind == 0 && !cache) || (int) c >= nch) return ran;
		if (!g_job.next.compare_exchange_strong(n, n + 1, std::memory_order_acq_rel)) continue;
		if (kind == 1) {
			const int y0 = (int) c * WIDE_CHUNK, y1 = y0 + WIDE_CHUNK < lines ? y0 + WIDE_CHUNK : lines;
			if (g_wide && who < mdw::Wide::COMPOSE_WHO) g_wide->compose_lines(y0, y1, who);
		} else {
			const unsigned nf = cache->run_chunk((int) c, sc);
			g_job.nonflat.fetch_add(nf, std::memory_order_relaxed);
		}
		if (helper) g_job.by_helpers.fetch_add(1, std::memory_order_relaxed);
		g_job.done.fetch_add(1, std::memory_order_acq_rel);
		ran++;
	}
}

static int help_thread(SceSize, void *argp)
{
	const int idx = *(int *) argp;
	while (!g_help_stop) {
		sceKernelWaitSema(g_help_sema[idx], 1, NULL);
		if (g_help_stop) break;
		job_work(g_help_scratch[idx], true, g_help_on_core0[idx].load(std::memory_order_relaxed) != 0, idx + 1);
	}
	return sceKernelExitThread(0);
}

static void help_start(int n_wanted)
{
	static int idx[3] = { 0, 1, 2 };
	for (int i = 0; i < n_wanted && i < 3; i++) {
		const pk::Slot *s = g_core_plan.find(pk::R_UPSCALE, i);
		if (!s) break;
		char nm[16];
		snprintf(nm, sizeof nm, "md_scale%d", i);
		g_help_on_core0[i] = s->mask == (unsigned) SCE_KERNEL_CPU_MASK_USER_0;
		g_help_sema[i] = sceKernelCreateSema(nm, 0, 0, 1 << 16, NULL);
		if (g_help_sema[i] < 0) break;
		g_help_thread[i] = sceKernelCreateThread(nm, help_thread, s->prio, 0x10000, 0, s->mask, NULL);
		if (g_help_thread[i] >= 0 && sceKernelStartThread(g_help_thread[i], sizeof(int), &idx[i]) < 0) { sceKernelDeleteThread(g_help_thread[i]); g_help_thread[i] = -1; }
		if (g_help_thread[i] < 0) break;
		g_help_n++;
		logf_("scaler helper %d: core mask %05x priority 0x%x\n", i, s->mask, s->prio);
	}
	logf_("scaler helpers: %d\n", g_help_n);
}

static void help_stop(void)
{
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
		g_job.kind = 0;
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

/* ---- the picture: upload, scaler ---------------------------------------------------------------------------------------------------- */

struct Pic { const uint16_t *p; int w, h, pitch; unsigned serial; int margin; bool mix = false; };       /* margin > 0: a wide view picture (w = the console's width + 2 margin) */

/* geometry of the pictures in flight, by serial (the emulating thread fills it before the hand-over, the drawing thread reads it after) */
static pic_info g_geo[16];
static unsigned g_geo_serial = 0xFFFFFFFFu;

static pic_info geo_now(const md_video_info &vi) { return pic_info_from_video(&vi); }

static pk::ScalerKind scaler_kind_for(const pic_settings &ps, const pic_info &gi)
{
	const int e = pic_scaler_effective(&ps, pic_scaler_ceiling(gi.system, gi.w, gi.h), ps.tier_steps);
	switch (e) {
	case SCALER_MMPX2: return pk::SC_MMPX2;
	case SCALER_XBR2: return pk::SC_XBR2;
	case SCALER_XBR3: return pk::SC_XBR3;
	default: return pk::SC_NONE;
	}
}

/* Smooth 50 Hz (pk_palsmooth.h): the drawing side keeps the previous picture and, for a picture marked `mix`, shows the mean of the two */
static std::atomic<int> g_pal_mix_on(0);           /* the loop sets it while the mode is on: the previous picture is then kept */
static uint16_t *g_mix_prev, *g_mix_out;           /* 512 x 512 each, allocated at the first use */
static int g_mix_w, g_mix_h, g_mix_margin, g_mix_valid;
static pic_info g_mix_gi;

/* Copies the picture into a texture the GPU is done with, and scales it when the look says so. Returns 1 when the picture changed. */
static int video_upload(const pic_settings &ps, int frame_no, const Pic &s0, const pic_info &gi)
{
	Pic s = s0;
	int w = s.w, h = s.h;
	if (w > 512) w = 512;
	if (h > 512) h = 512;
	if (g_pal_mix_on.load(std::memory_order_relaxed)) {
		/* Smooth 50 Hz: keep this picture for the next mix; a picture marked mix is shown as the mean of the last one and itself */
		if (!g_mix_prev) { g_mix_prev = (uint16_t *) malloc(512 * 512 * 2); g_mix_out = (uint16_t *) malloc(512 * 512 * 2); g_mix_valid = 0; }
		if (g_mix_prev && g_mix_out) {
			const bool same = g_mix_valid && g_mix_w == w && g_mix_h == h && g_mix_margin == s.margin;
			if (s.mix && same) pk::mix16_half((uint8_t *) g_mix_out, w * 2, (const uint8_t *) g_mix_prev, w * 2, (const uint8_t *) s.p, s.pitch, w, h);
			for (int y = 0; y < h; y++) memcpy(g_mix_prev + (size_t) y * w, (const uint8_t *) s.p + (size_t) y * s.pitch, (size_t) w * 2);
			g_mix_w = w; g_mix_h = h; g_mix_margin = s.margin; g_mix_gi = gi; g_mix_valid = 1;
			if (s.mix && same) { s.p = g_mix_out; s.pitch = w * 2; }
		}
	} else g_mix_valid = 0;
	if (ps.shape == PIC_ZOOM && frame_no % 20 == 0) {
		int mx = h / 8;
		int t = pic_blank_rows(s.p, s.pitch, w, h, 1, mx), b = pic_blank_rows(s.p, s.pitch, w, h, 0, mx);
		if (t == g_crop_cand_top && b == g_crop_cand_bot) g_crop_votes++; else { g_crop_cand_top = t; g_crop_cand_bot = b; g_crop_votes = 0; }
		if (g_crop_votes >= 2) { g_crop_top = t; g_crop_bot = b; }
		else { if (t < g_crop_top) g_crop_top = t; if (b < g_crop_bot) g_crop_bot = b; }
	}
	/* the scaler reads CPU memory (the mailbox's copy, or the Game Gear's mapped copy), never the texture */
	static uint16_t lcdbuf[256 * 160];
	const uint16_t *scaler_src = s.p;
	int scaler_pitch_px = s.pitch / 2;
	const bool lcd = g_is_gg && g_lcd && ps.gg_colors && w <= 256 && h <= 160;
	if (lcd) {
		for (int y = 0; y < h; y++) {
			const uint16_t *row = (const uint16_t *) ((const uint8_t *) s.p + (size_t) y * s.pitch);
			uint16_t *o = lcdbuf + (size_t) y * w;
			for (int x = 0; x < w; x++) o[x] = g_lcd[row[x]];
		}
		scaler_src = lcdbuf; scaler_pitch_px = w;
	}

	/* the pixel art scaler: the look's choice for this picture, into a scaled texture the GPU is done with */
	g_sc_f = 0;
	const pk::ScalerKind kind = (g_pl->look_mode() == 2) ? pk::SC_NONE : scaler_kind_for(ps, gi);
	g_scale_last.store((int) kind, std::memory_order_relaxed);
	if (kind != pk::SC_NONE && w == gi.w + 2 * s.margin && h == gi.h) {
		const int f = pk::scaler_factor(kind);
		int si = -1;
		for (int n = 1; n <= 3; n++) {
			int c = (g_sc_i + n + 3) % 3;
			if (gtex_writable(g_scaled[c])) { si = c; break; }
		}
		if (si >= 0 && w * f <= SC_W && h * f <= SC_H) {
			vita2d_texture *st = g_scaled[si].t;
			const uint64_t t0 = sceKernelGetProcessTimeWide();
			const unsigned nf = scale_picture(kind, scaler_src, w, h, scaler_pitch_px, (uint16_t *) vita2d_texture_get_datap(st),
			    (int) (vita2d_texture_get_stride(st) / 2), g_scaled_sync[si]);
			g_scale_us.fetch_add((unsigned) (sceKernelGetProcessTimeWide() - t0));
			g_scale_pics.fetch_add(1);
			g_scale_nonflat.fetch_add(nf);
			g_sc_i = si; g_sc_f = f;
		} else g_fence_skips++;
	}
	/* The source texture is only needed when the picture is shown unscaled (the glow samples the scaled picture when there is one): with a
	 * scaler on, its 143 KB copy is skipped. The first picture always makes one, so there is a texture to point at. */
	if (g_sc_f == 0 || g_src_i < 0) {
		int idx = -1;
		for (int n = 1; n <= 4; n++) {
			int c = (g_src_i + n + 4) % 4;
			if (gtex_writable(g_src[c])) { idx = c; break; }
		}
		if (idx < 0) { g_fence_skips++; return 0; }          /* the GPU is behind: show the old picture again */
		vita2d_texture *tx = g_src[idx].t;
		uint8_t *dst = (uint8_t *) vita2d_texture_get_datap(tx);
		const unsigned dpitch = vita2d_texture_get_stride(tx);
		for (int y = 0; y < h; y++)
			memcpy(dst + (size_t) y * dpitch, (const uint8_t *) scaler_src + (size_t) y * scaler_pitch_px * 2, (size_t) w * 2);
		g_src_i = idx;
	}
	g_serial_seen = ++g_up_serial;
	if (w != g_src_w || h != g_src_h || s.margin != g_src_margin) { g_crop_top = g_crop_bot = g_crop_cand_top = g_crop_cand_bot = 0; }
	g_src_w = w; g_src_h = h; g_src_info = gi; g_src_margin = s.margin;
	g_have_picture = 1;
	return 1;
}

static void filt(vita2d_texture *t, SceGxmTextureFilter f) { vita2d_texture_set_filters(t, f, f); }

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


/* Opens the frame's last scene and draws the picture. Returns false when no scene could be started (the display queue is full): the caller
 * then skips this frame. `extras` = 0 draws without the picture options (not used by the game loop: the governor never drops them). */
static bool frame_draw(const pic_settings &ps, int extras, int frame_no)
{
	(void) frame_no;
	if (g_pic_dirty.exchange(false)) { g_rtkey_valid = false; g_glow_serial = 0; g_glow_age = 99; }
	vita2d_pool_reset();
	pic_rect r;                  /* in the core's own picture pixels: the layout, the scanlines, the grid, the glow */
	pic_rect rs;                 /* the same in the pixels of the picture that is drawn (the scaler's output is g_sc_f times bigger) */
	memset(&r, 0, sizeof r);
	GTex *src = g_src_i >= 0 ? &g_src[g_src_i] : NULL;
	GTex *img = src;
	int F = 1;
	if (src && g_have_picture && g_sc_f > 0 && g_sc_i >= 0 && g_scaled[g_sc_i].t) { img = &g_scaled[g_sc_i]; F = g_sc_f; }
	if (src && g_have_picture) {
		if (g_src_margin > 0) pic_geometry_wide(&ps, &g_src_info, g_src_w, g_crop_top, g_crop_bot, SCREEN_W, SCREEN_H, &r);
		else pic_geometry(&ps, &g_src_info, g_crop_top, g_crop_bot, SCREEN_W, SCREEN_H, &r);
		rs = r;
		rs.sx *= F; rs.sy *= F; rs.sw *= F; rs.sh *= F;
	}
	const bool bezel = g_bezel.t != NULL;
	int kx = 1, ky = 1;
	bool use_rt = false, exact = false;
	if (src && g_have_picture) {
		pic_prescale(&rs, &kx, &ky);
		exact = fabsf(rs.dw - rs.sw * kx) < 0.5f && fabsf(rs.dh - rs.sh * ky) < 0.5f && kx == ky;
		use_rt = extras && ps.quality == PIC_SHARP && !exact && g_rt.t != NULL && (kx > 1 || ky > 1);
	}

	/* scene 1: glow, a few times a second (from the core's own picture: it is a blur) */
	bool glow_on = extras && src && g_have_picture && !bezel && ps.glow > 0 && ps.shape != PIC_FULL && g_glowA.t && g_glowB.t;
	if (glow_on && (++g_glow_age >= 4 || g_glow_serial == 0) && g_serial_seen != g_glow_serial &&
	    gtex_writable(g_glowA) && gtex_writable(g_glowB)) {
		filt(img->t, SCE_GXM_TEXTURE_FILTER_LINEAR);
		if (rt_scene(g_glowA)) {
			gtex_read(*img);
			draw_into_rt(img->t, 64, 64, rs.sx, rs.sy, rs.sw, rs.sh, 64, 64);
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
	/* scene 2: the sharp-bilinear prescale (of the scaler's picture when there is one) */
	if (use_rt) {
		RtKey key;
		memset(&key, 0, sizeof key);
		key.idx = (img == src ? g_src_i : 8 + g_sc_i); key.serial = g_serial_seen; key.kx = kx; key.ky = ky;
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
		/* CRT scanlines and LCD grid: dark bands between console lines / pixels, drawn as alpha rectangles */
		if (extras && (ps.scan > 0 || ps.grid > 0)) {
			float period = r.dh / r.logical_rows;
			int rows = r.logical_rows;
			float th = period * (0.18f + 0.03f * (ps.scan ? ps.scan : ps.grid));
			if (th > period * 0.5f) th = period * 0.5f;
			if (th < 1.0f) th = 1.0f;
			unsigned col = band_alpha(ps.scan ? ps.scan : ps.grid) << 24;
			rect_batch_begin(rows);
			for (int i = 1; i < rows; i++) rect_batch_add(r.dx, r.dy + i * period - th * 0.5f, r.dw, th);
			rect_batch_draw(col);
			if (ps.grid > 0 && r.sw > 1) {
				float cp = r.dw / r.sw;
				float tw = cp * (0.15f + 0.02f * ps.grid);
				if (tw < 1.0f) tw = 1.0f;
				unsigned gcol = band_alpha(ps.grid) << 24;
				rect_batch_begin(r.sw);
				for (int i = 1; i < r.sw; i++) rect_batch_add(r.dx + i * cp - tw * 0.5f, r.dy, tw, r.dh);
				rect_batch_draw(gcol);
			}
		}
	}
	if (bezel) vita2d_draw_texture_scale(g_bezel.t, 0, 0, (float) SCREEN_W / g_bezel_w, (float) SCREEN_H / g_bezel_h);
	return true;
}

static void frame_finish(void)
{
	vita2d_end_drawing();
	vita2d_swap_buffers();
}

/* ---- input ------------------------------------------------------------- */
/* Player 1 is the Vita's own controls (port 0). Pad 2 is port 2 when a controller is paired there, else port 1 on a handheld (external
 * pads through ds34vita and the like); on a PS TV port 1 is the same pad as port 0, so it is not used. The Game Gear has one pad. The ports
 * are looked at once a second. Mapping, sticks, turbo, shortcuts and the menu chord are playerkit's Mapper (player.cpp). */

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

static SceUID g_wsema = -1;
static volatile int g_wstop;
static bool g_ime_wanted;

static void log_open(void)
{
	if (g_log) return;
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(APP_ROOT, 0777);
	std::string d = std::string(APP_ROOT "/") + g_pl->console();
	sceIoMkdir(d.c_str(), 0777);
	g_data_root = d;
	g_log = fopen((d + "/log.txt").c_str(), "a");
}

struct VitaPlatform : mdp::Platform {
	uint64_t now_us() { return sceKernelGetProcessTimeWide(); }
	void pause_us(unsigned us) { sceKernelDelayThread(us); }
	bool wake_writer()
	{
		if (g_wthread < 0) return false;
		sceKernelSignalSema(g_wsema, 1);
		return true;
	}
	/* the newest picture, straight from its texture (read only while the drawing thread is idle and no new picture is fetched) */
	bool current_frame(pk::Frame *f)
	{
		if (g_pipe.running()) g_pipe.box().flush(300000);
		if (!g_have_picture || g_src_i < 0 || !g_src[g_src_i].t) return false;
		vita2d_texture *t = g_src[g_src_i].t;
		/* a wide view picture: the state's picture is the console's own, the columns in the middle */
		*f = pk::Frame((const uint16_t *) vita2d_texture_get_datap(t) + g_src_margin, g_src_w - 2 * g_src_margin, g_src_h, (int) vita2d_texture_get_stride(t));
		return true;
	}
	void ask_cheat_text() { g_ime_wanted = true; }
	void picture_changed() { g_pic_dirty.store(true); }
	void log_write(const std::string &chunk)
	{
		log_open();
		if (!g_log) return;
		fwrite(chunk.data(), 1, chunk.size(), g_log);
		fflush(g_log);
	}
};
static VitaPlatform g_platform;

/* the writer thread: battery RAM, states, the resume point, cheats, settings and the log; low priority, never in the game thread's way */
static int writer_thread(SceSize, void *)
{
	for (;;) {
		if (g_wd_writer >= 0) g_cores->watchdog().set_busy(g_wd_writer, false);
		sceKernelWaitSema(g_wsema, 1, NULL);
		if (g_wstop) break;
		if (g_wd_writer >= 0) g_cores->watchdog().set_busy(g_wd_writer, true);
		while (g_pl->writer().work_one(g_pl)) { if (g_wd_writer >= 0) g_cores->watchdog().beat(g_wd_writer); }
		if (g_wd_writer >= 0) g_cores->watchdog().beat(g_wd_writer);
	}
	return sceKernelExitThread(0);
}

static void writer_start(void)
{
	g_wsema = sceKernelCreateSema("md_writer", 0, 0, 1 << 20, NULL);
	if (g_wsema < 0) return;
	const pk::Slot *s = g_core_plan.find(pk::R_WRITER);
	const unsigned mask = s ? s->mask : (unsigned) SCE_KERNEL_CPU_MASK_USER_2;
	const int prio = s ? s->prio : 0xA0;
	g_wthread = sceKernelCreateThread("md_writer", writer_thread, prio, 0x10000, 0, mask, NULL);
	if (g_wthread >= 0 && sceKernelStartThread(g_wthread, 0, NULL) < 0) { sceKernelDeleteThread(g_wthread); g_wthread = -1; }
	if (g_wthread >= 0 && s && s->on_core3) {
		g_wd_writer = g_cores->watchdog().add("md_writer");
		g_cores->watchdog().watch(g_wd_writer, true);
	}
	logf_("writer thread %s (mask %05x, priority 0x%x)\n", g_wthread >= 0 ? "started" : "NOT started (writes run on the game thread)", mask, prio);
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

static pk::VitaGfx *g_gfx;
static pk::Text *g_text;

static int ini_int(const char *path, const char *key, int def);
static int prio_ok(int p, int def);

/* ---- frame hand-over ------------------------------------------------------------------------------
 * "Smooth hand-over" (Speed page): the emulating thread copies each finished picture into playerkit's mailbox (pipe.h) and goes on; the
 * drawing thread uploads it, scales it, draws it (bands, bezel, notices) and presents it. The main thread owns the display only while the
 * drawing thread is idle: flush() before the menu, before a state picture is read, and at the end. See playerkit/src/pk_handoff.h. */
struct VitaHandoffSys : pk::HandoffSys {
	uint64_t now_us() { return sceKernelGetProcessTimeWide(); }
	void *sem_create()
	{
		SceUID id = sceKernelCreateSema("md_hd", 0, 0, 1 << 16, NULL);
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

static double thread_cpu_ms(SceUID id)
{
	SceKernelThreadInfo ti;
	memset(&ti, 0, sizeof ti);
	ti.size = sizeof ti;
	if (id < 0 || sceKernelGetThreadInfo(id, &ti) < 0) return -1.0;
	return ti.runClocks / 1000.0;
}
static bool g_hd_ok;
static std::atomic<unsigned> g_hd_drawn(0), g_hd_failed(0), g_hd_draw_us(0);
static std::atomic<unsigned> g_hd_cum_us(0), g_hd_cum_n(0);       /* never drained: the draw time and the pictures finished in total (Quick response's picture check takes deltas) */
static pk::SpinLock g_hd_lk;
static pk::Notices g_hd_notices;
static std::atomic<int> g_hd_notice_on(0);
static int g_draw_core = 1;

static void hd_publish_notices(void)
{
	const bool on = g_pl->notices().active();
	if (!on && !g_hd_notice_on.load()) return;
	pk::SpinGuard g(g_hd_lk);
	g_hd_notices = g_pl->notices();
	g_hd_notice_on = on ? 1 : 0;
}

/* The picture to show for a frame that has a Meta: the wide view's composed picture, or the console's own when it cannot be made. */
static void wide_pic(mdw::Meta &m, const uint16_t *view0, int pitch_bytes, Pic *s)
{
	int ww = 0, wh = 0;
	const uint64_t c0 = sceKernelGetProcessTimeWide();
	/* the lines of the compose are shared with the scaler's helpers, the way the scaler's chunks are (a helper that is not scheduled costs
	 * nothing: this thread does every chunk nobody took) */
	const uint16_t *wp = NULL;
	const int nl = g_wide ? g_wide->compose_begin(m, view0, pitch_bytes / 2, &ww, &wh) : 0;
	if (nl > 0) {
		g_job.nchunks = 0;
		g_job.cache = 0;
		g_job.kind = 1; g_job.lines = nl;
		g_job.done.store(0); g_job.by_helpers.store(0);
		g_job.nchunks = (nl + WIDE_CHUNK - 1) / WIDE_CHUNK;
		g_job.next.store((uint64_t) (++g_job_no) << 32, std::memory_order_release);
		for (int i = 0; i < g_help_n; i++) sceKernelSignalSema(g_help_sema[i], 1);
		job_work(g_draw_scratch, false);
		while (g_job.done.load(std::memory_order_acquire) < g_job.nchunks) sceKernelDelayThread(20);
		g_wide_helped.fetch_add((unsigned) g_job.by_helpers.load());
		g_job.nchunks = 0;
		g_job.kind = 0;
		wp = g_wide->compose_end();
	}
	g_wide_compose_us.fetch_add((unsigned) (sceKernelGetProcessTimeWide() - c0));
	g_wide_composed.fetch_add(1);
	if (wp) { s->p = wp; s->w = ww; s->h = wh; s->pitch = ww * 2; s->margin = m.margin; }
	else { s->p = view0; s->w = m.vw; s->h = m.vh; s->pitch = pitch_bytes; s->margin = 0; }
}

/* The picture that was last shown mixed, as itself (it is what the mix kept): the refresh after the early frame shows it */
static bool pal_pure_pic(Pic *s, pic_info *gi)
{
	if (!g_mix_prev || !g_mix_valid) return false;
	s->p = g_mix_prev; s->w = g_mix_w; s->h = g_mix_h; s->pitch = g_mix_w * 2; s->serial = 0; s->margin = g_mix_margin; s->mix = false;
	*gi = g_mix_gi;
	return true;
}

static int hd_draw_thread(SceSize, void *)
{
	for (;;) {
		pk::HandoffFrame *f = g_pipe.box().take();
		if (!f) break;
		const uint64_t t0 = sceKernelGetProcessTimeWide();
		Pic s;
		s.p = (const uint16_t *) (f->data + f->offset); s.w = f->w; s.h = f->h; s.pitch = f->pitch; s.serial = (unsigned) (f->user & 0x0FFFFFFF); s.margin = 0; s.mix = (f->user & USER_MIX) != 0;
		if (f->user & USER_WIDE) { mdw::Meta &m = g_meta[f->seq & 7]; wide_pic(m, s.p + m.view_off, f->pitch, &s); }
		const pic_info gi = g_geo[s.serial & 15];
		const pic_settings ps = g_pl->picture();     /* one copy for this picture: the game thread may ease the look meanwhile */
		video_upload(ps, (int) f->seq, s, gi);
		bool drew = false;
		for (int tries = 0; tries < 4 && !drew; tries++) {
			if (tries) sceDisplayWaitVblankStart();
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
		g_hd_cum_us += (unsigned) dt;
		g_hd_cum_n += 1;
		if (drew && s.mix && g_mix_valid) {
			/* Smooth 50 Hz: this picture was the early frame's, shown mixed for one refresh; the next refresh shows it alone */
				Pic pure; pic_info pgi;
				if (pal_pure_pic(&pure, &pgi) && video_upload(ps, (int) f->seq, pure, pgi)) {
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

/* Quick response (run-ahead): the wide view's analysis has to read the machine while it is still the one of the picture just drawn, so
 * the player calls this between the look-ahead frame and the way back (md_host_run_frame_ahead). The loop fills the request first. */
static struct { bool armed, done; uint32_t frame; int margin, S; mdw::Meta *mt; md_video_info vi; uint64_t us; } g_ahead_an;
static void ahead_shown_hook(void *)
{
	if (!g_ahead_an.armed || !g_wide) return;
	const uint64_t a0 = sceKernelGetProcessTimeWide();
	md_host_video_info(&g_ahead_an.vi);
	g_wide->frame(g_ahead_an.frame, g_ahead_an.vi, g_ahead_an.margin, g_ahead_an.S, g_ahead_an.mt);
	g_ahead_an.us = sceKernelGetProcessTimeWide() - a0;
	g_ahead_an.done = true;
}

static void hd_start(const char *tini, double frame_hz)
{
	if (!g_pl->smooth_handoff()) { logf_("handoff: off (setting)\n"); return; }
	if (!g_pipe.start(&g_hd_sys, 2, true, 16683, (uint32_t) (1e6 / frame_hz + 0.5))) { logf_("handoff: NOT started (mailbox)\n"); return; }
	const pk::Slot *s = g_core_plan.find(pk::R_DRAW);
	const unsigned mask = s ? s->mask : (unsigned) SCE_KERNEL_CPU_MASK_USER_1;
	const int prio = prio_ok(ini_int(tini, "draw_prio", 0), 0) ? prio_ok(ini_int(tini, "draw_prio", 0), 0) : (s ? s->prio : 0x60);
	g_draw_core = mask == SCE_KERNEL_CPU_MASK_USER_0 ? 0 : mask == SCE_KERNEL_CPU_MASK_USER_2 ? 2 : 1;
	g_hd_thread = sceKernelCreateThread("md_draw", hd_draw_thread, prio, 0x20000, 0, mask, NULL);
	if (g_hd_thread >= 0 && sceKernelStartThread(g_hd_thread, 0, NULL) < 0) { sceKernelDeleteThread(g_hd_thread); g_hd_thread = -1; }
	g_hd_ok = g_hd_thread >= 0;
	if (!g_hd_ok) g_pipe.stop();
	logf_("handoff: %s, draw thread on core %d priority 0x%x, depth 2 (adaptive 2/1)\n", g_hd_ok ? "on" : "NOT started (thread)", g_draw_core, prio);
}

static bool hd_quiesce(void)
{
	if (!g_hd_ok) return true;
	const bool ok = g_pipe.box().flush(1000000);
	if (!ok) logf_("handoff: the drawing thread did not go idle in 1 s\n");
	return ok;
}

static void hd_after_menu(void)
{
	if (!g_hd_ok) return;
	g_pipe.box().set_enabled(g_pl->smooth_handoff());
	g_pipe.box().time_baseline();
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
	const char *argv[] = { "--from-md", NULL };   /* arguments only: the system adds the path */
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

static mdnet::Link *g_link;
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

static uint64_t link_now_us(void) { return sceKernelGetProcessTimeWide(); }
static void link_random(uint8_t *out, int n) { sceKernelGetRandomNumber(out, (SceSize) n); }
static void link_log(const char *line) { g_pl->logf("link: %s\n", line); }
static int link_commit(const uint8_t *d, size_t n) { return g_pl->link_commit_save(d, n); }

/* Connecting, waiting, the session's set-up: a plain screen with two lines and the way out. Returns true when the player asked to leave. */
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

static pk::MenuEvent menu_loop(int player)
{
	g_pace.set_paused(true);
	g_pl->open_menu(player);
	bool first = true;
	uint64_t prev = sceKernelGetProcessTimeWide();
	pk::MenuEvent ev = pk::MENU_NONE;
	for (;;) {
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
			std::string code, name, stitle;
			if (g_pl->search_value_wanted(&stitle)) {
				/* Search memory: a number (the same keyboard) */
				bool sok = ime_ask(stitle.c_str(), "", code);
				g_pl->search_value_entered(sok, code);
			} else {
			bool ok = ime_ask("Cheat code (Game Genie, Pro Action Replay or address:value)", "", code);
			if (ok && !code.empty()) ime_ask("A name for it (you can leave it empty)", "", name);
			g_pl->cheat_entered(ok, code, name);
			}
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
	g_pl = new mdp::MdPlayer(&g_platform);
	logf_("---- md.bin start (argc=%d)\n", argc);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
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
	SceUID pcb = sceKernelCreateCallback("md_power", 0, power_cb, NULL);
	if (pcb >= 0) scePowerRegisterCallback(pcb);

	vita2d_init();
	vita2d_set_clear_color(0xFF000000);
	g_gfx = new pk::VitaGfx();
	g_text = new pk::Text(g_gfx);
	pk::scale_init();
	video_init();

	/* the plan of the threads (threads.ini: core4=auto|off, audio_prio, main_prio, draw_prio, helpers=0..3, audio_legacy) */
	const char *tini = APP_ROOT "/md/threads.ini";
	g_vdp_mode = clampi_(ini_int(tini, "vdp_thread", 1), 0, 2);
	core4_start(tini);
	const int audio_prio = prio_ok(ini_int(tini, "audio_prio", PRIO_AUDIO_DEFAULT), PRIO_AUDIO_DEFAULT);
	const pk::Slot *mslot = g_core_plan.find(pk::R_EMU_MAIN);
	const int main_prio = prio_ok(ini_int(tini, "main_prio", mslot ? mslot->prio : PRIO_MAIN_DEFAULT), PRIO_MAIN_DEFAULT);
	const bool audio_legacy = ini_int(tini, "audio_legacy", 0) != 0;
	{
		int rc = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mslot ? mslot->mask : (unsigned) SCE_KERNEL_CPU_MASK_USER_0);
		logf_("main thread to its core rc=%08x\n", rc);
	}
	if (main_prio > 0) {
		int rc = sceKernelChangeThreadPriority(0, main_prio);
		logf_("main thread priority 0x%x rc=%08x now 0x%x\n", main_prio, rc, sceKernelGetThreadCurrentPriority());
	}
	md_host_set_log([](const char *line) { logf_("%s\n", line); });
	if (md_host_init() != 0) { fatal_screen("The Sega core could not start.", "Not enough memory."); return_to_vitadhoc(); }
	/* the VDP line renderer (when the plan has a place for it) and, for a Sega CD game, the disc read-ahead helper: the thread starts once a disc is
	 * loaded (cdio_begin), a cartridge never starts it */
	md_host_set_threads(&g_wk_ops, (g_vdp_mode && g_core_plan.find(pk::R_RENDER) ? (MD_THREAD_VDP | (g_vdp_mode == 2 ? MD_THREAD_VDP_SMS : 0u)) : 0u) | MD_THREAD_DISC);

	/* the game: launch file or argument, patch, saves, states, settings (player.cpp) */
	writer_start();
	mdp::StartResult sr = g_pl->start(argc, argv, APP_ROOT, enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS);
	g_pl->set_ahead_hook(ahead_shown_hook, NULL);
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
		mdnet::Env env;
		env.now_us = link_now_us;
		env.random = link_random;
		env.log = link_log;
		env.local_ip_be = 0;
		env.commit_save = link_commit;
		g_link = new mdnet::Link;
		bool ok = link_net_up();
		if (!ok) logf_("link: the network could not start\n");
		if (ok) ok = g_link->open(g_pl->request().link, g_pl->link_rom(), env);
		if (!ok) {
			const mdnet::Fail f = g_link->failure() != mdnet::F_NONE ? g_link->failure() : mdnet::F_NO_NETWORK;
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
	g_is_gg = md_host_system() == MD_SYS_GG;
	if (g_is_gg) g_lcd = pic_lcd_table();
	g_wide = new mdw::Wide;
	mdw::Wide::set_yield(wide_yield);
	g_canvas = (uint16_t *) calloc((size_t) MD_WIDE_PITCH * MD_MAX_HEIGHT, 2);
	if (!g_canvas) { delete g_wide; g_wide = NULL; }           /* built once, before any other thread needs it */

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
	bezel_load(g_pl->game_name(), g_pl->console());
	md_host_set_audio_rate(OUT_RATE);
	audio_init(audio_legacy, audio_prio > 0 ? audio_prio : PRIO_AUDIO_DEFAULT);
	g_pl->apply_start_mode();
	{	/* census: the memory bus, one 1 MB copy (cached to cached) at start; it is what the picture copies cost on this console */
		const size_t n = 1u << 20;
		unsigned char *s = (unsigned char *) malloc(n), *d = (unsigned char *) malloc(n);
		if (s && d) {
			memset(s, 0x5A, n); memcpy(d, s, n);
			const uint64_t t0 = sceKernelGetProcessTimeWide();
			for (int i = 0; i < 4; i++) { memcpy(d, s, n); s[i] ^= d[i * 977]; }
			const uint64_t us = sceKernelGetProcessTimeWide() - t0;
			logf_("bus memcpy_mb_s cached_to_cached=%.0f\n", us ? 4.0 * 1e6 / (double) us : 0.0);
		}
		free(s); free(d);
	}
	help_start(clampi_(ini_int(tini, "helpers", 3), 0, 3));
	g_pl->look_start(scePowerGetArmClockFrequency());   /* the look governor: the best look first, the game's memory of what it needed */

	double frame_hz = md_host_fps();
	const double vita_hz = 59.94;
	bool pal = frame_hz < 55.0;
	pk::PalSmooth pal_pace;
	bool pal_pure_pending = false;      /* Smooth 50 Hz: the early frame was shown mixed; the refresh after it shows the picture alone */
	if (pal) { pal_pace.set(frame_hz, vita_hz); logf_("PAL: %.3f Hz, %s\n", frame_hz, g_pl->pal_fast() ? "Fast 60 Hz chosen: one frame per refresh" : g_pl->pal_smooth() ? "Smooth 50 Hz: 5 of 6 refreshes emulate a frame, the sixth's frame comes one refresh early and is mixed with the one before" : "True 50 Hz: 5 of 6 refreshes emulate a frame"); }
	hd_start(tini, frame_hz);

	/* log state */
	uint64_t sec_start = sceKernelGetProcessTimeWide();
	int frames_run = 0, vblanks = 0, secs = 0;
	uint64_t emu_us = 0, main_us = 0, fetch_us = 0, draw_us = 0, finish_us = 0, vwait_us = 0;
	uint64_t last_frame_us = 0;
	unsigned drawn = 0, skipped = 0, pic_fresh = 0, not_rendered = 0;
	unsigned last_vc = sceDisplayGetVcount();
	apace::SpeedMeter speed_meter(8);
	bool stats_pending = false;
	uint32_t ur_seen = 0;
	uint64_t prev_loop0 = 0;
	unsigned w16 = 0, w20 = 0, w25 = 0;
	double hd_draw_ms = 0;
	unsigned hd_repeated = 0, missed_refr = 0, pal_planned = 0;
	uint64_t stall_max_us = 0, work_max_us = 0;
	unsigned s_w16 = 0, s_w20 = 0, s_w25 = 0;
	double s_stall = 0, s_work = 0, s_agap = 0;
	uint32_t emu_frame = 0;
	uint64_t render_emu_us = 0;
	/* Quick response's picture check: baselines of the drawing thread's totals, taken again after the menu and when the hand-over toggles */
	pk::PictureTotals pp;
	bool pp_hd = false, pp_rebase = true;
	unsigned wide_jumps_seen = 0;
	pk::ExitReason why = pk::EXIT_NORMAL;
	g_pl->log_flush(false);

	for (;;) {
		/* One pass per display refresh, counted by the display itself */
		unsigned vc = sceDisplayGetVcount();
		if (vc == last_vc) {
			uint64_t w0 = sceKernelGetProcessTimeWide();
			sceDisplayWaitVblankStart();
			vc = sceDisplayGetVcount();
			vwait_us += sceKernelGetProcessTimeWide() - w0;
		}
		unsigned elapsed = vc - last_vc;
		last_vc = vc;
		if (elapsed > 1) missed_refr += elapsed - 1;
		if (elapsed == 0) elapsed = 1;
		if (elapsed > 2) elapsed = 2;
		uint64_t loop0 = sceKernelGetProcessTimeWide();
		const uint64_t emu_us0 = emu_us;       /* the refresh's emulation time is the difference at its end (Unlimited fast forward's budget) */
		if (prev_loop0 && loop0 - prev_loop0 > stall_max_us) stall_max_us = loop0 - prev_loop0;
		prev_loop0 = loop0;
		if (g_wd_audio >= 0 || g_wd_writer >= 0) {
			g_cores->watchdog().ref_beat();
			if (g_cores->poll_watchdog(loop0)) core4_fallback();
		}

		pk::PadSample pads[pk::MAX_PLAYERS];
		pk::TouchPoint tp;
		uint8_t back = 0;
		bool tapped = false;
		touch_read(&tp, &back, loop0, &tapped);
		/* a linked game: the menu button means "end the session" for BOTH consoles; a brush of the screen must not */
		if (g_link) tapped = false;
		read_pads(pads, back);
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

		/* a linked game that is not playing yet (reaching the other console, the session's set-up) or no longer: a plain screen;
		 * the session is pumped by step(). Cancel leaves; over (done or failed) leaves with the result. */
		if (g_link && !g_link->playing()) {
			const int lr = g_link->step(0);
			if (lr == mdnet::STEP_OVER) { why = pk::EXIT_GAME_LIST; break; }
			if (g_link->playing()) { last_vc = sceDisplayGetVcount(); prev_loop0 = 0; continue; }
			if (link_screen(pads[0], g_pl->request().enter_is_cross)) { logf_("link: cancelled by the player\n"); why = pk::EXIT_GAME_LIST; break; }
			continue;
		}

		if (!g_link && g_wide) g_pl->wide_apply(SCREEN_W, SCREEN_H);
		if (g_wide && g_pl->wide_jumps() != wide_jumps_seen) {
			/* a state loaded, a rewind, a reset: what the analysis remembers of the screen is not this game's any more */
			wide_jumps_seen = g_pl->wide_jumps();
			hd_quiesce();
			g_wide->reset();
		}
		wide_core_apply();
		const bool half = g_pl->pace_mode() == pk::PACE_HALF;
		const bool hd = hd_active();
		if (hd) g_pipe.box().set_pace_mode(g_pl->pace_mode());
		int to_run = (half || hd) ? (int) elapsed : 1;
		if (g_link) to_run = 1;           /* lockstep: one frame per refresh, a missed refresh is not caught up */
		bool mix_early = false;
		if (pal && !g_pl->pal_fast()) {
			const int refr = to_run;
			/* Smooth 50 Hz (pk_palsmooth.h) only for a plain single refresh: not linked, not in half mode */
			pal_pace.smooth = g_pl->pal_smooth() && !g_link && !half;
			g_pal_mix_on.store(pal_pace.smooth ? 1 : 0, std::memory_order_relaxed);
			to_run = refr == 1 ? pal_pace.step(&mix_early) : pal_pace.owed(refr);
			pal_planned += (unsigned) (refr - to_run);       /* 0 on the 6th refresh (not with Smooth 50 Hz: that frame runs early) */
		}
		if (!g_link) { to_run = g_pl->speed_plan(to_run); if (to_run != 1) mix_early = false; }       /* fast forward runs 2 or 3 frames a refresh, rewinding one landing */
		if (hd) g_pipe.box().set_force_drop(!g_link && g_pl->speed_busy());
		bool fresh = false, want_menu = false, link_over = false;
		if (pal && to_run == 0 && tapped && g_pl->input(pads, true, loop0 / 1000, emu_frame)) want_menu = true;
		for (int i = 0; i < to_run; i++) {
			if (g_pl->input(pads, tapped && i == 0, loop0 / 1000, emu_frame)) { want_menu = true; break; }
			bool render = g_link ? true : g_pl->want_render(emu_frame);       /* a linked game draws every frame (it is part of the machine) */
			if (!g_link && hd && !half && i != to_run - 1) render = false;
			if (!g_link && !g_pl->speed_draws(i, to_run)) render = false;       /* fast forward draws the last frame of the refresh */
			md_host_set_render(render ? 1 : 0);
			/* no copy: the core draws straight into the mailbox buffer when the gate is open and nothing would be evicted (pipe.h direct_begin) */
			uint16_t *dfb = NULL;
			const bool wide_run = render && !g_link && g_wide && g_pl->wide_active();
			if (hd && render && !g_link) { int dp = 0; dfb = g_pipe.direct_begin(&dp); if (dfb) md_host_set_framebuffer(wide_run ? dfb + MD_WIDE_MAX : dfb, dp); }
			if (wide_run && !dfb && g_canvas) md_host_set_framebuffer(g_canvas + MD_WIDE_MAX, MD_WIDE_PITCH);
			const bool own_fb = dfb || (wide_run && g_canvas);
			/* the wide view's analysis of this frame, should the player run it ahead (ahead_shown_hook); frames_run is the count after this frame */
			g_ahead_an.armed = wide_run; g_ahead_an.done = false; g_ahead_an.us = 0;
			if (wide_run) { g_ahead_an.frame = (uint32_t) emu_frame + 1; g_ahead_an.margin = g_pl->wide_margin(); g_ahead_an.S = g_wide_S; g_ahead_an.mt = &g_meta[(frames_run + 1) & 7]; }
			uint64_t t0 = sceKernelGetProcessTimeWide();
			g_emu_busy.store(1, std::memory_order_relaxed);
			if (g_link) {
				/* both players' buttons are needed for a frame: a late partner is a stall (no frame this refresh) */
				const int lr = g_link->step((uint16_t) g_pl->link_pad());
				if (lr == mdnet::STEP_OVER) { why = pk::EXIT_GAME_LIST; link_over = true; g_emu_busy.store(0, std::memory_order_relaxed); break; }
				if (lr != mdnet::STEP_FRAME) { g_emu_busy.store(0, std::memory_order_relaxed); break; }
			} else g_pl->run_frame(render);
			g_emu_busy.store(0, std::memory_order_relaxed);
			uint64_t t1 = sceKernelGetProcessTimeWide();
			const uint16_t *dpix = NULL;
			int dw = 0, dh = 0, dpp = 0;
			if (own_fb) { dpix = md_host_video(&dw, &dh, &dpp); md_host_set_framebuffer(NULL, 0); }
			emu_us += t1 - t0;
			last_frame_us = t1 - t0;
			/* Quick response decides from the emulating thread's time for a frame at normal speed (with the hand-over the drawing is not on it) */
			if (!g_link && hd && !half && to_run == 1 && render) g_pl->ahead_cost((uint32_t) (t1 - t0 - g_ahead_an.us));
			frames_run++;
			emu_frame++;
			if (render) render_emu_us = t1 - t0; else not_rendered++;
			static int16_t abuf[2048 * 2];
			int n;
			while ((n = md_host_audio_take(abuf, 2048)) > 0) { n = (int) g_pl->audio_filter(abuf, (size_t) n); if (n > 0) g_pace.push(abuf, n); }
			if (render) {
				uint64_t f0 = sceKernelGetProcessTimeWide();
				int pw = 0, ph = 0, pp = 0;
				const uint16_t *pix = own_fb ? dpix : md_host_video(&pw, &ph, &pp);
				if (own_fb) { pw = dw; ph = dh; pp = dpp; }
				md_video_info vi;
				md_host_video_info(&vi);
				if (vi.serial != g_geo_serial) { g_geo[vi.serial & 15] = geo_now(vi); g_geo_serial = vi.serial; }
				if (pix && pw > 0 && ph > 0) {
					int user = (int) (vi.serial & 0x0FFFFFFFu) | (mix_early && render ? USER_MIX : 0);
					const uint16_t *src = pix;
					int sw = pw;
					mdw::Meta *mt = NULL;
					if (wide_run) {
						/* the wide view: this frame goes to widekit's analysis (the emulating thread's share is the hand-over), the picture is composed where it is drawn */
						mt = &g_meta[frames_run & 7];
						const uint64_t a0 = sceKernelGetProcessTimeWide();
						if (!g_ahead_an.done) g_wide->frame((uint32_t) emu_frame, vi, g_pl->wide_margin(), g_wide_S, mt);      /* run ahead: done in ahead_shown_hook */
						const uint64_t an = g_ahead_an.done ? g_ahead_an.us : sceKernelGetProcessTimeWide() - a0;
						g_wide_an_us += an; g_wide_an_n++;
						render_emu_us += an; emu_us += an;
						int off = vi.system == MD_SYS_GG ? 48 : 0;
						if (hd && !dfb && vi.system == MD_SYS_MD && g_wide_S > 0) { src = pix - g_wide_S; sw = pw + 2 * g_wide_S; off += g_wide_S; }       /* put() copies the strips with the picture */
						mt->view_off = off;
						user |= USER_WIDE;
						wide_cost_update((double) (t1 - t0 + an), 1e6 / frame_hz);
					}
					if (hd) {
						g_pipe.box().note_emulated(1);
						const int pr = dfb ? g_pipe.direct_commit(pix, pw, ph, pp, user, (uint32_t) frames_run, (uint32_t) render_emu_us)
						                   : g_pipe.put(src, sw, ph, pp, user, (uint32_t) frames_run, (uint32_t) render_emu_us);
						if (pr > 0) fresh = true;
					} else {
						Pic s; s.p = pix; s.w = pw; s.h = ph; s.pitch = pp; s.serial = vi.serial; s.margin = 0; s.mix = mix_early && render;
						if (mt) wide_pic(*mt, pix + mt->view_off, pp, &s);
						if (video_upload(g_pl->picture(), frames_run, s, geo_now(vi))) fresh = true;
					}
				}
				if (dfb && g_pipe.direct_open()) g_pipe.direct_abandon();
				fetch_us += sceKernelGetProcessTimeWide() - f0;
			} else if (hd) g_pipe.box().note_emulated(1);
			g_pl->after_frame(sceKernelGetProcessTimeWide());
		}
		/* Smooth 50 Hz: the refresh after an early frame runs none; it shows that picture alone (the early refresh showed it mixed).
		 * With the hand-over the drawing thread does it by itself right after the mixed picture. */
		if (pal_pure_pending && to_run == 0 && !g_link && !hd) {
			Pic pure; pic_info pgi;
			if (pal_pure_pic(&pure, &pgi) && video_upload(g_pl->picture(), frames_run, pure, pgi)) fresh = true;
			if (fresh && pal_planned) pal_planned--;     /* a new picture was shown: this refresh did not repeat one */
		}
		pal_pure_pending = mix_early;
		if (hd) {
			uint32_t hc;
			while (g_pipe.box().poll_cost(&hc)) g_pl->drawn_cost(hc);
			hd_publish_notices();
		}
		{
			/* once per refresh: what the drawing thread did since the last one (average work for a picture, refreshes it missed: pictures
			 * dropped and waits at the gate that ran out; a wait that merely queued is not a miss). Taken every refresh so the interval is
			 * short whatever blocks Quick response, but told to it only for a plain single refresh of a game the hand-over draws. */
			const unsigned cs = g_hd_cum_us.load(std::memory_order_relaxed), cn = g_hd_cum_n.load(std::memory_order_relaxed);
			unsigned cm = 0;
			if (hd) { const pk::HandoffCensus k = g_pipe.box().census(); cm = k.dropped + k.gate_timeouts; }
			if (hd != pp_hd || pp_rebase) { pp_hd = hd; pp_rebase = false; pp.rebase(cs, cn, cm); }
			else {
				uint32_t du, dm;
				pp.take(cs, cn, cm, &du, &dm);
				if (hd && !g_link && !half && !g_pl->speed_busy() && to_run == 1) g_pl->ahead_picture(du, dm);
			}
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
			pp_rebase = true;
			g_pl->look_safe_point(pk::LOOK_SAFE_MENU);
			if (g_pl->take_timing_changed()) {
				/* a region change: the console's frame rate changed; pacing, the mailbox's frame time and the speed meter follow */
				frame_hz = md_host_fps();
				pal = frame_hz < 55.0;
				if (pal) pal_pace.set(frame_hz, vita_hz);
				hd_stop();
				hd_start(tini, frame_hz);
				logf_("timing: %.4f Hz, %s\n", frame_hz, pal ? (g_pl->pal_fast() ? "PAL, Fast 60 Hz" : "PAL, true 50 Hz (5 of 6 refreshes)") : "60 Hz class");
			}
			last_vc = sceDisplayGetVcount();
			speed_meter.reset();
			prev_loop0 = 0;
			sec_start = sceKernelGetProcessTimeWide();
			frames_run = vblanks = 0;
			continue;
		}
		if (fresh) pic_fresh++;
		g_pace.legacy_update();

		/* Draw: every pass in full mode (a late picture is shown again), in half mode only when a new picture came. */
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
			uint64_t show_us = sceKernelGetProcessTimeWide() - d0;
			uint64_t cost = render_emu_us + show_us + (fresh ? 0 : 0);
			g_pl->drawn_cost((uint32_t) (cost > 0xFFFFFFFFull ? 0xFFFFFFFFu : cost));
		} else if (!hd) skipped++;
		vblanks += elapsed;
		main_us += work_end - loop0;
		uint64_t now = sceKernelGetProcessTimeWide();
		if (g_suspend_flag) { g_suspend_flag = 0; if (g_link) g_link->suspend(); g_pl->suspend(); }

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
				    u - ur_seen, e.out_frames / (double) OUT_RATE, e.ema_ms, e.target_ms, e.ratio_ppm, g_pace.fill_frames() * 1000 / MD_AUDIO_RATE,
				    work / 1000.0);
				ur_seen = u;
			}
		}

		if (g_hd_ok) {
			/* one census line a second while the hand-over is on: pictures made / shown / repeated / dropped, depth, and the game's
			 * time against the wall's */
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
			{	/* census: the VDP worker's share of the frame (md/core/vdp_thread.c), deltas over the second; emu_us / fm_us are not split yet */
				static unsigned long vc0[11];
				unsigned long vc[11];
				if (md_host_vdp_census(vc)) {
					logf_("core_split vdp_us=%lu status_joins=%lu join_wait_us=%lu records=%lu full_waits=%lu\n", vc[9] - vc0[9], vc[2] - vc0[2], vc[10] - vc0[10], vc[0] - vc0[0], vc[5] - vc0[5]);
					memcpy(vc0, vc, sizeof vc0);
				}
			}
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
				/* the look governor's second (costs as fractions of a 60 Hz refresh) */
				const double budget = 16683.0, refr = vblanks > 0 ? vblanks : 1;
				pk::LookSecond ls;
				ls.refreshes = vblanks;
				ls.emu = frames_run ? (float) (emu_us / (double) frames_run / budget) : 0.f;
				const double all_draw_us = g_hd_ok ? hd_draw_ms * 1000.0 : (double) (draw_us + fetch_us + finish_us);
				ls.filter = (float) (sc_us / refr / budget);
				ls.draw = (float) ((all_draw_us > sc_us ? all_draw_us - sc_us : 0.0) / refr / budget);
				const unsigned planned = pal_planned;
				pal_planned = 0;
				/* a PAL game runs 5 frames in 6 refreshes: its cost per refresh is 49.7 / 59.94 of the cost per frame */
				if (pal && !g_pl->pal_fast()) ls.emu *= (float) (frame_hz / vita_hz);
				const unsigned late = g_hd_ok ? (hd_repeated > planned ? hd_repeated - planned : 0) : missed_refr;
				ls.missed = (int) late;
				ls.parallel = hd;
				ls.pace_half = half;
				g_pl->look_second(ls);
				missed_refr = 0;
			}
			{
				/* the per-thread CPU census: ms of CPU each thread used in this second (the kernel's run time) */
				static SceUID s_main = sceKernelGetThreadId();
				static double prev[7] = { 0, 0, 0, 0, 0, 0, 0 };
				const SceUID ids[7] = { s_main, g_athread, g_hd_ok ? g_hd_thread : -1, g_wthread, g_help_thread[0], g_help_thread[1], g_disc_thread };
				double cur[7], d[7];
				for (int k = 0; k < 7; k++) { cur[k] = thread_cpu_ms(ids[k]); d[k] = cur[k] >= 0 && prev[k] >= 0 ? cur[k] - prev[k] : -1.0; prev[k] = cur[k]; }
				logf_("threads ms/s: main=%.0f audio=%.0f draw=%.0f writer=%.0f scale1=%.0f scale2=%.0f disc=%.0f (draw core %d)\n", d[0], d[1], d[2], d[3], d[4], d[5], d[6], g_draw_core);
			}
			if (md_host_is_cd()) {
				/* the disc's file layer (cdio.c) over this second: reads, how many came out of a block the helper had read ahead, how many the emulating
				 * thread read itself (misses), its waits for a block the helper was on, the time it lost to both, and the helper's own busy time
				 * (reading and decoding CHD hunks) */
				static unsigned long pc[10];
				unsigned long c[10], dc[10];
				md_host_cd_census(c);
				for (int k = 0; k < 10; k++) { dc[k] = c[k] >= pc[k] ? c[k] - pc[k] : c[k]; pc[k] = c[k]; }
				logf_("disc: helper=%d reads=%lu from_helper=%lu direct=%lu waits=%lu emu_lost_ms=%.1f helper_blocks=%lu helper_busy_ms=%.1f unused=%lu check_errors=%lu timeouts=%lu\n", md_host_cd_helper(),
				    dc[0], dc[1], dc[2], dc[3], dc[4] / 1000.0, dc[5], dc[6] / 1000.0, dc[7], dc[8], dc[9]);
			}
			if (g_wide && g_pl->wide_active()) {
				mdw::Stats ws;
				g_wide->census(&ws);
				const unsigned cu = g_wide_compose_us.exchange(0), cn = g_wide_composed.exchange(0);
				unsigned long wc[3] = { 0, 0, 0 };
				md_host_wide_cache_stat(wc);
				logf_("wide: margin=%d strips=%d rung=%d handover_us/fr=%.0f compose_us/pic=%.0f real=%d%% soft_lines=%d scenes=%d cost_ema_ms=%.2f map_cells=%d bound=%d map_px=%u rows=%u strips_copied=%lu drawn=%lu compose_chunks_by_helpers=%u\n", g_pl->wide_margin(), g_wide_S, g_wide_rung,
				    g_wide_an_n ? (double) g_wide_an_us / (double) g_wide_an_n : 0.0, cn ? (double) cu / cn : 0.0, ws.real_pct, ws.soft_lines, ws.scene_events, g_wide_ema / 1000.0,
				    ws.map_cells, ws.map_bound, ws.map_px, ws.rows, wc[0], wc[1], g_wide_helped.exchange(0));
				g_wide_an_us = g_wide_an_n = 0;
			}
			double fr = frames_run ? frames_run : 1;
			logf_("t=%d fps=%.1f emu_fps=%.1f emu_ms=%.2f last_ms=%.2f aq=%u", secs, drawn / dt, frames_run / dt,
			    frames_run ? emu_us / 1000.0 / frames_run : 0.0, last_frame_us / 1000.0, g_pace.fill_frames());
			logf_(" | main split ms/fr: emu=%.2f copy=%.2f gpu_submit=%.2f | drawn=%u skipped=%u not_rendered=%u new_pic=%u fence_skip=%u rt_reuse=%u",
			    emu_us / 1000.0 / fr, fetch_us / 1000.0 / fr, draw_us / 1000.0 / fr, drawn, skipped, not_rendered, pic_fresh, g_fence_skips, g_rt_reused);
			const unsigned sc_blocks = g_scale_blocks.exchange(0), sc_bdone = g_scale_blocks_done.exchange(0), sc_bmoved = g_scale_blocks_moved.exchange(0);
			logf_(" | scaler=%s pics=%u ms/pic=%.2f nonflat/pic=%u helper_chunks=%u waits=%u computed=%u%% moved=%u%%", pk::scaler_name((pk::ScalerKind) g_scale_last.load()),
			    sc_pics, sc_pics ? sc_us / 1000.0 / sc_pics : 0.0, sc_pics ? sc_nonflat / sc_pics : 0, sc_help, sc_waits,
			    sc_blocks ? sc_bdone * 100 / sc_blocks : 0, sc_blocks ? sc_bmoved * 100 / sc_blocks : 0);
			logf_(" | per s: finish=%.1f ms vwait=%.1f ms | %s\n", finish_us / 1000.0, vwait_us / 1000.0, g_pl->pace_line().c_str());
			if (g_pl->quick() && !g_link) logf_("%s\n", g_pl->ahead_line().c_str());
			sec_start = now;
			vblanks = frames_run = 0;
			emu_us = main_us = fetch_us = draw_us = finish_us = vwait_us = 0;
			drawn = skipped = pic_fresh = not_rendered = 0;
			secs++;
		}

		/* one compact audio / pacing line a second */
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
	g_quit = 1;
	help_stop();
	{
		char c[220];
		g_cores->census_line(c, sizeof c);
		logf_("%s\n", c);
	}
	if (g_link) {
		/* how the link ended, before the session is destroyed (a guest's own game is put back then) */
		const bool done = g_link->ok();
		const int gerr = g_link->gblink_error();
		const mdnet::Fail f = g_link->failure();
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
	md_host_shutdown();
	if (why == pk::EXIT_NORMAL) {
		/* "Exit": close VitAdhoc, as the app's own Exit does */
		if (g_log) { fclose(g_log); g_log = NULL; }
		sceKernelExitProcess(0);
	}
	return_to_vitadhoc();
	return 0;
}
