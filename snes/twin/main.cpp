/*
 * snes_twin: headless Linux runner for the SNES program (Snes9x core).
 * Loads a ROM, runs N frames with a scripted pad, writes per-frame hashes,
 * PNG screenshots, a WAV of the audio and a per-frame timing split.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>
#include <set>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "host.h"
#include "s9x_prof.h"

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static uint64_t cpu_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct PadEv { int frame; unsigned mask; };

static unsigned parse_buttons(const char *s)
{
	static const struct { const char *n; unsigned b; } tab[] = {
		{ "a", SNES_A }, { "b", SNES_B }, { "x", SNES_X }, { "y", SNES_Y }, { "l", SNES_L }, { "r", SNES_R },
		{ "start", SNES_START }, { "select", SNES_SELECT }, { "up", SNES_UP }, { "down", SNES_DOWN },
		{ "left", SNES_LEFT }, { "right", SNES_RIGHT }
	};
	unsigned m = 0;
	std::string t(s);
	size_t p = 0;
	while (p <= t.size()) {
		size_t q = t.find('+', p);
		if (q == std::string::npos) q = t.size();
		std::string w = t.substr(p, q - p);
		for (auto &e : tab) if (w == e.n) m |= e.b;
		p = q + 1;
	}
	return m;
}

/* "60:start,70:none,200:a+right" -> events sorted by frame */
static std::vector<PadEv> parse_script(const char *s)
{
	std::vector<PadEv> v;
	std::string t(s);
	size_t p = 0;
	while (p < t.size()) {
		size_t q = t.find(',', p);
		if (q == std::string::npos) q = t.size();
		std::string e = t.substr(p, q - p);
		size_t c = e.find(':');
		if (c != std::string::npos) v.push_back({ atoi(e.substr(0, c).c_str()), parse_buttons(e.substr(c + 1).c_str()) });
		p = q + 1;
	}
	return v;
}

static void write_wav(const char *path, const std::vector<int16_t> &pcm)
{
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint32_t data = (uint32_t) (pcm.size() * 2), rate = SNES_AUDIO_RATE, br = rate * 4, riff = 36 + data;
	uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
	uint32_t sixteen = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&sixteen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
	fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
	fwrite(pcm.data(), 2, pcm.size(), f);
	fclose(f);
}

static void write_png(const char *path, const uint16_t *px, int w, int h, int pitch)
{
	std::vector<uint8_t> rgb((size_t) w * h * 3);
	for (int y = 0; y < h; y++) {
		const uint16_t *row = (const uint16_t *) ((const uint8_t *) px + (size_t) y * pitch);
		for (int x = 0; x < w; x++) {
			uint16_t c = row[x];
			uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
			uint8_t *o = &rgb[((size_t) y * w + x) * 3];
			o[0] = (r << 3) | (r >> 2); o[1] = (g << 2) | (g >> 4); o[2] = (b << 3) | (b >> 2);
		}
	}
	stbi_write_png(path, w, h, 3, rgb.data(), w * 3);
}

static uint64_t fnv(const uint16_t *px, int w, int h, int pitch)
{
	uint64_t hsh = 1469598103934665603ull;
	for (int y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *) px + (size_t) y * pitch;
		for (int i = 0; i < w * 2; i++) { hsh ^= row[i]; hsh *= 1099511628211ull; }
	}
	return hsh;
}

