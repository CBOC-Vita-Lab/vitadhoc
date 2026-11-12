/*
 * nes.bin: the NES program of VitAdhoc (FCEUmm core, separate executable).
 *
 * Launched by VitAdhoc with sceAppMgrLoadExec("app0:nes.bin", {rom path}).
 * Quitting launches app0:eboot.bin again with "--from-nes". This file and the other files in
 * nes/frontend are new code (MIT); the FCEUmm core in nes/core is GPL-2.0-or-later, so the program
 * nes.bin as a whole is distributed under the GPL (see nes/core/PROVENANCE.md). Nothing from gpsp/ or
 * snes/ code is linked into this program except the shared audio pacer and the third party libraries.
 *
 * Copyright CBOC-Vita. MIT licence for this file (see nes/frontend/LICENSE).
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
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <vita2d.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "host.h"
#include "host_files.h"
#include "picture.h"
#include "audio_pace.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

extern "C" {
unsigned int sceUserMainThreadStackSize = 1024 * 1024;
unsigned int _newlib_heap_size_user = 48 * 1024 * 1024;
}

extern "C" const unsigned char nes_font_data[];
extern "C" const unsigned char nes_font_data_end[];

#define DATA_ROOT "ux0:data/GBAdhoc/nes"
#define BACKUP_DIR DATA_ROOT "/backups"
#define SCREEN_W 960
#define SCREEN_H 544

/* ---- log --------------------------------------------------------------- */

static FILE *g_log;
/* The log is kept in memory and written in one piece at most every 5 s, never while a battery RAM
 * write is running (see log_flush below): an fflush on every line was a main thread stall on the
 * memory card. Only the main thread logs. */
static std::string g_logbuf;
static uint64_t g_log_flushed_us;
static unsigned g_log_lost;
static void logf_(const char *fmt, ...)
{
	if (!g_log) return;
	char tmp[1024];
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n > 0) {
		if (g_logbuf.size() > 256 * 1024) g_log_lost++;      /* the card was busy for very long */
		else if ((size_t) n < sizeof tmp) g_logbuf.append(tmp, n);
		else {
			std::string big((size_t) n + 1, '\0');
			vsnprintf(&big[0], big.size(), fmt, ap2);
			g_logbuf.append(big.data(), n);
		}
	}
	va_end(ap2);
}
static void log_flush(uint64_t now_us, int mode);   /* defined after the battery RAM writer */

static void mkdirs(void)
{
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir("ux0:data/GBAdhoc", 0777);
	sceIoMkdir(DATA_ROOT, 0777);
	sceIoMkdir(DATA_ROOT "/states", 0777);
	sceIoMkdir(DATA_ROOT "/games", 0777);
	sceIoMkdir(BACKUP_DIR, 0777);
}

/* ---- text -------------------------------------------------------------- */

struct Glyph { float x0, y0, x1, y1, xoff, yoff, adv; };
static stbtt_bakedchar g_baked[96];
static vita2d_texture *g_font_tex;
static const int FONT_W = 512, FONT_H = 512;
static const float FONT_PX = 26.0f;

