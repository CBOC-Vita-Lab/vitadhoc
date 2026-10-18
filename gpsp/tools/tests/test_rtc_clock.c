/* test_rtc_clock.c -- which time the GBA cartridge clock shows (rtc_clock.h).
 *
 * The rule gba_memory.c applies at every clock read, driven the way a play
 * session drives it:
 *   1. No wall clock published: base + frames, bit for bit (every rig, the
 *      libretro core, a link session's second machine).
 *   2. Wall clock published: the wall clock, whatever the frame clock says --
 *      fast-forward cannot push it ahead, a pause or a sleep cannot leave it
 *      behind, a state load cannot rewind it.
 *   3. The guard: never earlier than the last value shown.
 *   4. A link session forces the frame clock and hands back without a step
 *      backwards.
 *   5. The player's report: fast-forward across midnight, save, boot again.
 *      The date at the next boot must not be before the date at the save. */
#include <stdio.h>
#include <stdlib.h>

#include "rtc_clock.h"

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
   printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define DAY 86400ll
#define T0  1791504000ll            /* 2026-10-09 00:00:00 */
/* GBA frames per second, as gba_memory.c's GBA_FRAME_SECONDS. */
#define FPS 59.7275

/* A session as gba_memory.c runs it: the base follows the shown time while
 * the wall clock is in use (so a state written now continues from now). */
typedef struct { long long base, last; unsigned frame; } cart;
static long long cart_read(cart *c, long long wall, int frames_only)
{
   long long elapsed = (long long)((double)c->frame / FPS);
   int used;
   long long t = rtc_clock_pick(c->base + elapsed, wall, frames_only,
                                &c->last, &used);
   if (used)
      c->base = t - elapsed;
   return t;
}

static void test_frames_unchanged(void)
{
   long long last = 0, f;
   int used = 1;
   for (f = 0; f < 5 * DAY; f += 977)
   {
      CHECK(rtc_clock_pick(T0 + f, 0, 0, &last, &used) == T0 + f);
      CHECK(!used && last == T0 + f);
   }
   /* a rewind (a state load) is the frame clock's own business */
   CHECK(rtc_clock_pick(T0 + 5, 0, 0, &last, &used) == T0 + 5);
   /* an implausible "wall clock" is no wall clock */
   CHECK(rtc_clock_pick(T0 + 9, 12345, 0, &last, &used) == T0 + 9 && !used);
   CHECK(rtc_clock_pick(T0 + 9, -1, 0, &last, &used) == T0 + 9 && !used);
   /* forced frames beats a published wall clock */
   CHECK(rtc_clock_pick(T0 + 9, T0 + DAY, 1, &last, &used) == T0 + 9 && !used);
}

static void test_wall(void)
{
   long long last = 0;
   int used = 0;
   /* the frame clock an hour ahead (fast-forward), a day behind (sleep), or
    * garbage: the wall clock is shown */
   CHECK(rtc_clock_pick(T0 + 3600, T0, 0, &last, &used) == T0 && used);
   CHECK(rtc_clock_pick(T0 - DAY, T0 + 1, 0, &last, &used) == T0 + 1);
   CHECK(rtc_clock_pick(0, T0 + 2, 0, &last, &used) == T0 + 2);
   /* THE GUARD: a reading behind the last one shown holds the clock */
   CHECK(rtc_clock_pick(0, T0 + 1, 0, &last, &used) == T0 + 2);
   CHECK(rtc_clock_pick(0, T0 - 500, 0, &last, &used) == T0 + 2);
   CHECK(rtc_clock_pick(0, T0 + 2, 0, &last, &used) == T0 + 2);
   CHECK(rtc_clock_pick(0, T0 + 3, 0, &last, &used) == T0 + 3);
   /* a failed reading (the frontend keeps the old value; were it to publish
    * nothing at all, the frame clock takes over, and the caller's base has
    * been following the wall clock, so it continues from there) */
   {
      cart c = { T0, 0, 0 };
      long long t1 = cart_read(&c, T0 + 100, 0), t2;
      c.frame += 600;                              /* ~10 s of frames */
      t2 = cart_read(&c, 0, 0);
      CHECK(t1 == T0 + 100 && t2 >= t1 && t2 <= t1 + 11);
   }
}

