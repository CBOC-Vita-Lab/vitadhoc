/* handoff_glue.cpp: the present's wait (vid_swap with the display queue full) is not drawing load.
 * The draw callback does 5 ms of work and a 12 ms present; the cost the pacer is given
 * (ho_poll_cost) and the drawing total (ho_draw_stats, which the look governor reads) must be 5 ms,
 * the 12 ms is kept apart (ho_swap_stats).  Without ho_note_present_wait the old reading, 17 ms, comes back.
 *   g++ -I vita -I frontend-common vita/tests/handoff/test_ho_swap.cpp vita/handoff_glue.cpp */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "handoff_glue.h"

static uint64_t g_now;
extern "C" uint64_t plat_time_us(void) { return g_now; }
extern "C" void fe_evt(const char *, ...) {}
extern "C" void plat_thread_watch(const char *, int) {}

static int g_report_wait = 1;
static unsigned g_swap_us = 12000;

static void draw(const uint16_t *, unsigned, unsigned, size_t, uint32_t, int, const void *)
{
   g_now += 5000;                               /* the work */
   g_now += g_swap_us;                          /* vid_swap waiting for the flip */
   if (g_report_wait)
      ho_note_present_wait(g_swap_us);
}

static uint32_t run(int report, unsigned swap_us, uint64_t *dsum, uint64_t *ssum, uint32_t *n)
{
   static uint16_t pix[16 * 16];
   uint32_t cost = 0, c, dn, sn, dn0, sn0;
   uint64_t d0, s0;
   ho_draw_stats(&d0, &dn0);                    /* the totals run on across games: measure the difference */
   ho_swap_stats(&s0, &sn0);
   g_report_wait = report;
   g_swap_us = swap_us;
   g_now = 1000000;
   assert(ho_start(16743, 0, draw, -1, -1) == 1);
   for (int i = 0; i < 4; i++) {
      assert(ho_stage(pix, 16, 16, 32, (uint32_t)i, 0));
      ho_commit(3000);
      ho_consume_inline();                      /* the twin's drawing thread: inline */
      while (ho_poll_cost(&c)) cost = c;
      g_now += 20000;
   }
   ho_draw_stats(dsum, &dn);
   ho_swap_stats(ssum, &sn);
   *dsum -= d0; *ssum -= s0; dn -= dn0; sn -= sn0;
   *n = dn;
   assert(dn == sn);
   ho_stop();
   return cost;
}

int main(void)
{
   uint64_t d, s;
   uint32_t n, cost;
   /* RED, the old reading: no report, so the 12 ms wait is counted as work */
   cost = run(0, 12000, &d, &s, &n);
   printf("unreported: cost %u us, draw %llu us / %u pictures, swap %llu\n", cost, (unsigned long long)d, n, (unsigned long long)s);
   assert(d / n == 17000 && s == 0);
   /* GREEN: reported, so only the 5 ms is the cost */
   cost = run(1, 12000, &d, &s, &n);
   printf("reported:   cost %u us, draw %llu us / %u pictures, swap %llu us\n", cost, (unsigned long long)d, n, (unsigned long long)s);
   assert(n == 4 && d == 4 * 5000u && s == 4 * 12000u);
   assert(cost >= 5000 && cost < 6000);        /* the larger of the two threads' work: 5 ms drawing vs 3 ms emulating */
   /* a swap that is not slower than nothing: the work is untouched */
   cost = run(1, 0, &d, &s, &n);
   assert(d == 4 * 5000u && s == 0);
   puts("handoff present-wait: ok");
   return 0;
}
