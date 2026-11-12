/*
 * wswan_twin: headless Linux runner for the WonderSwan host (Beetle WonderSwan core).
 * Loads a ROM, runs N frames with a scripted pad, writes per-frame hashes, PNG screenshots (raw, or rotated by the
 * detected orientation), a WAV of the audio, saves and states, and a summary line.
 *
 * Copyright CBOC-Vita. MIT licence (see wswan/frontend/LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "host.h"
#include "layout.h"
#include "safe_io.h"

static uint64_t cpu_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static time_t fixed_time(void) { return 1700000000; }   /* 2023-11-14 22:13:20 UTC */

struct PadEv { int frame; unsigned mask; };
struct Names { const char *n; unsigned b; };

static unsigned parse_buttons(const char *s, const Names *tab, int nt)
{
	unsigned m = 0;
	std::string t(s);
	size_t p = 0;
	while (p <= t.size()) {
		size_t q = t.find('+', p);
		if (q == std::string::npos) q = t.size();
		std::string w = t.substr(p, q - p);
		for (int i = 0; i < nt; i++) if (w == tab[i].n) m |= tab[i].b;
		p = q + 1;
	}
	return m;
}
static const Names WS_NAMES[] = {
	{ "x1", WSWAN_X1 }, { "x2", WSWAN_X2 }, { "x3", WSWAN_X3 }, { "x4", WSWAN_X4 },
	{ "y1", WSWAN_Y1 }, { "y2", WSWAN_Y2 }, { "y3", WSWAN_Y3 }, { "y4", WSWAN_Y4 },
	{ "a", WSWAN_A }, { "b", WSWAN_B }, { "start", WSWAN_START },
	{ "up", WSWAN_X1 }, { "right", WSWAN_X2 }, { "down", WSWAN_X3 }, { "left", WSWAN_X4 }
};
static const Names VITA_NAMES[] = {
	{ "up", WSL_UP }, { "down", WSL_DOWN }, { "left", WSL_LEFT }, { "right", WSL_RIGHT },
	{ "cross", WSL_CROSS }, { "circle", WSL_CIRCLE }, { "square", WSL_SQUARE }, { "triangle", WSL_TRIANGLE },
	{ "l", WSL_L }, { "r", WSL_R }, { "start", WSL_START }, { "select", WSL_SELECT },
	{ "rsup", WSL_RS_UP }, { "rsdown", WSL_RS_DOWN }, { "rsleft", WSL_RS_LEFT }, { "rsright", WSL_RS_RIGHT }
};
#define NWS (int) (sizeof WS_NAMES / sizeof WS_NAMES[0])
#define NVITA (int) (sizeof VITA_NAMES / sizeof VITA_NAMES[0])

/* "60:start,70:none,200:a+right" -> events sorted by frame */
static std::vector<PadEv> parse_script(const char *s, const Names *tab, int nt)
{
	std::vector<PadEv> v;
	std::string t(s);
	size_t p = 0;
	while (p < t.size()) {
		size_t q = t.find(',', p);
		if (q == std::string::npos) q = t.size();
		std::string e = t.substr(p, q - p);
		size_t c = e.find(':');
		if (c != std::string::npos) v.push_back({ atoi(e.substr(0, c).c_str()), parse_buttons(e.substr(c + 1).c_str(), tab, nt) });
		p = q + 1;
	}
	return v;
}

static void write_wav(const char *path, const std::vector<int16_t> &pcm)
{
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint32_t data = (uint32_t) (pcm.size() * 2), rate = wswan_host_audio_rate(), br = rate * 4, riff = 36 + data;
	uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
	uint32_t sixteen = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&sixteen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
	fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
	fwrite(pcm.data(), 2, pcm.size(), f);
	fclose(f);
}

/* mode: 0 raw 224x144, 1 turn CCW when the effective orientation is vertical, 2 always CCW */
static void write_png(const char *path, int mode, int scale)
{
	int w, h, pitch;
	const uint16_t *v = wswan_host_video(&w, &h, &pitch);
	std::vector<uint16_t> rot(w * h);
	int rotate = mode == 2 || (mode == 1 && wswan_host_orientation() == WSWAN_VERTICAL);
	const uint16_t *src = v; int ow = w, oh = h;
	if (rotate) { wsl_rotate_ccw(rot.data(), v); src = rot.data(); ow = h; oh = w; }
	std::vector<uint8_t> rgb((size_t) ow * scale * oh * scale * 3);
	for (int y = 0; y < oh * scale; y++)
		for (int x = 0; x < ow * scale; x++) {
			uint16_t p = src[(y / scale) * ow + x / scale];
			uint8_t *d = &rgb[((size_t) y * ow * scale + x) * 3];
			d[0] = (uint8_t) (((p >> 11) & 31) * 255 / 31); d[1] = (uint8_t) (((p >> 5) & 63) * 255 / 63); d[2] = (uint8_t) ((p & 31) * 255 / 31);
		}
	stbi_write_png(path, ow * scale, oh * scale, 3, rgb.data(), ow * scale * 3);
}

