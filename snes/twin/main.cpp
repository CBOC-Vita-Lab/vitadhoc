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
#include "audio_sim.h"

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
	    "  --cheats-root DIR  load/keep the game's cheat list in DIR/cheats/<rom name>.cht (RetroArch format)\n"
	    "  --cheat CODE       add a code (Game Genie XXXX-XXXX, Action Replay AAAAAAVV, raw AAAAAA=VV), repeatable\n"
	    "  --cheat-toggle F:I:0|1  at frame F switch entry I off/on;  --cheat-remove I;  --cheat-disable-all;  --cheat-list\n"
	    "  --peek HEX         print the work RAM byte at that offset at the end;  '-' as ROM with --cheat-decode CODE: decode only\n"
	    "  --srm FILE         battery RAM file (loaded if present, saved at exit; exit status 3 when the save failed)\n"
	    "  --srm-async        save the way the Vita's writer thread does (take / write / forget); --unblock PATH: if the save\n"
	    "                     failed remove PATH and try once more\n"
	    "  --backup-dir DIR   keep the verified backup of an existing save in DIR (default: <save>.bak)\n"
	    "  --save-state FILE --state-frame N   save a state after frame N\n"
	    "  --load-state FILE --state-frame N   load a state before frame N\n"
	    "  --pad2/--pad3/--pad4/--pad5 SCRIPT  scripts for pads 2..5 (pads 3-5 need --opt multitap=1)\n"
	    "  --nowait           use the non-blocking video fetch of the Vita (hash file lists new pictures: serial w h hash)\n"
	    "  --opt NAME=VALUE   core option (block_vram sprite_tiles interp superfx_clock gsu_cycles transparency echo_buffer), repeatable\n"
	    "  --audio-sim SCEN   audio pacing simulation in virtual time (S1..S7, S2+, S2-, all); ROM may be '-' for none.\n"
	    "                     with --tone (or ROM '-') the source is a synthetic tone, else the core's audio;\n"
	    "                     --legacy runs the first player's audio behaviour; --wav FILE writes the 48 kHz output;\n"
	    "                     --sim-minutes N sets the length; --sim-verbose prints a line per virtual second\n"
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
	std::vector<PadEv> script, script2, script3, script4, script5;
	struct Opt { std::string k; int v; };
	std::vector<Opt> opts;
	bool nowait = false;
	const char *backup_dir = 0, *unblock = 0;
	bool srm_async = false;
	bool summary = false;
	std::vector<std::string> cheat_codes, cheat_decode;
	struct CTog { int frame, index, on; };
	std::vector<CTog> ctog;
	const char *cheats_root = 0;
	int peek = -1, cheat_remove = -1;
	bool cheat_list = false, cheat_disable_all = false;
	AudioSimArgs sim;
	bool want_sim = false, force_tone = false;
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
		else if (arg("--backup-dir")) backup_dir = argv[++i];
		else if (arg("--unblock")) unblock = argv[++i];
		else if (!strcmp(argv[i], "--srm-async")) srm_async = true;
		else if (arg("--cheat")) cheat_codes.push_back(argv[++i]);
		else if (arg("--cheat-decode")) cheat_decode.push_back(argv[++i]);
		else if (arg("--cheats-root")) cheats_root = argv[++i];
		else if (arg("--cheat-toggle")) { CTog t; if (sscanf(argv[++i], "%d:%d:%d", &t.frame, &t.index, &t.on) == 3) ctog.push_back(t); }
		else if (arg("--cheat-remove")) cheat_remove = atoi(argv[++i]);
		else if (arg("--peek")) peek = (int) strtol(argv[++i], 0, 16);
		else if (!strcmp(argv[i], "--cheat-list")) cheat_list = true;
		else if (!strcmp(argv[i], "--cheat-disable-all")) cheat_disable_all = true;
		else if (arg("--save-state")) sstate = argv[++i];
		else if (arg("--load-state")) lstate = argv[++i];
		else if (arg("--state-frame")) state_frame = atoi(argv[++i]);
		else if (arg("--name")) name = argv[++i];
		else if (arg("--pad2")) script2 = parse_script(argv[++i]);
		else if (arg("--pad3")) script3 = parse_script(argv[++i]);
		else if (arg("--pad4")) script4 = parse_script(argv[++i]);
		else if (arg("--pad5")) script5 = parse_script(argv[++i]);
		else if (arg("--opt")) {
			char k[64]; int v = 0;
			if (sscanf(argv[++i], "%63[^=]=%d", k, &v) == 2) opts.push_back({ k, v });
		}
		else if (arg("--audio-sim")) { sim.scenario = argv[++i]; want_sim = true; }
		else if (arg("--sim-minutes")) sim.minutes = atof(argv[++i]);
		else if (!strcmp(argv[i], "--sim-verbose")) sim.verbose = true;
		else if (arg("--sim-kp")) sim.kp = atof(argv[++i]);
		else if (arg("--sim-ki")) sim.ki = atof(argv[++i]);
		else if (!strcmp(argv[i], "--tone")) force_tone = true;
		else if (!strcmp(argv[i], "--legacy")) sim.legacy = true;
		else if (!strcmp(argv[i], "--summary")) summary = true;
		else if (!strcmp(argv[i], "--nowait")) nowait = true;
		else { usage(); return 2; }
	}
	if (!cheat_decode.empty()) {
		/* no ROM needed: "snes_twin - --cheat-decode CODE" prints the code as address=value cells (Game Genie, Action Replay, raw), or the error */
		int bad = 0;
		for (auto &c : cheat_decode) {
			char raw[700], canon[700];
			int n = snes_host_cheat_decode(c.c_str(), raw, sizeof raw);
			if (n > 0 && snes_host_cheat_validate(c.c_str(), canon, sizeof canon) > 0) printf("%s -> %s (%d cell%s, %s)\n", c.c_str(), raw, n, n == 1 ? "" : "s", canon);
			else { printf("%s -> error %d\n", c.c_str(), n); bad++; }
		}
		return 0;
	}
	std::set<int> shotset;
	if (shots) {
		std::string t(shots);
		size_t p = 0;
		while (p < t.size()) { size_t q = t.find(',', p); if (q == std::string::npos) q = t.size(); shotset.insert(atoi(t.substr(p, q - p).c_str())); p = q + 1; }
	}

	if (want_sim) {
		sim.tone = force_tone || !strcmp(rom, "-");
		sim.wav = wav;
		if (!sim.tone) {
			if (snes_host_init()) { fprintf(stderr, "init failed\n"); return 1; }
			char e[128] = "";
			if (snes_host_load_rom(rom, NULL, e, sizeof e)) { fprintf(stderr, "load failed: %s\n", e); return 1; }
		}
		return audio_sim_run(sim);
	}
	if (snes_host_init()) { fprintf(stderr, "init failed\n"); return 1; }
	for (auto &o : opts) if (snes_host_set_option(o.k.c_str(), o.v)) { fprintf(stderr, "unknown option %s\n", o.k.c_str()); return 2; }
	if (backup_dir) snes_host_set_backup_dir(backup_dir);
	char err[128] = "";
	if (snes_host_load_rom(rom, srm, err, sizeof err)) { fprintf(stderr, "load failed: %s\n", err); return 1; }

	fprintf(stderr, "%s\n", snes_host_rom_info());
	if (cheats_root) {
		int r = snes_host_cheats_load(cheats_root);
		fprintf(stderr, "cheats: %d entries loaded\n", r);
	}
	if (cheat_remove >= 0) printf("cheat remove %d -> %d\n", cheat_remove, snes_host_cheat_remove(cheat_remove));
	for (auto &c : cheat_codes) {
		int r = snes_host_cheat_add(c.c_str(), NULL);
		char et[200];
		printf("cheat add %s -> %d\n", c.c_str(), r);
		if (snes_host_cheat_error(et, sizeof et)) printf("cheat file: %s\n", et);
	}
	FILE *hf = hashes ? fopen(hashes, "w") : 0;
	FILE *tf = timing ? fopen(timing, "w") : 0;
	if (tf) fprintf(tf, "frame,total_us,cpu_us,apu_us,ppu_us,chip_us,front_us\n");
	std::vector<int16_t> pcm;
	std::vector<int16_t> abuf(4096 * 2);

	uint64_t wall_tot_ns = 0, tot_ns = 0, cpu_tot_ns = 0, acc_ns[S9XP_N] = { 0 }, cnt_tot[S9XC_N] = { 0 };
	size_t ev = 0;
	unsigned pad = 0, last_serial = 0;
	g_s9xp.on = tf || summary ? 1 : 0;
	/* with --summary alone profiling is on; use no flags for a clean fps number */
	bool prof_on = g_s9xp.on;

	for (int f = 0; f < frames; f++) {
		while (ev < script.size() && script[ev].frame <= f) pad = script[ev++].mask;
		snes_host_set_pad(0, pad);
		{
			std::vector<PadEv> *ex[4] = { &script2, &script3, &script4, &script5 };
			static size_t eidx[4]; static unsigned em[4];
			for (int k = 0; k < 4; k++) {
				while (eidx[k] < ex[k]->size() && (*ex[k])[eidx[k]].frame <= f) em[k] = (*ex[k])[eidx[k]++].mask;
				snes_host_set_pad(k + 1, em[k]);
			}
		}
		for (auto &t : ctog) if (t.frame == f) printf("cheat toggle frame %d index %d on %d -> %d\n", f, t.index, t.on, snes_host_cheat_enable(t.index, t.on));
		if (lstate && f == state_frame) {
			int r = snes_host_state_load(lstate);
			fprintf(stderr, "load state: %d\n", r);
		}
		uint64_t t0 = now_ns(), c0 = cpu_ns();
		if (prof_on) { memset(g_s9xp.acc, 0, sizeof g_s9xp.acc); memset(g_s9xp.cnt, 0, sizeof g_s9xp.cnt); g_s9xp.cur = S9XP_CPU; g_s9xp.sp = 0; g_s9xp.last = s9xp_now(); }
		snes_host_run_frame();
		if (prof_on) s9xp_enter(S9XP_FRONT), s9xp_leave();
		uint64_t t1 = now_ns(), c1 = cpu_ns();
		tot_ns += t1 - t0;
		wall_tot_ns += t1 - t0;
		cpu_tot_ns += c1 - c0;
		if (prof_on) {
			for (int c = 0; c < S9XP_N; c++) acc_ns[c] += g_s9xp.acc[c];
			for (int c = 0; c < S9XC_N; c++) cnt_tot[c] += g_s9xp.cnt[c];
			if (tf) fprintf(tf, "%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", f, (t1 - t0) / 1e3, g_s9xp.acc[0] / 1e3,
			    g_s9xp.acc[1] / 1e3, g_s9xp.acc[2] / 1e3, g_s9xp.acc[3] / 1e3, g_s9xp.acc[4] / 1e3);
		}
		int n = snes_host_audio_take(abuf.data(), 4096);
		if (wav) pcm.insert(pcm.end(), abuf.begin(), abuf.begin() + n * 2);
		if (sstate && f == state_frame) fprintf(stderr, "save state: %d\n", snes_host_state_save(sstate));

		int w, h, pitch;
		const uint16_t *v;
		if (nowait) {
			/* the Vita's fetch: never waits; lists each new picture once (serial, size, hash) */
			unsigned serial = 0;
			w = 256; h = 224;
			v = snes_host_video_nowait(&w, &h, &pitch, &serial);
			if (hf && v && serial != last_serial) fprintf(hf, "%u %d %d %016llx\n", serial, w, h, (unsigned long long) fnv(v, w, h, pitch));
			last_serial = serial;
			if (shotset.count(f) && v) {
				char p[512];
				snprintf(p, sizeof p, "%s%05d.png", prefix.c_str(), f);
				write_png(p, v, w, h, pitch);
			}
			continue;
		}
		v = snes_host_video(&w, &h, &pitch);
		if (hf) fprintf(hf, "%d %d %d %016llx\n", f, w, h, (unsigned long long) fnv(v, w, h, pitch));
		if (shotset.count(f)) {
			char p[512];
			snprintf(p, sizeof p, "%s%05d.png", prefix.c_str(), f);
			write_png(p, v, w, h, pitch);
		}
	}
	snes_host_sync();
	{
		int n;
		while ((n = snes_host_audio_take(abuf.data(), 4096)) > 0)
			if (wav) pcm.insert(pcm.end(), abuf.begin(), abuf.begin() + n * 2);
	}
	if (peek >= 0) printf("peek %05X = %02X\n", peek, snes_host_peek_wram((uint32_t) peek));
	if (cheat_disable_all) snes_host_cheat_disable_all();
	if (cheat_list)
		for (int k = 0, n = snes_host_cheat_count(); k < n; k++) {
			snes_cheat_info ci;
			snes_host_cheat_get(k, &ci);
			printf("cheat %d: %s | %s | %s%s\n", k, ci.label, ci.code, ci.enabled ? "on" : "off", ci.usable ? "" : " (not a code)");
		}
	if (hf) fclose(hf);
	if (tf) fclose(tf);
	if (wav) write_wav(wav, pcm);
	int save_rc = 0;
	if (srm && snes_host_sram_dirty()) {
		/* the same calls as the Vita player: --srm-async copies the RAM (take), writes the copy (write) and, when that
		 * fails, forgets it so the RAM counts as unsaved again, exactly as the Vita's writer thread loop does */
		std::vector<uint8_t> buf(256 * 1024);
		for (int attempt = 0; attempt < 2; attempt++) {
			if (srm_async) {
				size_t n = snes_host_sram_take(buf.data(), buf.size());
				save_rc = snes_host_sram_write(buf.data(), n);
				if (save_rc) snes_host_sram_forget();
			} else
				save_rc = snes_host_sram_save();
			char why[200] = "";
			snes_host_save_error(why, sizeof why);
			fprintf(stderr, "sram save%s: %d%s%s (still unsaved: %d)\n", attempt ? " (retry)" : "", save_rc, why[0] ? " " : "", why, snes_host_sram_dirty());
			if (!save_rc || !unblock) break;
			remove(unblock);   /* the cause is gone: retry once, as the player does after its back-off */
			unblock = 0;
		}
	}

	/* fps comes from the thread's CPU time, not the wall clock: the build machine
	 * is shared and wall time includes time spent waiting for a core. */
	double secs = cpu_tot_ns / 1e9;
	double fps = frames / secs;
	if (summary) {
		double sum = 0;
		for (int c = 0; c < S9XP_N; c++) sum += acc_ns[c];
		printf("%-34s %s fps=%7.1f ms/frame=%6.3f cpu=%4.1f%% apu=%4.1f%% ppu=%4.1f%% chip=%4.1f%% front=%4.1f%%"
		    " ppu_flush/fr=%.1f apu_port/fr=%.1f apu_run/fr=%.1f chip_run/fr=%.1f\n",
		    name ? name : rom, snes_host_is_pal() ? "PAL " : "NTSC", fps, secs * 1000 / frames,
		    100 * acc_ns[0] / sum, 100 * acc_ns[1] / sum, 100 * acc_ns[2] / sum, 100 * acc_ns[3] / sum, 100 * acc_ns[4] / sum,
		    (double) cnt_tot[0] / frames, (double) cnt_tot[1] / frames, (double) cnt_tot[2] / frames, (double) cnt_tot[3] / frames);
	} else {
		printf("%s: %d frames in %.3f s = %.1f fps (CPU time, profiling %s)\n", name ? name : rom, frames, secs, fps, prof_on ? "on" : "off");
	}
	if (snes_host_threads_active()) {
		snes_thread_stats st;
		snes_host_thread_stats(&st);
		printf("threads(%d): main_cpu=%.3f s wall=%.3f s | apu busy=%.3f s cmds/fr=%.0f (65816 side ran %.0f/fr itself), waited %.1f/fr for %.3f s\n", snes_host_threads_active(), secs, wall_tot_ns / 1e9,
		    st.apu_busy_ns / 1e9, (double) st.apu_cmds / frames, (double) st.apu_helped / frames, (double) st.apu_waits / frames, st.apu_wait_ns / 1e9);
		printf("          ppu busy=%.3f s batches/fr=%.1f main waited %.2f/fr for %.3f s\n", st.ppu_busy_ns / 1e9, (double) st.ppu_batches / frames, (double) st.ppu_waits / frames, st.ppu_wait_ns / 1e9);
	}
	snes_host_shutdown();
	return save_rc ? 3 : 0;
}
