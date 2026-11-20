// Dumps the pictures behind the 2x-versus-3x ceiling decision: runs a ROM for N frames and writes the source picture and the xBR 2x and
// xBR 3x results as raw RGB565 files plus a text file with the layout facts (md/twin/sidebyside.py makes the 960x544 shots and the numbers).
//   sidebyside ROM --system md|sms|gg|sg --frame N --out PREFIX
// Copyright CBOC-Vita. MIT licence for this file; the core keeps its own licence.
#include "host.h"
#include "pk_scale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static unsigned auto_mask(int f)
{
	unsigned s = (unsigned) (f / 24) * 2654435761u;
	s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
	unsigned m = 0;
	if (s & 0x1) m |= MD_RIGHT;
	if (s & 0x2) m |= MD_LEFT;
	if (s & 0x4) m |= MD_UP;
	if (s & 0x8) m |= MD_DOWN;
	if (s & 0x10) m |= MD_A;
	if (s & 0x20) m |= MD_B;
	if (s & 0x40) m |= MD_C;
	if (m & MD_LEFT && m & MD_RIGHT) m &= ~MD_LEFT;
	if (m & MD_UP && m & MD_DOWN) m &= ~MD_UP;
	if ((f % 150) >= 40 && (f % 150) < 46) m |= MD_START;
	return m;
}

static void dump(const std::string &p, const uint16_t *px, int w, int h, int pitch_px)
{
	FILE *f = fopen(p.c_str(), "wb");
	if (!f) return;
	for (int y = 0; y < h; y++) fwrite(px + (size_t) y * pitch_px, 2, (size_t) w, f);
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *rom = NULL, *out = "sbs";
	int frame = 300, system = 0;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frame" && i + 1 < argc) frame = atoi(argv[++i]);
		else if (a == "--out" && i + 1 < argc) out = argv[++i];
		else if (a == "--system" && i + 1 < argc) { std::string r = argv[++i]; system = r == "md" ? MD_SYS_MD : r == "sms" ? MD_SYS_SMS : r == "gg" ? MD_SYS_GG : r == "sg" ? MD_SYS_SG : 0; }
		else if (a[0] != '-' && !rom) rom = argv[i];
		else return 2;
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
	for (int fr = 0; fr <= frame; fr++) { md_host_set_pad(0, auto_mask(fr)); md_host_run_frame(); }
	int w, h, pitch;
	const uint16_t *px = md_host_video(&w, &h, &pitch);
	md_video_info vi;
	md_host_video_info(&vi);
	const std::string o = out;
	dump(o + ".src", px, w, h, pitch / 2);
	pk::ScaleScratch sc;
	for (int k = 2; k <= 3; k++) {
		std::vector<uint16_t> dst((size_t) w * k * h * k);
		pk::scale_rows(k == 2 ? pk::SC_XBR2 : pk::SC_XBR3, px, w, h, pitch / 2, &dst[0], w * k, 0, h, sc);
		dump(o + (k == 2 ? ".x2" : ".x3"), &dst[0], w * k, h * k, w * k);
	}
	FILE *m = fopen((o + ".meta").c_str(), "w");
	fprintf(m, "name=%s\nw=%d\nh=%d\nnative_w=%d\nnative_h=%d\nsystem=%d\ndar=%.6f\npar=%.6f\nframe=%d\n", bn, w, h, vi.native_w, vi.native_h, vi.system, vi.dar, vi.par, frame);
	fclose(m);
	md_host_shutdown();
	return 0;
}
