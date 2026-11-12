/*
 * api_run: drives the shipped API (host.h, the wswancore.o the main app links) plus the blend, the way the app does:
 * per display refresh, n = wsb_frames_needed() emulated frames, each with a pad, the audio pulled, the picture submitted,
 * then wsb_present(). Writes, into OUTDIR: hashes.txt (one line per emulated frame: index, picture hash, audio hash,
 * orientation; one line per refresh: the presented picture hash), audio.raw (all stereo samples), state.txt (the state
 * size and hash at the middle of the run), game.srm (the save file at the end).
 * Built twice (x86 and armhf, see arm_compare.sh) against the two wswancore.o; the outputs must be byte identical.
 * Only integer arithmetic in this file and no ROM-specific code. Copyright CBOC-Vita. MIT licence.
 *
 *   api_run ROM FRAMES OUTDIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "host.h"
#include "blend.h"
#include "safe_io.h"

static uint64_t fnv(const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *) p;
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
	return h;
}

/* a deterministic pad schedule, integer arithmetic only (the same on every machine) */
static unsigned pad_for(unsigned f)
{
	static const unsigned seq[] = { 0, WSWAN_X1, WSWAN_X2 | WSWAN_A, WSWAN_Y1, WSWAN_Y3 | WSWAN_B, WSWAN_X3, WSWAN_X4,
	                                WSWAN_START, WSWAN_Y2, 0, WSWAN_A, WSWAN_Y4 | WSWAN_X1 };
	return seq[((f / 23) + (f * 7) / 101) % (sizeof seq / sizeof seq[0])];
}

int main(int argc, char **argv)
{
	if (argc != 4) { fprintf(stderr, "usage: api_run ROM FRAMES OUTDIR\n"); return 2; }
	unsigned frames = (unsigned) atoi(argv[2]);
	char path[1024];
	FILE *fh, *fa;
	uint8_t *rom = NULL; size_t n = 0;
	FILE *in = fopen(argv[1], "rb");
	if (!in) { fprintf(stderr, "cannot read ROM\n"); return 1; }
	fseek(in, 0, SEEK_END); long len = ftell(in); fseek(in, 0, SEEK_SET);
	if (len <= 0 || len > (16 << 20)) { fclose(in); return 1; }
	rom = (uint8_t *) malloc((size_t) len); n = fread(rom, 1, (size_t) len, in); fclose(in);
	if (n != (size_t) len) return 1;

	if (wswan_host_init()) return 1;
	char err[128] = "";
	if (wswan_host_load_mem(rom, n, argv[1], err, sizeof err)) { fprintf(stderr, "load: %s\n", err); return 1; }
	free(rom);

	wsb b;
	if (wsb_init(&b, 224, 144, 3072000, 40704, 60000, 1001)) return 1;
	uint16_t out[224 * 144];
	snprintf(path, sizeof path, "%s/hashes.txt", argv[3]); fh = fopen(path, "w");
	snprintf(path, sizeof path, "%s/audio.raw", argv[3]); fa = fopen(path, "wb");
	if (!fh || !fa) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }

	unsigned f = 0, refresh = 0;
	size_t state_n = 0; uint64_t state_h = 0; size_t state_cap = wswan_host_state_size();
	uint8_t *state = (uint8_t *) malloc(state_cap ? state_cap : 1);
	int16_t abuf[4096 * 2];
	while (f < frames) {
		int need = wsb_frames_needed(&b);
		for (int k = 0; k < need && f < frames; k++, f++) {
			wswan_host_set_pad(pad_for(f));
			wswan_host_set_render(1);
			wswan_host_run_frame();
			uint64_t ah = 1469598103934665603ull;
			int got;
			while ((got = wswan_host_audio_take(abuf, 4096)) > 0) {
				fwrite(abuf, 4, (size_t) got, fa);
				ah = fnv(abuf, (size_t) got * 4) ^ (ah * 31);
			}
			int w, h, p;
			const uint16_t *v = wswan_host_video(&w, &h, &p);
			uint64_t vh = fnv(v, (size_t) w * h * 2);
			wsb_submit(&b, v);
			fprintf(fh, "f %u %016llx %016llx %d\n", f, (unsigned long long) vh, (unsigned long long) ah, wswan_host_orientation());
			if (f == frames / 2 && state_cap) {
				state_n = wswan_host_state_save_mem(state, state_cap);
				state_h = fnv(state, state_n);
			}
		}
		wsb_present(&b, out);
		fprintf(fh, "r %u %016llx\n", refresh++, (unsigned long long) fnv(out, sizeof out));
	}
	fclose(fh); fclose(fa);
	snprintf(path, sizeof path, "%s/state.txt", argv[3]); fh = fopen(path, "w");
	fprintf(fh, "size %zu cap %zu hash %016llx\n", state_n, state_cap, (unsigned long long) state_h);
	fclose(fh);
	size_t srm_n = wswan_host_srm_size();
	uint8_t *srm = (uint8_t *) malloc(srm_n ? srm_n : 1);
	size_t got_srm = wswan_host_srm_export(srm, srm_n);
	snprintf(path, sizeof path, "%s/game.srm", argv[3]);
	char e[160] = "";
	if (wswan_write_safe(path, srm, got_srm, NULL, 0, NULL, e, sizeof e)) { fprintf(stderr, "srm: %s\n", e); return 3; }
	printf("api_run frames=%u srm=%zu state=%zu orient=%d\n", f, got_srm, state_n, wswan_host_orientation());
	free(srm); free(state);
	wsb_free(&b);
	wswan_host_shutdown();
	return 0;
}
