// Proof and measurement of the incremental scaler (playerkit pk_scale ScaleCache) on the pictures of real ROM runs.
// For every frame of a ROM it makes the picture two ways: scale_rows over the whole picture (the definition) and ScaleCache
// (plan, chunks, blit into three rotating "textures" as the player does), and compares them byte for byte. Prints one line:
//   SCALE <rom> kind=<k> size=WxH frames=N mismatches=M  rows computed p50/p95/p99 (percent) shifted_frames=S  host_us full=.. inc=.. (p50/p95/p99)
// usage: scale_proof ROM [--frames N] [--kind auto|mmpx|xbr2|xbr3] [--system md|sms|gg|sg] [--mode auto|right] [--noshift] [--widen M] [--quiet]
// --widen M: the picture is the wide view's (the console's picture plus M columns each side, composed by md/frontend/wide.cpp: the soft extension on
//          the 8 bit consoles) so the scaler cost of the wide picture is measured
// Copyright CBOC-Vita. MIT licence for this file; the core keeps its own licence.
#include "host.h"
#include "pk_scale.h"
#include "wide.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>
#include <algorithm>

static double now()
{
	struct timespec t;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return (double) t.tv_sec + (double) t.tv_nsec * 1e-9;
}

static unsigned auto_mask(int f, int right)
{
	unsigned s = (unsigned) (f / 24) * 2654435761u;
	s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
	unsigned m = 0;
	if (right) {
		m = MD_RIGHT;
		if ((f / 30) % 3 == 1) m |= MD_B | MD_C;
		if (s & 0x10) m |= MD_A;
		if ((f % 150) >= 40 && (f % 150) < 46) m |= MD_START;
		return m;
	}
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
	if ((f % 150) >= 40 && (f % 150) < 46) m |= MD_START;
	return m;
}

static double avg(const std::vector<double> &v)
{
	double s = 0;
	for (size_t i = 0; i < v.size(); i++) s += v[i];
	return v.empty() ? 0 : s / (double) v.size();
}

static double pct(std::vector<double> v, double p)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	return v[(size_t) ((double) (v.size() - 1) * p)];
}

