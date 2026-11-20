// Headless runner for the Sega host (md/frontend/host.h): runs a ROM for N frames with a scripted pad, prints hashes of the
// picture, the audio and the state at checkpoints, frame time statistics, and the save-state round trip.
// Copyright CBOC-Vita. MIT licence for this file; the core in ../core keeps its own licence (non-commercial).
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>
#include <algorithm>

typedef unsigned long long u64;
static const u64 FNV0 = 1469598103934665603ULL, FNVP = 1099511628211ULL;

static u64 hash_bytes(u64 h, const void *p, size_t n)
{
	const unsigned char *c = (const unsigned char *)p;
	size_t i = 0;
	for (; i + 4 <= n; i += 4) { unsigned w; memcpy(&w, c + i, 4); h = (h ^ w) * FNVP; h ^= h >> 29; }
	for (; i < n; i++) { h ^= c[i]; h *= FNVP; }
	return h;
}

static double now()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

struct Step { int frame; unsigned mask; };
struct Script { std::vector<Step> steps; bool autoplay = false; };

static unsigned parse_buttons(const std::string &s)
{
	unsigned m = 0;
	size_t i = 0;
	while (i <= s.size()) {
		size_t j = s.find('+', i);
		if (j == std::string::npos) j = s.size();
		std::string t = s.substr(i, j - i);
		if (t == "u" || t == "up") m |= MD_UP; else if (t == "d" || t == "down") m |= MD_DOWN;
		else if (t == "l" || t == "left") m |= MD_LEFT; else if (t == "r" || t == "right") m |= MD_RIGHT;
		else if (t == "a") m |= MD_A; else if (t == "b") m |= MD_B; else if (t == "c") m |= MD_C;
		else if (t == "x") m |= MD_X; else if (t == "y") m |= MD_Y; else if (t == "z") m |= MD_Z;
		else if (t == "start") m |= MD_START; else if (t == "mode" || t == "select") m |= MD_MODE;
		i = j + 1;
	}
	return m;
}

static bool parse_script(const char *s, Script &sc)
{
	std::string t = s;
	if (t == "auto") { sc.autoplay = true; return true; }
	if (t == "none") return true;
	size_t i = 0;
	while (i < t.size()) {
		size_t j = t.find(',', i);
		if (j == std::string::npos) j = t.size();
		std::string e = t.substr(i, j - i);
		size_t c = e.find(':');
		if (c == std::string::npos) return false;
		Step st; st.frame = atoi(e.substr(0, c).c_str()); st.mask = parse_buttons(e.substr(c + 1));
		sc.steps.push_back(st);
		i = j + 1;
	}
	return true;
}

// Default script ("auto"): a deterministic pseudo-random walk over every button, with Start pulses; it exercises menus, intros
// and games without knowing them. Not a recording of anything.
static unsigned auto_mask(int f, int player)
{
	unsigned s = (unsigned)(f / 24) * 2654435761u + (unsigned)player * 40503u;
	s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
	unsigned m = 0;
	if (s & 0x1) m |= MD_RIGHT;
	if (s & 0x2) m |= MD_LEFT;
	if (s & 0x4) m |= MD_UP;
	if (s & 0x8) m |= MD_DOWN;
	if (s & 0x10) m |= MD_A;
	if (s & 0x20) m |= MD_B;
	if (s & 0x40) m |= MD_C;
	if (s & 0x80) m |= MD_X;
	if (s & 0x100) m |= MD_Y;
	if (s & 0x200) m |= MD_Z;
	if (m & MD_LEFT && m & MD_RIGHT) m &= ~MD_LEFT;
	if (m & MD_UP && m & MD_DOWN) m &= ~MD_UP;
	if (player == 0 && (f % 150) >= 40 && (f % 150) < 46) m |= MD_START;
	return m;
}

static unsigned mask_at(const Script &sc, int f, int player)
{
	if (sc.autoplay) return auto_mask(f, player);
	unsigned m = 0;
	for (size_t i = 0; i < sc.steps.size(); i++) if (sc.steps[i].frame <= f) m = sc.steps[i].mask;
	return m;
}


// Debug aid (MD_TWIN_GLOBDIFF=frames): compares every writable global of the process (data + bss) between the continuous run and the
// run that went through a state load, N frames after the load. Prints the byte ranges that differ as offsets from md_host_run_frame
// (tools: md/twin/tools/globdiff.py maps them to the core's variable names with nm).
extern "C" char __data_start, _end;
static std::vector<uint8_t> snap_globals() { return std::vector<uint8_t>(&__data_start, &_end); }
static void print_globdiff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, const char *what)
{
	size_t k = 0, shown = 0;
	long base = (long)(uintptr_t)&md_host_run_frame;
	long d0 = (long)(uintptr_t)&__data_start - base;
	while (k < a.size() && k < b.size() && shown < 400) {
		if (a[k] != b[k]) {
			size_t e = k;
			while (e < a.size() && a[e] != b[e]) e++;
			printf("GLOBDIFF %s %ld %zu\n", what, d0 + (long)k, e - k);
			k = e; shown++;
		} else k++;
	}
}

