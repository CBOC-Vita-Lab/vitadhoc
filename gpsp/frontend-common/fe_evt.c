#include "fe_evt.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

/* ADR-0067: the playable build implies no telemetry.  Derived HERE, in the
 * translation unit that uses it, because a `#define` in psp/main_psp.c is
 * invisible to this file — the first version of this change put it there and
 * produced a "telemetry-free" build that linked the whole logger. */
/* GPSP_NO_TELEMETRY is derived in fe_evt.h now, so call sites and this
 * implementation cannot disagree about it.  GPSP_KEEP_TELEMETRY is the
 * diagnostic escape hatch: player DEFAULTS with the log still attached, which
 * is the only way the PLAY_* behaviour and the evidence for it can exist in
 * one binary.  A real playable build never passes it.
 *
 * The definition below must compile either way, so the no-op macro is undone
 * for this file alone; with telemetry off nothing calls the result. */
#undef fe_evt

static FILE *evt_file    = NULL;
/* Both of these exist for the async sink and the desktop echo; a build with
 * neither leaves them defined and unreferenced rather than wrong. */
static int   evt_echo __attribute__((unused)) = 0;

/* ADR-0021 log-I/O cost accounting. */
static unsigned long long (*evt_clock)(void);
static unsigned evt_lines, evt_us, evt_max_us;
static unsigned evt_limit, evt_bytes;
static int evt_timestamps __attribute__((unused));
static int evt_capped, evt_buffered;
static const char evt_limit_marker[] = "EVT evt_limit capped=1\n";

/* ---- async line ring (ADR-0024) -----------------------------------------
 * Producer: whichever thread calls fe_evt/fe_log (the emulation thread).
 * Consumer: exactly one writer thread, via fe_evt_service().
 * Free-running byte indices; FE_EVT_RING is a power of two so the wrap is a
 * mask.  `evt_r` is only ever advanced by the consumer and only AFTER the
 * bytes have been written, so a producer that races it merely sees the ring
 * as fuller than it is — it can never overwrite unflushed bytes. */
#define FE_EVT_RING 16384
#define FE_EVT_MASK (FE_EVT_RING - 1)
#define EVT_LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define EVT_STORE(x, value) __atomic_store_n(&(x), (value), __ATOMIC_RELEASE)

static char evt_ring[FE_EVT_RING];
static volatile unsigned evt_w, evt_r;
static void (*evt_wake)(void);
static unsigned evt_drop, evt_hi;
/* Drops already announced by an in-band gap marker (see emit()). */
static unsigned evt_drop_marked __attribute__((unused));
static unsigned evt_io_us, evt_io_max_us;

void fe_evt_set_clock(unsigned long long (*now_us)(void))
{
   evt_clock = now_us;
}

unsigned long long fe_evt_now_us(void)
{
   return evt_clock ? evt_clock() : 0ull;
}

void fe_evt_prof(unsigned *lines, unsigned *us, unsigned *max_us)
{
   if (lines)  *lines  = evt_lines;
   if (us)     *us     = evt_us;
   if (max_us) *max_us = evt_max_us;
}

void fe_evt_prof_io(unsigned *io_us, unsigned *io_max_us, unsigned *dropped,
                    unsigned *ring_hi)
{
   if (io_us)     *io_us     = EVT_LOAD(evt_io_us);
   if (io_max_us) *io_max_us = EVT_LOAD(evt_io_max_us);
   if (dropped)   *dropped   = evt_drop;
   if (ring_hi)   *ring_hi   = evt_hi;
}

/* ADR-0067: GPSP_NO_TELEMETRY — the PLAYABLE build has no event log at all.
 *
 * There is no runtime config that turns `fe_evt` off: `log_thread` and
 * `net_log_thread` choose HOW a line reaches the memory stick, never WHETHER
 * one is produced.  A build meant for someone to actually play should not be
 * opening a file on ms0:, formatting several hundred bytes a second into it,
 * or (worse) falling back to the `!evt_file` branch of emit(), which fwrites
 * and fflushes every line to stderr whether or not anything is listening.
 *
 * So this is a build flag, not a key: with it set `fe_evt_init` opens
 * nothing, `emit` returns immediately, and the ring, the writer thread and
 * the profiler all have nothing to do.  Call sites are untouched, so the
 * harness build is unaffected and the two cannot drift apart. */
int fe_evt_init(const char *path, int echo_stderr)
{
#ifdef GPSP_NO_TELEMETRY
   (void)path; (void)echo_stderr;
   return -1;
#else
   evt_echo = echo_stderr;
   if (evt_file)
   {
      fclose(evt_file);
      evt_file = NULL;
   }
   evt_w = evt_r = evt_drop = evt_drop_marked = evt_hi = 0;
   evt_lines = evt_us = evt_max_us = evt_io_us = evt_io_max_us = 0;
   evt_limit = evt_bytes = 0;
   evt_timestamps = evt_capped = evt_buffered = 0;
   evt_wake = NULL;
   if (path && path[0])
      evt_file = fopen(path, "w");
   return evt_file ? 0 : -1;
#endif
}

