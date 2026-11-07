// Copyright CBOC-Vita. MIT licence. Tests of frontend/picture.cpp (layout, settings file, governor).
#include "picture.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static bool near_(float a, float b, float e = 0.6f) { return fabsf(a - b) <= e; }
int main()
{
	pic_settings s; pic_defaults(&s);
	pic_rect r;
	CHECK(s.shape == PIC_ORIGINAL && s.quality == PIC_SHARP && s.glow > 0 && s.scan == 0);
	pic_geometry(&s, 256, 224, 0, 0, 960, 544, &r);                       // default: 4:3, full height
	CHECK(near_(r.dh, 544) && near_(r.dw, 725.33f) && near_(r.dx, 117.3f) && r.sw == 256 && r.sh == 224);
	pic_geometry(&s, 512, 448, 0, 0, 960, 544, &r);                       // hi-res: same shape
	CHECK(near_(r.dw, 725.33f) && r.logical_rows == 224 && r.sw == 512);
	pic_geometry(&s, 256, 239, 0, 0, 960, 544, &r);                       // PAL: still 4:3
	CHECK(near_(r.dw, 725.33f) && near_(r.dh, 544));
	s.shape = PIC_ZOOM;
	pic_geometry(&s, 256, 239, 8, 7, 960, 544, &r);                       // overscan rows cropped, not stretched
	CHECK(r.sy == 8 && r.sh == 224 && near_(r.dh, 544) && r.dw > 725 && near_(r.dw / r.dh * r.sh / 256 * 256, r.dw / r.dh * r.sh));
	{   // unstretched: the displayed pixel aspect equals the full picture's
		pic_rect f; pic_settings o; pic_defaults(&o);
		pic_geometry(&o, 256, 239, 0, 0, 960, 544, &f);
		float pa_full = (f.dw / 256) / (f.dh / 239), pa_zoom = (r.dw / 256) / (r.dh / r.sh);
		CHECK(near_(pa_full, pa_zoom, 0.01f));
	}
	s.shape = PIC_FULL;
	pic_geometry(&s, 256, 224, 0, 0, 960, 544, &r);
	CHECK(r.dx == 0 && r.dw == 960 && r.dh == 544);
	s.shape = PIC_PIXEL; s.pixel_sq = 0;
	pic_geometry(&s, 256, 224, 0, 0, 960, 544, &r);                       // 2x vertical, 8:7 wide
	CHECK(near_(r.dh, 448) && near_(r.dw, 585.14f));
	s.pixel_sq = 1;
	pic_geometry(&s, 256, 224, 0, 0, 960, 544, &r);
	CHECK(near_(r.dh, 448) && near_(r.dw, 512));
	pic_geometry(&s, 512, 448, 0, 0, 960, 544, &r);
	CHECK(near_(r.dh, 448) && near_(r.dw, 512));
	int kx, ky;
	s.shape = PIC_ORIGINAL;
	pic_geometry(&s, 256, 224, 0, 0, 960, 544, &r);
	pic_prescale(&r, &kx, &ky);
	CHECK(kx == 3 && ky == 3);
	pic_geometry(&s, 512, 448, 0, 0, 960, 544, &r);
	pic_prescale(&r, &kx, &ky);
	CHECK(kx == 2 && ky == 2);

	// settings file round trip
	pic_settings a; pic_defaults(&a);
	a.shape = PIC_ZOOM; a.quality = PIC_SMOOTH; a.glow = 3; a.scan = 7; a.grid = 2; a.pixel_sq = 1; a.hires_blend = 1; a.interlace_blend = 1; a.multitap = 2; a.use_game = 1;
	CHECK(pic_save("/tmp/pic_test.ini", &a) == 0);
	pic_settings b; pic_defaults(&b);
	CHECK(pic_load("/tmp/pic_test.ini", &b) == 1 && !memcmp(&a, &b, sizeof a));
	remove("/tmp/pic_test.ini");
	CHECK(pic_load("/tmp/does-not-exist.ini", &b) == 0);

	// blank rows
	static unsigned short pic[256 * 4];
	memset(pic, 0, sizeof pic); pic[256 * 2 + 5] = 1;
	CHECK(pic_blank_rows(pic, 512, 256, 4, 1, 4) == 2);
	CHECK(pic_blank_rows(pic, 512, 256, 4, 0, 4) == 1);

	// governor: steady fast stays 0; sustained slow degrades to 2; recovery after a long fast run
	pic_governor g; pic_gov_init(&g);
	for (int i = 0; i < 1000; i++) pic_gov_update(&g, 9000, 16000);
	CHECK(g.level == 0);
	for (int i = 0; i < 10; i++) pic_gov_update(&g, 20000, 16000);        // a short hiccup: no change
	for (int i = 0; i < 100; i++) pic_gov_update(&g, 9000, 16000);
	CHECK(g.level == 0);
	for (int i = 0; i < 200; i++) { pic_gov_update(&g, 20000, 16000); CHECK(g.level != 1); } // extras never dropped
	CHECK(g.level == 2);
	for (int i = 0; i < 300; i++) pic_gov_update(&g, 9000, 16000);        // too soon to step up
	CHECK(g.level == 2);
	for (int i = 0; i < 2000; i++) { pic_gov_update(&g, 9000, 16000); CHECK(g.level != 1); }
	CHECK(g.level == 0);
	printf(fails ? "picture_test FAILED\n" : "picture_test passed\n");
	return fails ? 1 : 0;
}
