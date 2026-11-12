/* Copyright CBOC-Vita. MIT licence.
 * Tiny headless libretro frontend used to compare other cores (stock libretro-fceumm, Nestopia) with ours on the public
 * test ROMs: lrhead CORE.so ROM FRAMES [--hash F] [--ram F] [--status] [--text] [--nt] [--summary] [--pad MASK].
 * --status/--text read $6000.. through the core's memory map (or SAVE_RAM); --nt prints the text of nametable 0 where the
 * core maps PPU memory (Nestopia: 0x80002000); --ram writes the 2 KiB system RAM. Build: cc -O2 -w lrhead.c -I<libretro.h dir> -ldl */
/* tiny headless libretro frontend: lrhead CORE.so ROM FRAMES [--hash FILE] [--status] [--audio] [--padscript]
   prints: fnv of last frame, status bytes at $6000.., frame-hash every frame to file */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <time.h>
#include "libretro.h"

static void *H;
static uint64_t vhash; static int vw, vh;
static unsigned fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint64_t vhash_fnv(const void *d, unsigned w, unsigned h, size_t pitch, unsigned bpp)
{
	const uint8_t *p = d; uint64_t x = 1469598103934665603ull;
	for (unsigned y = 0; y < h; y++) for (unsigned i = 0; i < w * bpp; i++) { x ^= p[y * pitch + i]; x *= 1099511628211ull; }
	return x;
}
static struct retro_memory_map mmap_; static struct retro_memory_descriptor descs[64]; static int ndesc;
static int mm_set;
static void logcb(enum retro_log_level l, const char *f, ...) { }
static unsigned padmask;
static bool env(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *) data)->log = logcb; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: fmt = *(unsigned *) data; return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **) data = "/tmp"; return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *) data = true; return true;
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
		const struct retro_memory_map *m = data; mm_set = 1; ndesc = m->num_descriptors > 64 ? 64 : m->num_descriptors;
		memcpy(descs, m->descriptors, ndesc * sizeof descs[0]); return true; }
	case RETRO_ENVIRONMENT_GET_VARIABLE: ((struct retro_variable *) data)->value = NULL; return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *) data = false; return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *) data = 0; return true;
	default: return false;
	}
}
static void video(const void *d, unsigned w, unsigned h, size_t pitch)
{ if (d) { vw = w; vh = h; vhash = vhash_fnv(d, w, h, pitch, fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2); } }
static size_t asamples;
static void audio1(int16_t l, int16_t r) { asamples++; }
static size_t audiob(const int16_t *d, size_t n) { asamples += n; return n; }
static void inpoll(void) {}
static int16_t instate(unsigned port, unsigned dev, unsigned idx, unsigned id)
{ if (port == 0 && dev == RETRO_DEVICE_JOYPAD) return (padmask >> id) & 1; return 0; }

static uint64_t ns(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1000000000ull + t.tv_nsec; }

