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
#include "picture.h"
#include "audio_pace.h"
#include "player.h"
#include "pipe.h"
#include "pal_pace.h"
#include "pk_gfx_vita.h"
#include "pk_cores.h"
#include "pk_scale.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

extern "C" {
unsigned int sceUserMainThreadStackSize = 1024 * 1024;
unsigned int _newlib_heap_size_user = 40 * 1024 * 1024;
}

#define APP_ROOT "ux0:data/GBAdhoc"
#define SCREEN_W 960
#define SCREEN_H 544

static mdp::MdPlayer *g_pl;
static mdp::PicturePipe g_pipe;      /* frame hand-over to the drawing thread (pipe.h); idle unless started */
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

/* the roles of the Sega player (pk_cores.h, CP_MD): the emulating thread, the drawing thread, the sound thread, the writer, and the scaler helpers */
static pk::Request core4_request(void)
{
	pk::Request rq(pk::CP_MD);
	rq.with(pk::R_EMU_MAIN).with(pk::R_DRAW).with(pk::R_AUDIO_OUT).with(pk::R_WRITER).with(pk::R_UPSCALE);
	rq.upscale_n = 2;     /* the plan gives one helper on three cores, two on four */
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

static SceUID g_wthread = -1, g_hd_thread = -1, g_help_thread[2] = { -1, -1 };

static void core4_fallback(void)
{
	g_core_plan = g_cores->plan(core4_request());
	const pk::Slot *a = g_core_plan.find(pk::R_AUDIO_OUT);
	const pk::Slot *w = g_core_plan.find(pk::R_WRITER);
	const int rc = a && g_athread >= 0 ? sceKernelChangeThreadCpuAffinityMask(g_athread, a->mask) : -1;
	const int rc2 = w && g_wthread >= 0 ? sceKernelChangeThreadCpuAffinityMask(g_wthread, w->mask) : -1;
	for (int i = 0; i < 2; i++) {
		const pk::Slot *h = g_core_plan.find(pk::R_UPSCALE, i);
		if (h && g_help_thread[i] >= 0) sceKernelChangeThreadCpuAffinityMask(g_help_thread[i], h->mask);
	}
	g_wd_audio = g_wd_writer = -1;
	char c[220];
	g_cores->census_line(c, sizeof c);
	logf_("threads back on the three-core layout (audio rc=%08x, writer rc=%08x)\n%s\n", rc, rc2, c);
}

/* ---- video ------------------------------------------------------------- */
/* The picture goes through the GPU: the core's frame is copied into a texture (through the Game Gear's LCD colour table when that is on),
 * optionally scaled by the CPU pixel art scaler (MMPX / xBR, playerkit pk_scale, on the drawing thread with one or two helpers taking
 * stripes) into a second texture, then
 *   1. (Sharp)  drawn with nearest sampling at an integer scale into a render target,
 *   2. (Glow)   every few frames shrunk twice (64x64, 16x16) into two tiny render targets,
 *   3. the screen: glow stretched over everything and darkened, the picture on top (bilinear from the prescaled target), scanline / grid
 *      bands, the bezel PNG.
 * A texture or target is only written again once the GPU has finished reading it (vita2d fragment fences). */

#define RT_SIZE 1024
#define SC_W 1024              /* the scaler's output texture: 640x448 (MMPX of a 320x224 picture) is the largest */
#define SC_H 512

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
static pic_info g_src_info;               /* the geometry of the uploaded picture */
static int g_sc_i = -1, g_sc_f = 0;       /* newest scaled texture and its factor (0: the picture is shown unscaled) */
static unsigned g_serial_seen, g_up_serial;
static int g_crop_top, g_crop_bot, g_crop_cand_top, g_crop_cand_bot, g_crop_votes;
static int g_have_picture;
static unsigned g_fence_skips;
static std::atomic<unsigned> g_scale_us(0), g_scale_pics(0), g_scale_nonflat(0), g_scale_helped(0), g_scale_waits(0);   /* the drawing thread's scaler, per second */
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

/* ---- the scaler's helpers ------------------------------------------------------------------------------------------------------------
 * One job at a time, cut into chunks of rows. The drawing thread publishes the job (fields first, then `next` with the job's number in
 * its upper half), takes chunks itself, and waits until every chunk has been finished; a helper that is awake takes chunks the same way
 * (compare-and-swap on `next`, so a late helper can never take a chunk of a job it did not read). A helper that is not scheduled costs
 * nothing: the drawing thread has done its chunks. */
struct ScaleJob {
	pk::ScalerKind kind;
	const uint16_t *src;
	int w, h, spitch;
	uint16_t *dst;
	int dpitch, chunk_rows, nchunks;
	std::atomic<uint64_t> next;      /* (job << 32) | next chunk */
	std::atomic<int> done;
	std::atomic<unsigned> nonflat;
	std::atomic<int> by_helpers;
	ScaleJob() : kind(pk::SC_NONE), src(0), w(0), h(0), spitch(0), dst(0), dpitch(0), chunk_rows(8), nchunks(0), next(0), done(0), nonflat(0), by_helpers(0) {}
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
		const pk::ScalerKind kind = g_job.kind;
		const uint16_t *src = g_job.src;
		const int w = g_job.w, h = g_job.h, sp = g_job.spitch, dp = g_job.dpitch, cr = g_job.chunk_rows, nch = g_job.nchunks;
		uint16_t *dst = g_job.dst;
		const uint32_t c = (uint32_t) (n & 0xFFFFFFFFu);
		if (kind == pk::SC_NONE || (int) c >= nch) return ran;
		if (!g_job.next.compare_exchange_strong(n, n + 1, std::memory_order_acq_rel)) continue;
		const int y0 = (int) c * cr, y1 = y0 + cr < h ? y0 + cr : h;
		const unsigned nf = pk::scale_rows(kind, src, w, h, sp, dst, dp, y0, y1, sc);
		g_job.nonflat.fetch_add(nf, std::memory_order_relaxed);
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
		job_work(g_help_scratch[idx], true);
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
		snprintf(nm, sizeof nm, "md_scale%d", i);
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

/* scale the whole picture: the helpers are woken, this thread works too and then waits for the chunks the helpers hold */
static unsigned scale_picture(pk::ScalerKind kind, const uint16_t *src, int w, int h, int spitch_px, uint16_t *dst, int dpitch_px)
{
	g_job.nchunks = 0;
	g_job.kind = kind; g_job.src = src; g_job.w = w; g_job.h = h; g_job.spitch = spitch_px; g_job.dst = dst; g_job.dpitch = dpitch_px;
	g_job.chunk_rows = 8;
	g_job.nchunks = (h + g_job.chunk_rows - 1) / g_job.chunk_rows;
	g_job.done.store(0); g_job.nonflat.store(0); g_job.by_helpers.store(0);
	g_job.next.store((uint64_t) (++g_job_no) << 32, std::memory_order_release);
	for (int i = 0; i < g_help_n; i++) sceKernelSignalSema(g_help_sema[i], 1);
	job_work(g_draw_scratch, false);
	unsigned waits = 0;
	while (g_job.done.load(std::memory_order_acquire) < g_job.nchunks) { sceKernelDelayThread(20); waits++; }
	g_scale_waits.fetch_add(waits);
	g_scale_helped.fetch_add((unsigned) g_job.by_helpers.load());
	const unsigned nf = g_job.nonflat.load();
	g_job.kind = pk::SC_NONE;          /* a helper that wakes late finds nothing to take */
	g_job.nchunks = 0;
	return nf;
}

/* ---- the picture: upload, scaler ---------------------------------------------------------------------------------------------------- */

struct Pic { const uint16_t *p; int w, h, pitch; unsigned serial; };

/* geometry of the pictures in flight, by serial (the emulating thread fills it before the hand-over, the drawing thread reads it after) */
static pic_info g_geo[16];
static unsigned g_geo_serial = 0xFFFFFFFFu;

static pic_info geo_now(const md_video_info &vi)
{
	pic_info g;
	g.w = vi.w; g.h = vi.h;
	g.logical_rows = vi.h > vi.native_h * 3 / 2 ? vi.h / 2 : vi.h;      /* interlaced double height: two picture rows per console line */
	g.dar = vi.dar; g.par = vi.par; g.system = vi.system;
	return g;
}

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

/* Copies the picture into a texture the GPU is done with, and scales it when the look says so. Returns 1 when the picture changed. */
static int video_upload(const pic_settings &ps, int frame_no, const Pic &s, const pic_info &gi)
{
	int w = s.w, h = s.h;
	int idx = -1;
	for (int n = 1; n <= 4; n++) {
		int c = (g_src_i + n) % 4;
		if (gtex_writable(g_src[c])) { idx = c; break; }
	}
	if (idx < 0) { g_fence_skips++; return 0; }          /* the GPU is behind: show the old picture again */
	g_serial_seen = ++g_up_serial;
	if (w > 512) w = 512;
	if (h > 512) h = 512;
	if (ps.shape == PIC_ZOOM && frame_no % 20 == 0) {
		int mx = h / 8;
		int t = pic_blank_rows(s.p, s.pitch, w, h, 1, mx), b = pic_blank_rows(s.p, s.pitch, w, h, 0, mx);
		if (t == g_crop_cand_top && b == g_crop_cand_bot) g_crop_votes++; else { g_crop_cand_top = t; g_crop_cand_bot = b; g_crop_votes = 0; }
		if (g_crop_votes >= 2) { g_crop_top = t; g_crop_bot = b; }
		else { if (t < g_crop_top) g_crop_top = t; if (b < g_crop_bot) g_crop_bot = b; }
	}
	vita2d_texture *tx = g_src[idx].t;
	uint8_t *dst = (uint8_t *) vita2d_texture_get_datap(tx);
	unsigned dpitch = vita2d_texture_get_stride(tx);
	/* the scaler reads CPU memory (the mailbox's copy, or the Game Gear's mapped copy), never the texture */
	static uint16_t lcdbuf[160 * 256];
	const uint16_t *scaler_src = s.p;
	int scaler_pitch_px = s.pitch / 2;
	if (g_is_gg && g_lcd && ps.gg_colors && w * h <= 160 * 256 && w <= 160) {
		for (int y = 0; y < h; y++) {
			const uint16_t *row = (const uint16_t *) ((const uint8_t *) s.p + (size_t) y * s.pitch);
			uint16_t *o = lcdbuf + (size_t) y * w;
			for (int x = 0; x < w; x++) o[x] = g_lcd[row[x]];
			memcpy(dst + (size_t) y * dpitch, o, (size_t) w * 2);
		}
		scaler_src = lcdbuf; scaler_pitch_px = w;
	} else {
		for (int y = 0; y < h; y++)
			memcpy(dst + (size_t) y * dpitch, (const uint8_t *) s.p + (size_t) y * s.pitch, (size_t) w * 2);
	}
	if (w != g_src_w || h != g_src_h) { g_crop_top = g_crop_bot = g_crop_cand_top = g_crop_cand_bot = 0; }
	g_src_i = idx; g_src_w = w; g_src_h = h; g_src_info = gi;
	g_have_picture = 1;

	/* the pixel art scaler: the look's choice for this picture, into a scaled texture the GPU is done with */
	g_sc_f = 0;
	const pk::ScalerKind kind = (g_pl->look_mode() == 2) ? pk::SC_NONE : scaler_kind_for(ps, gi);
	g_scale_last.store((int) kind, std::memory_order_relaxed);
	if (kind != pk::SC_NONE && w == gi.w && h == gi.h) {
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
			    (int) (vita2d_texture_get_stride(st) / 2));
			g_scale_us.fetch_add((unsigned) (sceKernelGetProcessTimeWide() - t0));
			g_scale_pics.fetch_add(1);
			g_scale_nonflat.fetch_add(nf);
			g_sc_i = si; g_sc_f = f;
		} else g_fence_skips++;
	}
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
		pic_geometry(&ps, &g_src_info, g_crop_top, g_crop_bot, SCREEN_W, SCREEN_H, &r);
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
		*f = pk::Frame((const uint16_t *) vita2d_texture_get_datap(t), g_src_w, g_src_h, (int) vita2d_texture_get_stride(t));
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

static int hd_draw_thread(SceSize, void *)
{
	for (;;) {
		pk::HandoffFrame *f = g_pipe.box().take();
		if (!f) break;
		const uint64_t t0 = sceKernelGetProcessTimeWide();
		Pic s;
		s.p = (const uint16_t *) f->data; s.w = f->w; s.h = f->h; s.pitch = f->pitch; s.serial = (unsigned) (f->user & 0x3FFFFFFF);
		const pic_info gi = g_geo[s.serial & 15];
		const pic_settings ps = g_pl->picture();     /* one copy for this picture: the game thread may ease the look meanwhile */
		video_upload(ps, (int) f->seq, s, gi);
		bool drew = false;
		for (int tries = 0; tries < 4 && !drew; tries++) {
			if (tries) sceDisplayWaitVblankStart();
			drew = frame_draw(ps, 1, (int) f->seq);
		}
		if (drew) {
			if (g_hd_notice_on.load()) {
				pk::Notices n;
				{ pk::SpinGuard g(g_hd_lk); n = g_hd_notices; }
				if (n.active()) n.draw(g_gfx, g_text, PK_HEX(0x7B6CFF, 255), 26, true);
			}
			frame_finish();
			g_hd_drawn++;
		} else g_hd_failed++;
		const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
		g_hd_draw_us += (unsigned) dt;
		g_pipe.box().release(f, (uint32_t) (dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : dt));
	}
	return sceKernelExitThread(0);
}

static bool hd_active(void) { return g_hd_ok && g_pipe.box().enabled() && g_pl->smooth_handoff(); }

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
	for (int i = 0; i < 300; i++) {
		vita2d_start_drawing();
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
			std::string code, name;
			bool ok = ime_ask("Cheat code (Game Genie, Pro Action Replay or address:value)", "", code);
			if (ok && !code.empty()) ime_ask("A name for it (you can leave it empty)", "", name);
			g_pl->cheat_entered(ok, code, name);
		}
		if (ev != pk::MENU_NONE && ev != pk::MENU_RESET_DONE && ev != pk::MENU_RESUME) break;
		if (!g_pl->menu_open()) break;
		if (g_suspend_flag) { g_suspend_flag = 0; g_pl->suspend(); }
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

	/* the plan of the threads (threads.ini: core4=auto|off, audio_prio, main_prio, draw_prio, helpers=0|1|2, audio_legacy) */
	const char *tini = APP_ROOT "/md/threads.ini";
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

	/* the game: launch file or argument, patch, saves, states, settings (player.cpp) */
	writer_start();
	mdp::StartResult sr = g_pl->start(argc, argv, APP_ROOT, enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS);
	if (!sr.ok) {
		logf_("cannot start: %s / %s\n", sr.fatal1.c_str(), sr.fatal2.c_str());
		g_pl->log_flush(true);
		fatal_screen(sr.fatal1, sr.fatal2);
		writer_stop();
		return_to_vitadhoc();
	}
	cdc.enterButtonAssign = sr.enter_is_cross ? SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS : SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
	sceCommonDialogSetConfigParam(&cdc);
	g_is_gg = md_host_system() == MD_SYS_GG;
	if (g_is_gg) g_lcd = pic_lcd_table();           /* built once, before any other thread needs it */

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
	help_start(clampi_(ini_int(tini, "helpers", 2), 0, 2));
	g_pl->look_start(scePowerGetArmClockFrequency());   /* the look governor: the best look first, the game's memory of what it needed */

	double frame_hz = md_host_fps();
	const double vita_hz = 59.94;
	bool pal = frame_hz < 55.0;
	FramePace pal_pace;
	if (pal) { pal_pace.set(frame_hz, vita_hz); logf_("PAL: %.3f Hz, %s\n", frame_hz, g_pl->pal_fast() ? "Fast 60 Hz chosen: one frame per refresh" : "True 50 Hz: 5 of 6 refreshes emulate a frame"); }
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
		read_pads(pads, back);
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

		const bool half = g_pl->pace_mode() == pk::PACE_HALF;
		const bool hd = hd_active();
		if (hd) g_pipe.box().set_pace_mode(g_pl->pace_mode());
		int to_run = (half || hd) ? (int) elapsed : 1;
		if (pal && !g_pl->pal_fast()) { const int refr = to_run; to_run = pal_pace.owed(refr); pal_planned += (unsigned) (refr - to_run); }
		bool fresh = false, want_menu = false;
		if (pal && to_run == 0 && tapped && g_pl->input(pads, true, loop0 / 1000, emu_frame)) want_menu = true;
		for (int i = 0; i < to_run; i++) {
			if (g_pl->input(pads, tapped && i == 0, loop0 / 1000, emu_frame)) { want_menu = true; break; }
			bool render = g_pl->want_render(emu_frame);
			if (hd && !half && i != to_run - 1) render = false;
			md_host_set_render(render ? 1 : 0);
			uint64_t t0 = sceKernelGetProcessTimeWide();
			md_host_run_frame();
			uint64_t t1 = sceKernelGetProcessTimeWide();
			emu_us += t1 - t0;
			last_frame_us = t1 - t0;
			frames_run++;
			emu_frame++;
			if (render) render_emu_us = t1 - t0; else not_rendered++;
			static int16_t abuf[2048 * 2];
			int n;
			while ((n = md_host_audio_take(abuf, 2048)) > 0) g_pace.push(abuf, n);
			if (render) {
				uint64_t f0 = sceKernelGetProcessTimeWide();
				int pw = 0, ph = 0, pp = 0;
				const uint16_t *pix = md_host_video(&pw, &ph, &pp);
				md_video_info vi;
				md_host_video_info(&vi);
				if (vi.serial != g_geo_serial) { g_geo[vi.serial & 15] = geo_now(vi); g_geo_serial = vi.serial; }
				if (pix && pw > 0 && ph > 0) {
					if (hd) {
						g_pipe.box().note_emulated(1);
						if (g_pipe.put(pix, pw, ph, pp, (int) (vi.serial & 0x3FFFFFFFu), (uint32_t) frames_run, (uint32_t) render_emu_us) > 0) fresh = true;
					} else {
						Pic s; s.p = pix; s.w = pw; s.h = ph; s.pitch = pp; s.serial = vi.serial;
						if (video_upload(g_pl->picture(), frames_run, s, geo_now(vi))) fresh = true;
					}
				}
				fetch_us += sceKernelGetProcessTimeWide() - f0;
			} else if (hd) g_pipe.box().note_emulated(1);
			g_pl->after_frame(sceKernelGetProcessTimeWide());
		}
		if (hd) {
			uint32_t hc;
			while (g_pipe.box().poll_cost(&hc)) g_pl->drawn_cost(hc);
			hd_publish_notices();
		}
		if (want_menu) {
			hd_quiesce();
			pk::MenuEvent ev = menu_loop(0);
			if (ev == pk::MENU_GAME_LIST) { why = pk::EXIT_GAME_LIST; break; }
			if (ev == pk::MENU_EXIT) { why = pk::EXIT_NORMAL; break; }
			hd_after_menu();
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
		if (g_suspend_flag) { g_suspend_flag = 0; g_pl->suspend(); }

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
				static double prev[6] = { 0, 0, 0, 0, 0, 0 };
				const SceUID ids[6] = { s_main, g_athread, g_hd_ok ? g_hd_thread : -1, g_wthread, g_help_thread[0], g_help_thread[1] };
				double cur[6], d[6];
				for (int k = 0; k < 6; k++) { cur[k] = thread_cpu_ms(ids[k]); d[k] = cur[k] >= 0 && prev[k] >= 0 ? cur[k] - prev[k] : -1.0; prev[k] = cur[k]; }
				logf_("threads ms/s: main=%.0f audio=%.0f draw=%.0f writer=%.0f scale1=%.0f scale2=%.0f (draw core %d)\n", d[0], d[1], d[2], d[3], d[4], d[5], g_draw_core);
			}
			double fr = frames_run ? frames_run : 1;
			logf_("t=%d fps=%.1f emu_fps=%.1f emu_ms=%.2f last_ms=%.2f aq=%u", secs, drawn / dt, frames_run / dt,
			    frames_run ? emu_us / 1000.0 / frames_run : 0.0, last_frame_us / 1000.0, g_pace.fill_frames());
			logf_(" | main split ms/fr: emu=%.2f copy=%.2f gpu_submit=%.2f | drawn=%u skipped=%u not_rendered=%u new_pic=%u fence_skip=%u rt_reuse=%u",
			    emu_us / 1000.0 / fr, fetch_us / 1000.0 / fr, draw_us / 1000.0 / fr, drawn, skipped, not_rendered, pic_fresh, g_fence_skips, g_rt_reused);
			logf_(" | scaler=%s pics=%u ms/pic=%.2f nonflat/pic=%u helper_chunks=%u waits=%u", pk::scaler_name((pk::ScalerKind) g_scale_last.load()),
			    sc_pics, sc_pics ? sc_us / 1000.0 / sc_pics : 0.0, sc_pics ? sc_nonflat / sc_pics : 0, sc_help, sc_waits);
			logf_(" | per s: finish=%.1f ms vwait=%.1f ms | %s\n", finish_us / 1000.0, vwait_us / 1000.0, g_pl->pace_line().c_str());
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
