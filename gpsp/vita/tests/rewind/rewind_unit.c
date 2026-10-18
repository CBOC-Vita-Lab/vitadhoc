/* rewind_unit.c -- tests/rewind: vita/rewind_vita.c against a machine whose
 * every moment is known.
 *
 * The real file, compiled as it is, with a stand-in for everything it calls:
 * fe_host is a "machine" of a few tens of kilobytes that changes some of
 * its bytes every frame and, now and then, writes its "battery" -- all of it
 * a pure function of the machine's own state, so frame k of its life is the
 * same bytes however often it is lived.  A straight run records a hash of
 * (machine, battery) per frame; after that, whatever the test does -- play,
 * rewind for a while, play on, clear, lock, change the settings, change the
 * game -- the machine must at every moment hash to the record of the frame
 * its own counter says it is at.  A state restored one byte wrong, a
 * battery left behind, a ring that handed out a place twice: each shows as
 * a mismatch at once.
 *
 *   rewind_unit [seed] [iterations] [async]     async = 1: the squeeze runs
 *                                               on a second thread, as on
 *                                               the console
 *
 * Also here: the codec against random and adversarial inputs, and its
 * speed (rewind_unit bench FILE... : pairs of state dumps).
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rewind_vita.h"
#include "app_vita.h"

/* ---- the machine ------------------------------------------------------------- */

#define ST_MAX   (96 * 1024)
#define BAT_MAX  8192
#define REF_MAX  400000

static struct
{
   uint32_t frame, rng;
   uint8_t  ram[ST_MAX];
} M;
static uint8_t  BAT[BAT_MAX];
static uint32_t bat_gen;
static size_t   st_len = 64 * 1024;      /* this "game"'s state size        */
static size_t   bat_len = BAT_MAX;       /* 0 = a GB game: no battery image  */
static unsigned churn = 300;             /* bytes rewritten a frame          */
static unsigned save_every = 97;         /* frames between "in-game saves"   */
static int      session;                 /* a link session owns the machine  */
static unsigned frames_run;

static uint64_t ref[REF_MAX];            /* hash of frame k, straight run    */
static unsigned ref_n;

/* Eight bytes at a time: every byte and its place count, and that is all
 * this needs of a hash (it runs once per frame of every game). */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
   const uint8_t *b = (const uint8_t *)p;
   size_t i;
   for (i = 0; i + 8 <= n; i += 8)
   {
      uint64_t v;
      memcpy(&v, b + i, 8);
      h = (h ^ v) * 0x9E3779B97F4A7C15ull;
      h ^= h >> 29;
   }
   for (; i < n; i++)
      h = (h ^ b[i]) * 1099511628211ull;
   return h;
}

static uint64_t machine_hash(void)
{
   uint64_t h = fnv(14695981039346656037ull, &M.frame, 8);
   h = fnv(h, M.ram, st_len - 8);
   return fnv(h, BAT, BAT_MAX);
}

static void machine_reset(uint32_t seed)
{
   memset(&M, 0, sizeof(M));
   memset(BAT, 0xFF, sizeof(BAT));
   M.rng = seed | 1;
   bat_gen = 0;
}

static void machine_step(void)
{
   unsigned i;
   size_t ram = st_len - 8;
   M.frame++;
   for (i = 0; i < churn; i++)
   {
      M.rng = M.rng * 1664525u + 1013904223u;
      M.ram[(M.rng >> 8) % ram] = (uint8_t)(M.rng >> 24);
   }
   /* a burst now and then: a new level's worth of memory in one frame */
   if ((M.frame % 1013u) == 0)
      for (i = 0; i < ram / 2; i++)
      {
         M.rng = M.rng * 1664525u + 1013904223u;
         M.ram[i] = (uint8_t)(M.rng >> 24);
      }
   /* (a game with no battery image -- GB: the cartridge RAM is inside the
    * state -- writes none) */
   if (bat_len && save_every && (M.frame % save_every) == 0)
   {
      unsigned at = (M.frame / save_every * 64u) % BAT_MAX;
      for (i = 0; i < 64; i++)
         BAT[(at + i) % BAT_MAX] = (uint8_t)((M.rng >> 16) + i);
      bat_gen++;
   }
   /* ...and one that writes the same bytes again: the count moves, the
    * image does not */
   if (bat_len && save_every && (M.frame % (save_every * 7u)) == 3)
      bat_gen++;
}

