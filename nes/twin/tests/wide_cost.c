/* Cost probe for the wide view: links nescore.o (built with SCOUT=1) only.
 *   wide_cost ROM STATE MAP|- MODE FRAMES
 * loads ROM, loads the saved state STATE (made by the twin: --save-state), optionally the learned map MAP, holds RIGHT and
 * runs FRAMES frames. MODE 0 = wide view off, 1 = on (84 px), 2 = on + scout (nes_host_scout_step(2, 0) per frame),
 * 3 = split API, capture only (what stays on the emulation thread; the compose side = mode 1 minus mode 3),
 * 4 = split API, capture + compose + scout_step(2) (the scout side = mode 4 minus mode 1 + the queue).
 * Instruction counts come from running it twice with different FRAMES (callgrind on x86, qemu-arm -one-insn-per-tb on ARM).
 * Copyright CBOC-Vita. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

static unsigned char *slurp(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb");
	unsigned char *b;
	long sz;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
	b = (unsigned char *) malloc((size_t) sz + 1);
	if (b && fread(b, 1, (size_t) sz, f) != (size_t) sz) { free(b); b = NULL; }
	fclose(f);
	*n = (size_t) sz;
	return b;
}

int main(int argc, char **argv)
{
	size_t rn = 0, sn = 0, mn = 0;
	unsigned char *rom, *st, *map = NULL;
	int mode, frames, f, scouted = 0;
	char err[128];
	static nes_wide_snapshot snap[2];
	int16_t audio[4096];
	unsigned long sum = 0;
	if (argc < 6) { fprintf(stderr, "usage: wide_cost ROM STATE MAP|- MODE FRAMES\n"); return 2; }
	mode = atoi(argv[4]); frames = atoi(argv[5]);
	rom = slurp(argv[1], &rn); st = slurp(argv[2], &sn);
	if (strcmp(argv[3], "-")) map = slurp(argv[3], &mn);
	if (!rom || !st) { fprintf(stderr, "cannot read\n"); return 1; }
	if (nes_host_init()) return 1;
	if (mode == 2 || mode == 4) nes_host_scout_enable(1);
	if (nes_host_load_mem(rom, rn, "game.nes", err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
	if (mode >= 1) { nes_host_wide_enable(1); nes_host_wide_margin(84); if (map) nes_host_wide_map_load(map, mn); }
	if (nes_host_state_load_mem(st, sn)) { fprintf(stderr, "state\n"); return 1; }
	nes_host_set_pad(0, NES_RIGHT);
	for (f = 0; f < frames; f++) {
		const uint16_t *wp; int w, p;
		nes_wide_stats ws;
		nes_host_run_frame();
		while (nes_host_audio_take(audio, 2048) > 0) {}
		if (mode == 1 || mode == 2) { if (nes_host_wide_frame(&wp, &w, &p, &ws)) sum += ws.real_l + ws.real_r; }
		if (mode == 3) sum += (unsigned long) nes_host_wide_capture(&snap[f & 1]);
		if (mode == 4 && nes_host_wide_capture(&snap[f & 1]) && nes_host_wide_compose(&snap[f & 1], &wp, &w, &p, &ws)) sum += ws.real_l + ws.real_r;
		if (mode == 2 || mode == 4) scouted += nes_host_scout_step(2, 0);
	}
	printf("frames %d real-sum %lu scout-frames %d\n", frames, sum, scouted);
	return 0;
}
