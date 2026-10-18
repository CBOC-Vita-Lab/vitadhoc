/* rtc_clock.h -- which time the GBA cartridge clock shows.
 *
 * Pure, so the host test (tools/tests/test_rtc_clock.c) runs exactly the
 * rule gba_memory.c uses.
 *
 * TWO SOURCES.
 *   frames  base + emulated frames (gba_memory.c rtc_base_time).  Repeatable:
 *           the same state and inputs give the same clock on any machine at
 *           any hour.  What every build did up to 3.5.0, and still what a
 *           link session and every test rig use.
 *   wall    the console's real clock, published by the frontend
 *           (gpsp_rtc_wall).  What a player gets.
 *
 * WHY THE WALL CLOCK.  The frame clock runs at the speed of emulation, not
 * of time: fast-forward pushes it ahead of the real clock (and even at 1x
 * the PSP's 59.94 Hz refresh against the GBA's 59.73 gains ~13 s an hour),
 * a pause or a sleep leaves it behind, a state load rewinds it.  A game
 * saved while its clock was ahead finds, at the next boot, a clock EARLIER
 * than the date in its own save: Pokemon Unbound then reports "RTC
 * tampering has been detected in your save file!" (reproduced; reported by
 * a player as save corruption).  The wall clock cannot be moved by anything
 * the emulator does.
 *
 * THE GUARD.  In wall mode the clock never returns less than the last value
 * the game was shown: a failed or slightly-behind reading, or the hand-back
 * from a link session whose frame clock ran a few seconds fast, holds the
 * clock still until real time catches up instead of stepping it back. */
#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

/* No real clock reads earlier than this (2000-01-01). */
#define RTC_CLOCK_PLAUSIBLE 946684800ll

/* `frames_time`: base + elapsed.  `wall`: the published wall clock, 0 (or
 * anything implausible) = none.  `frames_only`: nonzero forces the frame
 * clock (a link session).  `*last_shown`: the last value returned, kept by
 * the caller across calls (start it at 0).  Returns the time to show; sets
 * *used_wall (may be NULL). */
static inline long long rtc_clock_pick(long long frames_time, long long wall,
                                       int frames_only, long long *last_shown,
                                       int *used_wall)
{
   long long t = frames_time;
   int w = !frames_only && wall >= RTC_CLOCK_PLAUSIBLE;
   if (w)
   {
      t = wall;
      if (t < *last_shown)
         t = *last_shown;
   }
   *last_shown = t;
   if (used_wall)
      *used_wall = w;
   return t;
}

#endif