/* ---- fe_host, as rewind_vita.c sees it ----------------------------------------- */

size_t fe_host_state_mem_size(void) { return session ? 0 : st_len; }

long fe_host_state_mem_save(void *buf, size_t cap)
{
   if (session || cap < st_len)
      return -1;
   memcpy(buf, &M, st_len);
   return (long)st_len;
}

int fe_host_state_mem_load(const void *buf, size_t n)
{
   if (session || n != st_len)
      return -1;
   memcpy(&M, buf, st_len);
   return 0;
}

void *fe_host_battery_mem(size_t *size)
{
   if (!bat_len || session)
      return NULL;
   *size = bat_len;
   return BAT;
}

uint32_t fe_host_battery_gen(void) { return bat_len ? bat_gen : 0; }
void fe_host_battery_changed(void) { bat_gen++; }
unsigned fe_host_frame_count(void) { return frames_run; }

static int in_landing, landings_seen, fail;

void fe_host_run_frame(void)
{
   machine_step();
   frames_run++;
}

/* ---- the rest of the app --------------------------------------------------------- */

uint32_t g_app_keys;
int      g_app_drew;
static uint16_t a_frame[4];
static int cheats_applied, flushes, redraws;
static char chip[64];
static uint64_t fake_us;
static int verbose;

const uint16_t *app_last_frame(unsigned *w, unsigned *h, size_t *pitch)
{
   *w = 2; *h = 2; *pitch = 4;
   return a_frame;
}
void app_audio_flush(void) { flushes++; }
void app_pace_reset(void) { }
void cheats_apply(void) { cheats_applied++; in_landing = 1; }
void osd_chip_ff(const char *t) { snprintf(chip, sizeof(chip), "%s", t ? t : ""); }
void vid_draw_frame(const uint16_t *p, unsigned w, unsigned h, size_t pitch)
{
   (void)p; (void)w; (void)h; (void)pitch;
   redraws++;
}
uint64_t plat_time_us(void)
{
   return __atomic_add_fetch(&fake_us, 7, __ATOMIC_RELAXED);
}
void plat_sleep_us(uint32_t us) { usleep(us); }
size_t plat_heap_free(void) { return (size_t)-1; }
void plat_thread_yield(void) { usleep(20); }
void plat_frame_ran(void) { landings_seen++; }
void plat_power_tick(void) { }
void fe_evt(const char *fmt, ...)
{
   if (verbose)
   {
      va_list ap;
      va_start(ap, fmt);
      vfprintf(stderr, fmt, ap);
      va_end(ap);
      fputc('\n', stderr);
   }
}

/* ---- the worker thread (async) ----------------------------------------------------- */

static pthread_t       wk;
static pthread_mutex_t wk_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wk_cv = PTHREAD_COND_INITIALIZER;
static int wk_run, wk_kick, wk_slow;

static void *wk_main(void *arg)
{
   (void)arg;
   pthread_mutex_lock(&wk_mu);
   while (wk_run)
   {
      while (wk_run && !wk_kick)
         pthread_cond_wait(&wk_cv, &wk_mu);
      wk_kick = 0;
      pthread_mutex_unlock(&wk_mu);
      {
         int slow = __atomic_load_n(&wk_slow, __ATOMIC_RELAXED);
         if (slow)
            usleep((useconds_t)slow);   /* a save write that takes its time */
      }
      rewind_worker();
      pthread_mutex_lock(&wk_mu);
   }
   pthread_mutex_unlock(&wk_mu);
   rewind_worker();
   return NULL;
}

static void wk_wake(void)
{
   pthread_mutex_lock(&wk_mu);
   wk_kick = 1;
   pthread_cond_signal(&wk_cv);
   pthread_mutex_unlock(&wk_mu);
}

static void wk_start(void)
{
   wk_run = 1;
   pthread_create(&wk, NULL, wk_main, NULL);
   rewind_set_async(wk_wake);
}

