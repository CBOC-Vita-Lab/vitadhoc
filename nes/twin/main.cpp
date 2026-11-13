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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

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


/* ---- wide view truth checker (--wide-truth BASE): BASE.meta / BASE.truth come from tests/scroll_roms.py ---- */
static inline bool cen_x(int x, int m) { return x >= m && x < m + 256; }
struct WTruth {
	bool on = false;
	int W = 0, H = 0, n = 0, origin_x = 0, tx = 0x300, tset = 0x305, scene = 0x306;
	std::string select, mode;
	int scene_f = -100, scene_v = 0;   /* frame the scene variable last changed (the truth image lags a frame or two) */
	int hud_lo = 0, hud_hi = -1;       /* status bar rows of the cartridge (its numbers and text change; the truth holds the frame it started with) */
	std::vector<uint8_t> px;           /* n * W * H, bit7 known, bits0..5 colour */
	std::vector<char> uni;             /* per image per row: the whole row is one known colour */
	long pvh[16] = { 0 }, fade_l = 0, fade_r = 0, faded = 0, solid_l = 0, solid_r = 0, claimed = 0, wrong = 0, centre = 0, centre_bad = 0, hud_claimed = 0, soft_claim_frames = 0, bd_px = 0, bd_wrong = 0, bd_unknown = 0, maskfill = 0, maskfill_wrong = 0, maskfill_soft = 0, c_sprite = 0, c_trans = 0, c_hud = 0, c_mask = 0, c_unknown = 0, c_unexplained = 0;
	long lead_frames = 0; double lead_l = 0, lead_r = 0;
	std::vector<std::string> firsts;
	std::vector<std::pair<int, int>> wrongf;
	bool load(const std::string &base) {
		FILE *f = fopen((base + ".meta").c_str(), "r");
		if (!f) return false;
		char line[256];
		while (fgets(line, sizeof line, f)) {
			char k[64], v[128];
			if (sscanf(line, "%63[^=]=%127[^\n]", k, v) != 2) continue;
			if (!strcmp(k, "select")) select = v;
			else if (!strcmp(k, "mode")) mode = v;
			else if (!strcmp(k, "origin_x")) origin_x = atoi(v);
			else if (!strcmp(k, "ram_tx")) tx = (int) strtol(v, 0, 16);
			else if (!strcmp(k, "ram_tset")) tset = (int) strtol(v, 0, 16);
			else if (!strcmp(k, "ram_scene")) scene = (int) strtol(v, 0, 16);
			else if (!strcmp(k, "hud_rows")) { int a = 0, b = -1; if (sscanf(v, "%d-%d", &a, &b) == 2) { hud_lo = a; hud_hi = b; } }
		}
		fclose(f);
		f = fopen((base + ".truth").c_str(), "rb");
		if (!f) return false;
		uint32_t h[3];
		if (fread(h, 4, 3, f) != 3) { fclose(f); return false; }
		W = (int) h[0]; H = (int) h[1]; n = (int) h[2];
		px.resize((size_t) W * H * n);
		if (fread(px.data(), 1, px.size(), f) != px.size()) { fclose(f); return false; }
		fclose(f);
		uni.assign((size_t) n * H, 0);
		for (int i = 0; i < n; i++)
			for (int y = 0; y < H; y++) {
				const uint8_t *r = &px[((size_t) i * H + y) * W];
				bool u = (r[0] & 0x80) != 0;
				for (int x = 1; u && x < W; x++) u = r[x] == r[0];
				uni[(size_t) i * H + y] = u;
			}
		on = true;
		return true;
	}
	/* expected colour index at (world x, y) of image i, or -1 for unknown */
	int at(int i, int wx, int wy) const {
		if (wy < 0 || wy >= H) return -1;
		if (uni[(size_t) i * H + wy]) return px[((size_t) i * H + wy) * W] & 0x3F;
		if (wx < 0 || wx >= W) return -1;
		uint8_t v = px[((size_t) i * H + wy) * W + wx];
		return (v & 0x80) ? (v & 0x3F) : -1;
	}
};

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

