// Proof of the pixel art scaler on the pictures of real SNES runs (playerkit pk_scale, as snes.bin uses it).
// For every frame it makes the picture two ways: scale_rows over the whole picture (the definition) and ScaleCache (plan, chunks, blit
// into three rotating "textures" as the player does), and compares them byte for byte; every `--ref N` frames xBR 2x is also compared
// with xbr_reference_int (the written definition of the integer xBR). With --wide M the picture is the wide view's (256 + 2 M columns).
// The picture is first copied (as the hand-over mailbox does), so a difference between the direct and the handed-over picture would show.
// usage: scale_proof ROM [--frames N] [--pad SCRIPT] [--wide M] [--kind xbr2|mmpx2|both] [--ref N] [--quiet]
// Prints one line: SCALE <name> kind=.. size=.. frames=N scaled=S skipped=K mismatches=M ref_checked=R ref_mismatches=X computed% p50/p95
// Copyright CBOC-Vita. MIT licence for this file; the core keeps its own licence.
#include "host.h"
#include "wide.h"
#include "pk_scale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

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

struct Run {
	pk::ScalerKind kind;
	pk::ScaleCache cache;
	pk::ScaleScratch sc;
	pk::TexSync sync[3];
	std::vector<uint16_t> tex[3];
	int ti;
	long scaled, mism, refchk, refmism;
	std::vector<double> comp;
	Run(pk::ScalerKind k) : kind(k), ti(0), scaled(0), mism(0), refchk(0), refmism(0) { for (int i = 0; i < 3; i++) tex[i].assign(1024 * 512, 0); }
};

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: scale_proof ROM [--frames N] [--pad SCRIPT] [--wide M] [--kind xbr2|mmpx2|both] [--ref N] [--quiet]\n"); return 2; }
	const char *rom = argv[1];
	int frames = 600, wide = 0, refevery = 50;
	bool quiet = false, xbr = true, mmpx = true;
	std::vector<PadEv> script = parse_script("40:start,50:none,90:a,100:none,140:right,170:none,200:start,210:none,230:b+down,250:none");
	for (int i = 2; i < argc; i++) {
		auto arg = [&](const char *n) { return !strcmp(argv[i], n) && i + 1 < argc; };
		if (arg("--frames")) frames = atoi(argv[++i]);
		else if (arg("--pad")) script = parse_script(argv[++i]);
		else if (arg("--wide")) wide = atoi(argv[++i]);
		else if (arg("--ref")) refevery = atoi(argv[++i]);
		else if (arg("--kind")) { const char *k = argv[++i]; xbr = strcmp(k, "mmpx2") != 0; mmpx = strcmp(k, "xbr2") != 0; }
		else if (!strcmp(argv[i], "--quiet")) quiet = true;
	}
	pk::scale_init();
	if (snes_host_init()) { fprintf(stderr, "init failed\n"); return 1; }
	char err[128] = "";
	if (snes_host_load_rom(rom, NULL, err, sizeof err)) { fprintf(stderr, "load failed: %s\n", err); return 1; }
	if (wide) snes_wide_set(1, wide);
	std::vector<Run *> runs;
	if (xbr) runs.push_back(new Run(pk::SC_XBR2));
	if (mmpx) runs.push_back(new Run(pk::SC_MMPX2));
	std::vector<uint16_t> full(1024 * 512), ref(1024 * 512), hand;
	long skipped = 0, nframes = 0;
	int lw = 0, lh = 0;
	size_t ev = 0;
	unsigned pad = 0;
	for (int f = 0; f < frames; f++) {
		while (ev < script.size() && script[ev].frame <= f) pad = script[ev++].mask;
		snes_host_set_pad(0, pad);
		snes_host_run_frame();
		int w, h, pitch;
		const uint16_t *v = snes_host_video(&w, &h, &pitch);
		if (wide) {
			const uint16_t *wp; int ww, wh, wpp; snes_wide_stats st;
			if (snes_wide_frame(&wp, &ww, &wh, &wpp, &st) && wp) { v = wp; w = ww; h = wh; pitch = wpp * 2; }
		}
		if (!v) continue;
		nframes++;
		if (w > 448 || h > 240 || w < 1) { skipped++; continue; }      /* pic_scaler_ceiling: hi-res and interlaced pictures get no scaler */
		/* the hand-over's copy of the picture */
		hand.resize((size_t) w * h);
		for (int y = 0; y < h; y++) memcpy(&hand[(size_t) y * w], (const uint8_t *) v + (size_t) y * pitch, (size_t) w * 2);
		lw = w; lh = h;
		for (Run *r : runs) {
			const int F = pk::scaler_factor(r->kind);
			pk::ScaleScratch ref_sc;
			pk::scale_rows(r->kind, hand.data(), w, h, w, full.data(), 1024, 0, h, ref_sc);
			const int nch = r->cache.plan(r->kind, hand.data(), w, h, w);
			for (int i = 0; i < nch; i++) r->cache.run_chunk(i, r->sc);
			r->ti = (r->ti + 1) % 3;
			r->cache.blit(r->tex[r->ti].data(), 1024, r->sync[r->ti]);
			bool bad = false;
			for (int y = 0; y < h * F && !bad; y++)
				if (memcmp(&full[(size_t) y * 1024], &r->tex[r->ti][(size_t) y * 1024], (size_t) w * F * 2)) bad = true;
			if (bad) { r->mism++; if (!quiet) fprintf(stderr, "frame %d %s: scaled picture differs\n", f, pk::scaler_name(r->kind)); }
			r->scaled++;
			const pk::ScaleStats &s = r->cache.stats();
			r->comp.push_back(s.blocks ? 100.0 * s.computed / s.blocks : 0.0);
			if (r->kind == pk::SC_XBR2 && refevery > 0 && f % refevery == 0) {
				pk::xbr_reference_int(hand.data(), w, h, w, 2, ref.data(), 1024);
				bool rb = false;
				for (int y = 0; y < h * 2 && !rb; y++)
					if (memcmp(&full[(size_t) y * 1024], &ref[(size_t) y * 1024], (size_t) w * 4)) rb = true;
				r->refchk++;
				if (rb) { r->refmism++; if (!quiet) fprintf(stderr, "frame %d: scale_rows differs from xbr_reference_int\n", f); }
			}
		}
	}
	int bad = 0;
	const char *name = strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom;
	for (Run *r : runs) {
		std::vector<double> c = r->comp;
		std::sort(c.begin(), c.end());
		printf("SCALE %s kind=%s size=%dx%d frames=%ld scaled=%ld skipped=%ld mismatches=%ld ref_checked=%ld ref_mismatches=%ld computed%%p50/p95=%.0f/%.0f\n",
		    name, pk::scaler_name(r->kind), lw, lh, nframes, r->scaled, skipped, r->mism, r->refchk, r->refmism,
		    c.empty() ? 0.0 : c[c.size() / 2], c.empty() ? 0.0 : c[c.size() * 95 / 100]);
		bad += (int) (r->mism + r->refmism);
	}
	if (wide) snes_wide_set(0, 0);
	snes_host_shutdown();
	return bad ? 1 : 0;
}