int fe_evt_init_rotating(const char *path, int echo_stderr,
                          unsigned max_bytes, unsigned history)
{
#ifdef GPSP_NO_TELEMETRY
   (void)path; (void)echo_stderr; (void)max_bytes; (void)history;
   return -1;
#else
   char from[1024], to[1024];
   unsigned i;
   FILE *probe;
   if (!path || !*path || strlen(path) + 12 >= sizeof(from) || history > 3 ||
       max_bytes < 1024)
      return -1;
   fe_evt_close();
   for (i = history; i > 0; i--)
   {
      if (i == 1) snprintf(from, sizeof(from), "%s", path);
      else snprintf(from, sizeof(from), "%s.%u", path, i - 1);
      snprintf(to, sizeof(to), "%s.%u", path, i);
      probe = fopen(from, "rb");
      if (!probe)
      {
         if (errno == ENOENT) continue;
         return -1;
      }
      fclose(probe);
      /* Vita's rename need not replace an existing destination. */
      remove(to);
      if (rename(from, to) != 0) return -1; /* do not truncate the old session */
   }
   i = (unsigned)fe_evt_init(path, echo_stderr);
   evt_timestamps = 1;
   evt_limit = max_bytes;
   return (int)i;
#endif
}

static unsigned write_bounded(const char *p, unsigned n) __attribute__((unused));
static unsigned write_bounded(const char *p, unsigned n)
{
   unsigned wrote;
   if (evt_capped) return 0;
   if (evt_limit && n + sizeof(evt_limit_marker) - 1 > evt_limit - evt_bytes)
   {
      wrote = (unsigned)fwrite(evt_limit_marker, 1, sizeof(evt_limit_marker) - 1, evt_file);
      evt_bytes += wrote;
      evt_capped = 1;
      return 0;
   }
   wrote = (unsigned)fwrite(p, 1, n, evt_file);
   evt_bytes += wrote;
   return wrote;
}

/* Write everything the ring holds, then fflush once.  ONLY the writer
 * thread may call this while async mode is on; fe_evt_set_async(NULL) calls
 * it on the caller's thread once queueing has been switched off. */
int fe_evt_service(void)
{
#ifdef GPSP_NO_TELEMETRY
   /* ADR-0067: the playable build has no log file, so this was already inert
    * at runtime -- fe_evt_open() is compiled out, evt_file is permanently
    * NULL, and the guard below returned 0 on the first branch.  It was still
    * COMPILED, though, which linked fwrite/fflush and left a live stdio path
    * in a binary that is supposed to have none.  Caught by `psp-nm -u
    * fe_evt.o` still listing them after every token check passed -- the same
    * behavioural check that caught the GPSP_NO_TELEMETRY define landing in the
    * wrong translation unit.  Returning here removes the code, not just the
    * calls. */
   return 0;
#else
   unsigned long long t0;
   unsigned r = EVT_LOAD(evt_r);
   unsigned w = EVT_LOAD(evt_w); /* snapshot: the producer may run on */
   unsigned n = w - r, wrote = 0;

   if (!n)
      return 0;
   if (!evt_file)
   {
      EVT_STORE(evt_r, w);       /* nowhere to put it; do not wedge the ring */
      return 0;
   }

   t0 = evt_clock ? evt_clock() : 0;
   while (n)
   {
      unsigned off   = r & FE_EVT_MASK;
      unsigned first = FE_EVT_RING - off;
      unsigned chunk = (n < first) ? n : first;
      /* Budget is checked for the entire snapshot before either wrap chunk,
       * so a cap never cuts a line at the ring boundary. */
      if (evt_capped || (evt_limit && n + sizeof(evt_limit_marker) - 1 > evt_limit - evt_bytes))
      {
         write_bounded(&evt_ring[off], n);
         r += n;
         n = 0;
         break;
      }
      if (write_bounded(&evt_ring[off], chunk) != chunk)
         break;
      r     += chunk;
      n     -= chunk;
      wrote += chunk;
   }
   fflush(evt_file);
   EVT_STORE(evt_r, r);          /* release only after the bytes are out */

   if (evt_clock)
   {
      unsigned d = (unsigned)(evt_clock() - t0);
      EVT_STORE(evt_io_us, EVT_LOAD(evt_io_us) + d);
      if (d > EVT_LOAD(evt_io_max_us))
         EVT_STORE(evt_io_max_us, d);
   }
   return (int)wrote;
#endif   /* GPSP_NO_TELEMETRY */
}

unsigned fe_evt_drops(void)
{
   return evt_drop;
}

int fe_evt_pending(void)
{
   return (int)(EVT_LOAD(evt_w) - EVT_LOAD(evt_r));
}