static void font_init(void)
{
	unsigned char *bitmap = (unsigned char *) calloc(FONT_W * FONT_H, 1);
	if (!bitmap) return;
	int rc = stbtt_BakeFontBitmap(nes_font_data, 0, FONT_PX, bitmap, FONT_W, FONT_H, 32, 96, g_baked);
	if (rc <= 0) { free(bitmap); return; }
	g_font_tex = vita2d_create_empty_texture_format(FONT_W, FONT_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
	if (g_font_tex) {
		uint32_t *dst = (uint32_t *) vita2d_texture_get_datap(g_font_tex);
		unsigned stride = vita2d_texture_get_stride(g_font_tex) / 4;
		for (int y = 0; y < FONT_H; y++)
			for (int x = 0; x < FONT_W; x++) {
				uint32_t a = bitmap[y * FONT_W + x];
				dst[y * stride + x] = (a << 24) | 0x00ffffff;
			}
		vita2d_texture_set_filters(g_font_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	}
	free(bitmap);
}

static void text_draw(float x, float y, const char *s, unsigned color)
{
	if (!g_font_tex) return;
	/* white glyph texture tinted with color via the tint program */
	for (; *s; s++) {
		const unsigned char ch = (unsigned char) *s;
		if (ch < 32 || ch >= 128) continue;
		stbtt_aligned_quad q;
		float px = 0, py = 0;
		stbtt_GetBakedQuad(g_baked, FONT_W, FONT_H, ch - 32, &px, &py, &q, 1);
		float w = q.x1 - q.x0, h = q.y1 - q.y0;
		if (w > 0 && h > 0)
			vita2d_draw_texture_tint_part_scale(g_font_tex, x + q.x0, y + q.y0,
			    q.s0 * FONT_W, q.t0 * FONT_H, w, h, 1.0f, 1.0f, color);
		x += px;
	}
}

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

static void audio_push(const int16_t *src, int frames) { g_pace.push(src, frames); }

static int audio_thread(SceSize, void *)
{
	static int16_t out[apace::Pacer::GRAIN_LEGACY * 2];
	uint64_t prev = 0;
	while (!g_quit) {
		g_pace.render(out);
		sceAudioOutOutput(g_aport, out);
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
	cfg.in_rate = NES_AUDIO_RATE;
	cfg.out_rate = OUT_RATE;
	cfg.legacy = legacy;
	g_pace.init(cfg);
	g_aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, g_pace.grain(), OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
	if (g_aport < 0) { logf_("audio open failed %08x\n", g_aport); return; }
	int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume(g_aport, (SceAudioOutChannelFlag) (SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
	g_athread = sceKernelCreateThread("nes_audio", audio_thread, prio, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (g_athread >= 0) sceKernelStartThread(g_athread, 0, NULL);
	logf_("audio: %s, grain %d frames at %d Hz, thread priority 0x%x (%s)\n", legacy ? "LEGACY (first player's pacing)" : "pacer v2",
	    g_pace.grain(), OUT_RATE, prio, g_athread >= 0 ? "started" : "NOT started");
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

static GTex g_src[4];            /* the core's picture, 256x256 texture holding 256x240 RGB565 */
static GTex g_rt, g_glowA, g_glowB;
static GTex g_bezel;
static int g_bezel_w, g_bezel_h;
static int g_src_i = -1;         /* newest uploaded texture */
static const int SRC_W = 256, SRC_H = 240;
static unsigned g_serial_seen;
static int g_have_picture;
static unsigned g_fence_skips;

static bool gtex_writable(GTex &g) { return g.t && vita2d_fragment_fence_done(&g.rd); }
static void gtex_read(GTex &g) { if (g.t) vita2d_fragment_fence_record(&g.rd); }

static void video_init(void)
{
	for (int i = 0; i < 4; i++) {
		memset(&g_src[i], 0, sizeof g_src[i]);
		g_src[i].t = vita2d_create_empty_texture_format(256, 256, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
		if (g_src[i].t) vita2d_texture_set_filters(g_src[i].t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
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
	logf_("render targets: sharp=%d glow=%d/%d\n", g_rt.t != NULL, g_glowA.t != NULL, g_glowB.t != NULL);
}

/* Bezel / overlay PNG: overlays/<game>.png, else overlays/nes.png. Loaded once. */
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
		path = "ux0:data/GBAdhoc/overlays/nes.png";
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

/* Takes the newest finished picture from the core and copies it into a texture the GPU is done with.
 * Returns 1 when the picture changed (it does after every emulated frame). */
static int video_fetch(void)
{
	int w = 0, h = 0, pitch = 0;
	const uint16_t *src = nes_host_video(&w, &h, &pitch);
	if (!src || w != SRC_W || h != SRC_H) return 0;
	int idx = -1;
	for (int n = 1; n <= 4; n++) {
		int c = (g_src_i + n) % 4;
		if (gtex_writable(g_src[c])) { idx = c; break; }
	}
	if (idx < 0) { g_fence_skips++; return 0; }          /* the GPU is behind: show the old picture again */
	g_serial_seen++;
	vita2d_texture *tx = g_src[idx].t;
	uint8_t *dst = (uint8_t *) vita2d_texture_get_datap(tx);
	unsigned dpitch = vita2d_texture_get_stride(tx);
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t) y * dpitch, (const uint8_t *) src + (size_t) y * pitch, (size_t) w * 2);
	g_src_i = idx;
	g_have_picture = 1;
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
 * (the display queue is full): the caller then skips this frame. */
static bool frame_draw(const pic_settings &ps)
{
	vita2d_pool_reset();
	pic_rect r;
	GTex *src = g_src_i >= 0 ? &g_src[g_src_i] : NULL;
	if (src && g_have_picture)
		pic_geometry(&ps, SRC_W, SRC_H, pic_overscan_rows(&ps, nes_host_is_pal()), SCREEN_W, SCREEN_H, &r);
	bool bezel = g_bezel.t != NULL;
	int kx = 1, ky = 1;
	bool use_rt = false, exact = false;
	if (src && g_have_picture) {
		pic_prescale(&r, &kx, &ky);
		exact = fabsf(r.dw - r.sw * kx) < 0.5f && fabsf(r.dh - r.sh * ky) < 0.5f && kx == ky;
		use_rt = ps.quality == PIC_SHARP && !exact && g_rt.t != NULL && (kx > 1 || ky > 1);
	}

	/* scene 1: glow, a few times a second */
	bool glow_on = src && g_have_picture && !bezel && ps.glow > 0 && ps.shape != PIC_FULL && g_glowA.t && g_glowB.t;
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
	/* scene 2: the sharp-bilinear prescale; only redrawn when its inputs changed (a new picture, another
	 * crop or factor), so a pass that shows the same picture again (PAL games on the 60 Hz display, the
	 * pause menu) saves a whole GPU pass */
	if (use_rt) {
		RtKey key;
		memset(&key, 0, sizeof key);
		key.idx = g_src_i; key.serial = g_serial_seen; key.kx = kx; key.ky = ky;
		key.sx = r.sx; key.sy = r.sy; key.sw = r.sw; key.sh = r.sh;
		if (g_rtkey_valid && !memcmp(&key, &g_rtkey, sizeof key)) {
			g_rt_reused++;
		} else if (rt_scene(g_rt)) {
			gtex_read(*src);
			filt(src->t, SCE_GXM_TEXTURE_FILTER_POINT);
			draw_into_rt(src->t, RT_SIZE, RT_SIZE, r.sx, r.sy, r.sw, r.sh, r.sw * kx, r.sh * ky);
			vita2d_end_drawing();
			filt(src->t, SCE_GXM_TEXTURE_FILTER_LINEAR);
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
			vita2d_draw_texture_part_scale(g_rt.t, r.dx, r.dy, 0, 0, (float) (r.sw * kx), (float) (r.sh * ky),
			    r.dw / (r.sw * kx), r.dh / (r.sh * ky));
		} else {
			gtex_read(*src);
			filt(src->t, (ps.quality == PIC_SHARP && exact) ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
			vita2d_draw_texture_part_scale(src->t, r.dx, r.dy, (float) r.sx, (float) r.sy, (float) r.sw, (float) r.sh,
			    r.dw / r.sw, r.dh / r.sh);
			filt(src->t, SCE_GXM_TEXTURE_FILTER_LINEAR);
		}
		/* CRT scanlines and LCD grid: dark bands between picture lines / pixels, drawn as alpha rectangles
		 * (all bands of a kind in one draw call) */
		if (ps.scan > 0 || ps.grid > 0) {
			float period = r.dh / r.logical_rows;
			int rows = r.logical_rows;
			float th = period * (0.18f + 0.03f * (ps.scan ? ps.scan : ps.grid));
			if (th > period * 0.5f) th = period * 0.5f;
			if (th < 1.0f) th = 1.0f;
			unsigned col = band_alpha(ps.scan ? ps.scan : ps.grid) << 24;
			rect_batch_begin(rows);
			for (int i = 1; i < rows; i++)
				rect_batch_add(r.dx, r.dy + i * period - th * 0.5f, r.dw, th);
			rect_batch_draw(col);
			if (ps.grid > 0) {
				float cp = r.dw / r.sw;
				float cth = cp * (0.15f + 0.02f * ps.grid);
				if (cth < 1.0f) cth = 1.0f;
				unsigned ccol = band_alpha(ps.grid) << 24;
				rect_batch_begin(r.sw);
				for (int i = 1; i < r.sw; i++)
					rect_batch_add(r.dx + i * cp - cth * 0.5f, r.dy, cth, r.dh);
				rect_batch_draw(ccol);
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
/* Players. NES pad 1 is the Vita's own controls (port 0). Pad 2 is port 2 when a controller is paired
 * there, else port 1 on a handheld (external pads through ds34vita and the like); on a PS TV port 1 is
 * the same pad as port 0, so it is not used. Pads 3 and 4 are ports 3 and 4; they reach the game only
 * through the Four Score or the Famicom 4 player adapter (pause menu "Four Score", Auto turns it on for the
 * games known to use it). The ports are looked at once a second, so pads can be plugged in or removed
 * while playing.
 * Buttons: A is the console's Enter button (Cross on Western Vitas, Circle on Japanese ones), B the other
 * one; Triangle is turbo A and Square turbo B (15 presses a second). */

static int g_confirm_is_cross = 1;
static int g_pad_port[4] = { 0, -1, -1, -1 };
static int g_port_age;
static unsigned g_turbo_phase;   /* counts emulated frames */

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

/* One port: the NES buttons, and the raw buttons with L1/R1 folded into L/R (Ext2 reports the Vita's
 * own L and R as L1 and R1, and L2/R2 of a PS TV pad as the old L/R bits). Returns 0 when nobody is there. */
static int read_port(int port, unsigned *nes, unsigned *raw)
{
	SceCtrlData pad;
	memset(&pad, 0, sizeof pad);
	pad.lx = pad.ly = 128;
	*nes = 0; *raw = 0;
	if (port < 0) return 0;
	if (sceCtrlPeekBufferPositiveExt2(port, &pad, 1) < 0) return 0;
	unsigned b = pad.buttons, m = 0;
	if (b & SCE_CTRL_L1) b |= SCE_CTRL_LTRIGGER;
	if (b & SCE_CTRL_R1) b |= SCE_CTRL_RTRIGGER;
	*raw = b;
	const unsigned enter = g_confirm_is_cross ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	const unsigned other = g_confirm_is_cross ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
	const bool turbo_on = ((g_turbo_phase >> 1) & 1) == 0;       /* 2 frames down, 2 frames up */
	if (b & enter) m |= NES_A;
	if (b & other) m |= NES_B;
	if ((b & SCE_CTRL_TRIANGLE) && turbo_on) m |= NES_A;
	if ((b & SCE_CTRL_SQUARE) && turbo_on) m |= NES_B;
	if (b & SCE_CTRL_START) m |= NES_START;
	if (b & SCE_CTRL_SELECT) m |= NES_SELECT;
	if (b & SCE_CTRL_UP) m |= NES_UP;
	if (b & SCE_CTRL_DOWN) m |= NES_DOWN;
	if (b & SCE_CTRL_LEFT) m |= NES_LEFT;
	if (b & SCE_CTRL_RIGHT) m |= NES_RIGHT;
	if (pad.lx < 64) m |= NES_LEFT;
	if (pad.lx > 192) m |= NES_RIGHT;
	if (pad.ly < 64) m |= NES_UP;
	if (pad.ly > 192) m |= NES_DOWN;
	*nes = m;
	return 1;
}

/* Reads every player; pad 1's raw buttons are returned for the menu combination. */
static void read_pads(unsigned masks[4], unsigned *raw0)
{
	ports_update();
	for (int i = 0; i < 4; i++) {
		unsigned raw;
		if (!read_port(g_pad_port[i], &masks[i], &raw)) masks[i] = 0;
		if (i == 0) *raw0 = raw;
	}
}

/* ---- battery RAM writer ------------------------------------------------
 * The .srm is written on a thread of its own: a write (temp file, read-back, rename, verified backup the
 * first time) takes a third of a second on a memory card and must not stall the game's frames. The game
 * thread only copies the RAM (nes_host_sram_take) and hands it over; one write at a time. A Disk System
 * game's "battery RAM" is the whole disk image (up to a few hundred KB). */
static uint8_t g_srm_buf[1024 * 1024];
static size_t g_srm_len;
static SceUID g_srm_sema = -1, g_srm_thread = -1;
static std::atomic<int> g_srm_busy(0), g_srm_result(0), g_srm_done(0), g_srm_stop(0), g_srm_sync(0);

static int srm_writer(SceSize, void *)
{
	for (;;) {
		sceKernelWaitSema(g_srm_sema, 1, NULL);
		if (g_srm_stop) break;
		g_srm_result = nes_host_sram_write(g_srm_buf, g_srm_len);
		g_srm_done++;
		g_srm_busy = 0;
	}
	return sceKernelExitThread(0);
}

static void srm_writer_start(void)
{
	g_srm_sema = sceKernelCreateSema("nes_srm", 0, 0, 1, NULL);
	if (g_srm_sema < 0) return;
	/* low priority, beside the picture thread: never in the game thread's way */
	g_srm_thread = sceKernelCreateThread("nes_srm", srm_writer, 0xA0, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (g_srm_thread >= 0 && sceKernelStartThread(g_srm_thread, 0, NULL) < 0) g_srm_thread = -1;
}

static void srm_wait_idle(void)
{
	while (g_srm_busy) sceKernelDelayThread(2000);
}

/* hand the current battery RAM to the writer; 0 when it is still busy */
static int srm_write_async(void)
{
	if (g_srm_thread < 0 || g_srm_busy) return 0;
	g_srm_len = nes_host_sram_take(g_srm_buf, sizeof g_srm_buf);
	if (!g_srm_len) return 0;
	g_srm_busy = 1;
	sceKernelSignalSema(g_srm_sema, 1);
	return 1;
}

/* write now, on this thread (pause menu, suspend, quit): after the writer */
static int srm_write_sync(void)
{
	srm_wait_idle();
	g_srm_sync = 1;
	int r = nes_host_sram_save();
	g_srm_sync = 0;
	return r;
}

/* mode 0: periodic (at most every 5 s); 1: now, but still not during a battery RAM write; 2: now, final */
static void log_flush(uint64_t now_us, int mode)
{
	if (!g_log || g_logbuf.empty()) return;
	if (mode < 2 && (g_srm_busy || g_srm_sync)) return;
	if (mode == 0 && now_us - g_log_flushed_us < 5000000) return;
	if (g_log_lost) {
		char m[64];
		int n = snprintf(m, sizeof m, "(log: %u lines lost, buffer full)\n", g_log_lost);
		g_logbuf.append(m, n);
		g_log_lost = 0;
	}
	fwrite(g_logbuf.data(), 1, g_logbuf.size(), g_log);
	fflush(g_log);
	g_logbuf.clear();
	g_log_flushed_us = now_us;
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

static bool ends_with_ci(const char *s, const char *tail)
{
	size_t a = strlen(s), b = strlen(tail);
	return a >= b && strcasecmp(s + a - b, tail) == 0;
}

static std::string base_name(const std::string &p)
{
	size_t s = p.find_last_of("/:");
	std::string n = s == std::string::npos ? p : p.substr(s + 1);
	size_t d = n.find_last_of('.');
	return d == std::string::npos ? n : n.substr(0, d);
}

static std::string srm_path(const std::string &rom)
{
	size_t d = rom.find_last_of('.'), s = rom.find_last_of("/:");
	if (d == std::string::npos || (s != std::string::npos && d < s)) return rom + ".srm";
	return rom.substr(0, d) + ".srm";
}

/* ---- settings files ----------------------------------------------------- */

static pic_settings g_ps;
static std::string g_game_ini;

static void settings_load(const std::string &game)
{
	pic_defaults(&g_ps);
	pic_load(DATA_ROOT "/picture.ini", &g_ps);
	g_game_ini = std::string(DATA_ROOT "/games/") + game + ".ini";
	pic_settings gs = g_ps;
	if (pic_load(g_game_ini.c_str(), &gs) && gs.use_game) g_ps = gs;
}

static void settings_save(void)
{
	int r;
	if (g_ps.use_game) {
		r = pic_save(g_game_ini.c_str(), &g_ps);
	} else {
		r = pic_save(DATA_ROOT "/picture.ini", &g_ps);
		pic_settings gs = g_ps;
		if (pic_load(g_game_ini.c_str(), &gs) && gs.use_game) { gs.use_game = 0; pic_save(g_game_ini.c_str(), &gs); }
	}
	logf_("settings saved rc=%d (%s)\n", r, g_ps.use_game ? "this game" : "all games");
}

/* ---- launch / return --------------------------------------------------- */

static void return_to_vitadhoc(void)
{
	const char *argv[] = { "--from-nes", NULL };   /* arguments only: the system adds the path */
	logf_("returning to VitAdhoc\n");
	log_flush(sceKernelGetProcessTimeWide(), 2);
	if (g_log) { fclose(g_log); g_log = NULL; }
	sceAppMgrLoadExec("app0:eboot.bin", (char *const *) argv, NULL);
	sceKernelExitProcess(0);
}

/* ---- pause menu -------------------------------------------------------- */

static volatile int g_suspend_flag;
static int power_cb(int, int info, int, void *)
{
	if (info & (SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND)) g_suspend_flag = 1;
	return 0;
}

static const char *kShape[] = { "Original shape", "Zoom", "Full screen", "Pixel perfect" };
static const char *kAuto3[] = { "Auto", "On", "Off" };
static const char *kRegion[] = { "Auto", "NTSC", "PAL", "Dendy" };

static void level_text(char *b, size_t n, int v) { if (v) snprintf(b, n, "%d", v); else snprintf(b, n, "Off"); }

struct Nav { unsigned prev = 0; unsigned up = 0, down = 0, left = 0, right = 0, conf = 0, back = 0; };
static void nav_poll(Nav &nv)
{
	unsigned nes, raw;
	read_port(0, &nes, &raw);
	unsigned press = raw & ~nv.prev;
	if (nv.prev == 0xffffffffu) press = 0; /* buttons held when the menu opened */
	nv.prev = raw;
	unsigned conf = g_confirm_is_cross ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	unsigned back = g_confirm_is_cross ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
	nv.up = press & SCE_CTRL_UP; nv.down = press & SCE_CTRL_DOWN;
	nv.left = press & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER); nv.right = press & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER);
	nv.conf = press & conf; nv.back = press & back;
}

static bool menu_frame(bool dim)
{
	if (!frame_draw(g_ps)) { sceDisplayWaitVblankStart(); return false; }
	if (dim) vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, 0xC0000000);
	return true;
}

/* The core and the picture follow the settings (after loading a game, and after the menus) */
static void apply_core_settings(void)
{
	nes_host_set_option("region", g_ps.region);
	nes_host_set_option("no_sprite_limit", g_ps.no_sprite_limit);
	nes_host_set_fourscore(g_ps.fourscore == 1 ? NES_4P_ON : g_ps.fourscore == 2 ? NES_4P_OFF : NES_4P_AUTO);
}

static void picture_menu(void)
{
	enum { R_SHAPE, R_PIXEL, R_QUALITY, R_GLOW, R_SCAN, R_GRID, R_OVERSCAN, R_SPRITES, R_APPLY, R_BACK, R_N };
	int sel = 0;
	Nav nv = { 0xffffffffu };
	for (;;) {
		nav_poll(nv);
		if (nv.up) sel = (sel + R_N - 1) % R_N;
		if (nv.down) sel = (sel + 1) % R_N;
		int d = nv.right ? 1 : nv.left ? -1 : 0;
		if (nv.conf && sel != R_BACK) d = 1;
		if (d) {
			switch (sel) {
			case R_SHAPE: g_ps.shape = (g_ps.shape + PIC_SHAPES + d) % PIC_SHAPES; break;
			case R_PIXEL: g_ps.pixel_sq ^= 1; break;
			case R_QUALITY: g_ps.quality ^= 1; break;
			case R_GLOW: g_ps.glow = (g_ps.glow + 11 + d) % 11; break;
			case R_SCAN: g_ps.scan = (g_ps.scan + 11 + d) % 11; break;
			case R_GRID: g_ps.grid = (g_ps.grid + 11 + d) % 11; break;
			case R_OVERSCAN: g_ps.overscan = (g_ps.overscan + 3 + d) % 3; break;
			case R_SPRITES: g_ps.no_sprite_limit ^= 1; apply_core_settings(); break;
			case R_APPLY: g_ps.use_game ^= 1; break;
			}
		}
		if (nv.back || (nv.conf && sel == R_BACK)) break;

		if (!menu_frame(false)) continue;
		vita2d_draw_rectangle(16, 16, 470, 40 + R_N * 40 + 50, 0xD0302820);
		text_draw(34, 56, "Picture", 0xFFFFFFFF);
		for (int i = 0; i < R_N; i++) {
			char line[96], v[24];
			switch (i) {
			case R_SHAPE: snprintf(line, sizeof line, "Shape: %s", kShape[g_ps.shape]); break;
			case R_PIXEL: snprintf(line, sizeof line, "Pixels: %s", g_ps.pixel_sq ? "square" : "8:7"); break;
			case R_QUALITY: snprintf(line, sizeof line, "Quality: %s", g_ps.quality ? "Smooth" : "Sharp"); break;
			case R_GLOW: level_text(v, sizeof v, g_ps.glow); snprintf(line, sizeof line, "Side glow: %s", v); break;
			case R_SCAN: level_text(v, sizeof v, g_ps.scan); snprintf(line, sizeof line, "CRT scanlines: %s", v); break;
			case R_GRID: level_text(v, sizeof v, g_ps.grid); snprintf(line, sizeof line, "LCD grid: %s", v); break;
			case R_OVERSCAN: snprintf(line, sizeof line, "Crop top/bottom 8 lines: %s", g_ps.overscan == PIC_OVERSCAN_AUTO ? "Auto (NTSC)" : g_ps.overscan == PIC_OVERSCAN_ON ? "On" : "Off"); break;
			case R_SPRITES: snprintf(line, sizeof line, "Sprite limit: %s", g_ps.no_sprite_limit ? "removed (no flicker)" : "kept (as the console)"); break;
			case R_APPLY: snprintf(line, sizeof line, "Apply to: %s", g_ps.use_game ? "this game" : "all games"); break;
			default: snprintf(line, sizeof line, "Back"); break;
			}
			text_draw(40, 104 + i * 40, line, i == sel ? 0xFFFFD040 : 0xFFE0E0E0);
		}
		text_draw(34, 104 + R_N * 40 + 6, "Left/Right changes a row", 0xFFB0B0B0);
		frame_finish();
	}
	settings_save();
}

enum { M_RESUME, M_PICTURE, M_PLAYERS, M_REGION, M_DISK, M_SAVE, M_LOAD, M_RESET, M_QUIT };

/* Returns 1 when the player chose Quit. */
static int pause_menu(const std::string &game, std::string &status)
{
	int rows[16], n = 0;
	rows[n++] = M_RESUME; rows[n++] = M_PICTURE; rows[n++] = M_PLAYERS; rows[n++] = M_REGION;
	if (nes_host_is_fds()) rows[n++] = M_DISK;
	rows[n++] = M_SAVE; rows[n++] = M_LOAD; rows[n++] = M_RESET; rows[n++] = M_QUIT;
	int sel = 0, slot = 0;
	Nav nv = { 0xffffffffu };
	int closing = 0;
	g_pace.set_paused(true);
	for (;;) {
		nav_poll(nv);
		if (nv.up) sel = (sel + n - 1) % n;
		if (nv.down) sel = (sel + 1) % n;
		const int id = rows[sel];
		if (id == M_PLAYERS) {
			int d = nv.right || nv.conf ? 1 : nv.left ? 2 : 0;
			if (d) { g_ps.fourscore = (g_ps.fourscore + d) % 3; apply_core_settings(); settings_save(); }
		} else if (id == M_REGION) {
			int d = nv.right || nv.conf ? 1 : nv.left ? 3 : 0;
			if (d) { g_ps.region = (g_ps.region + d) % 4; apply_core_settings(); settings_save(); }
		} else if (id == M_DISK) {
			if (nv.conf) { nes_host_fds_toggle_disk(); }
			else if (nv.left || nv.right) {
				int side, sides, ins;
				nes_host_fds_info(&side, &sides, &ins);
				if (ins) status = "Eject the disk first";
				else if (sides > 1) { nes_host_fds_next_side(); status = ""; }
			}
		} else {
			if (nv.left) slot = (slot + 3) % 4;
			if (nv.right) slot = (slot + 1) % 4;
		}
		if (nv.back) closing = 1;
		if (nv.conf && id != M_PLAYERS && id != M_REGION && id != M_DISK) {
			char path[256];
			snprintf(path, sizeof path, DATA_ROOT "/states/%s.st%d", game.c_str(), slot);
			if (id == M_RESUME) closing = 1;
			else if (id == M_PICTURE) { picture_menu(); nv.prev = 0xffffffffu; continue; }
			else if (id == M_SAVE) {
				int r = nes_host_state_save(path);
				status = r ? std::string("Could not save the state: ") + nes_host_last_error() : "State saved";
			} else if (id == M_LOAD) {
				int r = nes_host_state_load(path);
				status = r == 2 ? "No state in this slot" : r ? "Could not load the state" : "State loaded";
				if (r == 0) closing = 1;
			} else if (id == M_RESET) { nes_host_reset(); closing = 1; }
			else if (id == M_QUIT) { g_pace.set_paused(false); return 1; }
		}
		if (closing) break;

		if (!menu_frame(true)) continue;
		vita2d_draw_rectangle(280, 60, 400, 440, 0xE0302820);
		text_draw(310, 106, "NES - paused", 0xFFFFFFFF);
		for (int i = 0; i < n; i++) {
			char line[96];
			switch (rows[i]) {
			case M_SAVE: case M_LOAD: snprintf(line, sizeof line, "%s  (slot %d)", rows[i] == M_SAVE ? "Save state" : "Load state", slot + 1); break;
			case M_PLAYERS: snprintf(line, sizeof line, "Four Score: %s%s", kAuto3[g_ps.fourscore], nes_host_fourscore_active() ? " (4 players)" : ""); break;
			case M_REGION: snprintf(line, sizeof line, "Region: %s%s", kRegion[g_ps.region], g_ps.region ? "" : (nes_host_region() == NES_REGION_PAL ? " (PAL)" : nes_host_region() == NES_REGION_DENDY ? " (Dendy)" : " (NTSC)")); break;
			case M_DISK: {
				int side, sides, ins;
				nes_host_fds_info(&side, &sides, &ins);
				snprintf(line, sizeof line, "Disk: side %d/%d, %s", side + 1, sides, ins ? "inserted" : "ejected");
				break;
			}
			case M_RESUME: snprintf(line, sizeof line, "Resume"); break;
			case M_PICTURE: snprintf(line, sizeof line, "Picture"); break;
			case M_RESET: snprintf(line, sizeof line, "Reset game"); break;
			default: snprintf(line, sizeof line, "Quit to VitAdhoc"); break;
			}
			text_draw(320, 156 + i * 38, line, i == sel ? 0xFFFFD040 : 0xFFE0E0E0);
		}
		if (rows[sel] == M_DISK) text_draw(310, 156 + n * 38 + 4, "Cross: eject / insert   Left/Right: other side", 0xFFB0B0B0);
		if (!status.empty()) text_draw(310, 156 + n * 38 + 40, status.c_str(), 0xFFA0FFA0);
		frame_finish();
	}
	/* wait until the confirm / back buttons are released so the game does not see them */
	for (;;) {
		unsigned nes, raw;
		read_port(0, &nes, &raw);
		if (!(raw & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) break;
		sceKernelDelayThread(16000);
	}
	g_pace.set_paused(false);
	return 0;
}

static void fatal_screen(const char *line1, const char *line2)
{
	for (int i = 0; i < 300; i++) {
		vita2d_start_drawing();
		vita2d_clear_screen();
		text_draw(60, 200, line1, 0xFFFFFFFF);
		text_draw(60, 250, line2, 0xFFC0C0C0);
		text_draw(60, 320, "Returning to VitAdhoc...", 0xFFC0C0C0);
		vita2d_end_drawing();
		vita2d_swap_buffers();
	}
}

/* ---- main -------------------------------------------------------------- */

/* 64 bit hash of a byte range (battery RAM change detection; not cryptographic) */
static uint64_t hash64(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t w;
		memcpy(&w, p + i, 8);
		h = (h ^ w) * 0x100000001b3ull;
		h ^= h >> 29;
	}
	for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
	return h ^ (h >> 32);
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


int main(int argc, char **argv)
{
	mkdirs();
	g_log = fopen(DATA_ROOT "/log.txt", "a");
	logf_("---- nes.bin start (argc=%d)\n", argc);

	int clk = scePowerSetArmClockFrequency(500);
	if (clk < 0) { scePowerSetArmClockFrequency(444); }
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	logf_("arm clock now %d MHz (500 request rc=%08x)\n", scePowerGetArmClockFrequency(), clk);
	/* what the core really runs at (a PSVshell profile can set 500 MHz while the system still reports 444) */
	logf_("clocks: the ARM core measures %u MHz\n", arm_mhz_measured());
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	/* emulation on core 0, the sound thread on core 2, the save writer on core 1 */
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
	int enter = 1;
	if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter) >= 0)
		g_confirm_is_cross = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	SceUID pcb = sceKernelCreateCallback("nes_power", 0, power_cb, NULL);
	if (pcb >= 0) scePowerRegisterCallback(pcb);

	vita2d_init();
	vita2d_set_clear_color(0xFF000000);
	font_init();
	video_init();

	/* The game is the last argument that is not this program's own name. */
	const char *arg = NULL;
	for (int i = argc - 1; i >= 1 && !arg; i--)
		if (argv[i] && argv[i][0] && !ends_with_ci(argv[i], "nes.bin")) arg = argv[i];
	logf_("arguments: %d, game argument: %s\n", argc, arg ? arg : "(none)");
	if (!arg) {
		logf_("no ROM path given\n");
		fatal_screen("No game was given.", "Start NES games from VitAdhoc.");
		return_to_vitadhoc();
	}
	std::string rom = arg;
	std::string srm_override;
	{
		size_t bar = rom.find('|'); /* "rom|save path": a zipped game keeps its save beside the zip */
		if (bar != std::string::npos) { srm_override = rom.substr(bar + 1); rom.resize(bar); }
	}
	std::string game = base_name(rom);
	logf_("rom: %s\n", rom.c_str());
	settings_load(game);
	bezel_load(game);

	const char *tini = DATA_ROOT "/threads.ini";
	const int audio_prio = ini_int(tini, "audio_prio", PRIO_AUDIO_DEFAULT), main_prio = ini_int(tini, "main_prio", PRIO_MAIN_DEFAULT);
	const bool audio_legacy = ini_int(tini, "audio_legacy", 0) != 0;
	if (main_prio > 0) {
		int rc = sceKernelChangeThreadPriority(0, main_prio);
		logf_("main thread priority 0x%x rc=%08x now 0x%x\n", main_prio, rc, sceKernelGetThreadCurrentPriority());
	}

	char err[160] = "";
	nes_host_set_backup_dir(BACKUP_DIR);
	nes_host_set_system_dir(DATA_ROOT);        /* disksys.rom (Disk System BIOS), supplied by the user */
	if (nes_host_init()) {
		logf_("core init failed\n");
		fatal_screen("The NES core could not be started.", "");
		return_to_vitadhoc();
	}
	apply_core_settings();
	if (nes_host_load_rom(rom.c_str(), (srm_override.empty() ? srm_path(rom) : srm_override).c_str(), err, sizeof err)) {
		logf_("load failed: %s\n", err);
		fatal_screen("This game could not be started.", err);
		return_to_vitadhoc();
	}
	logf_("cartridge: %s\n", nes_host_rom_info());
	logf_("title: %s  region=%d (detected %d)  fps=%.4f  sram=%u  fourscore=%d\n", nes_host_rom_title(), nes_host_region(),
	    nes_host_region_detected(), nes_host_fps(), (unsigned) nes_host_sram_size(), nes_host_fourscore_active());
	logf_("picture: shape=%d quality=%d glow=%d scan=%d grid=%d overscan=%d nosprlimit=%d fs=%d region=%d own=%d\n", g_ps.shape, g_ps.quality,
	    g_ps.glow, g_ps.scan, g_ps.grid, g_ps.overscan, g_ps.no_sprite_limit, g_ps.fourscore, g_ps.region, g_ps.use_game);

	audio_init(audio_legacy, audio_prio > 0 ? audio_prio : PRIO_AUDIO_DEFAULT);

	const double vita_hz = 59.94;
	/* One emulated frame per display refresh for 60 Hz games (the console runs at 60.0988 Hz; the audio rate
	 * control absorbs the 0.265% difference). A 50 Hz game (PAL, Dendy) gets 5 emulated frames in 6 refreshes:
	 * the accumulator below decides, and the other refresh shows the same picture again. */
	double acc = 1.0;
	std::string status;

	/* log state */
	uint64_t sec_start = sceKernelGetProcessTimeWide();
	int frames_run = 0, vblanks = 0, secs = 0;
	uint64_t emu_us = 0, main_us = 0, fetch_us = 0, draw_us = 0, finish_us = 0, vwait_us = 0, emu_worst_us = 0;
	uint64_t sram_dirty_since = 0;
	unsigned drawn = 0, skipped = 0;
	srm_writer_start();
	unsigned last_vc = sceDisplayGetVcount();
	apace::SpeedMeter speed_meter(8);
	bool stats_pending = false;
	uint32_t ur_seen = 0, ur_sec_total = 0;
	uint64_t prev_loop0 = 0;
	unsigned w16 = 0, w20 = 0, w25 = 0;
	uint64_t stall_max_us = 0, work_max_us = 0;
	unsigned s_w16 = 0, s_w20 = 0, s_w25 = 0;
	double s_stall = 0, s_work = 0, s_agap = 0;
	unsigned sram_pass = 0;
	uint64_t sram_hash = 0, sram_last_change = 0, sram_last_write = 0;
	bool sram_hash_valid = false, sram_dirty = false, sram_recheck = true;
	log_flush(sceKernelGetProcessTimeWide(), 1);

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
		if (elapsed == 0) elapsed = 1;
		if (elapsed > 2) elapsed = 2;
		uint64_t loop0 = sceKernelGetProcessTimeWide();
		if (prev_loop0 && loop0 - prev_loop0 > stall_max_us) stall_max_us = loop0 - prev_loop0;
		prev_loop0 = loop0;
		unsigned raw, masks[4];
		read_pads(masks, &raw);
		if ((raw & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) ==
		    (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) {
			if (nes_host_sram_dirty()) srm_write_sync();
			log_flush(sceKernelGetProcessTimeWide(), 1);
			int quit = pause_menu(game, status);
			if (quit) break;
			last_vc = sceDisplayGetVcount();
			/* the second that held the menu says nothing about the emulation's speed */
			speed_meter.reset();
			prev_loop0 = 0;
			sec_start = sceKernelGetProcessTimeWide();
			frames_run = vblanks = 0;
			sram_recheck = true;
			continue;
		}
		for (int i = 0; i < 4; i++) nes_host_set_pad(i, masks[i]);
		/* a game being played keeps the console awake */
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

		const double frame_hz = nes_host_fps();
		acc += frame_hz / vita_hz;
		if (acc > 2.0) acc = 2.0;
		bool fresh = false;
		if (acc >= 1.0) {
			acc -= 1.0;
			if (acc > 1.0) acc = 1.0;
			uint64_t t0 = sceKernelGetProcessTimeWide();
			nes_host_run_frame();
			uint64_t t1 = sceKernelGetProcessTimeWide();
			emu_us += t1 - t0;
			if (t1 - t0 > emu_worst_us) emu_worst_us = t1 - t0;
			frames_run++;
			g_turbo_phase++;
			static int16_t abuf[2048 * 2];
			int n;
			while ((n = nes_host_audio_take(abuf, 2048)) > 0) audio_push(abuf, n);
			uint64_t f0 = sceKernelGetProcessTimeWide();
			fresh = video_fetch() != 0;
			fetch_us += sceKernelGetProcessTimeWide() - f0;
		}
		(void) fresh;

		g_pace.legacy_update();

		uint64_t d0 = sceKernelGetProcessTimeWide();
		bool drew = frame_draw(g_ps);
		draw_us += sceKernelGetProcessTimeWide() - d0;
		uint64_t work_end = sceKernelGetProcessTimeWide();
		if (drew) {                                     /* queued; the next pass waits for the display */
			frame_finish();
			drawn++;
			finish_us += sceKernelGetProcessTimeWide() - work_end;
		}
		else skipped++;
		vblanks += elapsed;
		main_us += work_end - loop0;

		/* battery RAM: 2 s after it changed and settled, on the writer thread; at most every 30 s for games
		 * that keep changing it; at once, here, before suspend */
		uint64_t now = sceKernelGetProcessTimeWide();
		{
			static int srm_done_seen;
			int d = g_srm_done;
			if (d != srm_done_seen) {
				srm_done_seen = d;
				int r = g_srm_result;
				if (r) nes_host_sram_forget();   /* failed: still to be written */
				sram_recheck = true;
				if (r || d <= 3 || d % 50 == 0) logf_("sram save rc=%d (write %d)%s%s\n", r, d, r ? " " : "", r ? nes_host_last_error() : "");
			}
		}
		if (g_suspend_flag) {
			if (nes_host_sram_dirty()) logf_("sram save before suspend rc=%d\n", srm_write_sync());
			sram_dirty_since = 0;
			sram_recheck = true;
		} else {
			/* change detection without comparing the whole RAM every pass: a hash of it every 8th pass */
			if ((++sram_pass & 7) == 0 || sram_recheck) {
				const size_t sz = nes_host_sram_size();
				const bool recheck = sram_recheck;
				sram_recheck = false;
				if (sz) {
					const uint64_t h = hash64(nes_host_sram_data(), sz);
					const bool moved = !sram_hash_valid || h != sram_hash;
					if (moved) { sram_hash = h; sram_hash_valid = true; sram_last_change = now; }
					if (moved || recheck) sram_dirty = nes_host_sram_dirty() != 0;
				} else sram_dirty = false;
			}
			if (sram_dirty) {
				if (!sram_dirty_since) sram_dirty_since = now;
				if (now - sram_dirty_since > 2000000 &&
				    (now - sram_last_change > 1000000 || now - sram_last_write > 30000000) && srm_write_async()) {
					sram_dirty_since = 0;
					sram_last_write = now;
					sram_recheck = true;
				}
			} else sram_dirty_since = 0;
		}
		g_suspend_flag = 0;

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
				    u - ur_seen, e.out_frames / (double) OUT_RATE, e.ema_ms, e.target_ms, e.ratio_ppm, g_pace.fill_frames() * 1000 / NES_AUDIO_RATE,
				    work / 1000.0);
				ur_sec_total += u - ur_seen;
				ur_seen = u;
			}
		}

		if (now - sec_start >= 1000000) {
			double dt = (now - sec_start) / 1e6;
			{
				/* emulation speed for the audio pacer: console frames per second over the last 8 s */
				const double sp = speed_meter.update(frames_run, dt, frame_hz);
				if (sp >= 0) g_pace.report_speed(sp);
				s_w16 = w16; s_w20 = w20; s_w25 = w25; s_stall = stall_max_us / 1000.0; s_work = work_max_us / 1000.0;
				s_agap = g_agap_max_us.exchange(0) / 1000.0;
				w16 = w20 = w25 = 0; stall_max_us = work_max_us = 0;
				stats_pending = true;
			}
			double fr = frames_run ? frames_run : 1;
			/* the per-second line: display rate, emulated rate, frame time average / worst, audio underruns this second */
			logf_("t=%d fps=%.1f emu_fps=%.1f frame_ms avg=%.2f worst=%.2f ur=%u aq=%u | main ms/fr=%.2f copy=%.2f gpu_submit=%.2f | drawn=%u skipped=%u fence_skip=%u rt_reuse=%u | finish=%.1f vwait=%.1f ms\n",
			    secs, vblanks / dt, frames_run / dt, emu_us / 1000.0 / fr, emu_worst_us / 1000.0, ur_sec_total, g_pace.fill_frames(),
			    main_us / 1000.0 / fr, fetch_us / 1000.0 / fr, draw_us / 1000.0 / fr, drawn, skipped, g_fence_skips, g_rt_reused,
			    finish_us / 1000.0, vwait_us / 1000.0);
			sec_start = now;
			vblanks = frames_run = 0;
			emu_us = main_us = fetch_us = draw_us = finish_us = vwait_us = emu_worst_us = 0;
			drawn = skipped = 0;
			ur_sec_total = 0;
			secs++;
			char m[160];
			while (nes_host_take_message(m, sizeof m)) logf_("core: %s\n", m);
		}

		/* one compact audio / pacing line a second (the counters of the audio thread's last second) */
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
		log_flush(sceKernelGetProcessTimeWide(), 0);
	}

	g_quit = 1;
	settings_save();
	srm_wait_idle();
	if (nes_host_sram_dirty()) logf_("final sram save rc=%d\n", nes_host_sram_save());
	if (g_srm_thread >= 0) { g_srm_stop = 1; sceKernelSignalSema(g_srm_sema, 1); sceKernelWaitThreadEnd(g_srm_thread, NULL, NULL); sceKernelDeleteThread(g_srm_thread); }
	if (g_athread >= 0) sceKernelWaitThreadEnd(g_athread, NULL, NULL);
	if (g_aport >= 0) sceAudioOutReleasePort(g_aport);
	nes_host_shutdown();
	return_to_vitadhoc();
	return 0;
}
