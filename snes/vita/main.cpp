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
#include <vita2d.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "host.h"
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
	g_athread = sceKernelCreateThread("snes_audio", audio_thread, 0x40, 0x4000, 0, 0, NULL);
	if (g_athread >= 0) sceKernelStartThread(g_athread, 0, NULL);
}

/* ---- video ------------------------------------------------------------- */

static vita2d_texture *g_tex[3];   /* three: the GPU may still read the last two */
static int g_tex_i;

static void video_init(void)
{
	for (int i = 0; i < 3; i++) {
		g_tex[i] = vita2d_create_empty_texture_format(512, 512, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
		vita2d_texture_set_filters(g_tex[i], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	}
}

/* Copy the core's frame into the next texture; returns its size. */
static vita2d_texture *video_upload(int *w, int *h)
{
	int pitch;
	const uint16_t *src = snes_host_video(w, h, &pitch);
	g_tex_i = (g_tex_i + 1) % 3;
	vita2d_texture *t = g_tex[g_tex_i];
	uint8_t *dst = (uint8_t *) vita2d_texture_get_datap(t);
	unsigned dpitch = vita2d_texture_get_stride(t);
	int ww = *w > 512 ? 512 : *w, hh = *h > 512 ? 512 : *h;
	for (int y = 0; y < hh; y++)
		memcpy(dst + (size_t) y * dpitch, (const uint8_t *) src + (size_t) y * pitch, (size_t) ww * 2);
	return t;
}

/* 4:3 picture, full height; the SNES pixel aspect is 8:7 so 256x224 is 4:3. */
static void video_draw(vita2d_texture *t, int w, int h)
{
	float dh = SCREEN_H, dw = SCREEN_H * 4.0f / 3.0f;
	float x = (SCREEN_W - dw) / 2, y = 0;
	vita2d_draw_texture_part_scale(t, x, y, 0, 0, (float) w, (float) h, dw / w, dh / h);
}

/* ---- input ------------------------------------------------------------- */

static int g_confirm_is_cross = 1;

static unsigned read_pad(unsigned *raw)
{
	SceCtrlData pad;
	memset(&pad, 0, sizeof pad);
	sceCtrlPeekBufferPositive(0, &pad, 1);
	unsigned b = pad.buttons, m = 0;
	if (raw) *raw = b;
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
	return m;
}

/* ---- launch / return --------------------------------------------------- */

static void return_to_vitadhoc(void)
{
	const char *argv[] = { "eboot.bin", "--from-snes", NULL };
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

/* ---- pause menu -------------------------------------------------------- */

static volatile int g_suspend_flag;
static int power_cb(int, int info, int, void *)
{
	if (info & (SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND)) g_suspend_flag = 1;
	return 0;
}

static const char *kMenu[] = { "Resume", "Save state", "Load state", "Reset game", "Quit to VitAdhoc" };

/* Returns 1 when the player chose Quit. */
static int pause_menu(const std::string &game, vita2d_texture *bg, int bw, int bh, std::string &status)
{
	int sel = 0, slot = 0;
	unsigned prev = 0xffffffff, raw;
	unsigned conf = g_confirm_is_cross ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	unsigned back = g_confirm_is_cross ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
	int closing = 0;
	g_paused = 1;
	for (;;) {
		SceCtrlData pad;
		memset(&pad, 0, sizeof pad);
		sceCtrlPeekBufferPositive(0, &pad, 1);
		raw = pad.buttons;
		unsigned press = raw & ~prev;
		if (prev == 0xffffffff) press = 0; /* ignore buttons held when it opened */
		prev = raw;
		if (press & SCE_CTRL_UP) sel = (sel + 4) % 5;
		if (press & SCE_CTRL_DOWN) sel = (sel + 1) % 5;
		if (press & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER)) slot = (slot + 3) % 4;
		if (press & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) slot = (slot + 1) % 4;
		if (press & back) closing = 1;
		if (press & conf) {
			char path[256];
			snprintf(path, sizeof path, DATA_ROOT "/states/%s.st%d", game.c_str(), slot);
			if (sel == 0) closing = 1;
			else if (sel == 1) {
				int r = snes_host_state_save(path);
				status = r ? "Could not save the state" : "State saved";
			} else if (sel == 2) {
				int r = snes_host_state_load(path);
				status = r == 2 ? "No state in this slot" : r ? "Could not load the state" : "State loaded";
				if (r == 0) closing = 1;
			} else if (sel == 3) { snes_host_reset(); closing = 1; }
			else if (sel == 4) { g_paused = 0; return 1; }
		}
		if (closing) break;

		vita2d_start_drawing();
		vita2d_clear_screen();
		if (bg) video_draw(bg, bw, bh);
		vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, 0xC0000000);
		vita2d_draw_rectangle(300, 110, 360, 330, 0xE0302820);
		text_draw(330, 160, "SNES - paused", 0xFFFFFFFF);
		for (int i = 0; i < 5; i++) {
			char line[64];
			if (i == 1 || i == 2) snprintf(line, sizeof line, "%s  (slot %d)", kMenu[i], slot + 1);
			else snprintf(line, sizeof line, "%s", kMenu[i]);
			text_draw(340, 215 + i * 40, line, i == sel ? 0xFFFFD040 : 0xFFE0E0E0);
		}
		if (!status.empty()) text_draw(330, 420, status.c_str(), 0xFFA0FFA0);
		vita2d_end_drawing();
		vita2d_swap_buffers();
	}
	/* wait until the confirm / back buttons are released so the game does not see them */
	for (;;) {
		SceCtrlData pad;
		memset(&pad, 0, sizeof pad);
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (!(pad.buttons & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) break;
		sceKernelDelayThread(16000);
	}
	g_paused = 0;
	return 0;
}

/* ---- main -------------------------------------------------------------- */

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
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

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

	if (argc < 2 || !argv[1] || !argv[1][0]) {
		logf_("no ROM path given\n");
		fatal_screen("No game was given.", "Start SNES games from VitAdhoc.");
		return_to_vitadhoc();
	}
	std::string rom = argv[1];
	std::string game = base_name(rom);
	logf_("rom: %s\n", rom.c_str());

	char err[160] = "";
	if (snes_host_init() || snes_host_load_rom(rom.c_str(), srm_path(rom).c_str(), err, sizeof err)) {
		logf_("load failed: %s\n", err);
		fatal_screen("This game could not be started.", err);
		return_to_vitadhoc();
	}
	logf_("title: %s  pal=%d  sram=%u\n", snes_host_rom_title(), snes_host_is_pal(), (unsigned) snes_host_sram_size());

	audio_init();

	const double frame_hz = snes_host_is_pal() ? 50.0 : 60.0988;
	const double vita_hz = 59.94;
	double run_acc = 1.0; /* frames owed to the core */
	std::string status;
	int w = 256, h = 224;
	vita2d_texture *tex = NULL;

	/* log state */
	uint64_t sec_start = sceKernelGetProcessTimeWide();
	int frames_run = 0, vblanks = 0, secs = 0;
	uint64_t emu_us = 0;
	uint64_t prof_acc[S9XP_N] = { 0 };
	uint64_t prof_frames = 0;
	uint64_t sram_dirty_since = 0;
	uint64_t last_frame_us = 0;

	for (;;) {
		unsigned raw;
		unsigned pad = read_pad(&raw);
		if ((raw & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) ==
		    (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) {
			if (snes_host_sram_dirty()) snes_host_sram_save();
			int quit = pause_menu(game, tex, w, h, status);
			if (quit) break;
			run_acc = 1.0;
			continue;
		}
		snes_host_set_pad(0, pad);

		run_acc += frame_hz / vita_hz;
		int to_run = 0;
		while (run_acc >= 1.0 && to_run < 2) { run_acc -= 1.0; to_run++; }
		if (run_acc > 2.0) run_acc = 1.0;
		if (to_run > 1) to_run = 1; /* never run two in one vblank; the audio rate control absorbs the drift */

		for (int i = 0; i < to_run; i++) {
			uint64_t t0 = sceKernelGetProcessTimeWide();
#ifdef S9X_PROF
			int prof_this = (secs & 3) == 0;
			if (prof_this) {
				memset(g_s9xp.acc, 0, sizeof g_s9xp.acc);
				g_s9xp.cur = S9XP_CPU; g_s9xp.sp = 0; g_s9xp.on = 1; g_s9xp.last = s9xp_now();
			}
#endif
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
			tex = video_upload(&w, &h);
		}

		/* audio rate control: keep the queue near 60 ms */
		{
			double target = 0.06 * SNES_AUDIO_RATE, fill = q_fill();
			double e = (fill - target) / target; /* >0: too full, consume faster */
			if (e > 1) e = 1; if (e < -1) e = -1;
			g_ratio_adj = 1.0 + 0.01 * e;
		}

		vita2d_start_drawing();
		vita2d_clear_screen();
		if (tex) video_draw(tex, w, h);
		vita2d_end_drawing();
		vita2d_swap_buffers();
		vblanks++;

		/* battery RAM: write 2 s after it last changed, and before suspend */
		uint64_t now = sceKernelGetProcessTimeWide();
		if (snes_host_sram_dirty()) {
			if (!sram_dirty_since) sram_dirty_since = now;
			if (now - sram_dirty_since > 2000000 || g_suspend_flag) {
				int r = snes_host_sram_save();
				logf_("sram save rc=%d\n", r);
				sram_dirty_since = 0;
			}
		} else sram_dirty_since = 0;
		g_suspend_flag = 0;

		if (now - sec_start >= 1000000) {
			double dt = (now - sec_start) / 1e6;
			logf_("t=%d fps=%.1f emu_fps=%.1f emu_ms=%.2f last_ms=%.2f aq=%u", secs, vblanks / dt, frames_run / dt,
			    frames_run ? emu_us / 1000.0 / frames_run : 0.0, last_frame_us / 1000.0, q_fill());
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
			emu_us = 0;
			secs++;
		}
	}

	g_quit = 1;
	if (snes_host_sram_dirty()) logf_("final sram save rc=%d\n", snes_host_sram_save());
	if (g_athread >= 0) sceKernelWaitThreadEnd(g_athread, NULL, NULL);
	if (g_aport >= 0) sceAudioOutReleasePort(g_aport);
	snes_host_shutdown();
	return_to_vitadhoc();
	return 0;
}
