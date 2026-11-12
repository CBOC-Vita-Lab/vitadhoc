/* Copyright CBOC-Vita. MIT licence. Links against nescore.o together with globals of the same names FCEUmm uses
 * inside, to show that nescore.o exports nothing but nes_host_*; then loads a ROM from memory, runs frames and checks
 * the picture is not empty. Usage: link_test ROM.nes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

/* names the core defines for itself; ours must not collide or be touched */
int RAM = 12345;
int X = 7;
int PPU = 8;
int GameInfo = 9;
int XBuf = 10;
int dendy = 11;
unsigned crc32(unsigned a, const void *b, unsigned c) { (void) b; (void) c; return a ^ 0x5a5a5a5a; }
void FCEU_printf(const char *fmt, ...) { (void) fmt; }
int FCEUI_Emulate = 13;

int main(int argc, char **argv)
{
	if (argc < 2) return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 2;
	static uint8_t buf[1 << 20];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	char err[160] = "";
	if (nes_host_init()) return 1;
	if (nes_host_load_mem(buf, n, "link_test.nes", err, sizeof err)) { printf("load failed: %s\n", err); return 1; }
	for (int i = 0; i < 60; i++) nes_host_run_frame();
	int w, h, p;
	const uint16_t *v = nes_host_video(&w, &h, &p);
	unsigned colours = 0, seen[16] = { 0 };
	for (int i = 0; i < w * h; i++) {
		unsigned c = v[i] & 15;
		if (!seen[c]) { seen[c] = 1; colours++; }
	}
	printf("link_test: user globals intact: %d %d %d %d %d %d %d; crc32 hook %s; picture has %u distinct low-nibble colours\n",
	    RAM, X, PPU, GameInfo, XBuf, dendy, FCEUI_Emulate, crc32(1, 0, 0) == (1u ^ 0x5a5a5a5a) ? "ours" : "REPLACED", colours);
	int ok = RAM == 12345 && X == 7 && PPU == 8 && GameInfo == 9 && XBuf == 10 && dendy == 11 && FCEUI_Emulate == 13 && colours > 2;
	nes_host_shutdown();
	return ok ? 0 : 1;
}
