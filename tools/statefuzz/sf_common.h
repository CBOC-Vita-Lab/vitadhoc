/* sf_common.h - shared parts of the state-loader fuzz harnesses (one harness per core, see docs/TESTING.md "State loaders").
 * Header only, C++ (g++ -std=gnu++14 -pthread). MIT licence.
 *
 * What a harness does: take a genuine state of a running game, damage it the way a hostile peer would (the peer computes the
 * length and the checksums itself), repair the checksums, load it, run a few frames, repeat.  Build the core and the harness with
 * -fsanitize=address,undefined: any memory error, undefined behaviour or endless frame is a failure.  The state that failed is
 * written to the case directory (crash-N.st / hang-N.st) so it can be kept as a regression test.
 *
 *   sf_rng      deterministic generator (the same seed gives the same damage on every machine)
 *   sf_crc32    zlib-compatible CRC-32
 *   sf_mutate   damage a byte range of a buffer; `hot` ranges get most of the hits
 *   sf_arm      register the buffer about to be loaded; on a sanitizer abort, a fatal signal or a hang it is saved
 *   sf_beat     the harness calls it after every frame; no beat for `hang_s` seconds is a hang (the watchdog saves the case, exits 3)
 */
#ifndef SF_COMMON_H
#define SF_COMMON_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <vector>
#include <string>

struct sf_rng { uint64_t s; };
static inline void sf_seed(sf_rng *r, uint64_t seed) { r->s = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull; if (!r->s) r->s = 1; }
static inline uint32_t sf_rand(sf_rng *r)
{
	uint64_t x = r->s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; r->s = x;
	return (uint32_t)((x * 0x2545F4914F6CDD1Dull) >> 32);
}
static inline uint32_t sf_below(sf_rng *r, uint32_t n) { return n ? sf_rand(r) % n : 0; }

static inline uint32_t sf_crc32(const uint8_t *p, size_t n)
{
	static uint32_t t[256]; static int init;
	if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = 1; }
	uint32_t c = 0xFFFFFFFFu;
	while (n--) c = t[(c ^ *p++) & 255] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

struct sf_range { size_t off, len; };

static inline void sf_mutate(sf_rng *r, uint8_t *b, size_t blen, size_t lo, size_t hi, const std::vector<sf_range> &hot)
{
	static const uint32_t magic32[] = { 0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x100, 0xffff, 0x10000, 0x7fffffffu, 0x80000000u, 0xfffffff0u,
	                                    0xffffffffu, 0x00100000u, 0x01000000u, 0x0000ffffu, 0xdeadbeefu };
	if (hi > blen) hi = blen;
	if (hi <= lo + 8) return;
	int hits = (int[]){ 1, 1, 2, 4, 8, 16, 64 }[sf_below(r, 7)];
	for (int h = 0; h < hits; h++)
	{
		size_t at;
		if (!hot.empty() && sf_below(r, 3) != 0)
		{
			const sf_range &z = hot[sf_below(r, (uint32_t)hot.size())];
			if (z.len == 0 || z.off + z.len > blen) continue;
			at = z.off + sf_below(r, (uint32_t)z.len);
		}
		else at = lo + sf_below(r, (uint32_t)(hi - lo));
		size_t room = hi - at;
		switch (sf_below(r, 9))
		{
		case 0: b[at] = (uint8_t)sf_rand(r); break;
		case 1: b[at] ^= (uint8_t)(1u << sf_below(r, 8)); break;
		case 2: if (room >= 4) { uint32_t v = magic32[sf_below(r, sizeof magic32 / sizeof *magic32)]; memcpy(b + at, &v, 4); } break;
		case 3: if (room >= 2) { uint16_t v = (uint16_t)magic32[sf_below(r, sizeof magic32 / sizeof *magic32)]; memcpy(b + at, &v, 2); } break;
		case 4: { size_t n = 1 + sf_below(r, 64); if (n > room) n = room; for (size_t i = 0; i < n; i++) b[at + i] = (uint8_t)sf_rand(r); } break;
		case 5: { size_t n = 1 + sf_below(r, 256); if (n > room) n = room; memset(b + at, 0xff, n); } break;
		case 6: { size_t n = 1 + sf_below(r, 256); if (n > room) n = room; memset(b + at, 0, n); } break;
		case 7: { size_t n = 1 + sf_below(r, 128); size_t from = lo + sf_below(r, (uint32_t)(hi - lo)); if (n > room) n = room; if (from + n > hi) n = hi - from; memmove(b + at, b + from, n); } break;
		default: if (room >= 4) { uint32_t v; memcpy(&v, b + at, 4); v += (uint32_t)(int)(sf_below(r, 9) - 4); memcpy(b + at, &v, 4); } break;
		}
	}
}

/* ---- the case that is being loaded: saved when something goes wrong ---------------------------------------------------- */
static struct {
	const uint8_t *buf; size_t len; std::string dir; std::string tag; unsigned long iter; volatile unsigned long beat; int hang_s; int saved;
} sf_cur;

static inline void sf_dump(const char *kind)
{
	if (!sf_cur.buf || sf_cur.saved) return;
	sf_cur.saved = 1;
	char p[1024]; snprintf(p, sizeof p, "%s/%s-%s-%lu.st", sf_cur.dir.c_str(), kind, sf_cur.tag.c_str(), sf_cur.iter);
	FILE *f = fopen(p, "wb"); if (f) { fwrite(sf_cur.buf, 1, sf_cur.len, f); fclose(f); }
	fprintf(stderr, "STATEFUZZ %s: case saved to %s\n", kind, p);
}
static void sf_on_signal(int sig) { sf_dump("crash"); signal(sig, SIG_DFL); raise(sig); }
static void sf_on_death(void) { sf_dump("crash"); }
extern "C" void __sanitizer_set_death_callback(void (*)(void)) __attribute__((weak));

static void *sf_watchdog(void *)
{
	unsigned long last = (unsigned long)-1; int still = 0;
	for (;;)
	{
		struct timespec ts = { 0, 100 * 1000 * 1000 }; nanosleep(&ts, NULL);
		unsigned long now = sf_cur.beat;
		if (now != last) { last = now; still = 0; continue; }
		if (++still >= sf_cur.hang_s * 10) { fprintf(stderr, "STATEFUZZ hang: no frame finished for %d s\n", sf_cur.hang_s); sf_dump("hang"); fflush(stderr); _exit(3); }
	}
	return NULL;
}
static inline void sf_init(const char *dir, const char *tag, int hang_s)
{
	sf_cur.dir = dir; sf_cur.tag = tag; sf_cur.hang_s = hang_s;
	int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP };
	for (int s : sigs) signal(s, sf_on_signal);
	if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(sf_on_death);
	pthread_t t; pthread_create(&t, NULL, sf_watchdog, NULL); pthread_detach(t);
}
static inline void sf_arm(const uint8_t *buf, size_t len, unsigned long iter) { sf_cur.buf = buf; sf_cur.len = len; sf_cur.iter = iter; sf_cur.saved = 0; sf_cur.beat++; }
static inline void sf_beat(void) { sf_cur.beat++; }

static inline std::vector<uint8_t> sf_read_file(const char *path)
{
	std::vector<uint8_t> v; FILE *f = fopen(path, "rb"); if (!f) return v;
	uint8_t tmp[65536]; size_t n; while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) v.insert(v.end(), tmp, tmp + n);
	fclose(f); return v;
}
#endif
