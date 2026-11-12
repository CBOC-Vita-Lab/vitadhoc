/* Copyright CBOC-Vita. MIT licence.
 * Runs a ROM through nescore.o only (the in-process API of host.h, no file layer) and prints one line per frame
 * (picture hash), then the audio hash, the console RAM hash and the battery RAM hash. Built for x86_64 and for
 * armhf (run under qemu-arm) and compared byte for byte: the two must print the same text.
 *   obj_hash ROM FRAMES [no_sprite_limit 0/1]                                                                    */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "host.h"

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n--) { h ^= *b++; h *= 1099511628211ull; }
	return h;
}

int main(int argc, char **argv)
{
	if (argc < 3) return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 2;
	static uint8_t buf[4 << 20];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	int frames = atoi(argv[2]);
	char err[160] = "";
	if (nes_host_init()) return 1;
	if (argc > 3) nes_host_set_option("no_sprite_limit", atoi(argv[3]));
	if (nes_host_load_mem(buf, n, "rom.nes", err, sizeof err)) { printf("load failed: %s\n", err); return 1; }
	uint64_t ah = 1469598103934665603ull;
	static int16_t ab[8192 * 2];
	for (int i = 0; i < frames; i++) {
		unsigned m = 0;
		if (i >= 30 && i < 40) m |= NES_START;
		if (i >= 60 && i < 90) m |= NES_A;
		if (i >= 120 && i < 200) m |= NES_RIGHT | ((i & 8) ? NES_B : 0);
		if (i >= 200 && i < 240) m |= NES_LEFT | NES_UP;
		nes_host_set_pad(0, m);
		nes_host_set_pad(1, (i & 16) ? NES_A : 0);
		nes_host_run_frame();
		int w, h, p;
		const uint16_t *v = nes_host_video(&w, &h, &p);
		uint64_t fh = 1469598103934665603ull;
		for (int y = 0; y < h; y++) fh = fnv(fh, (const uint8_t *) v + (size_t) y * p, (size_t) w * 2);
		int got = nes_host_audio_take(ab, 8192);
		ah = fnv(ah, ab, (size_t) got * 4);
		printf("%d %016llx %d\n", i, (unsigned long long) fh, got);
	}
	printf("audio %016llx\n", (unsigned long long) ah);
	size_t rsz = 0;
	const uint8_t *rp = nes_host_ram(&rsz);
	printf("ram %016llx\n", (unsigned long long) fnv(1469598103934665603ull, rp, rsz));
	printf("sram %016llx n=%zu\n", (unsigned long long) fnv(1469598103934665603ull, nes_host_sram_data(), nes_host_sram_size()), (size_t) nes_host_sram_size());
	printf("state_size %zu\n", nes_host_state_size());
	nes_host_shutdown();
	return 0;
}
