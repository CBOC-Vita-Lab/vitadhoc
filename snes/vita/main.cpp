/*
 * snes.bin: the SNES program of VitAdhoc (Snes9x core, separate executable).
 *
 * Launched by VitAdhoc with sceAppMgrLoadExec("app0:snes.bin", {rom path}).
 * Quitting launches app0:eboot.bin again. This file and the other files in
 * snes/frontend are new code (MIT); the Snes9x core in snes/core keeps its
 * own non-commercial licence. Nothing from gpsp/ is linked into this program.
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
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <vita2d.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "host.h"
#include "picture.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "s9x_prof.h"

extern "C" {
unsigned int sceUserMainThreadStackSize = 1024 * 1024;
unsigned int _newlib_heap_size_user = 56 * 1024 * 1024;
}

extern "C" const unsigned char snes_font_data[];
extern "C" const unsigned char snes_font_data_end[];

#define DATA_ROOT "ux0:data/GBAdhoc/snes"
#define SCREEN_W 960
#define SCREEN_H 544

/* ---- log --------------------------------------------------------------- */

static FILE *g_log;
static void logf_(const char *fmt, ...)
{
	if (!g_log) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fflush(g_log);
}

static void mkdirs(void)
{
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir("ux0:data/GBAdhoc", 0777);
	sceIoMkdir(DATA_ROOT, 0777);
	sceIoMkdir(DATA_ROOT "/states", 0777);
	sceIoMkdir("ux0:data/GBAdhoc/backups", 0777);
	sceIoMkdir("ux0:data/GBAdhoc/backups/snes", 0777);
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
	int rc = stbtt_BakeFontBitmap(snes_font_data, 0, FONT_PX, bitmap, FONT_W, FONT_H, 32, 96, g_baked);
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

static float text_width(const char *s)
{
	float w = 0;
	for (; *s; s++) if (*s >= 32 && *s < 128) w += g_baked[*s - 32].xadvance;
	return w;
}

static void text_draw(float x, float y, const char *s, unsigned color)
{
	if (!g_font_tex) return;
	/* white glyph texture tinted with color via the tint program */
	for (; *s; s++) {
		if (*s < 32 || *s >= 128) continue;
		stbtt_aligned_quad q;
		float px = 0, py = 0;
		stbtt_GetBakedQuad(g_baked, FONT_W, FONT_H, *s - 32, &px, &py, &q, 1);
		float w = q.x1 - q.x0, h = q.y1 - q.y0;
		if (w > 0 && h > 0)
			vita2d_draw_texture_tint_part_scale(g_font_tex, x + q.x0, y + q.y0,
			    q.s0 * FONT_W, q.t0 * FONT_H, w, h, 1.0f, 1.0f, color);
		x += px;
	}
}

/* ---- audio ------------------------------------------------------------- */

static const int OUT_RATE = 48000, OUT_GRAIN = 1024;
static int g_aport = -1;
static SceUID g_athread = -1, g_amutex = -1;
static volatile int g_quit;
static int16_t g_q[32768 * 2]; /* 32040 Hz stereo queue (frames) */
static volatile unsigned g_q_r, g_q_w; /* frame counters */
static volatile int g_paused;
static double g_ratio_adj = 1.0;
static double g_frac;

static unsigned q_fill(void) { return g_q_w - g_q_r; }

static void audio_push(const int16_t *src, int frames)
{
	for (int i = 0; i < frames; i++) {
		if (q_fill() >= 32768 - 1) break; /* full: drop new audio */
		unsigned w = g_q_w & 32767;
		g_q[w * 2] = src[i * 2];
		g_q[w * 2 + 1] = src[i * 2 + 1];
		__sync_synchronize();
		g_q_w++;
	}
}

static int audio_thread(SceSize, void *)
{
	static int16_t out[OUT_GRAIN * 2];
	int16_t last[2] = { 0, 0 };
	while (!g_quit) {
		double step = (double) SNES_AUDIO_RATE / OUT_RATE * g_ratio_adj;
		for (int i = 0; i < OUT_GRAIN; i++) {
			if (g_paused || q_fill() < 3) { out[i * 2] = last[0]; out[i * 2 + 1] = last[1]; continue; }
			unsigned r = g_q_r & 32767, r1 = (g_q_r + 1) & 32767;
			double f = g_frac;
			out[i * 2] = (int16_t) (g_q[r * 2] * (1 - f) + g_q[r1 * 2] * f);
			out[i * 2 + 1] = (int16_t) (g_q[r * 2 + 1] * (1 - f) + g_q[r1 * 2 + 1] * f);
			last[0] = out[i * 2]; last[1] = out[i * 2 + 1];
			g_frac += step;
			unsigned adv = (unsigned) g_frac;
			g_frac -= adv;
			g_q_r += adv;
		}
		sceAudioOutOutput(g_aport, out);
	}
	return 0;
}

static void audio_init(void)
{
	g_aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
	if (g_aport < 0) { logf_("audio open failed %08x\n", g_aport); return; }
	int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume(g_aport, (SceAudioOutChannelFlag) (SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
	g_athread = sceKernelCreateThread("snes_audio", audio_thread, 0x50, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (g_athread >= 0) sceKernelStartThread(g_athread, 0, NULL);
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
static GTex g_rt, g_glowA, g_glowB, g_blend;   /* g_blend: 512x512, made when a blend option is first used */
static unsigned g_blend_serial;
static int g_blend_w, g_blend_h;
static GTex g_bezel;
static int g_bezel_w, g_bezel_h;
static int g_src_i = -1;         /* newest uploaded texture */
static int g_src_w = 256, g_src_h = 224;
static unsigned g_serial_seen;
static int g_crop_top, g_crop_bot, g_crop_cand_top, g_crop_cand_bot, g_crop_votes;
static int g_have_picture;
static unsigned g_fence_skips, g_frames_skipped_gpu;

static bool gtex_writable(GTex &g) { return g.t && vita2d_fragment_fence_done(&g.rd); }
static void gtex_read(GTex &g) { if (g.t) vita2d_fragment_fence_record(&g.rd); }

static void video_init(void)
{
	for (int i = 0; i < 4; i++) {
		memset(&g_src[i], 0, sizeof g_src[i]);
		g_src[i].t = vita2d_create_empty_texture_format(512, 512, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
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
static int video_fetch(const pic_settings &ps, int frame_no)
{
	int w = g_src_w, h = g_src_h, pitch = 0;
	unsigned serial = 0;
	const uint16_t *src = snes_host_video_nowait(&w, &h, &pitch, &serial);
	if (!src || serial == g_serial_seen) return 0;
	int idx = -1;
	for (int n = 1; n <= 4; n++) {
		int c = (g_src_i + n) % 4;
		if (gtex_writable(g_src[c])) { idx = c; break; }
	}
	if (idx < 0) { g_fence_skips++; return 0; }          /* the GPU is behind: show the old picture again */
	g_serial_seen = serial;
	if (w > 512) w = 512;
	if (h > 512) h = 512;
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
	if (w != g_src_w || h != g_src_h) { g_crop_top = g_crop_bot = g_crop_cand_top = g_crop_cand_bot = 0; }
	g_src_i = idx; g_src_w = w; g_src_h = h;
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
 * (the display queue is full): the caller then skips this frame. `extras` = 0 draws without the picture
 * options (not used by the game loop: the governor never drops them). */
static bool frame_draw(const pic_settings &ps, int extras, int frame_no)
{
	vita2d_pool_reset();
	pic_rect r;
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
	if (src && g_have_picture)
		pic_geometry(&ps, eff_w, eff_h, crop_top, crop_bot, SCREEN_W, SCREEN_H, &r);
	bool bezel = g_bezel.t != NULL;
	int kx = 1, ky = 1;
	bool use_rt = false, exact = false;
	if (src && g_have_picture) {
		pic_prescale(&r, &kx, &ky);
		exact = fabsf(r.dw - r.sw * kx) < 0.5f && fabsf(r.dh - r.sh * ky) < 0.5f && kx == ky;
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
		key.idx = (src == &g_blend) ? -2 : g_src_i; key.serial = g_serial_seen; key.kx = kx; key.ky = ky;
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
 * through the Super Multitap (pause menu "Multitap", Auto turns it on for games known to use it).
 * The ports are looked at once a second, so pads can be plugged in or removed while playing. */

static int g_confirm_is_cross = 1;
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

/* One port: the SNES buttons, and the raw buttons with L1/R1 folded into L/R (Ext2 reports the Vita's
 * own L and R as L1 and R1, and L2/R2 of a PS TV pad as the old L/R bits). Returns 0 when nobody is there. */
static int read_port(int port, unsigned *snes, unsigned *raw)
{
	SceCtrlData pad;
	memset(&pad, 0, sizeof pad);
	pad.lx = pad.ly = 128;
	*snes = 0; *raw = 0;
	if (port < 0) return 0;
	if (sceCtrlPeekBufferPositiveExt2(port, &pad, 1) < 0) return 0;
	unsigned b = pad.buttons, m = 0;
	if (b & SCE_CTRL_L1) b |= SCE_CTRL_LTRIGGER;
	if (b & SCE_CTRL_R1) b |= SCE_CTRL_RTRIGGER;
	*raw = b;
	if (b & SCE_CTRL_CROSS) m |= SNES_B;
	if (b & SCE_CTRL_CIRCLE) m |= SNES_A;
	if (b & SCE_CTRL_SQUARE) m |= SNES_Y;
	if (b & SCE_CTRL_TRIANGLE) m |= SNES_X;
	if (b & SCE_CTRL_LTRIGGER) m |= SNES_L;
	if (b & SCE_CTRL_RTRIGGER) m |= SNES_R;
	if (b & SCE_CTRL_START) m |= SNES_START;
	if (b & SCE_CTRL_SELECT) m |= SNES_SELECT;
	if (b & SCE_CTRL_UP) m |= SNES_UP;
	if (b & SCE_CTRL_DOWN) m |= SNES_DOWN;
	if (b & SCE_CTRL_LEFT) m |= SNES_LEFT;
	if (b & SCE_CTRL_RIGHT) m |= SNES_RIGHT;
	if (pad.lx < 64) m |= SNES_LEFT;
	if (pad.lx > 192) m |= SNES_RIGHT;
	if (pad.ly < 64) m |= SNES_UP;
	if (pad.ly > 192) m |= SNES_DOWN;
	*snes = m;
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

static const char *kMultitapGames[] = { "BOMBERMAN", "SECRET OF MANA", "SEIKEN DENSETSU 3", "NBA JAM", "NBA GIVE", "NHL ", "FIFA", "MADDEN", NULL };

static int multitap_wanted(const pic_settings &ps)
{
	if (ps.multitap == 1) return 1;
	if (ps.multitap == 2) return 0;
	std::string t = snes_host_rom_title();
	for (auto &c : t) if (c >= 'a' && c <= 'z') c -= 32;
	for (int i = 0; kMultitapGames[i]; i++)
		if (t.find(kMultitapGames[i]) != std::string::npos) return 1;
	return 0;
}

/* ---- battery RAM writer ------------------------------------------------
 * The .srm is written on a thread of its own: a write (file, .bak, rename) takes
 * a third of a second on a memory card, and Super FX games such as Yoshi's
 * Island change their battery RAM all the time, so writing it on the game's
 * thread every 2 s was a visible stutter. The game thread only copies the RAM
 * (snes_host_sram_take) and hands it over; one write at a time. */
static uint8_t g_srm_buf[256 * 1024];
static size_t g_srm_len;
static SceUID g_srm_sema = -1, g_srm_thread = -1;
static std::atomic<int> g_srm_busy(0), g_srm_result(0), g_srm_done(0), g_srm_stop(0);

static int srm_writer(SceSize, void *)
{
	for (;;) {
		sceKernelWaitSema(g_srm_sema, 1, NULL);
		if (g_srm_stop) break;
		g_srm_result = snes_host_sram_write(g_srm_buf, g_srm_len);
		g_srm_done++;
		g_srm_busy = 0;
	}
	return sceKernelExitThread(0);
}

static void srm_writer_start(void)
{
	g_srm_sema = sceKernelCreateSema("snes_srm", 0, 0, 1, NULL);
	if (g_srm_sema < 0) return;
	g_srm_thread = sceKernelCreateThread("snes_srm", srm_writer, 0x60, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
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
	g_srm_len = snes_host_sram_take(g_srm_buf, sizeof g_srm_buf);
	if (!g_srm_len) return 0;
	g_srm_busy = 1;
	sceKernelSignalSema(g_srm_sema, 1);
	return 1;
}

/* write now, on this thread (pause menu, suspend, quit): after the writer */
static int srm_write_sync(void)
{
	srm_wait_idle();
	return snes_host_sram_save();
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

/* ---- launch / return --------------------------------------------------- */

static void return_to_vitadhoc(void)
{
	const char *argv[] = { "--from-snes", NULL };   /* arguments only: the system adds the path */
	logf_("returning to VitAdhoc\n");
	if (g_log) { fclose(g_log); g_log = NULL; }
	sceAppMgrLoadExec("app0:eboot.bin", (char *const *) argv, NULL);
	sceKernelExitProcess(0);
}

/* ---- paths ------------------------------------------------------------- */

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

/* ---- pause menu -------------------------------------------------------- */

static volatile int g_suspend_flag;
static int power_cb(int, int info, int, void *)
{
	if (info & (SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND)) g_suspend_flag = 1;
	return 0;
}

static const char *kMenu[] = { "Resume", "Picture", "Multitap", "Save state", "Load state", "Reset game", "Quit to VitAdhoc" };
enum { M_N = 7 };
static const char *kShape[] = { "Original shape", "Zoom", "Full screen", "Pixel perfect" };
static const char *kMulti[] = { "Auto", "On", "Off" };

static void level_text(char *b, size_t n, int v) { if (v) snprintf(b, n, "%d", v); else snprintf(b, n, "Off"); }

struct Nav { unsigned prev; unsigned up, down, left, right, conf, back; };
static void nav_poll(Nav &nv)
{
	unsigned snes, raw;
	read_port(0, &snes, &raw);
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
	if (!frame_draw(g_ps, 1, 0)) { sceDisplayWaitVblankStart(); return false; }
	if (dim) vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, 0xC0000000);
	return true;
}

static void picture_menu(void)
{
	enum { R_SHAPE, R_PIXEL, R_QUALITY, R_GLOW, R_SCAN, R_GRID, R_HIRES, R_ILACE, R_APPLY, R_BACK, R_N };
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
			case R_HIRES: g_ps.hires_blend ^= 1; break;
			case R_ILACE: g_ps.interlace_blend ^= 1; break;
			case R_APPLY: g_ps.use_game ^= 1; break;
			}
		}
		if (nv.back || (nv.conf && sel == R_BACK)) break;

		if (!menu_frame(false)) continue;
		vita2d_draw_rectangle(16, 16, 400, 40 + R_N * 40 + 50, 0xD0302820);
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
			case R_HIRES: snprintf(line, sizeof line, "Blend hi-res columns: %s", g_ps.hires_blend ? "On" : "Off"); break;
			case R_ILACE: snprintf(line, sizeof line, "Blend interlace lines: %s", g_ps.interlace_blend ? "On" : "Off"); break;
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

/* Returns 1 when the player chose Quit. */
static int pause_menu(const std::string &game, std::string &status)
{
	int sel = 0, slot = 0;
	Nav nv = { 0xffffffffu };
	int closing = 0;
	g_paused = 1;
	for (;;) {
		nav_poll(nv);
		if (nv.up) sel = (sel + M_N - 1) % M_N;
		if (nv.down) sel = (sel + 1) % M_N;
		if (sel == 2) {
			if (nv.left) g_ps.multitap = (g_ps.multitap + 2) % 3;
			if (nv.right || nv.conf) g_ps.multitap = (g_ps.multitap + 1) % 3;
			if (nv.left || nv.right || nv.conf) { snes_host_set_multitap(multitap_wanted(g_ps)); settings_save(); }
		} else {
			if (nv.left) slot = (slot + 3) % 4;
			if (nv.right) slot = (slot + 1) % 4;
		}
		if (nv.back) closing = 1;
		if (nv.conf && sel != 2) {
			char path[256];
			snprintf(path, sizeof path, DATA_ROOT "/states/%s.st%d", game.c_str(), slot);
			if (sel == 0) closing = 1;
			else if (sel == 1) { picture_menu(); nv.prev = 0xffffffffu; continue; }
			else if (sel == 3) {
				int r = snes_host_state_save(path);
				status = r ? "Could not save the state" : "State saved";
			} else if (sel == 4) {
				int r = snes_host_state_load(path);
				status = r == 2 ? "No state in this slot" : r ? "Could not load the state" : "State loaded";
				if (r == 0) closing = 1;
			} else if (sel == 5) { snes_host_reset(); closing = 1; }
			else if (sel == 6) { g_paused = 0; return 1; }
		}
		if (closing) break;

		if (!menu_frame(true)) continue;
		vita2d_draw_rectangle(300, 90, 360, 380, 0xE0302820);
		text_draw(330, 140, "SNES - paused", 0xFFFFFFFF);
		for (int i = 0; i < M_N; i++) {
			char line[64];
			if (i == 3 || i == 4) snprintf(line, sizeof line, "%s  (slot %d)", kMenu[i], slot + 1);
			else if (i == 2) snprintf(line, sizeof line, "Multitap: %s", kMulti[g_ps.multitap]);
			else snprintf(line, sizeof line, "%s", kMenu[i]);
			text_draw(340, 190 + i * 40, line, i == sel ? 0xFFFFD040 : 0xFFE0E0E0);
		}
		if (!status.empty()) text_draw(330, 470, status.c_str(), 0xFFA0FFA0);
		frame_finish();
	}
	/* wait until the confirm / back buttons are released so the game does not see them */
	for (;;) {
		unsigned snes, raw;
		read_port(0, &snes, &raw);
		if (!(raw & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) break;
		sceKernelDelayThread(16000);
	}
	g_paused = 0;
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
	logf_("---- snes.bin start (argc=%d)\n", argc);

	int clk = scePowerSetArmClockFrequency(500);
	if (clk < 0) { scePowerSetArmClockFrequency(444); }
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	logf_("arm clock now %d MHz (500 request rc=%08x)\n", scePowerGetArmClockFrequency(), clk);
	/* what the core really runs at (a PSVshell profile can set 500 MHz while
	 * the system still reports 444): the same count as VitAdhoc's cpuclock */
	logf_("clocks: the ARM core measures %u MHz\n", arm_mhz_measured());
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
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
	int enter = 1;
	if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter) >= 0)
		g_confirm_is_cross = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	SceUID pcb = sceKernelCreateCallback("snes_power", 0, power_cb, NULL);
	if (pcb >= 0) scePowerRegisterCallback(pcb);

	vita2d_init();
	vita2d_set_clear_color(0xFF000000);
	font_init();
	video_init();

	/* The game is the last argument. (VitAdhoc 1.3's first builds also passed
	 * the program's own name in front of it: "snes.bin" is never a game.) */
	const char *arg = NULL;
	for (int i = argc - 1; i >= 1 && !arg; i--)
		if (argv[i] && argv[i][0] && !ends_with_ci(argv[i], "snes.bin")) arg = argv[i];
	logf_("arguments: %d, game argument: %s\n", argc, arg ? arg : "(none)");
	if (!arg) {
		logf_("no ROM path given\n");
		fatal_screen("No game was given.", "Start SNES games from VitAdhoc.");
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
	sceIoMkdir(DATA_ROOT "/games", 0777);
	settings_load(game);
	bezel_load(game);

	/* worker threads: switches in threads.ini (apu_thread, ppu_thread, spin); on by default */
	const char *tini = DATA_ROOT "/threads.ini";
	int want_apu = ini_int(tini, "apu_thread", 1), want_ppu = ini_int(tini, "ppu_thread", 1);
	int spin = ini_int(tini, "spin", 300);
	snes_host_thread_config(2, 0x60, 1, 0x60, spin);
	snes_host_set_option("apu_thread", want_apu);
	snes_host_set_option("ppu_thread", want_ppu);

	char err[160] = "";
	snes_host_set_backup_dir("ux0:data/GBAdhoc/backups/snes");
	if (snes_host_init() || snes_host_load_rom(rom.c_str(), (srm_override.empty() ? srm_path(rom) : srm_override).c_str(), err, sizeof err)) {
		logf_("load failed: %s\n", err);
		fatal_screen("This game could not be started.", err);
		return_to_vitadhoc();
	}
	logf_("title: %s  pal=%d  sram=%u  threads=%d (apu %d ppu %d spin %d)\n", snes_host_rom_title(), snes_host_is_pal(),
	    (unsigned) snes_host_sram_size(), snes_host_threads_active(), want_apu, want_ppu, spin);
	logf_("picture: shape=%d quality=%d glow=%d scan=%d grid=%d multitap=%d own=%d\n", g_ps.shape, g_ps.quality, g_ps.glow,
	    g_ps.scan, g_ps.grid, g_ps.multitap, g_ps.use_game);
	snes_host_set_multitap(multitap_wanted(g_ps));

	audio_init();

	const double frame_hz = snes_host_is_pal() ? 50.0 : 60.0988;
	const double vita_hz = 59.94;
	double run_acc = 1.0; /* frames owed to the core */
	std::string status;
	pic_governor gov;
	pic_gov_init(&gov);

	/* log state */
	uint64_t sec_start = sceKernelGetProcessTimeWide();
	int frames_run = 0, vblanks = 0, secs = 0;
	uint64_t emu_us = 0, main_us = 0, fetch_us = 0, draw_us = 0, finish_us = 0, vwait_us = 0;
	uint64_t prof_acc[S9XP_N] = { 0 };
	uint64_t prof_frames = 0;
	uint64_t sram_dirty_since = 0;
	uint64_t last_frame_us = 0;
	snes_thread_stats st0, st1;
	snes_host_thread_stats(&st0);
	unsigned drawn = 0, skipped = 0, pic_fresh = 0;
	int gov_level_seen = 0;
	srm_writer_start();
	unsigned last_vc = sceDisplayGetVcount();
	int gov_warmup = 180;   /* the first 3 s (caches, the game's own start-up) do not count */

	for (;;) {
		/* One pass per display refresh, counted by the display itself: a pass
		 * whose picture went into the queue without waiting must not start the
		 * next frame early (that ran games at twice their speed on a Vita). */
		unsigned vc = sceDisplayGetVcount();
		if (vc == last_vc) {
			uint64_t w0 = sceKernelGetProcessTimeWide();
			sceDisplayWaitVblankStart();
			vc = sceDisplayGetVcount();
			vwait_us += sceKernelGetProcessTimeWide() - w0;
		}
		unsigned elapsed = vc - last_vc;
		last_vc = vc;
		if (elapsed == 0) elapsed = 1;      /* the count did not move: count the wait */
		if (elapsed > 2) elapsed = 2;       /* a long stall (menu, suspend) is not caught up */
		uint64_t loop0 = sceKernelGetProcessTimeWide();
		unsigned raw, masks[4];
		read_pads(masks, &raw);
		if ((raw & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) ==
		    (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) {
			if (snes_host_sram_dirty()) srm_write_sync();
			int quit = pause_menu(game, status);
			if (quit) break;
			snes_host_set_multitap(multitap_wanted(g_ps));
			run_acc = 1.0;
			last_vc = sceDisplayGetVcount();
			gov_warmup = 60;
			continue;
		}
		for (int i = 0; i < 4; i++) snes_host_set_pad(i, masks[i]);
		/* a game being played keeps the console awake, as VitAdhoc does: without
		 * this the screen turned off (and the game stopped) a few seconds into a
		 * demo or a cutscene with no buttons pressed */
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

		run_acc += frame_hz / vita_hz * elapsed;
		int to_run = 0;
		while (run_acc >= 1.0 && to_run < 2) { run_acc -= 1.0; to_run++; }
		if (run_acc > 2.0) run_acc = 1.0;
		if (to_run > 1) to_run = 1; /* never run two in one vblank; the audio rate control absorbs the drift */

		/* governor level 2: every other frame is not drawn (the core skips its own drawing too when it
		 * draws on this thread; with the picture thread on, only the copy and the GPU work are saved) */
		bool skip_draw = gov.level >= 2 && (vblanks & 1);
		bool fresh = false;
		for (int i = 0; i < to_run; i++) {
			uint64_t t0 = sceKernelGetProcessTimeWide();
#ifdef S9X_PROF
			int prof_this = (secs & 3) == 0 && !snes_host_threads_active(); /* the hooks are not thread safe */
			if (prof_this) {
				memset(g_s9xp.acc, 0, sizeof g_s9xp.acc);
				g_s9xp.cur = S9XP_CPU; g_s9xp.sp = 0; g_s9xp.on = 1; g_s9xp.last = s9xp_now();
			}
#endif
			if (!(snes_host_threads_active() & 2)) snes_host_set_render(!skip_draw);
			snes_host_run_frame();
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
			frames_run++;
			static int16_t abuf[2048 * 2];
			int n;
			while ((n = snes_host_audio_take(abuf, 2048)) > 0) audio_push(abuf, n);
			if (!skip_draw) {
				uint64_t f0 = sceKernelGetProcessTimeWide();
				fresh = video_fetch(g_ps, frames_run) != 0;
				fetch_us += sceKernelGetProcessTimeWide() - f0;
			}
		}
		if (fresh) pic_fresh++;

		/* audio rate control: keep the queue near 60 ms */
		{
			double target = 0.06 * SNES_AUDIO_RATE, fill = q_fill();
			double e = (fill - target) / target; /* >0: too full, consume faster */
			if (e > 1) e = 1; if (e < -1) e = -1;
			g_ratio_adj = 1.0 + 0.01 * e;
		}

		bool drew = false;
		uint64_t d0 = sceKernelGetProcessTimeWide();
		if (!skip_draw) drew = frame_draw(g_ps, 1, frames_run);
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
		if (gov_warmup > 0) gov_warmup--;
		else pic_gov_update(&gov, (unsigned) (work_end - loop0), 15500);
		if (gov.level != gov_level_seen) {
			logf_("governor: level %d -> %d (work %.1f ms)\n", gov_level_seen, gov.level, (work_end - loop0) / 1000.0);
			gov_level_seen = gov.level;
		}

		/* battery RAM: 2 s after it changed, on the writer thread; at most every
		 * 30 s for games that keep changing it (work RAM in Super FX carts); at
		 * once, here, before suspend */
		uint64_t now = sceKernelGetProcessTimeWide();
		{
			static int srm_done_seen;
			int d = g_srm_done;
			if (d != srm_done_seen) {
				srm_done_seen = d;
				int r = g_srm_result;
				if (r) snes_host_sram_forget();   /* failed: still to be written */
				if (r || d <= 3 || d % 50 == 0) logf_("sram save rc=%d (write %d)\n", r, d);
			}
		}
		if (g_suspend_flag) {
			if (snes_host_sram_dirty()) logf_("sram save before suspend rc=%d\n", srm_write_sync());
			sram_dirty_since = 0;
		} else if (snes_host_sram_dirty()) {
			/* settled (unchanged for 1 s) or 30 s since the last write */
			static uint8_t shadow[sizeof g_srm_buf];
			static uint64_t last_change, last_write;
			size_t sz = snes_host_sram_size();
			if (sz <= sizeof shadow && memcmp(shadow, snes_host_sram_data(), sz) != 0) {
				memcpy(shadow, snes_host_sram_data(), sz);
				last_change = now;
			}
			if (!sram_dirty_since) sram_dirty_since = now;
			if (now - sram_dirty_since > 2000000 &&
			    (now - last_change > 1000000 || now - last_write > 30000000) && srm_write_async()) {
				sram_dirty_since = 0;
				last_write = now;
			}
		} else sram_dirty_since = 0;
		g_suspend_flag = 0;

		if (now - sec_start >= 1000000) {
			double dt = (now - sec_start) / 1e6;
			snes_host_thread_stats(&st1);
			double fr = frames_run ? frames_run : 1;
			logf_("t=%d fps=%.1f emu_fps=%.1f emu_ms=%.2f last_ms=%.2f aq=%u", secs, vblanks / dt, frames_run / dt,
			    frames_run ? emu_us / 1000.0 / frames_run : 0.0, last_frame_us / 1000.0, q_fill());
			/* busy time per emulated frame: the main thread's whole loop work (emulation, copy, GPU submission,
			 * without the wait for the display) and the two workers' time spent working */
			logf_(" | busy ms/fr: main=%.2f ppu=%.2f apu=%.2f | waits/s: ppu=%llu apu=%llu (%.1f ms) | main split ms/fr: emu=%.2f copy=%.2f gpu_submit=%.2f | gov=%d drawn=%u skipped=%u new_pic=%u fence_skip=%u rt_reuse=%u",
			    main_us / 1000.0 / fr, (st1.ppu_busy_ns - st0.ppu_busy_ns) / 1e6 / fr, (st1.apu_busy_ns - st0.apu_busy_ns) / 1e6 / fr,
			    (unsigned long long) (st1.ppu_waits - st0.ppu_waits), (unsigned long long) (st1.apu_waits - st0.apu_waits),
			    ((st1.ppu_wait_ns - st0.ppu_wait_ns) + (st1.apu_wait_ns - st0.apu_wait_ns)) / 1e6,
			    emu_us / 1000.0 / fr, fetch_us / 1000.0 / fr, draw_us / 1000.0 / fr,
			    gov.level, drawn, skipped, pic_fresh, g_fence_skips, g_rt_reused);
			/* where the rest of each refresh went: handing the picture over (blocks
			 * while the display queue or the GPU is behind) and the wait for the
			 * next refresh */
			logf_(" | per s: finish=%.1f ms vwait=%.1f ms", finish_us / 1000.0, vwait_us / 1000.0);
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
			drawn = skipped = pic_fresh = 0;
			secs++;
		}
	}

	g_quit = 1;
	settings_save();
	srm_wait_idle();
	if (snes_host_sram_dirty()) logf_("final sram save rc=%d\n", snes_host_sram_save());
	if (g_srm_thread >= 0) { g_srm_stop = 1; sceKernelSignalSema(g_srm_sema, 1); sceKernelWaitThreadEnd(g_srm_thread, NULL, NULL); sceKernelDeleteThread(g_srm_thread); }
	if (g_athread >= 0) sceKernelWaitThreadEnd(g_athread, NULL, NULL);
	if (g_aport >= 0) sceAudioOutReleasePort(g_aport);
	snes_host_shutdown();
	return_to_vitadhoc();
	return 0;
}
