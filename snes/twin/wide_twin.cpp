/*
 * snes_wide_twin: headless runner for the wide view. Runs a ROM with the scripted pad, optionally with the wide view on,
 * and writes what the proofs need:
 *   --hashes F       per frame: frame width height fnv1a64 of the NORMAL picture (snes_host_video), as snes_twin does
 *   --whashes F      per frame: frame, wide width, fnv1a64 of the 256 centre columns of the wide picture, fnv1a64 of the whole wide
 *                    picture, claimed real px (left right means), available
 *   --state-hash N F every N frames an fnv1a64 of the whole saved state (CPU, PPU, VRAM, CGRAM, OAM, SPC, DSP, RAM, cart RAM)
 *   --wav F          all audio
 *   --dump DIR --dump-frames a,b,c   raw wide frames with their decisions and the first 8 KB of work RAM (truth tests)
 *   --png-dir DIR --shots a,b,c      PNG of the wide frame (and the normal picture's centre check)
 *   --wide M         switch the wide view on with M px per side (224 lines); --wopt name=value (adapter option, repeatable)
 *   --map FILE       load the learned map from FILE at the start and save it at the end (a first pass makes the file)
 *   --summary        mean claimed real px and the adapter's mean wall time per frame
 * Everything else as snes_twin (--frames, --pad, --opt name=value, --srm).
 * Copyright CBOC-Vita. MIT licence.
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
#include "wide.h"

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

static std::set<int> parse_set(const char *s)
{
	std::set<int> r;
	std::string t(s);
	size_t p = 0;
	while (p < t.size()) {
		size_t q = t.find(',', p);
		if (q == std::string::npos) q = t.size();
		std::string e = t.substr(p, q - p);
		size_t dash = e.find('-');
		if (dash != std::string::npos) { for (int i = atoi(e.c_str()); i <= atoi(e.c_str() + dash + 1); i++) r.insert(i); }
		else r.insert(atoi(e.c_str()));
		p = q + 1;
	}
	return r;
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

static void write_png(const char *path, const uint16_t *px, int w, int h, int pitch_px, const uint8_t *prov = 0)
{
	std::vector<uint8_t> rgb((size_t) w * h * 3);
	for (int y = 0; y < h; y++) {
		const uint16_t *row = px + (size_t) y * pitch_px;
		for (int x = 0; x < w; x++) {
			uint16_t c = row[x];
			uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
			uint8_t *o = &rgb[((size_t) y * w + x) * 3];
			o[0] = (r << 3) | (r >> 2); o[1] = (g << 2) | (g >> 4); o[2] = (b << 3) | (b >> 2);
			if (prov) {
				/* provenance tint: soft = red, exact/real = none, fade = yellow */
				int p = prov[(size_t) y * w + x] & 15;
				if (p == 2) { o[0] = (o[0] + 255) / 2; o[1] = o[1] / 2; o[2] = o[2] / 2; }
			}
		}
	}
	stbi_write_png(path, w, h, 3, rgb.data(), w * 3);
}

static uint64_t fnv(const uint16_t *px, int w, int h, int pitch_bytes)
{
	uint64_t hsh = 1469598103934665603ull;
	for (int y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *) px + (size_t) y * pitch_bytes;
		for (int i = 0; i < w * 2; i++) { hsh ^= row[i]; hsh *= 1099511628211ull; }
	}
	return hsh;
}

static uint64_t fnv_bytes(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ull)
{
	for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
	return h;
}