static void wk_stop(void)
{
   pthread_mutex_lock(&wk_mu);
   wk_run = 0;
   pthread_cond_signal(&wk_cv);
   pthread_mutex_unlock(&wk_mu);
   pthread_join(wk, NULL);
   rewind_set_async(NULL);
}

/* ---- checks -------------------------------------------------------------------------- */

#define CHECK(c, ...) do { if (!(c)) { fail++; if (fail < 20) { \
   fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)

static uint32_t trng = 12345;
static unsigned tr(unsigned n)
{
   trng ^= trng << 13; trng ^= trng >> 17; trng ^= trng << 5;
   return n ? trng % n : 0;
}

static void machine_check(const char *when)
{
   if (M.frame >= ref_n)
      return;
   CHECK(machine_hash() == ref[M.frame], "%s: the machine at frame %u is "
         "not frame %u of a straight run", when, M.frame, M.frame);
}

/* ---- the codec ------------------------------------------------------------------------ */

static void codec_case(const uint8_t *a, const uint8_t *b, size_t n,
                       const char *what)
{
   size_t cap = RW_BOUND(n);
   uint8_t *enc = (uint8_t *)malloc(cap + 1), *w = (uint8_t *)malloc(n + 1);
   long len = rw_encode(a, b, n, enc, cap);
   CHECK(len >= 0 && (size_t)len <= cap, "%s: encode %ld of bound %zu", what,
         len, cap);
   if (len >= 0)
   {
      memcpy(w, a, n);
      w[n] = 0x5A;
      CHECK(rw_apply(w, n, enc, (size_t)len) == 0 && !memcmp(w, b, n) &&
            w[n] == 0x5A, "%s: a -> b", what);
      CHECK(rw_apply(w, n, enc, (size_t)len) == 0 && !memcmp(w, a, n),
            "%s: b -> a", what);
      /* a buffer one byte too small is refused, never overrun */
      if (len > 0)
      {
         uint8_t *tight = (uint8_t *)malloc((size_t)len);
         long l2 = rw_encode(a, b, n, tight, (size_t)len - 1);
         CHECK(l2 == -1, "%s: cap-1 gave %ld", what, l2);
         free(tight);
      }
      /* a truncated or corrupted stream is refused or stays in bounds */
      if (len > 2)
      {
         size_t k;
         for (k = 0; k < 8; k++)
         {
            size_t cut = tr((unsigned)len);
            memcpy(w, a, n);
            (void)rw_apply(w, n, enc, cut);
            CHECK(w[n] == 0x5A, "%s: truncated stream wrote past the end",
                  what);
            enc[tr((unsigned)len)] ^= (uint8_t)(1u << tr(8));
            memcpy(w, a, n);
            (void)rw_apply(w, n, enc, (size_t)len);
            CHECK(w[n] == 0x5A, "%s: damaged stream wrote past the end", what);
         }
      }
   }
   free(enc);
   free(w);
}

static void codec_tests(void)
{
   enum { N = 70000 };
   uint8_t *a = (uint8_t *)malloc(N), *b = (uint8_t *)malloc(N);
   size_t i, n;
   int round;

   for (i = 0; i < N; i++)
      a[i] = (uint8_t)tr(256);
   memcpy(b, a, N);
   codec_case(a, b, N, "equal");
   CHECK(rw_encode(a, b, N, b, 0) == 0, "equal buffers encode to nothing");
   codec_case(a, b, 0, "empty");
   for (i = 0; i < N; i++)
      b[i] = (uint8_t)~a[i];
   codec_case(a, b, N, "all different");
   /* the dearest pattern: one changed byte, RW_GAP equal ones, repeat */
   memcpy(b, a, N);
   for (i = 0; i < N; i += 4)
      b[i] ^= 1;
   codec_case(a, b, N, "1 in 4");
   memcpy(b, a, N);
   for (i = 0; i < N; i += 3)
      b[i] ^= 0x80;
   codec_case(a, b, N, "1 in 3");
   memcpy(b, a, N);
   b[0] ^= 1;
   b[N - 1] ^= 1;
   codec_case(a, b, N, "first and last");
   for (round = 0; round < 300; round++)
   {
      unsigned runs = 1 + tr(40), r;
      n = 1 + tr(N - 1);
      memcpy(b, a, N);
      for (r = 0; r < runs; r++)
      {
         size_t at = tr((unsigned)n), len = 1 + tr(1 + tr(3000));
         for (i = at; i < at + len && i < n; i++)
            if (tr(8))
               b[i] ^= (uint8_t)(1 + tr(255));
      }
      codec_case(a, b, n, "random");
   }
   free(a);
   free(b);
}