static uint64_t fnvb(const uint8_t *p, size_t n)
{
	uint64_t hsh = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) { hsh ^= p[i]; hsh *= 1099511628211ull; }
	return hsh;
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
    "  --wide-hashes FILE  write 'frame width height picture prov real' fnv1a64 of each composed wide frame (golden test)\n"
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
	    "  --wide M           switch the wide view on with M margin px per side (0..96): nes_host_wide_frame runs every frame\n"
	    "  --wide-shots LIST  PNG of the composed wide picture at these frames (prefix + 'w')\n"
	    "  --wide-truth BASE  compare the wide picture with BASE.truth/.meta (tests/scroll_roms.py): counts claimed-real margin px that are wrong\n"
	    "  --wide-strips LIST PNG of truth (above) and composed picture (below) at these frames (prefix + 's')\n"
	    "  --wide-map-save F / --wide-map-load F   write / read the learned level map image (second pass)\n"
	    "  --wide-report      print the wide view statistics (mean real px per side, lines, scene events, map)\n"
	    "  --wide-opt NAME=V  wide view option (period, debug, fade_px, appear_frames)\n"
	    "  --wide-split       with --wide: capture on the main thread, compose (and the scout step) on real threads, one frame behind\n"
	    "  --scout N          wide view scout on: up to N scout frames after each frame (nes_host_scout_step)\n"
	    "  --scout-opt NAME=V scout option (ahead, resync)\n"
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
	const char *hashes = 0, *whashes = 0, *shots = 0, *wav = 0, *srm = 0, *sstate = 0, *lstate = 0, *name = 0, *backup = 0, *sysdir = 0;
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
	int wide = 0;
	bool wide_report = false;
	const char *wide_shots = 0, *wtruth_base = 0, *wstrips = 0, *wmap_save = 0, *wmap_load = 0;
	std::vector<Opt> wopts, sopts;
	int scout_n = 0;
	bool wide_split = false;
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
		else if (arg("--wide-hashes")) whashes = argv[++i];
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
		else if (arg("--wide")) wide = atoi(argv[++i]);
		else if (arg("--wide-shots")) wide_shots = argv[++i];
		else if (arg("--wide-truth")) wtruth_base = argv[++i];
		else if (arg("--wide-strips")) wstrips = argv[++i];
		else if (arg("--wide-map-save")) wmap_save = argv[++i];
		else if (arg("--wide-map-load")) wmap_load = argv[++i];
		else if (!strcmp(argv[i], "--wide-report")) wide_report = true;
		else if (!strcmp(argv[i], "--wide-split")) wide_split = true;
		else if (arg("--scout")) scout_n = atoi(argv[++i]);
		else if (arg("--scout-opt")) {
			char k[64]; int v = 0;
			if (sscanf(argv[++i], "%63[^=]=%d", k, &v) == 2) sopts.push_back({ k, v });
		}
		else if (arg("--wide-opt")) {
			char k[64]; int v = 0;
			if (sscanf(argv[++i], "%63[^=]=%d", k, &v) == 2) wopts.push_back({ k, v });
		}
		else if (!strcmp(argv[i], "--summary")) summary = true;
		else if (!strcmp(argv[i], "--info")) info = true;
		else if (!strcmp(argv[i], "--audio-report")) areport = true;
		else { usage(); return 2; }
	}
	std::set<int> shotset, wshotset, wstripset;
	if (wstrips) {
		std::string t(wstrips);
		size_t p = 0;
		while (p < t.size()) { size_t q = t.find(',', p); if (q == std::string::npos) q = t.size(); wstripset.insert(atoi(t.substr(p, q - p).c_str())); p = q + 1; }
	}
	if (wide_shots) {
		std::string t(wide_shots);
		size_t p = 0;
		while (p < t.size()) { size_t q = t.find(',', p); if (q == std::string::npos) q = t.size(); wshotset.insert(atoi(t.substr(p, q - p).c_str())); p = q + 1; }
	}
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
	if (scout_n > 0) {
		nes_host_scout_enable(1);
		for (auto &o : sopts) if (nes_host_scout_option(o.k.c_str(), o.v)) { fprintf(stderr, "unknown scout option %s\n", o.k.c_str()); return 2; }
	}
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

	if (wide > 0) {
		for (auto &o : wopts) if (nes_host_wide_option(o.k.c_str(), o.v)) { fprintf(stderr, "unknown wide option %s\n", o.k.c_str()); return 2; }
		nes_host_wide_enable(1);
		nes_host_wide_margin(wide);
		if (wtruth_base) nes_host_wide_option("debug", 1);
		if (wmap_load) {
			FILE *mf = fopen(wmap_load, "rb");
			if (mf) {
				std::vector<uint8_t> mb(8 << 20);
				size_t mn = fread(mb.data(), 1, mb.size(), mf);
				fclose(mf);
				fprintf(stderr, "map load: %d (%zu bytes)\n", nes_host_wide_map_load(mb.data(), mn), mn);
			}
		}
	}
	WTruth wt;
	if (wtruth_base && !wt.load(wtruth_base)) { fprintf(stderr, "cannot read truth %s\n", wtruth_base); return 2; }
	int cam[8] = { 0 };
	uint8_t oam[256] = { 0 };   /* the game's sprite list ($0200) as of the end of the previous frame: what the next frame draws */
	double rowl_sum = 0, rowr_sum = 0; long row_n = 0;
	double wsum_l = 0, wsum_r = 0, wlines = 0;
	long wcentre_diff = 0, wmaskfill = 0, scout_cost_frames = 0;
	long wframes = 0, wevents = 0, wmiss = 0, wchk = 0, wactive = 0, wblocked = 0;
	nes_wide_stats wst;
	memset(&wst, 0, sizeof wst);
	FILE *hf = hashes ? fopen(hashes, "w") : 0;
	FILE *whf = whashes ? fopen(whashes, "w") : 0;
	std::vector<int16_t> pcm, abuf(4096 * 2);
	uint64_t cpu_tot_ns = 0;
	size_t ev[4] = { 0, 0, 0, 0 };
	unsigned mask[4] = { 0, 0, 0, 0 };
	size_t fev = 0;
	size_t gev = 0, pev = 0;
	int rc = 0;

	/* everything done with a composed wide frame (hashes, centre check, truth); cam = the game's camera bytes read after the
	   previous frame. Split mode calls it one frame late with copies of that frame's video and cam. */
	auto wide_process = [&](int f, bool wok, const uint16_t *wp, int ww, int wpit, const nes_wide_stats &wst, const uint16_t *v, int w, int h, int pitch, const int *camin, const uint8_t *oam) {
		int cam[8];
		memcpy(cam, camin, sizeof cam);
		if (wok && whf) {
				const uint8_t *wprov = nes_host_wide_prov();
				const uint8_t *wreal = nes_host_wide_real();
				fprintf(whf, "%d %d %d %016llx %016llx %016llx\n", f, ww, h, (unsigned long long) fnv((const uint16_t *) wp, ww, h, wpit),
					(unsigned long long) (wprov ? fnvb(wprov, (size_t) ww * h) : 0), (unsigned long long) (wreal ? fnvb(wreal, (size_t) h * 2) : 0));
			}
			if (wok) {
				wframes++;
				/* the centre of the wide picture must be the original picture (the 8 masked columns may be filled) */
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++)
						if (wp[y * (wpit / 2) + wide + x] != v[y * (pitch / 2) + x]) { if (x < 8) wmaskfill++; else wcentre_diff++; }
				wsum_l += wst.real_l; wsum_r += wst.real_r; wlines += 1;
				wevents += wst.scene_event; wmiss = wst.hs_missed; wchk = wst.hs_checked;
				if (wst.layer_active) wactive++;
				if (wst.lines_blocked) wblocked++;
				if (wt.on && f >= 2) {
			if (cam[6] != wt.scene_v) { wt.scene_v = cam[6]; wt.scene_f = f; }
					int m = wide, img = 0;
					if (wt.select == "tset") img = cam[5]; else if (wt.select == "scene") img = cam[6];
					if (img < 0 || img >= wt.n) img = 0;
					int ent = 0;
					const uint16_t *pal = nes_host_palette(&ent);
					int cx = wt.mode == "static" ? wt.origin_x : cam[0] * 8 + cam[1];
					int cy = wt.mode == "static" ? 0 : cam[2] * 8 + cam[3];
					const uint8_t *real = nes_host_wide_real();
					const uint8_t *prov = nes_host_wide_prov();
					std::vector<uint16_t> strip;
					bool want = wstripset.count(f) != 0;
					if (want) strip.assign((size_t) ww * (h * 2 + 4), 0);
					for (int y = 0; y < h; y++) {
						int rl = real ? real[y * 2] : 0, rr = real ? real[y * 2 + 1] : 0;
						rowl_sum += rl; rowr_sum += rr; row_n++;
						for (int x = 0; x < ww; x++) {
							int e = wt.at(img, cx + x - m, cy + y);
							uint16_t got = wp[y * (wpit / 2) + x];
							bool ok = e >= 0 && pal[0x80 | e] == got;
							int pv = prov ? (prov[(size_t) y * ww + x] & 15) : 0;
							bool inreal = (x < m && x >= m - rl) || (x >= m + 256 && x < m + 256 + rr);
							bool claim = !cen_x(x, m) && (pv == 4 || pv == 8 || pv == 1);
							if (!cen_x(x, m) && (pv == 5 || pv == 9)) { wt.faded++; if (x < m) wt.fade_l++; else wt.fade_r++; }
							if (inreal) wt.pvh[pv]++;
							if (!cen_x(x, m) && pv == 3) { wt.bd_px++; if (e < 0) wt.bd_unknown++; else if (!ok) wt.bd_wrong++; }
							if (claim) { if (x < m) wt.solid_l++; else wt.solid_r++; }
							bool cen = x >= m && x < m + 256;
							if (claim) {
								wt.claimed++;
								if (!ok) {
									wt.wrong++;
									if (wt.wrongf.empty() || wt.wrongf.back().first != f) wt.wrongf.push_back({ f, 0 });
									wt.wrongf.back().second++;
									if (wt.firsts.size() < 6) {
										char b[160];
										snprintf(b, sizeof b, "frame %d x=%d y=%d (world %d,%d) expect %d got %04x", f, x - m, y, cx + x - m, cy + y, e, got);
										wt.firsts.push_back(b);
									}
								}
							}
							if (cen) {
								wt.centre++;
								if (!ok) {
									/* where the centre differs from the true BACKGROUND picture: sprites (the truth has none), the masked
									   columns, rows the truth does not know, or nothing explains it (must be 0) */
									wt.centre_bad++;
									bool spr = false;
									for (int k = 0; k < 64 && !spr; k++) {
										int sx = oam[k * 4 + 3], sy = oam[k * 4] + 1;
										if (oam[k * 4] < 240 && x - m >= sx && x - m < sx + 8 && y >= sy && y < sy + 8) spr = true;
									}
									if (spr) wt.c_sprite++; else if (e < 0) wt.c_unknown++; else if (f < 8 || f - wt.scene_f < 4) wt.c_trans++; else if (y >= wt.hud_lo && y <= wt.hud_hi) wt.c_hud++; else if (x - m < 8) wt.c_mask++; else wt.c_unexplained++;
								}
							}
							/* the masked left columns the wide view filled in: they must be the true picture */
							if (cen && x < m + 8 && got != v[y * (pitch / 2) + x - m]) { wt.maskfill++; if (pv == 2) wt.maskfill_soft++; else if (!ok) wt.maskfill_wrong++; }
							if (want) {
								strip[(size_t) y * ww + x] = e >= 0 ? pal[0x80 | e] : 0x4010;
								uint16_t c = got;
								if (claim && !ok) c = 0xF800;
								strip[(size_t) (y + h + 4) * ww + x] = c;
							}
						}
					}
					if (want) {
						long cb = 0, wr = 0, ylo = 999, yhi = -1, xlo = 9999, xhi = -9999;
						for (int y = 0; y < h; y++) {
							for (int x = 0; x < ww; x++) {
								int e = wt.at(img, cx + x - m, cy + y);
								bool ok = e >= 0 && pal[0x80 | e] == wp[y * (wpit / 2) + x];
								if (x >= m && x < m + 256 && !ok) cb++;
								if (!cen_x(x, m) && prov && ((prov[(size_t) y * ww + x] & 15) == 4 || (prov[(size_t) y * ww + x] & 15) == 8 || (prov[(size_t) y * ww + x] & 15) == 1) && !ok) { wr++; if (y < ylo) ylo = y; if (y > yhi) yhi = y; if (x - m < xlo) xlo = x - m; if (x - m > xhi) xhi = x - m; }
							}
						}
						fprintf(stderr, "frame %d: cam tx=%d fx=%d ty=%d fy=%d img=%d centre_bad=%ld wrong_real=%ld rows %ld..%ld x %ld..%ld\n", f, cam[0], cam[1], cam[2], cam[3], img, cb, wr, ylo, yhi, xlo, xhi);
					}
					if (want) {
						char p[512];
						snprintf(p, sizeof p, "%ss%05d.png", prefix.c_str(), f);
						write_png(p, strip.data(), ww, h * 2 + 4, ww * 2);
					}
				}
				if (wshotset.count(f)) {
					char p[512];
					snprintf(p, sizeof p, "%sw%05d.png", prefix.c_str(), f);
					write_png(p, wp, ww, h, wpit);
				}
			}
	};
	/* --wide-split: capture here, compose + scout step on worker threads */
	struct Worker {
		std::thread th; std::mutex m; std::condition_variable cv; std::function<void()> job; bool has = false, busy = false, quit = false;
		void start() { th = std::thread([this] { std::unique_lock<std::mutex> l(m); for (;;) { cv.wait(l, [this] { return has || quit; }); if (quit) return; std::function<void()> j = job; has = false; busy = true; l.unlock(); j(); l.lock(); busy = false; cv.notify_all(); } }); }
		void run(std::function<void()> j) { std::unique_lock<std::mutex> l(m); job = j; has = true; busy = true; cv.notify_all(); }
		void wait() { std::unique_lock<std::mutex> l(m); cv.wait(l, [this] { return !has && !busy; }); }
		void stop() { { std::lock_guard<std::mutex> l(m); quit = true; } cv.notify_all(); if (th.joinable()) th.join(); }
	};
	Worker wcomp, wscout;
	if (wide_split) { wcomp.start(); wscout.start(); }
	std::vector<nes_wide_snapshot> snaps(wide_split ? 2 : 0);
	for (auto &sn : snaps) memset(&sn, 0, sizeof sn);
	struct Pend { bool on = false; int f = 0, w = 0, h = 0, pitch = 0; std::vector<uint16_t> v; int cam[8]; uint8_t oam[256]; int slot = 0; bool scout = false; } pend;
	const uint16_t *cwp = 0; int cww = 0, cwpit = 0; int cwok = 0; nes_wide_stats cst;
	memset(&cst, 0, sizeof cst);
	auto finish_pending = [&]() {   // after the compose / scout step of the pending frame ran: process its results
		if (!pend.on) return;
		wcomp.wait(); wscout.wait();
		if (cwok) { wst = cst; wide_process(pend.f, true, cwp, cww, cwpit, wst, pend.v.data(), pend.w, pend.h, pend.pitch, pend.cam, pend.oam); }
		pend.on = false;
	};
	auto start_pending = [&]() {   // compose (and the scout step) of the pending snapshot run on their threads
		if (!pend.on) return;
		cwok = 0;
		const nes_wide_snapshot *ps = &snaps[pend.slot];
		wcomp.run([&, ps] { cwok = nes_host_wide_compose(ps, &cwp, &cww, &cwpit, &cst); });
		if (scout_n > 0) wscout.run([&] { scout_cost_frames += nes_host_scout_step(scout_n, 0); });
	};
	for (int f = 0; f < frames; f++) {
		for (int k = 0; k < 4; k++) {
			while (ev[k] < script[k].size() && script[k][ev[k]].frame <= f) mask[k] = script[k][ev[k]++].mask;
			nes_host_set_pad(k, mask[k]);
		}
		while (gev < gun.size() && gun[gev].frame <= f) { nes_host_set_zapper(gun[gev].x, gun[gev].y, gun[gev].t); gev++; }
		while (pev < pdl.size() && pdl[pev].frame <= f) { nes_host_set_paddle(pdl[pev].x, pdl[pev].t); pev++; }
		if (wide_split && ((lstate && f == state_frame) || (fev < fdsev.size() && fdsev[fev].frame <= f))) finish_pending();
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
		if (wide > 0 && !wide_split) {
			const uint16_t *wp = 0;
			int ww = 0, wpit = 0;
			int wok = nes_host_wide_frame(&wp, &ww, &wpit, &wst);
			if (wok && scout_n > 0) scout_cost_frames += nes_host_scout_step(scout_n, 0);
			wide_process(f, wok != 0, wp, ww, wpit, wst, v, w, h, pitch, cam, oam);
		}
		if (wide > 0 && wide_split) {
			/* the frame before this one: compose it now (it overlaps nothing of this frame's emulation that it reads) */
			finish_pending();
			if (nes_host_wide_capture(&snaps[f & 1])) {
				pend.on = true; pend.f = f; pend.w = w; pend.h = h; pend.pitch = pitch; pend.slot = f & 1;
				pend.v.assign(v, v + (size_t) (pitch / 2) * h);
				memcpy(pend.cam, cam, sizeof cam);
				memcpy(pend.oam, oam, sizeof oam);
				start_pending();   // overlaps the next iteration: its events, run_frame and audio
			}
		}
		if (wt.on) { for (int k = 0; k < 8; k++) cam[k] = nes_host_peek(wt.tx + k); for (int k = 0; k < 256; k++) oam[k] = (uint8_t) nes_host_peek(0x200 + k); }
		if (hf) fprintf(hf, "%d %d %d %016llx\n", f, w, h, (unsigned long long) fnv(v, w, h, pitch));
		if (shotset.count(f)) {
			char p[512];
			snprintf(p, sizeof p, "%s%05d.png", prefix.c_str(), f);
			write_png(p, v, w, h, pitch);
		}
	}
	if (wide_split) { finish_pending(); wcomp.stop(); wscout.stop(); }
	if (hf) fclose(hf);
	if (whf) fclose(whf);
	if (wide_report)
		printf("wide-centre: differing_px=%ld masked_columns_filled_px=%ld\n", wcentre_diff, wmaskfill),
		printf("wide: frames=%ld layer_active=%ld blocked=%ld mean_real_l=%.1f mean_real_r=%.1f scene_events=%ld hindsight=%ld/%ld ring=%dx%d map_cells=%d rooms=%d\n",
		    wframes, wactive, wblocked, wlines ? wsum_l / wlines : 0.0, wlines ? wsum_r / wlines : 0.0, wevents, wmiss, wchk, wst.ring_w, wst.ring_h, wst.map_cells, wst.map_rooms);
	if (scout_n > 0) {
		nes_scout_stats ss;
		nes_host_scout_stats(&ss);
		printf("scout: available=%d snapshots=%u frames=%u stops=%u cells=%u confirmed=%u contradicted=%u step_frames=%ld\n", ss.available, ss.snapshots, ss.frames, ss.stops, ss.cells, ss.confirmed, ss.contradicted, scout_cost_frames);
	}
	if (wt.on)
		printf("truth: claimed_real=%ld wrong=%ld faded_px=%ld solid_lead_l=%.1f solid_lead_r=%.1f drawn_lead_l=%.1f drawn_lead_r=%.1f centre_px=%ld centre_mismatch=%ld offered_l=%.1f offered_r=%.1f\n", wt.claimed, wt.wrong, wt.faded,
		    row_n ? (double) wt.solid_l / row_n : 0.0, row_n ? (double) wt.solid_r / row_n : 0.0,
		    row_n ? (double) (wt.solid_l + wt.fade_l) / row_n : 0.0, row_n ? (double) (wt.solid_r + wt.fade_r) / row_n : 0.0, wt.centre, wt.centre_bad, row_n ? rowl_sum / row_n : 0.0, row_n ? rowr_sum / row_n : 0.0);
	if (wt.on) printf("  backdrop continuation: px=%ld wrong=%ld unverifiable=%ld\n  masked columns filled: px=%ld wrong=%ld (continued as soft where no true pixel is known: px=%ld)\n  centre differs from the true background: %ld px = sprites %ld + status bar rows %ld + scene-change frames %ld + masked column %ld + truth unknown %ld + unexplained %ld\n", wt.bd_px, wt.bd_wrong, wt.bd_unknown, wt.maskfill, wt.maskfill_wrong, wt.maskfill_soft, wt.centre_bad, wt.c_sprite, wt.c_hud, wt.c_trans, wt.c_mask, wt.c_unknown, wt.c_unexplained);
	if (!wt.wrongf.empty()) { printf("  wrong frames (%zu):", wt.wrongf.size()); for (size_t i = 0; i < wt.wrongf.size() && i < 24; i++) printf(" %d:%d", wt.wrongf[i].first, wt.wrongf[i].second); printf("\n"); }
	if (wt.on) { printf("  prov in real px:"); for (int k = 0; k < 16; k++) if (wt.pvh[k]) printf(" %d:%ld", k, wt.pvh[k]); printf("\n"); }
	for (auto &t : wt.firsts) printf("  wrong: %s\n", t.c_str());
	if (wmap_save && wide > 0) {
		size_t need = nes_host_wide_map_save(0, 0);
		std::vector<uint8_t> mb(need ? need : 1);
		size_t got = need ? nes_host_wide_map_save(mb.data(), mb.size()) : 0;
		FILE *mf = fopen(wmap_save, "wb");
		if (mf) { fwrite(mb.data(), 1, got, mf); fclose(mf); }
		fprintf(stderr, "map save: %zu bytes\n", got);
	}
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