struct Acc { u64 v = FNV0, a = FNV0; unsigned frames = 0; size_t samples = 0; };

static std::vector<uint8_t> g_rom;
static Script g_p1, g_p2;

static std::vector<u64> g_fh;   // per frame picture hash (debug: MD_TWIN_VFRAMES)

static void step_frame(int f, Acc &acc, double *dt)
{
	static int16_t abuf[4800 * 2];
	md_host_set_pad(0, mask_at(g_p1, f, 0));
	md_host_set_pad(1, mask_at(g_p2, f, 1));
	double t0 = now();
	md_host_run_frame();
	double t1 = now();
	if (dt) *dt = t1 - t0;
	int w, h, pitch;
	const uint16_t *px = md_host_video(&w, &h, &pitch);
	int dim[2] = { w, h };
	u64 v0 = acc.v;
	acc.v = hash_bytes(acc.v, dim, sizeof dim);
	for (int y = 0; y < h; y++) acc.v = hash_bytes(acc.v, (const char *)px + (size_t)y * pitch, (size_t)w * 2);
	g_fh.push_back(acc.v ^ v0);
	int n = md_host_audio_take(abuf, 4800);
	acc.a = hash_bytes(acc.a, abuf, (size_t)n * 4);
	acc.samples += (size_t)n; acc.frames++;
}

static u64 state_hash(std::vector<uint8_t> &buf)
{
	size_t n = md_host_state_save_mem(buf.data(), buf.size());
	return n ? hash_bytes(FNV0, buf.data(), n) : 0;
}

static void usage()
{
	puts("usage: md_twin ROM [--frames N] [--pad SCRIPT|auto|none] [--pad2 SCRIPT] [--seed N] [--region auto|usa|eur|jp|jppal]\n"
	     "       [--system md|sms|gg|sg] [--pad3|--pad6] [--opt name=value]... [--checkpoint N] [--hashes FILE] [--state-roundtrip [N]]\n"
	     "       [--srm FILE] [--save-state FILE] [--load-state FILE] [--state-frame N] [--audio-rate HZ] [--no-render] [--quiet]\n"
	     "       [--cheat CODE] [--peek HEXADDR]... [--reset-at FRAME] [--info]\n"
	     "SCRIPT = FRAME:buttons,FRAME:buttons ... (buttons joined with +: u d l r a b c x y z start mode; none)");
}