static void usage(void)
{
	fprintf(stderr,
	    "usage: snes_twin ROM [options]\n"
	    "  --frames N         frames to run (default 600)\n"
	    "  --pad SCRIPT       e.g. 60:start,70:none,200:a+right (buttons: a b x y l r start select up down left right)\n"
	    "  --hashes FILE      write 'frame width height fnv1a64' per frame\n"
	    "  --shots LIST       comma separated frame numbers to save as PNG\n"
	    "  --shot-prefix P    PNG file prefix (default shot_)\n"
	    "  --wav FILE         write all audio (32040 Hz stereo)\n"
	    "  --timing FILE      CSV: frame,total_us,cpu_us,apu_us,ppu_us,chip_us,front_us (enables profiling)\n"
	    "  --srm FILE         battery RAM file (loaded if present, saved at exit)\n"
	    "  --save-state FILE --state-frame N   save a state after frame N\n"
	    "  --load-state FILE --state-frame N   load a state before frame N\n"
	    "  --opt NAME=VALUE   core option (block_vram sprite_tiles interp superfx_clock transparency echo_buffer), repeatable\n"
	    "  --summary          print one summary line (fps, split) on stdout\n"
	    "  --name NAME        label used in the summary line\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) { usage(); return 2; }
	const char *rom = argv[1];
	int frames = 600, state_frame = -1;
	const char *hashes = 0, *shots = 0, *wav = 0, *timing = 0, *srm = 0, *sstate = 0, *lstate = 0, *name = 0;
	std::string prefix = "shot_";
	std::vector<PadEv> script;
	struct Opt { std::string k; int v; };
	std::vector<Opt> opts;
	bool summary = false;
	for (int i = 2; i < argc; i++) {
		auto arg = [&](const char *n) { return !strcmp(argv[i], n) && i + 1 < argc; };
		if (arg("--frames")) frames = atoi(argv[++i]);
		else if (arg("--pad")) script = parse_script(argv[++i]);
		else if (arg("--hashes")) hashes = argv[++i];
		else if (arg("--shots")) shots = argv[++i];
		else if (arg("--shot-prefix")) prefix = argv[++i];
		else if (arg("--wav")) wav = argv[++i];
		else if (arg("--timing")) timing = argv[++i];
		else if (arg("--srm")) srm = argv[++i];
		else if (arg("--save-state")) sstate = argv[++i];
		else if (arg("--load-state")) lstate = argv[++i];
		else if (arg("--state-frame")) state_frame = atoi(argv[++i]);
		else if (arg("--name")) name = argv[++i];
		else if (arg("--opt")) {
			char k[64]; int v = 0;
			if (sscanf(argv[++i], "%63[^=]=%d", k, &v) == 2) opts.push_back({ k, v });
		}
		else if (!strcmp(argv[i], "--summary")) summary = true;
		else { usage(); return 2; }
	}
	std::set<int> shotset;
	if (shots) {
		std::string t(shots);
		size_t p = 0;
		while (p < t.size()) { size_t q = t.find(',', p); if (q == std::string::npos) q = t.size(); shotset.insert(atoi(t.substr(p, q - p).c_str())); p = q + 1; }
	}

	if (snes_host_init()) { fprintf(stderr, "init failed\n"); return 1; }
	for (auto &o : opts) if (snes_host_set_option(o.k.c_str(), o.v)) { fprintf(stderr, "unknown option %s\n", o.k.c_str()); return 2; }
	char err[128] = "";
	if (snes_host_load_rom(rom, srm, err, sizeof err)) { fprintf(stderr, "load failed: %s\n", err); return 1; }

	fprintf(stderr, "%s\n", snes_host_rom_info());
	FILE *hf = hashes ? fopen(hashes, "w") : 0;
	FILE *tf = timing ? fopen(timing, "w") : 0;
	if (tf) fprintf(tf, "frame,total_us,cpu_us,apu_us,ppu_us,chip_us,front_us\n");
	std::vector<int16_t> pcm;
	std::vector<int16_t> abuf(4096 * 2);

	uint64_t tot_ns = 0, cpu_tot_ns = 0, acc_ns[S9XP_N] = { 0 };
	size_t ev = 0;
	unsigned pad = 0;
	g_s9xp.on = tf || summary ? 1 : 0;
	/* with --summary alone profiling is on; use no flags for a clean fps number */
	bool prof_on = g_s9xp.on;

	for (int f = 0; f < frames; f++) {
		while (ev < script.size() && script[ev].frame <= f) pad = script[ev++].mask;
		snes_host_set_pad(0, pad);
		if (lstate && f == state_frame) {
			int r = snes_host_state_load(lstate);
			fprintf(stderr, "load state: %d\n", r);
		}
		uint64_t t0 = now_ns(), c0 = cpu_ns();
		if (prof_on) { memset(g_s9xp.acc, 0, sizeof g_s9xp.acc); g_s9xp.cur = S9XP_CPU; g_s9xp.sp = 0; g_s9xp.last = s9xp_now(); }
		snes_host_run_frame();
		if (prof_on) s9xp_enter(S9XP_FRONT), s9xp_leave();
		uint64_t t1 = now_ns(), c1 = cpu_ns();
		tot_ns += t1 - t0;
		cpu_tot_ns += c1 - c0;
		if (prof_on) {
			for (int c = 0; c < S9XP_N; c++) acc_ns[c] += g_s9xp.acc[c];
			if (tf) fprintf(tf, "%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", f, (t1 - t0) / 1e3, g_s9xp.acc[0] / 1e3,
			    g_s9xp.acc[1] / 1e3, g_s9xp.acc[2] / 1e3, g_s9xp.acc[3] / 1e3, g_s9xp.acc[4] / 1e3);
		}
		int n = snes_host_audio_take(abuf.data(), 4096);
		if (wav) pcm.insert(pcm.end(), abuf.begin(), abuf.begin() + n * 2);
		if (sstate && f == state_frame) fprintf(stderr, "save state: %d\n", snes_host_state_save(sstate));

		int w, h, pitch;
		const uint16_t *v = snes_host_video(&w, &h, &pitch);
		if (hf) fprintf(hf, "%d %d %d %016llx\n", f, w, h, (unsigned long long) fnv(v, w, h, pitch));
		if (shotset.count(f)) {
			char p[512];
			snprintf(p, sizeof p, "%s%05d.png", prefix.c_str(), f);
			write_png(p, v, w, h, pitch);
		}
	}
	if (hf) fclose(hf);
	if (tf) fclose(tf);
	if (wav) write_wav(wav, pcm);
	if (srm && snes_host_sram_dirty()) fprintf(stderr, "sram save: %d\n", snes_host_sram_save());

	/* fps comes from the thread's CPU time, not the wall clock: the build machine
	 * is shared and wall time includes time spent waiting for a core. */
	double secs = cpu_tot_ns / 1e9;
	double fps = frames / secs;
	if (summary) {
		double sum = 0;
		for (int c = 0; c < S9XP_N; c++) sum += acc_ns[c];
		printf("%-34s %s fps=%7.1f ms/frame=%6.3f cpu=%4.1f%% apu=%4.1f%% ppu=%4.1f%% chip=%4.1f%% front=%4.1f%%\n",
		    name ? name : rom, snes_host_is_pal() ? "PAL " : "NTSC", fps, secs * 1000 / frames,
		    100 * acc_ns[0] / sum, 100 * acc_ns[1] / sum, 100 * acc_ns[2] / sum, 100 * acc_ns[3] / sum, 100 * acc_ns[4] / sum);
	} else {
		printf("%s: %d frames in %.3f s = %.1f fps (CPU time, profiling %s)\n", name ? name : rom, frames, secs, fps, prof_on ? "on" : "off");
	}
	snes_host_shutdown();
	return 0;
}