int main(int argc, char **argv)
{
	if (argc < 4) return 2;
	H = dlopen(argv[1], RTLD_NOW); if (!H) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define S(n) void (*n##_)() ; *(void **) &n##_ = dlsym(H, #n); if (!n##_) { fprintf(stderr, "missing " #n "\n"); return 1; }
	void (*set_env)(retro_environment_t) = dlsym(H, "retro_set_environment");
	void (*set_video)(retro_video_refresh_t) = dlsym(H, "retro_set_video_refresh");
	void (*set_audio)(retro_audio_sample_t) = dlsym(H, "retro_set_audio_sample");
	void (*set_audiob)(retro_audio_sample_batch_t) = dlsym(H, "retro_set_audio_sample_batch");
	void (*set_poll)(retro_input_poll_t) = dlsym(H, "retro_set_input_poll");
	void (*set_in)(retro_input_state_t) = dlsym(H, "retro_set_input_state");
	void (*init)(void) = dlsym(H, "retro_init");
	bool (*load)(const struct retro_game_info *) = dlsym(H, "retro_load_game");
	void (*run)(void) = dlsym(H, "retro_run");
	void *(*getmem)(unsigned) = dlsym(H, "retro_get_memory_data");
	size_t (*getsz)(unsigned) = dlsym(H, "retro_get_memory_size");
	void (*sysinfo)(struct retro_system_info *) = dlsym(H, "retro_get_system_info");
	set_env(env); set_video(video); set_audio(audio1); set_audiob(audiob); set_poll(inpoll); set_in(instate);
	init();
	struct retro_system_info si; sysinfo(&si);
	FILE *f = fopen(argv[2], "rb"); if (!f) return 1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
	struct retro_game_info gi = { argv[2], si.need_fullpath ? NULL : buf, n, NULL };
	if (!load(&gi)) { printf("LOADFAIL\n"); return 3; }
	int frames = atoi(argv[3]); const char *hf = 0; int status = 0, text = 0, pad = 0, wantpad = 0; const char *ramf = 0; int ntdump = 0;
	for (int i = 4; i < argc; i++) { if (!strcmp(argv[i], "--hash")) hf = argv[++i]; else if (!strcmp(argv[i], "--status")) status = 1; else if (!strcmp(argv[i], "--text")) text = 1; else if (!strcmp(argv[i], "--summary")) pad = 1; else if (!strcmp(argv[i], "--nt")) ntdump = 1; else if (!strcmp(argv[i], "--ram")) ramf = argv[++i]; else if (!strcmp(argv[i], "--pad")) padmask = strtoul(argv[++i], 0, 0); }
	FILE *o = hf ? fopen(hf, "w") : 0; uint64_t t0 = 0, tot = 0;
	for (int i = 0; i < frames; i++) { t0 = ns(); run(); tot += ns() - t0; if (o) fprintf(o, "%d %016llx\n", i, (unsigned long long) vhash); }
	if (o) fclose(o);
	/* find $6000: memory map descriptor covering 0x6000, else SAVE_RAM */
	const uint8_t *w = NULL; size_t wl = 0;
	for (int i = 0; i < ndesc; i++) if (descs[i].start <= 0x6000 && 0x6000 < descs[i].start + descs[i].len && descs[i].ptr) {
		w = (const uint8_t *) descs[i].ptr + descs[i].offset + (0x6000 - descs[i].start); wl = descs[i].len - (0x6000 - descs[i].start); }
	if (!w) { w = getmem(RETRO_MEMORY_SAVE_RAM); wl = w ? getsz(RETRO_MEMORY_SAVE_RAM) : 0; }
	if (ramf) { const uint8_t *r = getmem(RETRO_MEMORY_SYSTEM_RAM); size_t rn = getsz(RETRO_MEMORY_SYSTEM_RAM); FILE *rf = fopen(ramf, "wb"); if (rf && r) { fwrite(r, 1, rn > 2048 ? 2048 : rn, rf); } if (rf) fclose(rf); }
	if (getenv("LRDBG")) for (int i = 0; i < ndesc; i++) fprintf(stderr, "desc %d start=%05x len=%x flags=%x ptr=%p off=%zx\n", i, (unsigned) descs[i].start, (unsigned) descs[i].len, (unsigned) descs[i].flags, descs[i].ptr, (size_t) descs[i].offset);
	if (ntdump) {
		for (int i = 0; i < ndesc; i++) if (descs[i].start == 0x80002000u && descs[i].ptr) {
			const uint8_t *nt = (const uint8_t *) descs[i].ptr + descs[i].offset; size_t len = descs[i].len > 960 ? 960 : descs[i].len;
			printf("nt:"); for (size_t y = 0; y < len / 32; y++) { char ln[33]; int any = 0; for (int x = 0; x < 32; x++) { unsigned c = nt[y * 32 + x]; ln[x] = c >= 32 && c < 127 ? c : ' '; if (ln[x] != ' ') any = 1; } ln[32] = 0; if (any) printf(" | %s", ln); }
			printf("\n"); break; }
	}
	if (status) {
		if (wl >= 4) printf("peek $6000=%02x sig=%02x%02x%02x\n", w[0], w[1], w[2], w[3]); else printf("peek $6000=00 sig=000000 (no wram)\n");
	}
	if (text && wl > 4) { printf("text: "); for (size_t i = 4; i < wl && w[i]; i++) putchar(w[i]); printf("\n"); }
	if (pad) printf("fps=%8.1f ms/frame=%6.3f lasthash=%016llx audio=%zu\n", tot ? frames / (tot / 1e9) : 0.0, tot / 1e6 / frames, (unsigned long long) vhash, asamples);
	return 0;
}