static uint64_t fnv(const uint16_t *p, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	const uint8_t *b = (const uint8_t *) p;
	for (size_t i = 0; i < n * 2; i++) { h ^= b[i]; h *= 1099511628211ull; }
	return h;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: wswan_twin ROM [options]\n"
		"  --frames N            frames to run (default 300)\n"
		"  --pad SCRIPT          'frame:key+key,frame:none' keys x1-x4 y1-y4 a b start (up/right/down/left = x1..x4)\n"
		"  --vpad SCRIPT         the same with Vita buttons through wsl_map_pad (up down left right cross circle square triangle l r start rsup..)\n"
		"  --vita-presentation upright|turned   (default upright)\n"
		"  --hashes FILE         'frame hash orientation' per frame\n"
		"  --shot FRAME:FILE[,..]  PNG after that frame   --shot-mode raw|auto|ccw (default raw)  --shot-scale N\n"
		"  --png-every N --png-prefix P   a PNG every N frames (auto-rotated)\n"
		"  --wav FILE            audio of the run\n"
		"  --srm FILE            cartridge save (RetroArch .srm layout + internal EEPROM trailer): loaded, written if dirty\n"
		"  --backup-dir DIR      --save-state FILE --load-state FILE --state-frame N (save at, and load at unless --load-frame M)\n"
		"  --orient auto|h|v     --orient-log FILE (changes of the effective orientation)\n"
		"  --render-off-from N   skip drawing from frame N on\n"
		"  --palette N           mono palette 0..3        --rate HZ   audio rate\n"
		"  --cheat CODE          --rtc-fixed (clock = 2023-11-14 22:13:20 UTC)\n"
		"  --table DEV:CART:h|v  orientation table entry (hex ids)\n"
		"  --quiet\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) { usage(); return 2; }
	const char *rom = argv[1];
	int frames = 300, shot_scale = 1, shot_mode = 0, png_every = 0, state_frame = 100, load_frame = -1, render_off = -1, palette = 0, quiet = 0, rate = 0;
	bool turned = false, rtc_fixed = false;
	std::string hashes, wav, srm, backup, save_state, load_state, orient_log, png_prefix = "shot", cheat, table;
	std::vector<std::pair<int, std::string>> shots;
	std::vector<PadEv> pad, vpad;
	int orient_mode = WSWAN_ORIENT_AUTO;
	for (int i = 2; i < argc; i++) {
		std::string a = argv[i];
		auto nxt = [&]() -> const char * { if (i + 1 >= argc) { usage(); exit(2); } return argv[++i]; };
		if (a == "--frames") frames = atoi(nxt());
		else if (a == "--pad") pad = parse_script(nxt(), WS_NAMES, NWS);
		else if (a == "--pad-file" || a == "--vpad-file") {
			FILE *pf = fopen(nxt(), "r"); std::string t; int ch;
			if (!pf) { fprintf(stderr, "cannot read pad file\n"); return 1; }
			while ((ch = fgetc(pf)) != EOF) if (ch != '\n' && ch != ' ') t += (char) ch;
			fclose(pf);
			if (a == "--pad-file") pad = parse_script(t.c_str(), WS_NAMES, NWS); else vpad = parse_script(t.c_str(), VITA_NAMES, NVITA);
		}
		else if (a == "--vpad") vpad = parse_script(nxt(), VITA_NAMES, NVITA);
		else if (a == "--vita-presentation") turned = std::string(nxt()) == "turned";
		else if (a == "--hashes") hashes = nxt();
		else if (a == "--shot") {
			std::string t = nxt(); size_t p = 0;
			while (p < t.size()) { size_t q = t.find(',', p); if (q == std::string::npos) q = t.size(); std::string e = t.substr(p, q - p); size_t c = e.find(':');
				if (c != std::string::npos) shots.push_back({ atoi(e.substr(0, c).c_str()), e.substr(c + 1) });
				p = q + 1; }
		}
		else if (a == "--shot-mode") { std::string m = nxt(); shot_mode = m == "auto" ? 1 : m == "ccw" ? 2 : 0; }
		else if (a == "--shot-scale") shot_scale = atoi(nxt());
		else if (a == "--png-every") png_every = atoi(nxt());
		else if (a == "--png-prefix") png_prefix = nxt();
		else if (a == "--wav") wav = nxt();
		else if (a == "--srm") srm = nxt();
		else if (a == "--backup-dir") backup = nxt();
		else if (a == "--save-state") save_state = nxt();
		else if (a == "--load-state") load_state = nxt();
		else if (a == "--state-frame") state_frame = atoi(nxt());
		else if (a == "--load-frame") load_frame = atoi(nxt());
		else if (a == "--orient") { std::string m = nxt(); orient_mode = m == "h" ? WSWAN_ORIENT_HORIZONTAL : m == "v" ? WSWAN_ORIENT_VERTICAL : WSWAN_ORIENT_AUTO; }
		else if (a == "--orient-log") orient_log = nxt();
		else if (a == "--render-off-from") render_off = atoi(nxt());
		else if (a == "--palette") palette = atoi(nxt());
		else if (a == "--rate") rate = atoi(nxt());
		else if (a == "--cheat") cheat = nxt();
		else if (a == "--rtc-fixed") rtc_fixed = true;
		else if (a == "--table") table = nxt();
		else if (a == "--quiet") quiet = 1;
		else { usage(); return 2; }
	}

	uint8_t *data = NULL; size_t n = 0;
	if (wswan_file_read(rom, &data, &n, 32u << 20)) { fprintf(stderr, "cannot read %s\n", rom); return 1; }
	wswan_host_init();
	if (rate) wswan_host_set_audio_rate(rate);
	if (rtc_fixed) wswan_host_set_time_source(fixed_time);
	wswan_host_set_mono_palette(palette);
	wswan_host_set_orientation_mode(orient_mode);
	if (!table.empty()) {
		unsigned d, c; char m;
		if (sscanf(table.c_str(), "%x:%x:%c", &d, &c, &m) == 3) wswan_host_orientation_table_set(d, c, m == 'v' ? WSWAN_ORIENT_VERTICAL : WSWAN_ORIENT_HORIZONTAL);
	}
	char err[128] = "";
	int rc = wswan_host_load_mem(data, n, rom, err, sizeof err);
	free(data);
	if (rc) { fprintf(stderr, "load failed (%d): %s\n", rc, err); return 1; }
	if (!quiet) printf("rom: %s\n", wswan_host_rom_info());
	if (!srm.empty()) {
		wswan_recover_old(srm.c_str());
		uint8_t *b = NULL; size_t bn = 0;
		if (wswan_file_read(srm.c_str(), &b, &bn, 64u << 20) == 0 && bn) { size_t u = wswan_host_srm_import(b, bn); if (!quiet) printf("srm: loaded %zu of %zu bytes\n", u, bn); }
		free(b);
	}
	if (!cheat.empty()) printf("cheats accepted: %d\n", wswan_host_cheat_add(cheat.c_str()));

	FILE *hf = hashes.empty() ? NULL : fopen(hashes.c_str(), "w");
	FILE *of = orient_log.empty() ? NULL : fopen(orient_log.c_str(), "w");
	std::vector<int16_t> pcm;
	std::vector<int16_t> abuf(4096 * 2);
	size_t pi = 0, vi = 0;
	unsigned cur = 0, vcur = 0;
	int last_orient = -1;
	uint64_t cpu = 0, cpu_max = 0;
	long clipped = 0; int peak = 0; double sq = 0; long samples = 0;
	std::vector<uint8_t> stbuf;
	int save_errs = 0;
	for (int f = 0; f < frames; f++) {
		while (pi < pad.size() && pad[pi].frame <= f) cur = pad[pi++].mask;
		while (vi < vpad.size() && vpad[vi].frame <= f) vcur = vpad[vi++].mask;
		if (!load_state.empty() && f == (load_frame >= 0 ? load_frame : state_frame)) {
			uint8_t *b = NULL; size_t bn = 0;
			if (wswan_file_read(load_state.c_str(), &b, &bn, 64u << 20) == 0) {
				int r = wswan_host_state_load_mem(b, bn);
				if (r) fprintf(stderr, "state load failed: %d\n", r);
			}
			free(b);
		}
		if (!vpad.empty()) cur = wsl_map_pad(vcur, wswan_host_orientation(), turned ? WSL_TURNED : WSL_UPRIGHT);
		wswan_host_set_pad(cur);
		if (render_off >= 0 && f == render_off) wswan_host_set_render(0);
		uint64_t t0 = cpu_ns();
		wswan_host_run_frame();
		uint64_t dt = cpu_ns() - t0;
		cpu += dt; if (dt > cpu_max) cpu_max = dt;
		int got;
		while ((got = wswan_host_audio_take(abuf.data(), 4096)) > 0) {
			for (int i = 0; i < got * 2; i++) { int v = abuf[i]; int a = v < 0 ? -v : v; if (a > peak) peak = a; if (a >= 32767) clipped++; sq += (double) v * v; }
			samples += got * 2;
			if (!wav.empty()) pcm.insert(pcm.end(), abuf.begin(), abuf.begin() + got * 2);
		}
		uint64_t h = 0;
		{
			int w, hh, p; const uint16_t *v = wswan_host_video(&w, &hh, &p); h = fnv(v, (size_t) w * hh);
		}
		if (hf) { wswan_orientation_info q; wswan_host_orientation_info(&q); fprintf(hf, "%d %016llx %d %d %d\n", f, (unsigned long long) h, wswan_host_orientation(), q.keys_last_frame, q.icon_bits); }
		int o = wswan_host_orientation();
		if (o != last_orient) {
			wswan_orientation_info oi; wswan_host_orientation_info(&oi);
			if (of) fprintf(of, "frame %d orientation %s source %d keys %d icon %d switches %u\n", f, o ? "vertical" : "horizontal", oi.source, oi.keys_last_frame, oi.icon_bits, oi.switches);
			last_orient = o;
		}
		if (!save_state.empty() && f == state_frame) {
			stbuf.resize(wswan_host_state_size());
			size_t r = wswan_host_state_save_mem(stbuf.data(), stbuf.size());
			if (!r) fprintf(stderr, "state save failed\n");
			else { char e[128]; if (wswan_write_safe(save_state.c_str(), stbuf.data(), r, NULL, 0, NULL, e, sizeof e)) fprintf(stderr, "state write: %s\n", e); }
		}
		for (auto &s : shots) if (s.first == f) write_png(s.second.c_str(), shot_mode, shot_scale);
		if (png_every && f % png_every == 0) { char nm[512]; snprintf(nm, sizeof nm, "%s_%05d.png", png_prefix.c_str(), f); write_png(nm, 1, shot_scale); }
	}
	if (!srm.empty() && wswan_host_srm_dirty()) {
		std::vector<uint8_t> out(wswan_host_srm_size());
		size_t r = wswan_host_srm_export(out.data(), out.size());
		char e[160] = ""; int keep = 1, backed = 0;
		int rc2 = wswan_write_safe(srm.c_str(), out.data(), r, backup.empty() ? NULL : backup.c_str(), keep, &backed, e, sizeof e);
		if (rc2) { fprintf(stderr, "sram save: %d %s\n", rc2, e); wswan_host_srm_mark_unsaved(); save_errs++; }
		else if (!quiet) printf("sram save: 0 (%zu bytes)\n", r);
	}
	if (hf) fclose(hf);
	if (of) fclose(of);
	if (!wav.empty()) write_wav(wav.c_str(), pcm);
	wswan_orientation_info oi; wswan_host_orientation_info(&oi);
	uint64_t last_hash; { int w, hh, p; const uint16_t *v = wswan_host_video(&w, &hh, &p); last_hash = fnv(v, (size_t) w * hh); }
	printf("frames=%d hash=%016llx cpu_ms_per_frame=%.3f cpu_ms_max=%.3f audio_peak=%d audio_rms=%.0f audio_clipped=%ld orient=%s switches=%u src=%d\n",
	       frames, (unsigned long long) last_hash, cpu / 1e6 / (frames ? frames : 1), cpu_max / 1e6, peak, samples ? sqrt(sq / samples) : 0.0,
	       clipped, wswan_host_orientation() ? "vertical" : "horizontal", oi.switches, oi.source);
	wswan_host_shutdown();
	return save_errs ? 3 : 0;
}