int main(int argc, char **argv)
{
	const char *rom = NULL;
	int frames = 400, system = 0, noshift = 0, quiet = 0, right = 0, refs = 0, widen = 0;
	unsigned long px_tot = 0, px_float_diff = 0, px_int_diff = 0, ch_over1 = 0;
	std::string kind = "auto";
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]);
		else if (a == "--kind" && i + 1 < argc) kind = argv[++i];
		else if (a == "--system" && i + 1 < argc) { std::string r = argv[++i]; system = r == "md" ? MD_SYS_MD : r == "sms" ? MD_SYS_SMS : r == "gg" ? MD_SYS_GG : r == "sg" ? MD_SYS_SG : 0; }
		else if (a == "--mode" && i + 1 < argc) right = std::string(argv[++i]) == "right";
		else if (a == "--noshift") noshift = 1;
		else if (a == "--widen" && i + 1 < argc) widen = atoi(argv[++i]);
		else if (a == "--refs") refs = 1;
		else if (a == "--quiet") quiet = 1;
		else if (a[0] != '-' && !rom) rom = argv[i];
		else { fprintf(stderr, "usage: scale_proof ROM [--frames N] [--kind auto|mmpx|xbr2|xbr3] [--system md|sms|gg|sg] [--mode auto|right] [--noshift]\n"); return 2; }
	}
	if (!rom) return 2;
	FILE *f = fopen(rom, "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> data((size_t) n);
	if (fread(data.data(), 1, data.size(), f) != data.size()) { fclose(f); return 1; }
	fclose(f);
	if (md_host_init()) return 1;
	md_host_set_seed(1);
	char err[128];
	const char *bn = strrchr(rom, '/'); bn = bn ? bn + 1 : rom;
	if (md_host_load_mem(data.data(), data.size(), bn, system, err, sizeof err)) { printf("LOADFAIL %s\n", err); return 1; }

	mdw::Wide wide;
	mdw::Meta meta;
	pk::ScaleCache cache;
	pk::ScaleScratch sc;
	std::vector<uint16_t> ref, tex[3];
	pk::TexSync ts[3];
	int ti = 0;
	unsigned long mism = 0, sframes = 0, restarts = 0;
	std::vector<double> comp, tfull, tinc;
	int W = 0, H = 0;
	pk::ScalerKind K = pk::SC_NONE;
	std::vector<uint16_t> lastsrc;
	for (int fr = 0; fr < frames; fr++) {
		md_host_set_pad(0, auto_mask(fr, right));
		md_host_run_frame();
		int w, h, pitch;
		const uint16_t *px = md_host_video(&w, &h, &pitch);
		if (w > 352 || h > 256) continue;
		if (widen > 0) {
			md_video_info vi;
			md_host_video_info(&vi);
			wide.analyse((uint32_t) fr + 1, vi, widen, 0, &meta);
			int ww = 0, wh = 0;
			const uint16_t *wp = wide.compose(meta, px, pitch / 2, &ww, &wh);
			if (!wp) continue;
			px = wp; w = ww; h = wh; pitch = ww * 2;
		}
		pk::ScalerKind k = kind == "mmpx" ? pk::SC_MMPX2 : kind == "xbr2" ? pk::SC_XBR2 : kind == "xbr3" ? pk::SC_XBR3 : (w <= 160 ? pk::SC_XBR3 : w <= 256 ? pk::SC_XBR2 : pk::SC_MMPX2);
		const int fct = pk::scaler_factor(k);
		const int ow = w * fct;
		W = w; H = h; K = k;
		ref.assign((size_t) ow * (size_t) (h * fct), 0);
		double t0 = now();
		pk::scale_rows(k, px, w, h, pitch / 2, &ref[0], ow, 0, h, sc);
		double t1 = now();
		const int nch = cache.plan(k, px, w, h, pitch / 2, !noshift);
		for (int c = 0; c < nch; c++) cache.run_chunk(c, sc);
		const pk::ScaleStats st = cache.stats();
		ti = (ti + 1) % 3;
		if ((int) tex[ti].size() < 1024 * 576) { tex[ti].assign(1024 * 576, 0); ts[ti] = pk::TexSync(); }
		cache.blit(&tex[ti][0], 1024, ts[ti]);
		double t2 = now();
		if (refs && (k == pk::SC_XBR2 || k == pk::SC_XBR3) && fr % 8 == 0) {
			std::vector<uint16_t> rf((size_t) ow * h * fct), ri(rf.size());
			pk::xbr_reference(px, w, h, pitch / 2, fct, &rf[0], ow);
			pk::xbr_reference_int(px, w, h, pitch / 2, fct, &ri[0], ow);
			for (size_t i = 0; i < rf.size(); i++) {
				px_tot++;
				px_int_diff += ref[i] != ri[i];
				if (ref[i] != rf[i]) {
					px_float_diff++;
					const int dr = abs(((ref[i] >> 11) & 31) - ((rf[i] >> 11) & 31)), dg = abs(((ref[i] >> 5) & 63) - ((rf[i] >> 5) & 63)), db = abs((ref[i] & 31) - (rf[i] & 31));
					if (dr > 1 || dg > 1 || db > 1) ch_over1++;
				}
			}
		}
		bool bad = false;
		if (memcmp(cache.out(), &ref[0], ref.size() * 2) != 0) bad = true;
		for (int y = 0; y < h * fct && !bad; y++)
			if (memcmp(&tex[ti][(size_t) y * 1024], &ref[(size_t) y * ow], (size_t) ow * 2) != 0) bad = true;
		if (bad) {
			if (mism < 3) {
				int fy = -1, fx = -1;
				for (int y = 0; y < h * fct && fy < 0; y++)
					for (int x = 0; x < ow; x++) if (cache.out()[(size_t) y * ow + x] != ref[(size_t) y * ow + x]) { fy = y; fx = x; break; }
				printf("  MISMATCH %s frame %d (shape %d k %d computed %d/%d moved %d kept %d) first at out row %d col %d (src row %d block %d)\n", bn, fr, st.shape, st.shift_k, st.computed, st.blocks,
				       st.shifted, st.same, fy, fx, fy / fct, fx / fct / 8);
			}
			if (mism < 1 && lastsrc.size() == (size_t) w * h) {
				int cnt = 0;
				for (int y = 0; y < h && cnt < 12; y++)
					for (int x = 0; x < w && cnt < 12; x++)
						if (px[(size_t) y * (pitch / 2) + x] != lastsrc[(size_t) y * w + x] && y >= 36 && y <= 52 && x >= 190 && x <= 230) { printf("    src changed at row %d col %d\n", y, x); cnt++; }
			}
			mism++;
		}
		lastsrc.assign((size_t) w * h, 0);
		for (int y = 0; y < h; y++) memcpy(&lastsrc[(size_t) y * w], px + (size_t) y * (pitch / 2), (size_t) w * 2);
		if (st.restarted) restarts++;
		else {
			comp.push_back(100.0 * st.computed / (double) st.blocks);
			tfull.push_back((t1 - t0) * 1e6);
			tinc.push_back((t2 - t1) * 1e6);
			if (st.shape) sframes++;
		}
	}
	if (!quiet)
		printf("SCALE %s kind=%s size=%dx%d frames=%d mismatches=%lu computed%%(p50/p95/p99)=%.0f/%.0f/%.0f shifted_frames=%lu full_us(p50/p95/p99)=%.0f/%.0f/%.0f inc_us=%.0f/%.0f/%.0f avg_full=%.0f avg_inc=%.0f\n",
		    bn, pk::scaler_name(K), W, H, frames, mism, pct(comp, .5), pct(comp, .95), pct(comp, .99), sframes,
		    pct(tfull, .5), pct(tfull, .95), pct(tfull, .99), pct(tinc, .5), pct(tinc, .95), pct(tinc, .99),
		    avg(tfull), avg(tinc));
	if (refs) printf("REFS %s px=%lu differ_from_int=%lu differ_from_float=%lu (%.4f%%) channel_off_by_more_than_one=%lu\n", bn, px_tot, px_int_diff, px_float_diff, px_tot ? 100.0 * (double) px_float_diff / (double) px_tot : 0.0, ch_over1);
	md_host_shutdown();
	return mism ? 1 : 0;
}