static std::vector<uint8_t> read_all(const char *path)
{
	std::vector<uint8_t> v;
	FILE *f = fopen(path, "rb");
	if (!f) return v;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	v.resize(n > 0 ? n : 0);
	if (n > 0 && fread(v.data(), 1, n, f) != (size_t) n) v.clear();
	fclose(f);
	return v;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: snes_wide_twin ROM [--frames N] [--pad S] [--opt k=v] [--wide M] ...\n"); return 2; }
	const char *rom = argv[1], *hashes = 0, *whashes = 0, *wav = 0, *dumpdir = 0, *pngdir = 0, *statef = 0, *mapf = 0, *prefix = "f";
	int frames = 300, wide = 0, state_every = 0;
	bool summary = false;
	std::vector<PadEv> script;
	std::set<int> dumpset, shotset, statset;
	int trace_every = 0;
	std::vector<std::pair<uint32_t, uint8_t>> pokes;
	struct KV { std::string k; int v; };
	std::vector<KV> opts, wopts;
	for (int i = 2; i < argc; i++) {
		auto arg = [&](const char *n) { return !strcmp(argv[i], n) && i + 1 < argc; };
		if (arg("--frames")) frames = atoi(argv[++i]);
		else if (arg("--pad")) script = parse_script(argv[++i]);
		else if (arg("--hashes")) hashes = argv[++i];
		else if (arg("--whashes")) whashes = argv[++i];
		else if (arg("--wav")) wav = argv[++i];
		else if (arg("--wide")) wide = atoi(argv[++i]);
		else if (arg("--dump")) dumpdir = argv[++i];
		else if (arg("--dump-frames")) dumpset = parse_set(argv[++i]);
		else if (arg("--stat-frames")) statset = parse_set(argv[++i]);
		else if (arg("--trace")) trace_every = atoi(argv[++i]);
		else if (arg("--poke")) { unsigned a, b; if (sscanf(argv[++i], "%x=%x", &a, &b) == 2) pokes.push_back({ a, (uint8_t) b }); }
		else if (arg("--png-dir")) pngdir = argv[++i];
		else if (arg("--shots")) shotset = parse_set(argv[++i]);
		else if (arg("--shot-prefix")) prefix = argv[++i];
		else if (arg("--map")) mapf = argv[++i];
		else if (arg("--state-hash") && i + 2 < argc) { state_every = atoi(argv[++i]); statef = argv[++i]; }
		else if (!strcmp(argv[i], "--summary")) summary = true;
		else if (arg("--opt") || arg("--wopt")) {
			bool w = !strcmp(argv[i], "--wopt");
			std::string t(argv[++i]);
			size_t e = t.find('=');
			if (e != std::string::npos) (w ? wopts : opts).push_back({ t.substr(0, e), atoi(t.c_str() + e + 1) });
		}
	}
	if (snes_host_init()) { fprintf(stderr, "init failed\n"); return 1; }
	for (auto &o : opts) if (snes_host_set_option(o.k.c_str(), o.v)) { fprintf(stderr, "unknown option %s\n", o.k.c_str()); return 2; }
	char err[128] = "";
	if (snes_host_load_rom(rom, NULL, err, sizeof err)) { fprintf(stderr, "load failed: %s\n", err); return 1; }

	if (wide) {
		size_t rn = 0;
		const uint8_t *rimg = snes_host_rom_image(&rn);
		snes_wide_set_game(rimg, rn, snes_host_rom_title());
		for (auto &o : wopts) if (snes_wide_option(o.k.c_str(), o.v)) { fprintf(stderr, "unknown wide option %s\n", o.k.c_str()); return 2; }
		if (mapf) {
			std::vector<uint8_t> m = read_all(mapf);
			if (!m.empty()) fprintf(stderr, "map load: %d\n", snes_wide_map_load(m.data(), m.size()));
		}
		snes_host_snapshot_enable(1);
		snes_wide_set(1, wide);
	}
	FILE *hf = hashes ? fopen(hashes, "w") : 0, *wf = whashes ? fopen(whashes, "w") : 0, *sf = statef ? fopen(statef, "w") : 0;
	std::vector<int16_t> pcm, abuf(4096 * 2);
	std::vector<uint8_t> sbuf;
	size_t ev = 0;
	unsigned pad = 0;
	double sum_l = 0, sum_r = 0, sum_us = 0;
	long n_avail = 0, n_wide = 0;
	if (sf) sbuf.resize(snes_host_state_size() + 4096);
	for (int f = 0; f < frames; f++) {
		while (ev < script.size() && script[ev].frame <= f) pad = script[ev++].mask;
		for (auto &pk : pokes) snes_host_poke_wram(pk.first, pk.second);
		snes_host_set_pad(0, pad);
		snes_host_run_frame();
		int n = snes_host_audio_take(abuf.data(), 4096);
		if (wav) pcm.insert(pcm.end(), abuf.begin(), abuf.begin() + n * 2);
		int w, h, pitch;
		const uint16_t *v = snes_host_video(&w, &h, &pitch);
		if (trace_every && f % trace_every == 0) {
			int sh[4], sv[4];
			int md = snes_host_ppu_scroll(sh, sv);
			uint64_t luma = 0;
			for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) { unsigned p = ((const uint16_t *) ((const uint8_t *) v + (size_t) y * pitch))[x]; luma += ((p >> 11) & 31) * 2 + ((p >> 5) & 63) + (p & 31) * 1; }
			fprintf(stderr, "trace f=%d luma=%llu mode=%d h=%d,%d,%d,%d v=%d,%d,%d,%d centre=%016llx\n", f, (unsigned long long) (luma / ((w / 4) * (h / 4) + 1)), md, sh[0], sh[1], sh[2], sh[3], sv[0], sv[1], sv[2], sv[3],
			        (unsigned long long) fnv(v, w, h, pitch));
		}
		uint64_t ch = fnv(v, w, h, pitch);
		if (hf) fprintf(hf, "%d %d %d %016llx\n", f, w, h, (unsigned long long) ch);
		if (sf && f % state_every == 0) {
			size_t len = 0;
			int r = snes_host_state_save_buf(sbuf.data(), sbuf.size(), &len);
			fprintf(sf, "%d %d %zu %016llx\n", f, r, len, (unsigned long long) fnv_bytes(sbuf.data(), len));
		}
		if (wide) {
			const uint16_t *wp; int ww, wh, wpitch;
			snes_wide_stats st;
			int ok = snes_wide_frame(&wp, &ww, &wh, &wpitch, &st);
			n_wide++;
			if (ok) {
				n_avail++;
				sum_l += st.real_l; sum_r += st.real_r; sum_us += st.instr_us;
				int M = (ww - 256) / 2;
				/* centre of the wide picture, row by row, as the normal picture */
				uint64_t cwh = 1469598103934665603ull;
				for (int y = 0; y < wh; y++)
					cwh = fnv_bytes((const uint8_t *) (wp + (size_t) y * wpitch + M), 512, cwh);
				/* the reference is the picture as it stood at the frame end (some cartridges' main loop runs on a few lines of the
				 * next frame before snes_host_run_frame returns, so the picture read after it can already hold those) */
				int sw = 0, sh_ = 0;
				const uint16_t *snap = snes_host_video_snapshot(&sw, &sh_);
				const uint8_t *ref = snap ? (const uint8_t *) snap : (const uint8_t *) v;
				const int rpitch = snap ? sw * 2 : pitch, rh = snap ? sh_ : h;
				uint64_t nrm = 1469598103934665603ull;
				for (int y = 0; y < rh && y < wh; y++)
					nrm = fnv_bytes(ref + (size_t) y * rpitch, 512, nrm);
				if (wf) fprintf(wf, "%d %d %016llx %016llx %d %d %d %d\n", f, ww, (unsigned long long) cwh, (unsigned long long) fnv(wp, ww, wh, wpitch * 2),
				                st.real_l, st.real_r, st.available, cwh == nrm);
				if (cwh != nrm) {
					int first = -1, cnt = 0;
					for (int y = 0; y < rh && y < wh; y++)
						if (memcmp(wp + (size_t) y * wpitch + M, ref + (size_t) y * rpitch, 512)) { if (first < 0) first = y; cnt++; if (cnt <= 20) fprintf(stderr, " row %d", y); }
					fprintf(stderr, "frame %d: wide centre differs from the normal picture (rows differing %d, first %d; picture %dx%d wide %dx%d)\n", f, cnt, first, w, h, ww, wh);
				}
				if (statset.count(f))
					fprintf(stderr, "stat f=%d mode7=%d lines=%d drawn=%d nomargin=%d hud=%d exact=%d real=%d/%d layers=%x ring=%dx%d,%dx%d,%dx%d,%dx%d kind=%d,%d,%d,%d scene=%d epoch=%d hs=%d/%d map cells=%d rooms=%d bound=%d served=%d\n",
					        f, st.mode7, st.lines, st.lines_drawn, st.lines_nomargin, st.lines_hud, st.lines_exact, st.real_l, st.real_r, st.layers,
					        st.ring_w[0], st.ring_h[0], st.ring_w[1], st.ring_h[1], st.ring_w[2], st.ring_h[2], st.ring_w[3], st.ring_h[3],
					        st.kind[0], st.kind[1], st.kind[2], st.kind[3], st.scene_event, st.epoch, st.hs_checked, st.hs_missed, st.map_cells, st.map_rooms, st.map_bound, st.map_served);
				if (dumpdir && dumpset.count(f)) {
					char p[600];
					snprintf(p, sizeof p, "%s/%s%05d.wide", dumpdir, prefix, f);
					FILE *df = fopen(p, "wb");
					if (df) {
						int32_t hdr[8] = { ww, wh, M, (int32_t) st.serial, st.mode7, st.layers, st.lines_hud, (int32_t) st.frame };
						fwrite(hdr, 4, 8, df);
						fwrite(snes_wide_real(), 1, (size_t) wh * 2, df);
						for (int y = 0; y < wh; y++) fwrite(wp + (size_t) y * wpitch, 2, ww, df);
						static uint8_t wr[0x2000];
						for (int i = 0; i < 0x2000; i++) wr[i] = (uint8_t) snes_host_peek_wram(i);
						fwrite(wr, 1, sizeof wr, df);
						const uint8_t *pv = snes_wide_prov();
						uint32_t has = pv ? 1 : 0;
						fwrite(&has, 4, 1, df);
						if (pv) fwrite(pv, 1, (size_t) ww * wh, df);
						fclose(df);
					}
				}
				if (pngdir && shotset.count(f)) {
					char p[600];
					snprintf(p, sizeof p, "%s/%s%05d.png", pngdir, prefix, f);
					write_png(p, wp, ww, wh, wpitch, snes_wide_prov());
				}
			} else {
				if (wf) fprintf(wf, "%d 0 0 0 0 0 0 1\n", f);
				if (pngdir && shotset.count(f)) {
					char p[600];
					snprintf(p, sizeof p, "%s/%s%05d.png", pngdir, prefix, f);
					write_png(p, v, w, h, pitch / 2);
				}
			}
		} else if (pngdir && shotset.count(f)) {
			char p[600];
			snprintf(p, sizeof p, "%s/%s%05d.png", pngdir, prefix, f);
			write_png(p, v, w, h, pitch / 2);
		}
	}
	snes_host_sync();
	int n;
	while ((n = snes_host_audio_take(abuf.data(), 4096)) > 0)
		if (wav) pcm.insert(pcm.end(), abuf.begin(), abuf.begin() + n * 2);
	if (wide && mapf) {
		size_t need = snes_wide_map_save(NULL, 0);
		if (need) {
			std::vector<uint8_t> m(need);
			size_t got = snes_wide_map_save(m.data(), m.size());
			FILE *mf = fopen(mapf, "wb");
			if (mf && got) { fwrite(m.data(), 1, got, mf); fclose(mf); }
			fprintf(stderr, "map saved: %zu bytes\n", got);
		}
	}
	if (summary && wide)
		printf("wide: frames=%ld available=%ld mean real px L=%.1f R=%.1f, adapter %.0f us/frame\n", n_wide, n_avail,
		       n_avail ? sum_l / n_avail : 0, n_avail ? sum_r / n_avail : 0, n_avail ? sum_us / n_avail : 0);
		snes_wide_census c;
		snes_wide_census_get(&c);
		const double tot = (double) (c.px_tilemap + c.px_learned + c.px_backdrop + c.px_unknown);
		printf("censuspx: %llu %llu %llu %llu soft %llu reveal %llu\n", (unsigned long long) c.px_tilemap, (unsigned long long) c.px_learned, (unsigned long long) c.px_backdrop, (unsigned long long) c.px_unknown, (unsigned long long) c.px_soft, (unsigned long long) c.px_reveal);
		printf("census: frames=%u wide=%u fallback=%u (nothing_drawn=%u profile=%u setup=%u) ambient=%u crossfade=%u switches=%u scenes=%u cells=%u cov=%d%% auto=%d\n"
		       "census: margin px tilemap %.1f%% learned %.1f%% backdrop %.1f%% unknown %.1f%%\n",
		       c.frames, c.frames_wide, c.frames_fallback, c.fb_nothing_drawn, c.fb_profile_off, c.fb_setup, c.frames_ambient, c.frames_crossfade,
		       c.auto_switches, c.scenes, c.map_cells, c.coverage_pct, c.auto_wide,
		       tot ? 100.0 * c.px_tilemap / tot : 0, tot ? 100.0 * c.px_learned / tot : 0, tot ? 100.0 * c.px_backdrop / tot : 0, tot ? 100.0 * c.px_unknown / tot : 0);
	if (hf) fclose(hf);
	if (wf) fclose(wf);
	if (sf) fclose(sf);
	if (wav) write_wav(wav, pcm);
	if (wide) snes_wide_set(0, 0);
	snes_host_shutdown();
	return 0;
}