void fe_evt_set_async(void (*wake)(void))
{
   if (!wake)
   {
      evt_wake = NULL;          /* stop queueing FIRST... */
      evt_buffered = 0;
      fe_evt_service();         /* ...then drain here, single consumer */
      return;
   }
   /* Scope the emulation-thread maximum to the async era.  Boot logging is
    * deliberately synchronous, and on a memory stick those few lines carry
    * exactly the ~12 ms flush this change exists to remove — leaving them
    * in a free-running max would make every future field log read as if
    * nothing had improved.  `evt_us`/`evt_lines` stay cumulative because
    * sess_cost deltas them; only the max needs re-basing. */
   evt_max_us = 0;
   evt_buffered = 1;
   evt_wake = wake;
}

void fe_evt_set_buffered(void)
{
   evt_wake = NULL;
   evt_buffered = 1;
   evt_max_us = 0;
}

void fe_evt_finish_buffered(void)
{
#ifndef GPSP_NO_TELEMETRY
   /* Keep queueing enabled until after the final marker has space. This also
    * avoids stderr fallback if the file could not be opened. */
   fe_evt_service();
   fe_evt("evt_finish dropped=%u pending=%d", fe_evt_drops(), fe_evt_pending());
   fe_evt_service();
   evt_wake = NULL;
   evt_buffered = 0;
#endif
}

void fe_evt_close(void)
{
   evt_wake = NULL;
   evt_buffered = 0;
   fe_evt_service();
   if (evt_file)
   {
      fclose(evt_file);
      evt_file = NULL;
   }
}

/* Single producer.  Returns 0 if the line does not fit (dropped whole —
 * never half a line, so the log is always parseable). */
static int ring_push(const char *p, unsigned n) __attribute__((unused));
static int ring_push(const char *p, unsigned n)
{
   unsigned w    = EVT_LOAD(evt_w);
   unsigned used = w - EVT_LOAD(evt_r);
   unsigned i;

   if (used + n > FE_EVT_RING)
      return 0;
   for (i = 0; i < n; i++)
      evt_ring[(w + i) & FE_EVT_MASK] = p[i];
   EVT_STORE(evt_w, w + n);      /* publish only after the copy */
   if (used + n > evt_hi)
      evt_hi = used + n;
   return 1;
}

static void emit(const char *prefix, const char *fmt, va_list ap)
{
#ifdef GPSP_NO_TELEMETRY
   (void)prefix; (void)fmt; (void)ap;   /* ADR-0067, see fe_evt_init */
#else
   char line[512];
   unsigned long long t0 = evt_clock ? evt_clock() : 0;
   unsigned pl = (unsigned)strlen(prefix);
   unsigned n;
   int fn;

   memcpy(line, prefix, pl);
   unsigned reserve = evt_timestamps ? 48 : 1;
   fn = vsnprintf(line + pl, sizeof(line) - pl - reserve, fmt, ap);
   if (fn < 0)
      fn = 0;
   if ((unsigned)fn > sizeof(line) - pl - reserve - 1)
      fn = (int)(sizeof(line) - pl - reserve - 1);
   n = pl + (unsigned)fn;
   if (evt_timestamps)
      n += (unsigned)snprintf(line + n, sizeof(line) - n, " ts_us=%llu", t0);
   line[n++] = '\n';

   if (evt_buffered)
   {
      /* Async: the emulation thread pays a memcpy and a semaphore signal;
       * the memory stick is the writer thread's problem (ADR-0024). */
      /* A GAP IN THE LOG IS MARKED WHERE IT HAPPENED.  The drop counter used
       * to reach the file only inside sess_cost -- a line that goes through
       * this same ring, so a starved writer (the io thread sits BELOW the
       * emulation thread and a main loop that never blocks never lets it run)
       * lost the very report of its own loss.  Now the first line that fits
       * after any drop is preceded by `EVT evt_gap dropped=N total=T`, so a
       * reader sees exactly where lines vanished and how many.  If even the
       * marker does not fit, the line is dropped too and the marker waits for
       * the next one: the gap only grows, it is never silent.  The exit path
       * also writes the total synchronously (fe_evt_drops()). */
      if (evt_drop != evt_drop_marked)
      {
         char mk[96];
         int mn = snprintf(mk, sizeof(mk), "EVT evt_gap dropped=%u total=%u\n",
                           evt_drop - evt_drop_marked, evt_drop);
         if (mn > 0 && (EVT_LOAD(evt_w) - EVT_LOAD(evt_r)) + (unsigned)mn + n <= FE_EVT_RING &&
             ring_push(mk, (unsigned)mn))
            evt_drop_marked = evt_drop;
      }
      if (evt_drop != evt_drop_marked || !ring_push(line, n))
         evt_drop++;
      if (evt_wake) evt_wake();
   }
   else if (evt_file)
   {
      write_bounded(line, n);
      fflush(evt_file);
   }
   if (!evt_buffered && (evt_echo || !evt_file))
   {
      fwrite(line, 1, n, stderr);
      fflush(stderr);
   }

   evt_lines++;
   if (evt_clock)
   {
      unsigned d = (unsigned)(evt_clock() - t0);
      evt_us += d;
      if (d > evt_max_us)
         evt_max_us = d;
   }
#endif
}

void fe_evt(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   emit("EVT ", fmt, ap);
   va_end(ap);
}

void fe_log(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   emit("LOG ", fmt, ap);
   va_end(ap);
}