/* ---- speed: rewind_unit bench A B [A B ...] --------------------------------------------- */

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint8_t *slurp(const char *path, size_t *n)
{
   FILE *f = fopen(path, "rb");
   uint8_t *p;
   long sz;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   p = (uint8_t *)malloc((size_t)sz + 1);
   *n = fread(p, 1, (size_t)sz, f);
   fclose(f);
   return p;
}

static int bench(int argc, char **argv)
{
   int i;
   for (i = 2; i + 1 < argc; i += 2)
   {
      size_t na, nb, cap;
      uint8_t *a = slurp(argv[i], &na), *b = slurp(argv[i + 1], &nb), *enc;
      long len = 0;
      int k, reps = 200;
      double t0, te, ta;
      if (!a || !b || na != nb)
      {
         fprintf(stderr, "bench: %s / %s unreadable or unequal\n", argv[i],
                 argv[i + 1]);
         return 1;
      }
      cap = RW_BOUND(na);
      enc = (uint8_t *)malloc(cap);
      t0 = now_s();
      for (k = 0; k < reps; k++)
         len = rw_encode(a, b, na, enc, cap);
      te = (now_s() - t0) / reps;
      t0 = now_s();
      for (k = 0; k < reps; k++)
         rw_apply(a, na, enc, (size_t)len);
      ta = (now_s() - t0) / reps;
      printf("bench %s: %zu -> %ld bytes (%.2f %%), encode %.0f us, apply "
             "%.0f us\n", argv[i + 1], na, len, 100.0 * len / na, te * 1e6,
             ta * 1e6);
      free(a); free(b); free(enc);
   }
   return 0;
}

/* ---- the long random session ------------------------------------------------------------- */