int main(int argc, char **argv)
{
	std::vector<unsigned> peeks;
	int reset_at = -1;
	const char *rom = NULL, *hashes = NULL, *srm = NULL, *save_st = NULL, *load_st = NULL, *cheat = NULL;
	int frames = 1500, checkpoint = 100, region = 0, system = 0, padtype = 0, roundtrip = 0, state_frame = 300, quiet = 0, norender = 0, info = 0;
	unsigned seed = 1, arate = 0;
	std::vector<std::string> opts;
	g_p1.autoplay = true;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto need = [&](void) -> const char * { if (i + 1 >= argc) { usage(); exit(2); } return argv[++i]; };
		if (a == "--frames") frames = atoi(need());
		else if (a == "--pad") { g_p1 = Script(); if (!parse_script(need(), g_p1)) { usage(); return 2; } }
		else if (a == "--pad2") { g_p2 = Script(); if (!parse_script(need(), g_p2)) { usage(); return 2; } }
		else if (a == "--seed") seed = (unsigned)strtoul(need(), NULL, 0);
		else if (a == "--region") { std::string r = need(); region = r == "usa" ? MD_REGION_USA : r == "eur" ? MD_REGION_EUROPE : r == "jp" ? MD_REGION_JAPAN_NTSC : r == "jppal" ? MD_REGION_JAPAN_PAL : 0; }
		else if (a == "--system") { std::string r = need(); system = r == "md" ? MD_SYS_MD : r == "sms" ? MD_SYS_SMS : r == "gg" ? MD_SYS_GG : r == "sg" ? MD_SYS_SG : 0; }
		else if (a == "--pad3") padtype = MD_PAD_3;
		else if (a == "--pad6") padtype = MD_PAD_6;
		else if (a == "--opt") opts.push_back(need());
		else if (a == "--checkpoint") checkpoint = atoi(need());
		else if (a == "--hashes") hashes = need();
		else if (a == "--state-roundtrip") { roundtrip = 1; if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') state_frame = atoi(argv[++i]); }
		else if (a == "--state-frame") state_frame = atoi(need());
		else if (a == "--srm") srm = need();
		else if (a == "--save-state") save_st = need();
		else if (a == "--load-state") load_st = need();
		else if (a == "--audio-rate") arate = (unsigned)atoi(need());
		else if (a == "--no-render") norender = 1;
		else if (a == "--cheat") cheat = need();
		else if (a == "--peek") peeks.push_back((unsigned)strtoul(need(), NULL, 16));
		else if (a == "--reset-at") reset_at = atoi(need());
		else if (a == "--info") info = 1;
		else if (a == "--quiet") quiet = 1;
		else if (a[0] != '-' && !rom) rom = argv[i];
		else { usage(); return 2; }
	}
	if (!rom) { usage(); return 2; }
	FILE *f = fopen(rom, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", rom); return 1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	g_rom.resize((size_t)n);
	if (fread(g_rom.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }
	fclose(f);

	if (md_host_init()) { fprintf(stderr, "init failed\n"); return 1; }
	for (size_t i = 0; i < opts.size(); i++) {
		size_t e = opts[i].find('=');
		if (e == std::string::npos || md_host_set_option(opts[i].substr(0, e).c_str(), atoi(opts[i].c_str() + e + 1))) { fprintf(stderr, "bad option %s\n", opts[i].c_str()); return 2; }
	}
	md_host_set_seed(seed);
	if (arate) md_host_set_audio_rate(arate);
	if (region) md_host_set_region(region);
	if (padtype) { md_host_set_pad_type(0, padtype); md_host_set_pad_type(1, padtype); }
	char err[128];
	const char *bn = strrchr(rom, '/'); bn = bn ? bn + 1 : rom;
	if (md_host_load_mem(g_rom.data(), g_rom.size(), bn, system, err, sizeof err)) { printf("LOADFAIL %s: %s\n", bn, err); return 1; }
	if (padtype) { md_host_set_pad_type(0, padtype); md_host_set_pad_type(1, padtype); }
	if (norender) md_host_set_render(0);
	if (cheat) printf("cheats accepted: %d\n", md_host_cheat_add(cheat));
	if (info) {
		md_video_info vi; md_host_video_info(&vi);
		printf("system=%d hw=\"%s\" title=\"%s\" crc=%08X region=%d(detected %d) pal=%d fps=%.4f pad0=%d pad1=%d sram_cap=%zu state=%zu\n%s\n",
			md_host_system(), md_host_system_hw(), md_host_rom_title(), md_host_rom_crc(), md_host_region(), md_host_region_detected(),
			md_host_is_pal(), md_host_fps(), md_host_pad_type(0), md_host_pad_type(1), md_host_sram_capacity(), md_host_state_size(), md_host_rom_info());
		printf("video %dx%d native %dx%d h40=%d interlace=%d pal=%d left_mask=%d gg_window=%d dar=%.4f par=%.4f\n", vi.w, vi.h, vi.native_w, vi.native_h, vi.h40, vi.interlace, vi.pal, vi.left_mask, vi.gg_window, vi.dar, vi.par);
	}
	if (srm) {
		FILE *s = fopen(srm, "rb");
		if (s) { std::vector<uint8_t> b(0x10000); size_t k = fread(b.data(), 1, b.size(), s); fclose(s); md_host_sram_write(b.data(), k); }
	}
	std::vector<uint8_t> sbuf(md_host_state_size());
	if (load_st) {
		FILE *s = fopen(load_st, "rb");
		std::vector<uint8_t> b(md_host_state_size());
		if (!s || fread(b.data(), 1, b.size(), s) != b.size() || md_host_state_load_mem(b.data(), b.size())) { printf("LOADSTATE FAIL\n"); return 1; }
		fclose(s);
	}

	FILE *hf = hashes ? fopen(hashes, "w") : NULL;
	Acc all;
	std::vector<double> dts;
	dts.reserve((size_t)frames);
	int f0 = load_st ? state_frame : 0;
	unsigned long long nz = 0;
	for (int fr = 0; fr < frames; fr++) {
		double dt;
		if (fr == reset_at) md_host_reset();
		step_frame(f0 + fr, all, &dt);
		dts.push_back(dt);
		if (save_st && fr + 1 == state_frame) {
			size_t sn = md_host_state_save_mem(sbuf.data(), sbuf.size());
			FILE *s = fopen(save_st, "wb"); if (s) { fwrite(sbuf.data(), 1, sn, s); fclose(s); }
		}
		if (checkpoint > 0 && ((fr + 1) % checkpoint == 0 || fr + 1 == frames)) {
			u64 sh = state_hash(sbuf);
			if (hf) fprintf(hf, "%d %016llx %016llx %016llx\n", f0 + fr + 1, all.v, all.a, sh);
		}
	}
	(void)nz;
	for (unsigned pa : peeks) printf("PEEK %06X=%02X\n", pa, md_host_peek(pa));
	if (hf) fclose(hf);
	u64 sh = state_hash(sbuf);

	// battery RAM
	if (srm && md_host_sram_dirty()) {
		std::vector<uint8_t> b(0x10000);
		size_t k = md_host_sram_read(b.data(), b.size());
		FILE *s = fopen(srm, "wb"); if (s) { fwrite(b.data(), 1, k, s); fclose(s); }
		md_host_sram_mark_saved();
	}

	std::vector<double> s2 = dts;
	size_t skip = s2.size() > 60 ? 30 : 0;
	std::sort(s2.begin() + (long)skip, s2.end());
	double p50 = s2.size() > skip ? s2[skip + (s2.size() - skip) / 2] : 0, p99 = s2.size() > skip ? s2[skip + (s2.size() - skip) * 99 / 100] : 0;
	double tot = 0; for (size_t i = 0; i < dts.size(); i++) tot += dts[i];
	md_video_info vi; md_host_video_info(&vi);
	u64 rh = FNV0;
	{ std::vector<uint8_t> rb(0x10000); int rn = md_host_ra_read(0, rb.data(), (unsigned)rb.size()); if (rn > 0) rh = hash_bytes(FNV0, rb.data(), (size_t)rn); }
	if (!quiet)
		printf("RESULT %s sys=%d frames=%d video=%016llx audio=%016llx state=%016llx ram=%016llx samples=%zu size=%dx%d p50=%.3fms p99=%.3fms avg=%.3fms\n",
			bn, md_host_system(), frames, all.v, all.a, sh, rh, all.samples, vi.w, vi.h, p50 * 1e3, p99 * 1e3, tot / (double)dts.size() * 1e3);

	if (roundtrip) {
		// run to state_frame from a fresh power on, save, run M frames hashing video, load, run M frames again, compare
		md_host_load_mem(g_rom.data(), g_rom.size(), bn, system, err, sizeof err);
		if (padtype) { md_host_set_pad_type(0, padtype); md_host_set_pad_type(1, padtype); }
		Acc skipacc;
		for (int fr = 0; fr < state_frame; fr++) step_frame(fr, skipacc, NULL);
		std::vector<uint8_t> st(md_host_state_size());
		if (!md_host_state_save_mem(st.data(), st.size())) { printf("ROUNDTRIP %s save failed\n", bn); return 1; }
		const int M = 300;
		Acc a1, a2;
		int gdn = getenv("MD_TWIN_GLOBDIFF") ? atoi(getenv("MD_TWIN_GLOBDIFF")) : -1;
		std::vector<uint8_t> gA, gB;
		for (int fr = 0; fr < M; fr++) { step_frame(state_frame + fr, a1, NULL); if (fr + 1 == gdn) gA = snap_globals(); }
		u64 s1 = state_hash(sbuf);
		std::vector<uint8_t> sA = sbuf;
		std::vector<u64> fh1(g_fh.end() - M, g_fh.end());
		int rc = md_host_state_load_mem(st.data(), st.size());
		if (gdn == 0) gA = std::vector<uint8_t>();
		for (int fr = 0; fr < M; fr++) { step_frame(state_frame + fr, a2, NULL); if (fr + 1 == gdn) { gB = snap_globals(); print_globdiff(gA, gB, "after"); } }
		u64 s2h = state_hash(sbuf);
		if (getenv("MD_TWIN_VFRAMES")) { int shown = 0; for (int i = 0; i < M && shown < 5; i++) if (fh1[i] != g_fh[g_fh.size() - M + i]) { printf("  picture differs at frame +%d\n", i + 1); shown++; } }
		if (s1 != s2h && getenv("MD_TWIN_STATEDIFF")) {
			size_t k = 0, shown = 0;
			while (k < sA.size() && shown < 12) {
				if (sA[k] != sbuf[k]) { size_t e = k; while (e < sA.size() && sA[e] != sbuf[e]) e++; printf("  state differs at payload offset %zu..%zu (%zu bytes)\n", k < 16 ? k : k - 16, e - (e > 16 ? 16 : 0), e - k); k = e; shown++; } else k++;
			}
		}
		// the state with a flipped byte must be refused and leave the machine alone
		std::vector<uint8_t> bad = st; bad[1000] ^= 0x55;
		int rb = md_host_state_load_mem(bad.data(), bad.size());
		printf("ROUNDTRIP %s load=%d video=%s state=%s audio=%s corrupt_refused=%s\n", bn, rc, a1.v == a2.v ? "SAME" : "DIFF", s1 == s2h ? "SAME" : "DIFF",
			a1.a == a2.a ? "SAME" : "DIFF", rb == 3 ? "yes" : "NO");
	}
	md_host_shutdown();
	return 0;
}
