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
	CHECK(s.shape == PIC_ORIGINAL && s.quality == PIC_SHARP && s.glow > 0 && s.scan == 0 && s.grid == 0);
	CHECK(s.overscan == PIC_OVERSCAN_AUTO && s.no_sprite_limit == 1);
	CHECK(pic_overscan_rows(&s, 0) == 8 && pic_overscan_rows(&s, 1) == 0);   // automatic: NTSC only
	s.overscan = PIC_OVERSCAN_ON;  CHECK(pic_overscan_rows(&s, 1) == 8);
	s.overscan = PIC_OVERSCAN_OFF; CHECK(pic_overscan_rows(&s, 0) == 0);
	pic_defaults(&s);
	pic_geometry(&s, 256, 240, 8, 960, 544, &r);                          // default NTSC: 224 rows, 8:7 pixels, full height
	CHECK(r.sy == 8 && r.sh == 224 && r.sw == 256 && near_(r.dh, 544) && near_(r.dw, 544 * 256 * (8.0f / 7) / 224) && near_(r.dx, (960 - r.dw) / 2));
	CHECK(near_(r.dw / r.dh, 1.3061f, 0.001f));
	pic_geometry(&s, 256, 240, 0, 960, 544, &r);                          // PAL, 240 rows
	CHECK(r.sy == 0 && r.sh == 240 && near_(r.dh, 544) && near_(r.dw / r.dh, 1.2190f, 0.001f));
	pic_geometry(&s, 256, 240, 0, 400, 544, &r);                          // a narrow screen: limited by width, centred
	CHECK(near_(r.dw, 400) && r.dh < 544 && near_(r.dy, (544 - r.dh) / 2));
	s.shape = PIC_ZOOM;
	pic_geometry(&s, 256, 240, 8, 960, 544, &r);                          // 8 columns off each side, same pixel shape
	CHECK(r.sx == 8 && r.sw == 240 && r.sh == 224 && near_(r.dh, 544) && near_(r.dw, 544 * 240 * (8.0f / 7) / 224));
	s.shape = PIC_FULL;
	pic_geometry(&s, 256, 240, 8, 960, 544, &r);
	CHECK(r.dx == 0 && r.dw == 960 && r.dh == 544 && r.sh == 224);
	s.shape = PIC_PIXEL; s.pixel_sq = 0;
	pic_geometry(&s, 256, 240, 8, 960, 544, &r);                          // 2x vertical (448), 8:7 wide
	CHECK(near_(r.dh, 448) && near_(r.dw, 256 * 2 * 8.0f / 7));
	s.pixel_sq = 1;
	pic_geometry(&s, 256, 240, 8, 960, 544, &r);
	CHECK(near_(r.dh, 448) && near_(r.dw, 512));
	pic_geometry(&s, 256, 240, 0, 960, 544, &r);                          // 240 rows: 2x = 480
	CHECK(near_(r.dh, 480));
	int kx, ky;
	s.shape = PIC_ORIGINAL;
	pic_geometry(&s, 256, 240, 8, 960, 544, &r);
	pic_prescale(&r, &kx, &ky);
	CHECK(kx == 3 && ky == 3);

	// settings file round trip
	pic_settings a; pic_defaults(&a);
	a.shape = PIC_ZOOM; a.quality = PIC_SMOOTH; a.glow = 3; a.scan = 7; a.grid = 2; a.pixel_sq = 1; a.overscan = 2; a.no_sprite_limit = 0; a.fourscore = 1; a.region = 3; a.use_game = 1;
	CHECK(pic_save("/tmp/pic_test_nes.ini", &a) == 0);
	pic_settings b; pic_defaults(&b);
	CHECK(pic_load("/tmp/pic_test_nes.ini", &b) == 1 && !memcmp(&a, &b, sizeof a));
	remove("/tmp/pic_test_nes.ini");
	CHECK(pic_load("/tmp/does-not-exist.ini", &b) == 0);

	// governor: steady fast stays 0; sustained slow degrades to 2; recovery after a long fast run
	pic_governor g; pic_gov_init(&g);
	for (int i = 0; i < 1000; i++) pic_gov_update(&g, 9000, 16000);
	CHECK(g.level == 0);
	for (int i = 0; i < 10; i++) pic_gov_update(&g, 20000, 16000);
	for (int i = 0; i < 100; i++) pic_gov_update(&g, 9000, 16000);
	CHECK(g.level == 0);
	for (int i = 0; i < 200; i++) { pic_gov_update(&g, 20000, 16000); CHECK(g.level != 1); }
	CHECK(g.level == 2);
	for (int i = 0; i < 300; i++) pic_gov_update(&g, 9000, 16000);
	CHECK(g.level == 2);
	for (int i = 0; i < 2000; i++) { pic_gov_update(&g, 9000, 16000); CHECK(g.level != 1); }
	CHECK(g.level == 0);
	printf(fails ? "picture_test FAILED\n" : "picture_test passed\n");
	return fails ? 1 : 0;
}