int main(int argc, char **argv)
{
   unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 1;
   unsigned iters = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 300000;
   int async = argc > 3 ? atoi(argv[3]) : 0;
   unsigned it, games = 0, rewinds = 0, oldest_hits = 0, max_back = 0;
   unsigned phase_left = 0, mode = 0;       /* 0 play, 1 rewind */
   int locked = 0, budget = REWIND_MB_MIN, seconds = 30, want_was = 0;
   int hurry = 0;
   rewind_stats st;

   if (argc > 1 && !strcmp(argv[1], "bench"))
      return bench(argc, argv);
   verbose = getenv("RW_VERBOSE") != NULL;
   trng = seed * 2654435761u + 1;

   codec_tests();
   printf("codec: %d failures\n", fail);

   /* the record: one straight life of the machine, per game shape */
   for (;;)
   {
      unsigned k, played, checks = 0;

      /* a new game: a GBA-like one with a battery, or a small one without */
      st_len = (games & 1) ? 24 * 1024 + 8 * tr(1000) : 64 * 1024 + 8 * tr(4000);
      bat_len = (games & 1) ? 0 : BAT_MAX;
      churn = 20 + tr(1 + tr(3000));
      save_every = 50 + tr(300);
      {
         uint32_t s0 = tr(0x7fffffff);
         machine_reset(s0);
         ref_n = REF_MAX < iters ? REF_MAX : iters;
         for (k = 0; k < ref_n; k++)
         {
            ref[k] = machine_hash();
            machine_step();
         }
         machine_reset(s0);
      }
      seconds = (int[]){ 30, 60, 180, 300 }[tr(4)];
      budget = REWIND_MB_MIN + (int)tr(3);
      rewind_configure(seconds, budget);
      if (async)
         wk_start();
      rewind_game_begin();
      rewind_get_stats(&st);
      CHECK(st.on, "rewind is on for game %u", games);
      CHECK(st.total_bytes <= ((unsigned)budget << 20), "game %u: %u bytes "
            "allocated of a %d MiB budget", games, st.total_bytes, budget);
      mode = 0;
      phase_left = 1 + tr(2000);
      locked = 0;
      played = 0;

      for (it = 0; it < iters / 4 && M.frame + 2 < ref_n; it++)
      {
         int want, rc;
         unsigned frame_before = M.frame;

         if (!phase_left--)
         {
            unsigned r = tr(100);
            mode = !mode;
            phase_left = mode ? 1 + tr(1 + tr(900)) : 1 + tr(1 + tr(6000));
            if (!mode && r < 4)
               rewind_clear();                    /* "a state was loaded" */
            else if (!mode && r < 8)
            {
               locked = 1;                         /* a link session begins */
               session = tr(2);                    /* ...which may own it   */
            }
            else if (!mode && r < 12)
            {
               seconds = (int[]){ 0, 30, 60, 180, 300 }[tr(5)];
               budget = REWIND_MB_MIN + (int)tr(3);
               rewind_configure(seconds, budget);
            }
            else if (r < 30)
               locked = session = 0;
            if (async)
            {
               __atomic_store_n(&wk_slow, tr(4) ? 0 : (int)tr(300),
                                __ATOMIC_RELAXED);
               hurry = !tr(4);
            }
         }

         /* (a setting changed or a state "loaded" in the middle of a
          * rewind still held is part of the test: one time in four) */
         want = mode == 1 || (phase_left > 3 && !tr(4) && it > 0 && want_was);
         want_was = mode == 1;
         in_landing = 0;
         rc = rewind_frame(want, locked);
         (void)rewind_notice();
         if (rc)
         {
            CHECK(!locked, "rewinding while locked");
            CHECK(M.frame <= frame_before + 1, "a step went FORWARD: %u -> %u",
                  frame_before, M.frame);
            CHECK(rewind_active(), "owns the iteration but is not active");
            CHECK(chip[0], "no chip while rewinding");
            machine_check("after a step back");
            checks++;
            if (frame_before > M.frame && frame_before - M.frame > max_back)
               max_back = frame_before - M.frame;
            if (strstr(chip, "oldest"))
               oldest_hits++;
            continue;
         }
         CHECK(!rewind_active(), "active but does not own the iteration");
         CHECK(!chip[0] , "the chip is still up: %s", chip);
         if (want)
            rewinds++;
         /* an ordinary frame */
         fe_host_run_frame();
         rewind_capture();
         played++;
         /* The console's frame takes sixteen milliseconds and the worker
          * keeps up; here a frame takes none.  So most of the time wait
          * for the worker (reading the numbers does), and a quarter of
          * the time do not: then states are skipped, which is the other
          * thing to test. */
         if (async && !hurry)
            rewind_get_stats(&st);
         if ((it % 257) == 0)
         {
            machine_check("in play");
            rewind_get_stats(&st);
            CHECK(st.used_bytes <= st.ring_bytes, "ring: %u used of %u",
                  st.used_bytes, st.ring_bytes);
            CHECK(!st.on || st.total_bytes <= ((unsigned)budget << 20),
                  "%u bytes allocated of a %d MiB budget", st.total_bytes,
                  budget);
            CHECK(st.interval >= REWIND_IV_MIN_SMALL &&
                  st.interval <= REWIND_IV_MAX, "interval %u", st.interval);
         }
      }
      rewind_get_stats(&st);
      if (verbose || games < 2)
         printf("game %u: state=%zu battery=%zu churn=%u seconds=%d "
                "budget=%d MiB: played %u, %u step checks, interval %u, "
                "%u steps (%u frames) held in %u bytes, dropped %u, "
                "skipped %u, ratio %.1f %%\n", games, st_len, bat_len, churn,
                seconds, budget, played, checks, st.interval, st.entries,
                st.frames, st.used_bytes, st.dropped, st.skipped,
                st.raw_bytes ? 100.0 * st.packed_bytes / st.raw_bytes : 0.0);
      rewind_game_end();
      if (async)
         wk_stop();
      session = 0;
      games++;
      if (games * (iters / 4) >= iters)
         break;
   }
   printf("%s: seed %u, %u games, %u rewinds, deepest step %u frames, %u "
          "refreshes at the oldest point, %d landings, %d failures\n",
          async ? "async" : "sync", seed, games, rewinds, max_back,
          oldest_hits, landings_seen, fail);
   return fail ? 1 : 0;
}
