/*
 * nes_twin: headless Linux runner for the NES program (FCEUmm core).
 * Loads a ROM, runs N frames with a scripted pad, writes per-frame hashes,
 * PNG screenshots, a WAV of the audio and a summary line.
 *
 * Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <string>
#include <vector>
#include <set>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "host.h"
#include "host_files.h"

extern "C" uint8_t NTARAM[0x800];   /* the core's nametable RAM (twin only: nescore.o does not export it) */

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
		{ "a", NES_A }, { "b", NES_B }, { "start", NES_START }, { "select", NES_SELECT },
		{ "up", NES_UP }, { "down", NES_DOWN }, { "left", NES_LEFT }, { "right", NES_RIGHT }
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
	uint32_t data = (uint32_t) (pcm.size() * 2), rate = nes_host_audio_rate(), br = rate * 4, riff = 36 + data;
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
	    "usage: nes_twin ROM [options]\n"
	    "  --frames N         frames to run (default 600)\n"
	    "  --pad SCRIPT       e.g. 60:start,70:none,200:a+right (buttons: a b start select up down left right)\n"
	    "  --pad2/--pad3/--pad4 SCRIPT   scripts for pads 2..4 (pads 3 and 4 need --opt fourscore=1 or a Four Score game)\n"
	    "  --hashes FILE      write 'frame width height fnv1a64' per frame\n"
	    "  --shots LIST       comma separated frame numbers to save as PNG\n"
	    "  --shot-prefix P    PNG file prefix (default shot_)\n"
	    "  --wav FILE         write all audio (stereo, at the audio rate)\n"
	    "  --srm FILE         battery RAM file (loaded if present, saved at exit)\n"
	    "  --backup-dir DIR   keep the first backup of an existing save in DIR (default: beside it as .bak)\n"
	    "  --sysdir DIR       folder that may hold disksys.rom (a disksys.rom beside the ROM is tried first)\n"
	    "  --save-state FILE --state-frame N   save a state after frame N\n"
	    "  --load-state FILE --state-frame N   load a state before frame N\n"
	    "  --opt NAME=VALUE   core option (region no_sprite_limit fourscore ram_init sound_quality lowpass), repeatable\n"
	    "  --fds-script LIST  FDS drive actions: 'frame:eject|insert|side', e.g. 100:eject,110:side,120:insert\n"
	    "  --audio-rate N     audio rate in Hz (default 48000)\n"
	    "  --cheat CODE       add cheat code(s): Game Genie, AAAA:VV or AAAA?CC:VV (repeatable)\n"
	    "  --render-off A-B   no drawing for frames A..B (fast forward path); the game itself is unchanged\n"
	    "  --state-sizes      save a state in memory every 25 frames, print the fixed size and whether all were equal\n"
	    "  --mem-load         load the ROM through the memory API (nes_host_load_mem) instead of the file API\n"
	    "  --verify-indexed   rebuild the picture from nes_host_palette + nes_host_video_indexed and compare it with nes_host_video\n"
	    "  --peek ADDR        print the byte the CPU would read at ADDR (hex) after the last frame (cheats show)\n"
	    "  --ra ADDR,LEN      print LEN bytes at ADDR (hex) through the achievements memory map after the last frame\n"
	    "  --info             print the cartridge line and exit after the load\n"
	    "  --audio-report     print frames, RMS, peak and the dominant frequency of the left channel\n"
	    "  --ram-dump FILE    write the 2 KiB of console RAM after the last frame\n"
	    "  --status           print the blargg style status bytes at $6000-$6003 after the last frame\n"
	    "  --nt-text          print the text on the nametables (tile number = ASCII, as the 2005 blargg ROMs draw it)\n"
	    "  --text             print the zero terminated text at $6004 after the last frame\n"
	    "  --sprite-exception CRC   add a CRC-32 (hex) to the no-flicker exception table\n"
	    "  --zapper SCRIPT    light gun: frame:x:y:trigger entries, e.g. 10:60:120:0,20:60:120:1 (x or y < 0 = off screen)\n"
	    "  --paddle SCRIPT    Arkanoid paddle: frame:x:button entries\n"
	    "  --summary          print one summary line (fps, ms/frame)\n"
	    "  --name NAME        label used in the summary line\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) { usage(); return 2; }
	const char *rom = argv[1];
	int peek_addr = -1;
	int arate = 0, roff_a = -1, roff_b = -2;
	unsigned ra_addr = 0, ra_len = 0;
	bool ra_on = false, vindexed = false, ssizes = false, memload = false;
	std::vector<std::string> cheats;
	int frames = 600, state_frame = -1;
	const char *hashes = 0, *shots = 0, *wav = 0, *srm = 0, *sstate = 0, *lstate = 0, *name = 0, *backup = 0, *sysdir = 0;
	const char *fdss = 0, *ramdump = 0;
	int peek = -1;
	bool text = false;
	bool nttext = false;
	std::string prefix = "shot_";
	std::vector<PadEv> script[4];
	struct GunEv { int frame, x, y, t; };
	std::vector<GunEv> gun, pdl;
	std::vector<uint32_t> sprite_ex;

	struct Opt { std::string k; int v; };
	std::vector<Opt> opts;
	bool summary = false, info = false, areport = false;
	for (int i = 2; i < argc; i++) {
		auto arg = [&](const char *n) { return !strcmp(argv[i], n) && i + 1 < argc; };
		if (arg("--frames")) frames = atoi(argv[++i]);
		else if (arg("--pad")) script[0] = parse_script(argv[++i]);
		else if (arg("--pad2")) script[1] = parse_script(argv[++i]);
		else if (arg("--pad3")) script[2] = parse_script(argv[++i]);
		else if (arg("--pad4")) script[3] = parse_script(argv[++i]);
		else if (arg("--zapper") || arg("--paddle")) {
			bool z = !strcmp(argv[i], "--zapper");
			std::string t(argv[++i]);
			size_t p = 0;
			while (p < t.size()) {
				size_t q = t.find(',', p);
				if (q == std::string::npos) q = t.size();
				GunEv e = { 0, 0, 0, 0 };
				std::string w = t.substr(p, q - p);
				if (z ? sscanf(w.c_str(), "%d:%d:%d:%d", &e.frame, &e.x, &e.y, &e.t) == 4
				      : sscanf(w.c_str(), "%d:%d:%d", &e.frame, &e.x, &e.t) == 3) (z ? gun : pdl).push_back(e);
				p = q + 1;
			}
		}
		else if (arg("--sprite-exception")) sprite_ex.push_back((uint32_t) strtoul(argv[++i], 0, 16));
		else if (arg("--hashes")) hashes = argv[++i];
		else if (arg("--shots")) shots = argv[++i];
		else if (arg("--shot-prefix")) prefix = argv[++i];
		else if (arg("--wav")) wav = argv[++i];
		else if (arg("--srm")) srm = argv[++i];
		else if (arg("--backup-dir")) backup = argv[++i];
		else if (arg("--sysdir")) sysdir = argv[++i];
		else if (arg("--save-state")) sstate = argv[++i];
		else if (arg("--load-state")) lstate = argv[++i];
		else if (arg("--state-frame")) state_frame = atoi(argv[++i]);
		else if (arg("--name")) name = argv[++i];
		else if (arg("--fds-script")) fdss = argv[++i];
		else if (arg("--ram-dump")) ramdump = argv[++i];
		else if (arg("--audio-rate")) arate = atoi(argv[++i]);
		else if (arg("--peek")) { peek_addr = (int) strtol(argv[++i], 0, 16); }
		else if (arg("--cheat")) cheats.push_back(argv[++i]);
		else if (arg("--render-off")) { if (sscanf(argv[++i], "%d-%d", &roff_a, &roff_b) != 2) { usage(); return 2; } }
		else if (arg("--ra")) { if (sscanf(argv[++i], "%x,%u", &ra_addr, &ra_len) != 2) { usage(); return 2; } ra_on = true; }
		else if (!strcmp(argv[i], "--state-sizes")) ssizes = true;
		else if (!strcmp(argv[i], "--verify-indexed")) vindexed = true;
		else if (!strcmp(argv[i], "--mem-load")) memload = true;
		else if (!strcmp(argv[i], "--status")) peek = 1;
		else if (!strcmp(argv[i], "--text")) text = true;
		else if (!strcmp(argv[i], "--nt-text")) nttext = true;
		else if (arg("--opt")) {
			char k[64]; int v = 0;
			if (sscanf(argv[++i], "%63[^=]=%d", k, &v) == 2) opts.push_back({ k, v });
		}
		else if (!strcmp(argv[i], "--summary")) summary = true;
		else if (!strcmp(argv[i], "--info")) info = true;
		else if (!strcmp(argv[i], "--audio-report")) areport = true;
		else { usage(); return 2; }
	}
	std::set<int> shotset;
	if (shots) {
		std::string t(shots);
		size_t p = 0;
		while (p < t.size()) { size_t q = t.find(',', p); if (q == std::string::npos) q = t.size(); shotset.insert(atoi(t.substr(p, q - p).c_str())); p = q + 1; }
	}
	struct FdsEv { int frame; int what; };
	std::vector<FdsEv> fdsev;
	if (fdss) {
		std::string t(fdss);
		size_t p = 0;
		while (p < t.size()) {
			size_t q = t.find(',', p);
			if (q == std::string::npos) q = t.size();
			std::string e = t.substr(p, q - p);
			size_t c = e.find(':');
			if (c != std::string::npos) {
				std::string w = e.substr(c + 1);
				fdsev.push_back({ atoi(e.substr(0, c).c_str()), w == "side" ? 1 : 0 });
			}
			p = q + 1;
		}
	}

	if (nes_host_init()) { fprintf(stderr, "init failed\n"); return 1; }
	for (auto &o : opts) if (nes_host_set_option(o.k.c_str(), o.v)) { fprintf(stderr, "unknown option %s\n", o.k.c_str()); return 2; }
	for (uint32_t c : sprite_ex) nes_host_sprite_exception_add(c);
	if (backup) nes_host_set_backup_dir(backup);
	if (sysdir) nes_host_set_system_dir(sysdir);
	if (arate && nes_host_set_audio_rate((unsigned) arate)) { fprintf(stderr, "bad audio rate\n"); return 2; }
	char err[200] = "";
	if (memload) {
		/* the in-process route: the caller owns the buffers; no file layer */
		FILE *rf = fopen(rom, "rb");
		if (!rf) { fprintf(stderr, "load failed: cannot open\n"); return 1; }
		std::vector<uint8_t> rb;
		uint8_t tmp[65536];
		size_t got;
		while ((got = fread(tmp, 1, sizeof tmp, rf)) > 0) rb.insert(rb.end(), tmp, tmp + got);
		fclose(rf);
		if (sysdir) {
			std::string bp = std::string(sysdir) + "/disksys.rom";
			FILE *bf = fopen(bp.c_str(), "rb");
			if (bf) {
				uint8_t bios[8192];
				size_t bn = fread(bios, 1, sizeof bios, bf);
				fclose(bf);
				nes_host_set_fds_bios(bios, bn);
			}
		}
		if (nes_host_load_mem(rb.data(), rb.size(), rom, err, sizeof err)) { fprintf(stderr, "load failed: %s\n", err); return 1; }
		if (srm) {
			FILE *sf = fopen(srm, "rb");
			if (sf) {
				std::vector<uint8_t> sb(nes_host_sram_size() + 1);
				size_t sn = fread(sb.data(), 1, sb.size(), sf);
				fclose(sf);
				nes_host_sram_load(sb.data(), sn);
			}
		}
	} else if (nes_host_load_rom(rom, srm, err, sizeof err)) { fprintf(stderr, "load failed: %s\n", err); return 1; }
	for (auto &c : cheats) fprintf(stderr, "cheat '%s': %d accepted\n", c.c_str(), nes_host_cheat_add(c.c_str()));
	std::vector<size_t> ssz;
	std::vector<uint8_t> sbuf(nes_host_state_size());
	fprintf(stderr, "%s\n", nes_host_rom_info());
	if (info) {
		printf("%s\n", nes_host_rom_info());
		printf("fourscore_active=%d region=%d detected=%d sram=%u\n", nes_host_fourscore_active(), nes_host_region(),
		    nes_host_region_detected(), (unsigned) nes_host_sram_size());
		printf("zapper_active=%d paddle_active=%d\n", nes_host_zapper_active(), nes_host_paddle_active());
		nes_host_shutdown();
		return 0;
	}

	FILE *hf = hashes ? fopen(hashes, "w") : 0;
	std::vector<int16_t> pcm, abuf(4096 * 2);
	uint64_t cpu_tot_ns = 0;
	size_t ev[4] = { 0, 0, 0, 0 };
	unsigned mask[4] = { 0, 0, 0, 0 };
	size_t fev = 0;
	size_t gev = 0, pev = 0;
	int rc = 0;

	for (int f = 0; f < frames; f++) {
		for (int k = 0; k < 4; k++) {
			while (ev[k] < script[k].size() && script[k][ev[k]].frame <= f) mask[k] = script[k][ev[k]++].mask;
			nes_host_set_pad(k, mask[k]);
		}
		while (gev < gun.size() && gun[gev].frame <= f) { nes_host_set_zapper(gun[gev].x, gun[gev].y, gun[gev].t); gev++; }
		while (pev < pdl.size() && pdl[pev].frame <= f) { nes_host_set_paddle(pdl[pev].x, pdl[pev].t); pev++; }
		if (lstate && f == state_frame) {
			int r = nes_host_state_load(lstate);
			fprintf(stderr, "load state: %d\n", r);
			if (r) rc = 1;
		}
		while (fev < fdsev.size() && fdsev[fev].frame <= f) {
			if (fdsev[fev].what == 1) nes_host_fds_next_side(); else nes_host_fds_toggle_disk();
			fev++;
		}
		nes_host_set_render(!(f >= roff_a && f <= roff_b));
		uint64_t c0 = cpu_ns();
		nes_host_run_frame();
		cpu_tot_ns += cpu_ns() - c0;
		if (ssizes && f % 25 == 0) ssz.push_back(nes_host_state_save_mem(sbuf.data(), sbuf.size()));
		int n = nes_host_audio_take(abuf.data(), 4096);
		if (wav || areport) pcm.insert(pcm.end(), abuf.begin(), abuf.begin() + n * 2);
		if (sstate && f == state_frame) {
			int r = nes_host_state_save(sstate);
			fprintf(stderr, "save state: %d\n", r);
			if (r) rc = 1;
		}
		int w, h, pitch;
		const uint16_t *v = nes_host_video(&w, &h, &pitch);
		if (hf) fprintf(hf, "%d %d %d %016llx\n", f, w, h, (unsigned long long) fnv(v, w, h, pitch));
		if (shotset.count(f)) {
			char p[512];
			snprintf(p, sizeof p, "%s%05d.png", prefix.c_str(), f);
			write_png(p, v, w, h, pitch);
		}
	}
	if (hf) fclose(hf);
	if (ssizes) {
		bool eq = !ssz.empty() && ssz[0] != 0;
		for (size_t sz : ssz) eq = eq && sz == ssz[0];
		printf("state_size=%zu samples=%zu all_equal=%d\n", nes_host_state_size(), ssz.size(), eq ? 1 : 0);
		if (!eq) rc = 1;
	}
	if (vindexed) {
		const uint8_t *idx = 0, *emph = 0;
		int ent = 0, w, h, pitch;
		const uint16_t *pal = nes_host_palette(&ent);
		const uint16_t *v = nes_host_video(&w, &h, &pitch);
		int ok = nes_host_video_indexed(&idx, &emph) && ent >= 768;
		unsigned bad = 0;
		for (int y = 0; ok && y < h; y++)
			for (int x = 0; x < w; x++) {
				unsigned d = emph[y * 256], px = idx[y * 256 + x];
				uint16_t c = d ? pal[256 + ((d & 7) << 6) + (px & 0x3F)] : pal[px];
				if (c != v[y * (pitch / 2) + x]) bad++;
			}
		printf("indexed_match=%d bad_pixels=%u\n", ok && !bad ? 1 : 0, bad);
		if (!ok || bad) rc = 1;
	}
	if (peek_addr >= 0) printf("peek %04x=%02x\n", peek_addr, nes_host_peek((unsigned) peek_addr));
	if (ra_on) {
		std::vector<uint8_t> rb(ra_len + 1);
		int got = nes_host_ra_read(ra_addr, rb.data(), ra_len);
		printf("ra %04x n=%d:", ra_addr, got);
		for (int i = 0; i < got; i++) printf(" %02x", rb[i]);
		printf("\n");
	}
	if (wav) write_wav(wav, pcm);
	if (areport) {
		/* dominant frequency of the left channel by counting rising zero crossings */
		size_t frames_out = pcm.size() / 2;
		double sum = 0;
		int peak = 0;
		unsigned cross = 0;
		for (size_t i = 0; i < frames_out; i++) {
			int s = pcm[i * 2];
			sum += (double) s * s;
			if (abs(s) > peak) peak = abs(s);
			if (i && pcm[(i - 1) * 2] < 0 && s >= 0) cross++;
		}
		printf("audio frames=%zu rms=%.1f peak=%d rising_crossings=%u approx_hz=%.1f\n", frames_out,
		    frames_out ? sqrt(sum / frames_out) : 0.0, peak, cross, frames_out ? cross * (double) nes_host_audio_rate() / frames_out : 0.0);
	}
	if (ramdump) {
		FILE *rf = fopen(ramdump, "wb");
		if (rf) {
			for (unsigned a = 0; a < 0x800; a++) fputc(nes_host_peek(a), rf);
			fclose(rf);
		}
	}
	if (peek >= 0) {
		/* a blargg style result: status byte at $6000, signature DE B0 61 at $6001, text from $6004 */
		printf("peek $6000=%02x sig=%02x%02x%02x\n", nes_host_peek(0x6000), nes_host_peek(0x6001), nes_host_peek(0x6002), nes_host_peek(0x6003));
	}
	if (nttext) {
		printf("nt:");
		for (int pg = 0; pg < 2; pg++)
			for (int y = 0; y < 30; y++) {
				char ln[33];
				bool any = false;
				for (int x = 0; x < 32; x++) {
					unsigned c = NTARAM[pg * 0x400 + y * 32 + x];
					ln[x] = c >= 32 && c < 127 ? (char) c : ' ';
					if (ln[x] != ' ') any = true;
				}
				ln[32] = 0;
				if (any) printf(" | %s", ln);
			}
		printf("\n");
	}
	if (text) {
		std::string t;
		for (unsigned a = 0x6004; a < 0x8000; a++) {
			int c = nes_host_peek(a);
			if (c <= 0) break;
			t += (char) c;
		}
		printf("text: %s\n", t.c_str());
	}
	if (srm && nes_host_sram_dirty()) {
		int r = nes_host_sram_save();
		fprintf(stderr, "sram save: %d%s%s\n", r, r ? " " : "", r ? nes_host_last_error() : "");
		if (r) rc = 1;
	}

	/* fps comes from the thread's CPU time, not the wall clock: the build machine is shared */
	double secs = cpu_tot_ns / 1e9;
	double fps = secs > 0 ? frames / secs : 0;
	if (summary)
		printf("%-34s %s fps=%8.1f ms/frame=%6.3f\n", name ? name : rom, nes_host_is_pal() ? "PAL " : "NTSC", fps, secs * 1000 / frames);
	else
		printf("%s: %d frames in %.3f s = %.1f fps (CPU time)\n", name ? name : rom, frames, secs, fps);
	if (summary) {
		/* peak resident memory of this process (Linux): the console, the buffers and the runner itself */
		FILE *sf = fopen("/proc/self/status", "r");
		char ln[128];
		while (sf && fgets(ln, sizeof ln, sf))
			if (!strncmp(ln, "VmHWM:", 6)) { printf("peak_rss_kb=%ld\n", strtol(ln + 6, 0, 10)); break; }
		if (sf) fclose(sf);
	}
	nes_host_shutdown();
	return rc;
}
