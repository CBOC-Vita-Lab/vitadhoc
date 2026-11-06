/*
 * CBOC-Vita local addition (not part of upstream Snes9x).
 * Optional timing hooks that split run time into CPU (65816 + everything not
 * claimed below), APU (SPC700 + DSP + resampler), PPU (scanline rendering),
 * CHIP (Super FX, SA-1) and FRONT (frontend work outside the core).
 * Compiles to nothing unless S9X_PROF is defined. See snes/README.md.
 */
#ifndef S9X_PROF_H
#define S9X_PROF_H

#ifdef S9X_PROF
#include <stdint.h>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
static inline uint64_t s9xp_now(void) { return sceKernelGetProcessTimeWide() * 1000ull; }
#else
#include <time.h>
static inline uint64_t s9xp_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
#endif

enum { S9XP_CPU, S9XP_APU, S9XP_PPU, S9XP_CHIP, S9XP_FRONT, S9XP_N };
/* event counters: how often the work would have to be synchronised if it ran on another core */
enum { S9XC_PPU_FLUSH, S9XC_APU_PORT, S9XC_APU_RUN, S9XC_CHIP_RUN, S9XC_N };

struct S9xProfState
{
	uint64_t acc[S9XP_N];	/* nanoseconds per category */
	uint64_t cnt[S9XC_N];	/* event counts */
	uint64_t last;
	int cur, sp, on;
	int stack[8];
};
extern struct S9xProfState g_s9xp;

static inline void s9xp_enter(int c)
{
	if (!g_s9xp.on) return;
	uint64_t t = s9xp_now();
	g_s9xp.acc[g_s9xp.cur] += t - g_s9xp.last;
	g_s9xp.last = t;
	g_s9xp.stack[g_s9xp.sp++ & 7] = g_s9xp.cur;
	g_s9xp.cur = c;
}
static inline void s9xp_leave(void)
{
	if (!g_s9xp.on) return;
	uint64_t t = s9xp_now();
	g_s9xp.acc[g_s9xp.cur] += t - g_s9xp.last;
	g_s9xp.last = t;
	g_s9xp.cur = g_s9xp.stack[--g_s9xp.sp & 7];
}
#define S9XP_COUNT(c) do { if (g_s9xp.on) g_s9xp.cnt[S9XC_##c]++; } while (0)
#define S9XP_ENTER(c) s9xp_enter(S9XP_##c)
#define S9XP_LEAVE()  s9xp_leave()
#else
#define S9XP_COUNT(c) ((void)0)
#define S9XP_ENTER(c) ((void)0)
#define S9XP_LEAVE()  ((void)0)
#endif
#endif