/* Fast-forward: 8 emulated seconds a second for six real hours, read once a
 * frame.  The cartridge clock is the wall clock throughout. */
static void test_fast_forward(void)
{
   cart c = { T0 + 22 * 3600, 0, 0 };              /* boot at 22:00 */
   long long wall = T0 + 22 * 3600, s, k;
   for (s = 0; s < 6 * 3600; s++, wall++)
      for (k = 0; k < 8; k++)
      {
         c.frame += 60;
         CHECK(cart_read(&c, wall, 0) == wall);
      }
   /* ...where the frame clock alone would be two days ahead */
   CHECK((long long)((double)c.frame / FPS) > 47 * 3600);
}

/* THE REPORT.  Session 1: boot 23:30, fast-forward so that an hour of game
 * time passes in ten real minutes, save.  Session 2 boots five minutes
 * later.  With the frame clock the save is dated the next day and the boot
 * the day before; with the wall clock the date never goes back. */
static void test_report(void)
{
   long long boot1 = T0 + 23 * 3600 + 1800, real_at_save = boot1 + 600;
   long long boot2 = real_at_save + 300;
   unsigned frames = (unsigned)(3600.0 * FPS);
   /* 3.5.0 */
   {
      cart c = { boot1, 0, 0 };
      long long saved, next;
      c.frame = frames;
      saved = cart_read(&c, 0, 0);
      {
         cart d = { boot2, 0, 0 };
         next = cart_read(&d, 0, 0);
      }
      CHECK(saved / DAY > next / DAY);             /* the bug */
   }
   /* this build */
   {
      cart c = { boot1, 0, 0 };
      long long saved, next;
      c.frame = frames;
      saved = cart_read(&c, real_at_save, 0);
      {
         cart d = { boot2, 0, 0 };
         next = cart_read(&d, boot2, 0);
      }
      CHECK(saved == real_at_save);
      CHECK(next >= saved && saved / DAY <= next / DAY);
   }
}

/* A state made last night, loaded this afternoon: the base and frame count
 * come back from the state, the clock does not go with them. */
static void test_state_load(void)
{
   long long night = T0 + 23 * 3600, noon = T0 + DAY + 12 * 3600;
   cart c = { night - 100, 0, 6000 };
   cart state;
   CHECK(cart_read(&c, night, 0) == night);
   state = c;                                      /* savestate */
   c.frame += 100000;
   CHECK(cart_read(&c, noon, 0) == noon);
   c.base = state.base; c.frame = state.frame;     /* load: last is not state */
   CHECK(cart_read(&c, noon + 1, 0) == noon + 1);
   /* and the state itself, loaded on a frame clock, continues from the
    * moment it was made */
   {
      cart f = state;
      f.last = 0;
      CHECK(cart_read(&f, 0, 0) == night);
   }
}

/* A link session: frames only, on a base that was following the wall clock,
 * running 0.36 % fast (59.94 Hz against 59.7275) for half an hour; then
 * back to the wall clock.  No step backwards at either edge. */
static void test_session(void)
{
   cart c = { T0, 0, 0 };
   long long wall = T0 + 5000, before, in, after, s;
   before = cart_read(&c, wall, 0);
   in = cart_read(&c, wall, 1);
   CHECK(in == before);                            /* entry: continuous */
   for (s = 0; s < 1800; s++)
   {
      long long t;
      wall++;
      c.frame += 60;                               /* 60 frames a real second */
      t = cart_read(&c, wall, 1);
      CHECK(t >= in);
      in = t;
   }
   CHECK(in > wall && in - wall < 15);             /* ran a few seconds fast */
   after = cart_read(&c, wall, 0);
   CHECK(after == in);                             /* exit: held, not stepped back */
   for (s = 0; s < 30; s++)
   {
      long long t = cart_read(&c, ++wall, 0);
      CHECK(t >= after);
      after = t;
   }
   CHECK(after == wall);                           /* and real time took over */
}

int main(void)
{
   test_frames_unchanged();
   test_wall();
   test_fast_forward();
   test_report();
   test_state_load();
   test_session();
   printf("test_rtc_clock: OK (%d checks)\n", checks);
   return 0;
}
