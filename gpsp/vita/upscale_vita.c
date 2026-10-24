/* upscale_vita.c -- see upscale_vita.h. */
#include "upscale_vita.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pspcompat.h"          /* pspcompat_time_us */

#ifdef __vita__
#include <psp2/kernel/clib.h>
/* newlib's memmove is a plain word loop with no prefetch (a slid 4x picture,
 * 2.4 MB, took it 10-15 ms on a console); the system's own is not. */
#define UP_MEMMOVE sceClibMemmove
#else
#define UP_MEMMOVE memmove
#endif

void xbrz_c_init(void);
void xbrz_c_dist_mode(int mode);
void xbrz_c_scale(int factor, const uint32_t *src, uint32_t *dst,
                  int w, int h, int y0, int y1);
void xbrz_c_scale_rect(int factor, const uint32_t *src, uint32_t *dst,
                       int w, int h, int y0, int y1, int x0, int x1);
void xbrz_c_scale_rect_to(int factor, const uint32_t *src, int w, int h,
                          int y0, int y1, int x0, int x1, uint32_t *at,
                          int pitch);

/* The most output pixels a picture may have: the wide GBA picture
 * (282x160, gpsp/wide.h) at 4x. */
#define UP_BUDGET_PX 724000u
/* Helper threads.  A console has three cores for an application: the
 * emulator's and these two. */
#define UP_HELPERS 2
/* A picture is cut into stripes of this many source rows... */
#define UP_STRIPE 8
#define UP_MAX_H 256
#define UP_MAX_STRIPES (UP_MAX_H / UP_STRIPE)
/* ...and a pixel is scaled again when it, or one within this many of it in
 * any direction, changed: xBRZ looks two pixels each way (a pixel's corners
 * are decided by its neighbours' 4x4 kernels), Scale3x one. */
#define UP_MARGIN 3
/* A picture that took longer than this did not make the next refresh. */
#define UP_LATE_US 15000u
/* This many late pictures in a row: one factor less.  This many pictures
 * that would have fitted at the larger factor: back up (doubled each time
 * it has to step down again, so a game that sits on the edge settles). */
#define UP_LATE_RUN 20
#define UP_FAST_RUN 600
/* A FADE (or a flash, a battle's wipe, a new map drawn whole): a picture
 * that is mostly new and did not just scroll costs a whole redraw -- 20 to
 * 33 ms at 4x on the two helpers (measured on a console), so every other
 * smoothed picture of a fade was dropped, and after twenty of them in a
 * row the slot stepped down for time and up again later, each time with
 * new textures made on the emulator's thread (12 to 30 ms: frames lost).
 * Such pictures are scaled smaller at once (upscale_fade_drop: 2x, the
 * default; 3x was measured still late a third of the time), and the slot
 * stays there until this many pictures in a row were not mostly new; the
 * first one at 4x again is a whole redraw (one picture late, once).  The
 * textures stay the 4x ones throughout. */
#define UP_HOT_HOLD 30

/* ---- 565 -> RGBA8 --------------------------------------------------------- */

static uint32_t g_lut[2][65536];       /* [bgr][565] */
static int g_lut_ready;

static void lut_build(void)
{
   unsigned v;
   for (v = 0; v < 65536; v++)
   {
      unsigned hi = (v >> 11) & 31, g6 = (v >> 5) & 63, lo = v & 31;
      unsigned h8 = (hi << 3) | (hi >> 2), g8 = (g6 << 2) | (g6 >> 4);
      unsigned l8 = (lo << 3) | (lo >> 2);
      /* libretro: red is the high field; PSP order: red is the low one. */
      g_lut[0][v] = 0xFF000000u | (l8 << 16) | (g8 << 8) | h8;
      g_lut[1][v] = 0xFF000000u | (h8 << 16) | (g8 << 8) | l8;
   }
   xbrz_c_init();
   g_lut_ready = 1;
}

/* Scale2x / Scale3x, rows [y0, y1) and columns [x0, x1) of a tightly packed
 * w x h 565 picture; source pixel (x0, y0)'s block is written at `at`, rows
 * `dp` pixels apart.  Edges repeat the border pixel. */
static void epx_rows(const uint16_t *src, int w, int h, int f, int bgr,
                     uint32_t *at, size_t dp, int y0, int y1, int x0, int x1)
{
   const uint32_t *lut = g_lut[bgr];
   int x, y;
   for (y = y0; y < y1; y++)
   {
      const uint16_t *rm = src + (size_t)w * (size_t)y;
      const uint16_t *ru = y > 0 ? rm - w : rm;
      const uint16_t *rd = y < h - 1 ? rm + w : rm;
      uint32_t *o = at + dp * (size_t)f * (size_t)(y - y0);
      for (x = x0; x < x1; x++)
      {
         const int xl = x > 0 ? x - 1 : x, xr = x < w - 1 ? x + 1 : x;
         const uint16_t B = ru[x], D = rm[xl], E = rm[x], F = rm[xr],
                        H = rd[x];
         if (f == 2)
         {
            uint16_t e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F)
            {
               if (D == B) e0 = D;
               if (B == F) e1 = F;
               if (D == H) e2 = D;
               if (H == F) e3 = F;
            }
            o[0] = lut[e0]; o[1] = lut[e1];
            o[dp] = lut[e2]; o[dp + 1] = lut[e3];
            o += 2;
         }
         else
         {
            const uint16_t A = ru[xl], C = ru[xr], G = rd[xl], I = rd[xr];
            uint16_t e[9];
            int i;
            for (i = 0; i < 9; i++)
               e[i] = E;
            if (B != H && D != F)
            {
               if (D == B) e[0] = D;
               if ((D == B && E != C) || (B == F && E != A)) e[1] = B;
               if (B == F) e[2] = F;
               if ((D == B && E != G) || (D == H && E != A)) e[3] = D;
               if ((B == F && E != I) || (H == F && E != C)) e[5] = F;
               if (D == H) e[6] = D;
               if ((D == H && E != I) || (H == F && E != G)) e[7] = H;
               if (H == F) e[8] = F;
            }
            o[0] = lut[e[0]]; o[1] = lut[e[1]]; o[2] = lut[e[2]];
            o[dp] = lut[e[3]]; o[dp + 1] = lut[e[4]]; o[dp + 2] = lut[e[5]];
            o[2 * dp] = lut[e[6]]; o[2 * dp + 1] = lut[e[7]];
            o[2 * dp + 2] = lut[e[8]];
            o += 3;
         }
      }
   }
}

/* ---- columns of a stripe --------------------------------------------------- */

#define UP_MAX_W 320
#define UP_WORDS (UP_MAX_W / 32)

typedef struct { uint32_t b[UP_WORDS]; } colmask;

static void cm_set_all(colmask *m, int w)
{
   int i;
   for (i = 0; i < UP_WORDS; i++)
   {
      int lo = i * 32;
      m->b[i] = w >= lo + 32 ? 0xFFFFFFFFu
              : w > lo ? (1u << (w - lo)) - 1u : 0u;
   }
}

static int cm_any(const colmask *m)
{
   int i;
   for (i = 0; i < UP_WORDS; i++)
      if (m->b[i])
         return 1;
   return 0;
}

static void cm_or(colmask *m, const colmask *o)
{
   int i;
   for (i = 0; i < UP_WORDS; i++)
      m->b[i] |= o->b[i];
}

static void cm_set_range(colmask *m, int x0, int x1)
{
   int x;
   for (x = x0; x < x1; x++)
      m->b[x >> 5] |= 1u << (x & 31);
}

/* Every set column and `r` columns either side of it, within w. */
static void cm_dilate(colmask *m, int r, int w)
{
   colmask valid;
   int k, i;
   for (k = 0; k < r; k++)
   {
      uint32_t carry_up = 0, down[UP_WORDS];
      for (i = 0; i < UP_WORDS; i++)
         down[i] = (m->b[i] >> 1) |
                   (i + 1 < UP_WORDS ? m->b[i + 1] << 31 : 0u);
      for (i = 0; i < UP_WORDS; i++)
      {
         uint32_t v = m->b[i];
         m->b[i] = v | (v << 1) | carry_up | down[i];
         carry_up = v >> 31;
      }
   }
   cm_set_all(&valid, w);
   for (i = 0; i < UP_WORDS; i++)
      m->b[i] &= valid.b[i];
}

/* The next run of set columns at or after x: 1 and [*x0, *x1), or 0.  Runs
 * closer than `join` columns are given as one (a run costs a column of
 * preparation and its own first row). */
static int cm_run(const colmask *m, int x, int w, int join, int *x0, int *x1)
{
#define CM_BIT(m, x) (((m)->b[(x) >> 5] >> ((x) & 31)) & 1u)
   int gap;
   while (x < w && !CM_BIT(m, x))
   {
      /* a whole empty word at a time */
      if (!(x & 31) && !m->b[x >> 5])
         x += 32;
      else
         x++;
   }
   if (x >= w)
      return 0;
   *x0 = x;
   for (;;)
   {
      while (x < w && CM_BIT(m, x))
         x++;
      *x1 = x;
      for (gap = 0; x < w && gap < join && !CM_BIT(m, x); gap++)
         x++;
      if (x >= w || !CM_BIT(m, x))
         return 1;
   }
#undef CM_BIT
}

/* ---- a slot: one picture's standing data and its job --------------------- */

enum { ST_IDLE = 0, ST_BUSY, ST_DONE };

typedef struct
{
   /* What `big` holds a scaled copy of. */
   int kind, f, bgr;
   unsigned w, h;
   uint16_t *cur, *old;        /* the last picture and the one before it,
                                * w x h, tightly packed                    */
   uint32_t *src32;            /* the last picture expanded, for xBRZ      */
   uint32_t *big;              /* (f*w) x (f*h), a ring (below)            */
   size_t cap_src, cap_big;    /* pixels allocated                        */
   /* THE RING.  The picture's row y, column x is held at row (roy + y) mod
    * h, column (rox + x) mod w of `big` -- and of both textures, which the
    * renderer draws turned back (upscale_ring).  A picture that is the last
    * one scrolled turns the ring and nothing moves: only what came into
    * view is scaled and written. */
   int rox, roy;
   /* Per ring row, the ring columns of `big` that each texture does not
    * have yet (a texture is written every other picture). */
   colmask pend[2][UP_MAX_H];
   unsigned char *out[2];
   size_t out_pitch;
   int last_dx, last_dy;       /* how the picture last scrolled            */

   /* The job (written by upscale_submit while nobody looks at the slot). */
   colmask dirty[UP_MAX_STRIPES];         /* columns to scale again       */
   int slid;                   /* the picture is the last one, scrolled   */
   int target, nstripes;
   unsigned t0;
   int next, done;             /* atomics: stripes claimed, finished      */
   int state;                  /* atomic: ST_*                            */
   int inside;                 /* atomic: threads looking at the job      */
   int scaled_px;              /* atomic: source pixels this job scaled   */

   /* The factor's history. */
   int late_run, fast_run, fast_need, step_down;
   int hot;                    /* pictures to go at the fade's factor      */
} up_slot;

static up_slot g_slot[UPSCALE_SLOTS];

#define A_LOAD(x)      __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define A_STORE(x, v)  __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define A_ADD(x, v)    __atomic_fetch_add(&(x), (v), __ATOMIC_SEQ_CST)

/* Statistics (atomics): written by whoever finishes a job. */
static struct
{
   int jobs, sum_us, max_us, dropped, stripes, helped, factor, shifted;
   int px_k, scaled_k;         /* thousands of source pixels: seen, scaled */
   int prep_us;                /* the caller's own time in upscale_submit  */
   int job_us[2];              /* the stripes' time: scaling, writing the
                                * texture (all threads)                   */
   int late_f[3];              /* ...of them at 2x, 3x, 4x                */
   int stepped;                /* times a slot stepped down for time      */
   int late, late_slid, late_pct;  /* pictures over a frame: how many, of
                                    * them slid, their scaled share summed */
} g_st;

/* ...and where it went (the caller's thread only): the copy, the
 * comparison, finding the slide, moving buffers, marking the columns,
 * expanding them and the stripes' masks. */
enum { PP_COPY, PP_CMP, PP_FIND, PP_MOVE, PP_MARK, PP_LUT, PP_N };
static unsigned g_prep[PP_N];

/* How a scrolled picture is followed (upscale_slide_mode). */
static int g_slide_mode = 2;

/* How many factors a fade's pictures go down (upscale_fade_drop). */
static int g_fade_drop = 2;

static int g_async;            /* the caller does not wait for a picture */

/* Ring row / column of the picture's row y / column x (y < h, x < w). */
static int ring_y(const up_slot *S, int y)
{
   const int r = S->roy + y;
   return r >= (int)S->h ? r - (int)S->h : r;
}

static int ring_x(const up_slot *S, int x)
{
   const int r = S->rox + x;
   return r >= (int)S->w ? r - (int)S->w : r;
}

/* Rows [y0, y1), columns [x0, x1) of the picture, none across the ring's
 * seam: scaled into their place in the ring, and both textures lack them. */
static void scale_piece(up_slot *S, int y0, int y1, int x0, int x1)
{
   const int f = S->f, W = f * (int)S->w;
   const int rx0 = ring_x(S, x0), rx1 = rx0 + (x1 - x0);
   uint32_t *at = S->big + (size_t)ring_y(S, y0) * (size_t)f * (size_t)W +
                  (size_t)rx0 * (size_t)f;
   int y;
   if (S->kind == UPSCALE_XBRZ)
      xbrz_c_scale_rect_to(f, S->src32, (int)S->w, (int)S->h, y0, y1, x0, x1,
                           at, W);
   else
      epx_rows(S->cur, (int)S->w, (int)S->h, f, S->bgr, at, (size_t)W, y0, y1,
               x0, x1);
   for (y = y0; y < y1; y++)
   {
      const int r = ring_y(S, y);
      cm_set_range(&S->pend[0][r], rx0, rx1);
      cm_set_range(&S->pend[1][r], rx0, rx1);
   }
}

static void stripe_run(up_slot *S, int i)
{
   const int y0 = i * UP_STRIPE;
   const int y1 = y0 + UP_STRIPE < (int)S->h ? y0 + UP_STRIPE : (int)S->h;
   const int w = (int)S->w, f = S->f;
   /* where the ring's seam falls in the picture */
   const int ys = (int)S->h - S->roy, xs = w - S->rox;
   int x = 0, x0, x1, px = 0, y;
   unsigned t1 = pspcompat_time_us(), t2;

   /* Scale what changed, in up to four pieces each side of the seam... */
   while (cm_run(&S->dirty[i], x, w, 6, &x0, &x1))
   {
      int ya, yb, xa, xb;
      for (ya = y0; ya < y1; ya = yb)
      {
         yb = ya < ys && ys < y1 ? ys : y1;
         for (xa = x0; xa < x1; xa = xb)
         {
            xb = xa < xs && xs < x1 ? xs : x1;
            scale_piece(S, ya, yb, xa, xb);
         }
      }
      px += (x1 - x0) * (y1 - y0);
      x = x1;
   }
   if (px)
      A_ADD(S->scaled_px, px);
   t2 = pspcompat_time_us();

   /* ...and bring the texture that is not on screen up to date, this
    * stripe's rows of it (in the ring, each picture row is one ring row: no
    * other stripe touches them).  The GPU's memory is only ever written, in
    * whole runs of a row. */
   for (y = y0; y < y1; y++)
   {
      const int r = ring_y(S, y);
      colmask *pm = &S->pend[S->target][r];
      const size_t bigrow = (size_t)f * S->w * sizeof(uint32_t);
      unsigned char *o = S->out[S->target];
      if (!cm_any(pm))
         continue;
      x = 0;
      while (cm_run(pm, x, w, 16, &x0, &x1))
      {
         const size_t off = (size_t)x0 * f * sizeof(uint32_t);
         const size_t n = (size_t)(x1 - x0) * f * sizeof(uint32_t);
         int Y;
         for (Y = r * f; Y < r * f + f; Y++)
            memcpy(o + S->out_pitch * (size_t)Y + off,
                   (const unsigned char *)S->big + bigrow * (size_t)Y + off,
                   n);
         x = x1;
      }
      memset(pm, 0, sizeof(colmask));
   }
   A_ADD(g_st.job_us[0], (int)(t2 - t1));
   A_ADD(g_st.job_us[1], (int)(pspcompat_time_us() - t2));
}

/* The last stripe of a job was finished, by this thread. */
static void job_finish(up_slot *S)
{
   const unsigned dt = pspcompat_time_us() - S->t0;
   const int scaled = A_LOAD(S->scaled_px);
   const int total = (int)(S->w * S->h);

   A_ADD(g_st.jobs, 1);
   A_ADD(g_st.sum_us, (int)dt);
   if ((int)dt > A_LOAD(g_st.max_us))
      A_STORE(g_st.max_us, (int)dt);
   A_ADD(g_st.stripes, S->nstripes);
   A_ADD(g_st.px_k, (total + 500) / 1000);
   A_ADD(g_st.scaled_k, (scaled + 500) / 1000);
   A_STORE(g_st.factor, S->f);
   if (dt > 16683u)
   {
      A_ADD(g_st.late, 1);
      A_ADD(g_st.late_slid, S->slid ? 1 : 0);
      if (S->f >= 2 && S->f <= 4)
         A_ADD(g_st.late_f[S->f - 2], 1);
      A_ADD(g_st.late_pct, total ? (int)((long long)scaled * 100 / total) : 0);
   }

   /* The factor follows what the cores can do -- only where a late picture
    * is a dropped one (a host twin waits, and must draw the same picture
    * whatever the machine it runs on), and only on pictures that were
    * mostly new (a still one says nothing). */
   if (g_async && scaled * 2 >= total)
   {
      if (!A_LOAD(S->step_down))
      {
         S->late_run = dt > UP_LATE_US ? S->late_run + 1 : 0;
         if (S->late_run >= UP_LATE_RUN && S->f > 2)
         {
            A_STORE(S->step_down, 1);
            A_ADD(g_st.stepped, 1);
            S->late_run = S->fast_run = 0;
            S->fast_need = S->fast_need ? S->fast_need * 2 : UP_FAST_RUN;
         }
      }
      else
      {
         /* One factor up is (f+1)^2 / f^2 of the output; the analysis of
          * the source is the same.  Call it twice the time. */
         S->fast_run = dt * 2u < UP_LATE_US ? S->fast_run + 1 : 0;
         if (S->fast_run >= S->fast_need)
         {
            A_STORE(S->step_down, 0);
            S->late_run = S->fast_run = 0;
         }
      }
   }
   A_STORE(S->state, ST_DONE);
}

/* One stripe of slot `si`, if there is one to do.
 *
 * `inside` is how upscale_submit knows nobody is still looking at the job
 * before it writes the next one: a thread counts itself in BEFORE it reads
 * `state`, and submit -- which only runs when the state is not BUSY -- waits
 * for the count to be zero.  Whoever counts in after that reads a state
 * that is not BUSY and leaves without touching anything, until the new job
 * is whole and BUSY is stored last. */
static int claim_and_run(int si)
{
   up_slot *S = &g_slot[si];
   int i, ran = 0;
   A_ADD(S->inside, 1);
   if (A_LOAD(S->state) == ST_BUSY)
   {
      i = A_ADD(S->next, 1);
      if (i < S->nstripes)
      {
         stripe_run(S, i);
         if (A_ADD(S->done, 1) + 1 == S->nstripes)
            job_finish(S);
         ran = 1;
      }
   }
   A_ADD(S->inside, -1);
   return ran;
}

static void work_all(void)
{
   int si, did;
   do
   {
      did = 0;
      for (si = 0; si < UPSCALE_SLOTS; si++)
         while (claim_and_run(si))
            did = 1;
   } while (did);
}

/* ---- the helpers ----------------------------------------------------------- */

static int g_pool = -1;        /* -1 untried, else the helpers running */
static volatile int g_pool_quit;

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>

static SceUID g_th[UP_HELPERS], g_go[UP_HELPERS];

static int helper_entry(SceSize args, void *argp)
{
   int i = *(int *)argp;
   (void)args;
   for (;;)
   {
      sceKernelWaitSema(g_go[i], 1, NULL);
      if (g_pool_quit)
         break;
      work_all();
   }
   return 0;
}

static void pool_start(void)
{
   int i;
   g_pool = 0;
   g_async = 1;
   for (i = 0; i < UP_HELPERS; i++)
   {
      /* One wake-up pending is enough: a helper that is told twice finds
       * nothing the second time. */
      g_go[i] = sceKernelCreateSema("gbadhoc_up_go", 0, 0, 1, NULL);
      /* The main thread's priority, each on a core of its own: the two the
       * emulator's thread is least likely to be on. */
      g_th[i] = sceKernelCreateThread("gbadhoc_up", helper_entry, 0x10000100,
                                      0x10000, 0,
                                      i ? SCE_KERNEL_CPU_MASK_USER_2
                                        : SCE_KERNEL_CPU_MASK_USER_1, NULL);
      if (g_go[i] < 0 || g_th[i] < 0 ||
          sceKernelStartThread(g_th[i], sizeof(i), &i) < 0)
         break;                  /* fewer helpers than hoped */
      g_pool = i + 1;
   }
   /* With no helper at all the caller scales, and waits. */
   if (!g_pool)
      g_async = 0;
}

static void pool_go(void)
{
   int i;
   for (i = 0; i < g_pool; i++)
      sceKernelSignalSema(g_go[i], 1);   /* (fails when one is pending: fine) */
}

static void pool_stop(void)
{
   int i;
   g_pool_quit = 1;
   for (i = 0; i < g_pool; i++)
   {
      sceKernelSignalSema(g_go[i], 1);
      sceKernelWaitThreadEnd(g_th[i], NULL, NULL);
      sceKernelDeleteThread(g_th[i]);
      sceKernelDeleteSema(g_go[i]);
   }
}

static void nap(void) { sceKernelDelayThread(200); }

int upscale_thread_ids(int *ids, int max)
{
   int i, n = 0;
   for (i = 0; i < g_pool && n < max; i++)
      ids[n++] = (int)g_th[i];
   return n;
}

#else /* a host twin */
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>

static pthread_t g_th[UP_HELPERS];
static sem_t g_go[UP_HELPERS];
static int g_idx[UP_HELPERS];

static void *helper_entry(void *p)
{
   int i = *(int *)p;
   for (;;)
   {
      while (sem_wait(&g_go[i]) != 0)
         ;
      if (g_pool_quit)
         break;
      work_all();
   }
   return NULL;
}

static void pool_start(void)
{
   int i;
   g_pool = 0;
   g_async = getenv("GBADHOC_UPSCALE_ASYNC") != NULL;
   if (getenv("GBADHOC_UPSCALE_SERIAL"))
   {
      g_async = 0;
      return;
   }
   for (i = 0; i < UP_HELPERS; i++)
   {
      g_idx[i] = i;
      sem_init(&g_go[i], 0, 0);
      if (pthread_create(&g_th[i], NULL, helper_entry, &g_idx[i]) != 0)
         break;
      g_pool = i + 1;
   }
   if (!g_pool)
      g_async = 0;
}

static void pool_go(void)
{
   int i;
   for (i = 0; i < g_pool; i++)
      sem_post(&g_go[i]);
}

static void pool_stop(void)
{
   int i;
   g_pool_quit = 1;
   for (i = 0; i < g_pool; i++)
   {
      sem_post(&g_go[i]);
      pthread_join(g_th[i], NULL);
      sem_destroy(&g_go[i]);
   }
}

static void nap(void) { sched_yield(); }

int upscale_thread_ids(int *ids, int max)
{
   (void)ids; (void)max;
   return 0;
}
#endif

/* ---- the interface --------------------------------------------------------- */

static int g_quality;

void upscale_quality(int quality)
{
   g_quality = quality >= 0 && quality <= 3 ? quality : 0;
}

int upscale_factor(int slot, int kind, unsigned w, unsigned h, int dw)
{
   int f, fmax = kind == UPSCALE_XBRZ ? 4 : kind == UPSCALE_SCALE ? 3 : 0;
   if (slot < 0 || slot >= UPSCALE_SLOTS || !fmax || !w || !h || dw <= 0 ||
       g_quality == 1)
      return 0;
   /* The whole factor AT OR ABOVE the size on screen (a twentieth of
    * tolerance): "fit" is 3.4 times, and a picture scaled four times and
    * drawn a little smaller is cleaner than one scaled three times and
    * stretched. */
   f = g_quality >= 2 ? g_quality
     : (dw * 20 + (int)w * 19 - (int)w) / ((int)w * 20);
   if (f > fmax)
      f = fmax;
   while (f > 2 && (unsigned)(f * f) * w * h > UP_BUDGET_PX)
      f--;
   return f < 2 ? 0 : f;
}

void upscale_fade_drop(int n)
{
   g_fade_drop = n < 0 ? 0 : n > 2 ? 2 : n;
}

int upscale_job_factor(int slot)
{
   return slot >= 0 && slot < UPSCALE_SLOTS ? g_slot[slot].f : 0;
}

int upscale_busy(int slot)
{
   return slot >= 0 && slot < UPSCALE_SLOTS &&
          A_LOAD(g_slot[slot].state) == ST_BUSY;
}

int upscale_collect(int slot)
{
   int want = ST_DONE;
   if (slot < 0 || slot >= UPSCALE_SLOTS)
      return 0;
   return __atomic_compare_exchange_n(&g_slot[slot].state, &want, ST_IDLE, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* The slot's job is over and nobody is looking at it any more. */
static void slot_wait(up_slot *S)
{
   while (A_LOAD(S->state) == ST_BUSY || A_LOAD(S->inside))
      nap();
}

void upscale_forget(int slot)
{
   up_slot *S;
   if (slot < 0 || slot >= UPSCALE_SLOTS)
      return;
   S = &g_slot[slot];
   slot_wait(S);
   A_STORE(S->state, ST_IDLE);
   S->kind = UPSCALE_NONE;       /* the next picture starts from nothing */
   S->out[0] = S->out[1] = NULL;
   S->late_run = S->fast_run = S->fast_need = 0;
   S->hot = 0;
   A_STORE(S->step_down, 0);
}

/* ---- a picture that scrolled ---------------------------------------------
 * Walking is the expensive case: every pixel differs from the frame before.
 * But the background only MOVED, and what xBRZ makes of a pixel depends on
 * nothing but the pixels around it -- so when the new picture is the old
 * one slid by (dx, dy), the scaled picture is the old one slid by f times
 * that -- the ring turns (up_slot) -- and only what is really new is scaled:
 * the edge that came into view, sprites that moved some other way, the
 * status bar that did not move at all.
 *
 * Finding the slide costs little: a grid of sample pixels is compared at
 * each candidate offset; the offset of the frame before is tried first
 * (a scroll goes on at the speed it has).  A wrong guess only costs time:
 * every pixel is still compared with the one it is supposed to have come
 * from, and scaled again if it differs. */
#define UP_SLIDE_MAX 8
#define UP_PROBE_ROWS 12
#define UP_PROBE_COLS 16
#define UP_PROBES (UP_PROBE_ROWS * UP_PROBE_COLS)

/* Probes that match at the slide `shift` (dy * w + dx), sample by sample,
 * giving up as soon as the rest could not bring the count to `need`: such a
 * slide can neither win nor be used, so where it stops does not matter.
 * `idx` holds each sample's offset in the picture, worked out once. */
static int slide_score(const up_slot *S, const int *idx, int shift, int need)
{
   const uint16_t *cur = S->cur, *old = S->old;
   int i, n = 0, miss_left = UP_PROBES - need;
   for (i = 0; i < UP_PROBES; i++)
   {
      if (cur[idx[i]] == old[idx[i] - shift])
         n++;
      else if (--miss_left < 0)
         return -1;
   }
   return n;
}

static void slide_find(up_slot *S, int *pdx, int *pdy)
{
   const int w = (int)S->w, h = (int)S->h;
   const int m = UP_SLIDE_MAX + 1;
   int idx[UP_PROBES];
   int r, c, dx, dy, base, best, bdx = 0, bdy = 0, floor_;

   *pdx = *pdy = 0;
   if (w < 6 * m || h < 6 * m)
      return;
   /* The samples: a grid that keeps clear of the edges by the largest
    * slide, staggered row by row so it does not sit on a 16-pixel tile's
    * grid. */
   for (r = 0; r < UP_PROBE_ROWS; r++)
      for (c = 0; c < UP_PROBE_COLS; c++)
      {
         const int py = m + (h - 2 * m - 1) * (2 * r + 1) / (2 * UP_PROBE_ROWS);
         const int px = m + ((w - 2 * m - 1) * (2 * c + 1) /
                             (2 * UP_PROBE_COLS) + r * 5) % (w - 2 * m);
         idx[r * UP_PROBE_COLS + c] = py * w + px;
      }
   base = slide_score(S, idx, 0, 0);
   if (base * 4 >= UP_PROBES * 3)
      return;                    /* mostly the same where it is */
   if (S->last_dx || S->last_dy)
   {
      /* (counted only as far as deciding "three quarters or not") */
      int s = slide_score(S, idx, S->last_dy * w + S->last_dx,
                          (UP_PROBES * 3 + 3) / 4);
      if (s * 4 >= UP_PROBES * 3)
      {
         *pdx = S->last_dx;
         *pdy = S->last_dy;
         return;
      }
   }
   /* A slide is used only if it explains three fifths of the samples and
    * an eighth more than staying put: nothing below that can be the
    * answer, so a slide is abandoned once it cannot reach it. */
   floor_ = (UP_PROBES * 3 + 4) / 5;
   if (floor_ < base + UP_PROBES / 8)
      floor_ = base + UP_PROBES / 8;
   if (floor_ > UP_PROBES)
      return;
   best = base;
   for (dy = -UP_SLIDE_MAX; dy <= UP_SLIDE_MAX; dy++)
      for (dx = -UP_SLIDE_MAX; dx <= UP_SLIDE_MAX; dx++)
      {
         int s, need = best > floor_ ? best : floor_;
         if (!dx && !dy)
            continue;
         s = slide_score(S, idx, dy * w + dx, need);
         if (s < 0)
            continue;
         /* ties (flat ground matches anywhere): the smaller slide */
         if (s > best || (s == best && (bdx || bdy) &&
                          abs(dx) + abs(dy) < abs(bdx) + abs(bdy)))
         {
            best = s;
            bdx = dx;
            bdy = dy;
         }
      }
   /* Worth it when it explains most of the picture and clearly more than
    * staying put does. */
   if ((bdx || bdy) && best * 5 >= UP_PROBES * 3 &&
       best >= base + UP_PROBES / 8)
   {
      *pdx = bdx;
      *pdy = bdy;
   }
}

/* Two rows of w pixels are the same.  newlib's memcmp is a plain word loop
 * that tests every word (two of them a picture: a millisecond a frame on a
 * console); this ORs eight words at a time and leaves at the first
 * difference. */
static int row_same(const uint16_t *a, const uint16_t *b, unsigned w)
{
   const uint32_t *p = (const uint32_t *)(const void *)a;
   const uint32_t *q = (const uint32_t *)(const void *)b;
   unsigned n, i = 0;
   uint32_t acc = 0;
   if ((((uintptr_t)a | (uintptr_t)b) & 3u) || (w & 1u))
      return memcmp(a, b, (size_t)w * 2u) == 0;
   n = w / 2u;
   for (; i + 8u <= n; i += 8u)
   {
      /* the picture before is usually out of the cache by now: ask for
       * the lines ahead before they are needed */
      __builtin_prefetch(q + i + 32);
      __builtin_prefetch(p + i + 32);
      acc = (p[i] ^ q[i]) | (p[i + 1] ^ q[i + 1]) | (p[i + 2] ^ q[i + 2]) |
            (p[i + 3] ^ q[i + 3]) | (p[i + 4] ^ q[i + 4]) |
            (p[i + 5] ^ q[i + 5]) | (p[i + 6] ^ q[i + 6]) |
            (p[i + 7] ^ q[i + 7]);
      if (acc)
         return 0;
   }
   for (; i < n; i++)
      acc |= p[i] ^ q[i];
   return acc == 0;
}

/* The time since *tp, to part `part` of the preparation. */
static void up_lap(unsigned *tp, int part)
{
   const unsigned now = pspcompat_time_us();
   g_prep[part] += now - *tp;
   *tp = now;
}

void upscale_ring(int slot, int *ox, int *oy)
{
   int x = 0, y = 0;
   if (slot >= 0 && slot < UPSCALE_SLOTS)
   {
      x = g_slot[slot].rox;
      y = g_slot[slot].roy;
   }
   if (ox) *ox = x;
   if (oy) *oy = y;
}

void upscale_slide_mode(int mode)
{
   int i;
   if (mode < 0 || mode > 2)
      mode = 2;
   for (i = 0; i < UPSCALE_SLOTS; i++)
   {
      slot_wait(&g_slot[i]);
      g_slot[i].kind = UPSCALE_NONE;     /* start from a whole picture */
   }
   g_slide_mode = mode;
}

int upscale_submit(int slot, int kind, int f, const uint16_t *src,
                   unsigned w, unsigned h, size_t pitch_bytes, int bgr,
                   void *const out[2], size_t out_pitch, int target)
{
   static colmask rowm[UP_MAX_H];        /* the caller's thread only */
   static unsigned char rsame[UP_MAX_H]; /* ...rows as they were         */
   up_slot *S;
   const size_t pitch = pitch_bytes / 2;
   const unsigned tin = pspcompat_time_us();
   unsigned tp = tin;
   size_t npx, nbig;
   unsigned y, same = 0;
   int i, fresh, any = 0, dx = 0, dy = 0, fj, whole = 0;

   if (slot < 0 || slot >= UPSCALE_SLOTS || !src || !out || !out[0] ||
       !out[1] || !w || !h || h > UP_MAX_H || w > UP_MAX_W ||
       pitch_bytes < w * 2u || (target != 0 && target != 1) ||
       (kind == UPSCALE_SCALE && (f < 2 || f > 3)) ||
       (kind == UPSCALE_XBRZ && (f < 2 || f > 4)) ||
       (kind != UPSCALE_SCALE && kind != UPSCALE_XBRZ) ||
       out_pitch < (size_t)f * w * sizeof(uint32_t))
      return -1;
   S = &g_slot[slot];
   if (A_LOAD(S->state) == ST_BUSY)
   {
      A_ADD(g_st.dropped, 1);
      return 0;
   }
   if (!g_lut_ready)
      lut_build();
   if (g_pool < 0)
      pool_start();
   /* A helper may still be on its way out of the job before this one
    * (claim_and_run): a few instructions, almost never seen. */
   {
      int spins = 0;
      while (A_LOAD(S->inside))
         if (++spins > 200)
            nap();
   }

   /* ---- the slot's buffers ------------------------------------------------ */
   bgr = bgr ? 1 : 0;
   npx = (size_t)w * h;
   nbig = npx * (size_t)f * (size_t)f;
#ifndef __vita__
   {
      /* A test's reference: every picture scaled whole. */
      static int noskip = -1;
      if (noskip < 0)
         noskip = getenv("GBADHOC_UPSCALE_NOSKIP") != NULL;
      if (noskip)
         S->kind = UPSCALE_NONE;
   }
#endif
   /* (the picture's place: the same as the last one's, or start over;
    * the factor is decided below, once it is known what changed) */
   fresh = S->kind != kind || S->bgr != bgr || S->w != w ||
           S->h != h || S->out[0] != (unsigned char *)out[0] ||
           S->out[1] != (unsigned char *)out[1] || S->out_pitch != out_pitch;
   if (npx > S->cap_src)
   {
      free(S->cur);
      free(S->old);
      free(S->src32);
      S->cur = (uint16_t *)malloc(npx * sizeof(uint16_t));
      S->old = (uint16_t *)malloc(npx * sizeof(uint16_t));
      S->src32 = (uint32_t *)malloc(npx * sizeof(uint32_t));
      S->cap_src = S->cur && S->old && S->src32 ? npx : 0;
      fresh = 1;
   }
   if (nbig > S->cap_big)
   {
      free(S->big);
      S->big = (uint32_t *)malloc(nbig * sizeof(uint32_t));
      S->cap_big = S->big ? nbig : 0;
      fresh = 1;
   }
   if (!S->cap_src || !S->cap_big)
   {
      S->kind = UPSCALE_NONE;
      return -1;
   }
   S->nstripes = (int)((h + UP_STRIPE - 1) / UP_STRIPE);

   /* ---- the new picture, and what of it is new --------------------------- */
   {
      uint16_t *t = S->old;      /* the one before last is written over */
      S->old = S->cur;
      S->cur = t;
      for (y = 0; y < h; y++)
         memcpy(S->cur + (size_t)w * y, src + pitch * y, w * 2u);
   }
   S->slid = 0;
   up_lap(&tp, PP_COPY);

   /* ---- what changed, and the factor this picture is scaled by ----------- */
   if (!fresh)
   {
      for (y = 0; y < h; y++)
      {
         rsame[y] = (unsigned char)row_same(S->cur + (size_t)w * y,
                                            S->old + (size_t)w * y, w);
         same += rsame[y];
      }
      up_lap(&tp, PP_CMP);
      if (same * 2 < h && g_slide_mode)
         slide_find(S, &dx, &dy);
      up_lap(&tp, PP_FIND);
      whole = same * 2 < h && !dx && !dy;
   }
   if (whole)
      S->hot = UP_HOT_HOLD;
   else if (S->hot)
      S->hot--;
   /* one less during a fade (4x only: 3x of a whole picture is in time),
    * or while the pictures have been late (job_finish) */
   fj = f;
   if (fj > 3 && S->hot && g_fade_drop)
      fj = fj - g_fade_drop < 2 ? 2 : fj - g_fade_drop;
   else if (fj > 2 && A_LOAD(S->step_down))
      fj--;
   if (S->f != fj)
      fresh = 1;                 /* both textures need it all again        */

   if (fresh)
   {
      dx = dy = 0;
      S->kind = kind; S->f = fj; S->bgr = bgr; S->w = w; S->h = h;
      S->out[0] = (unsigned char *)out[0];
      S->out[1] = (unsigned char *)out[1];
      S->out_pitch = out_pitch;
      S->last_dx = S->last_dy = 0;
      S->rox = S->roy = 0;
      for (y = 0; y < h; y++)
      {
         cm_set_all(&rowm[y], (int)w);
         cm_set_all(&S->pend[0][y], (int)w);   /* neither texture has any */
         cm_set_all(&S->pend[1][y], (int)w);
      }
   }
   else
   {
      if (dx || dy)
      {
         /* The expanded source moves, here, at once (xBRZ reads across
          * stripes; a fifth of a megabyte).  The scaled picture: the ring
          * turns (nothing moves) -- or, as the first builds did, it is
          * moved in place and both textures written whole again. */
         const long so = (long)dy * (long)w + dx;
         if (g_slide_mode == 1)
         {
            const long bo = (long)dy * fj * (long)(fj * w) + (long)dx * fj;
            if (so > 0)
               memmove(S->src32 + so, S->src32, (npx - (size_t)so) * 4);
            else
               memmove(S->src32, S->src32 - so, (npx - (size_t)-so) * 4);
            const size_t nb = npx * (size_t)fj * (size_t)fj;
            if (bo > 0)
               memmove(S->big + bo, S->big, (nb - (size_t)bo) * 4);
            else
               memmove(S->big, S->big - bo, (nb - (size_t)-bo) * 4);
            for (y = 0; y < h; y++)
            {
               cm_set_all(&S->pend[0][y], (int)w);
               cm_set_all(&S->pend[1][y], (int)w);
            }
         }
         else
         {
            if (so > 0)
               UP_MEMMOVE(S->src32 + so, S->src32, (npx - (size_t)so) * 4);
            else
               UP_MEMMOVE(S->src32, S->src32 - so, (npx - (size_t)-so) * 4);
            /* the old pixel (x - dx, y - dy) is where the new (x, y) is */
            S->rox = (int)(((long)S->rox - dx + (long)w * 8) % (long)w);
            S->roy = (int)(((long)S->roy - dy + (long)h * 8) % (long)h);
         }
         S->slid = 1;
         A_ADD(g_st.shifted, 1);
      }
      up_lap(&tp, PP_MOVE);
      S->last_dx = dx;
      S->last_dy = dy;
      for (y = 0; y < h; y++)
      {
         const int sy = (int)y - dy;
         const uint16_t *c = S->cur + (size_t)w * y;
         memset(&rowm[y], 0, sizeof(colmask));
         if (sy < 0 || sy >= (int)h ||
             (dy && ((int)y < UP_MARGIN + abs(dy) ||
                     (int)y >= (int)h - UP_MARGIN - abs(dy))))
            cm_set_all(&rowm[y], (int)w);
         else
         {
            const uint16_t *o = S->old + (size_t)w * (unsigned)sy;
            int x0 = dx > 0 ? dx : 0, x1 = dx < 0 ? (int)w + dx : (int)w, x;
            if (dx)
            {
               /* the columns that came into view, and those whose
                * neighbours beyond the picture's edge are not what they
                * were */
               cm_set_range(&rowm[y], 0, UP_MARGIN + abs(dx));
               cm_set_range(&rowm[y], (int)w - UP_MARGIN - abs(dx), (int)w);
            }
            /* (in place, the first pass already knows the rows that are
             * the same) */
            if (!dx && !dy ? !rsame[y]
                : memcmp(c + x0, o + x0 - dx, (size_t)(x1 - x0) * 2u) != 0)
               for (x = x0; x < x1; x++)
                  if (c[x] != o[x - dx])
                     rowm[y].b[x >> 5] |= 1u << (x & 31);
         }
      }
      up_lap(&tp, PP_MARK);
   }
   if (kind == UPSCALE_XBRZ)
   {
      const uint32_t *lut = g_lut[bgr];
      for (y = 0; y < h; y++)
         if (cm_any(&rowm[y]))
         {
            const uint16_t *c = S->cur + (size_t)w * y;
            uint32_t *e = S->src32 + (size_t)w * y;
            int x = 0, x0, x1;
            while (cm_run(&rowm[y], x, (int)w, 0, &x0, &x1))
            {
               for (x = x0; x < x1; x++)
                  e[x] = lut[c[x]];
               x = x1;
            }
         }
   }
   for (i = 0; i < S->nstripes; i++)
   {
      int y0 = i * UP_STRIPE - UP_MARGIN, y1 = (i + 1) * UP_STRIPE + UP_MARGIN;
      int yy;
      colmask m;
      memset(&m, 0, sizeof(m));
      if (y0 < 0) y0 = 0;
      if (y1 > (int)h) y1 = (int)h;
      for (yy = y0; yy < y1; yy++)
         cm_or(&m, &rowm[yy]);
      if (cm_any(&m))
         cm_dilate(&m, UP_MARGIN, (int)w);
      S->dirty[i] = m;
      any |= cm_any(&m);
   }
   for (y = 0; y < h && !any; y++)
      any = cm_any(&S->pend[target][y]);

   up_lap(&tp, PP_LUT);

   /* ---- the job ---------------------------------------------------------- */
   S->target = target;
   S->t0 = pspcompat_time_us();
   A_ADD(g_st.prep_us, (int)(S->t0 - tin));
   A_STORE(S->scaled_px, 0);
   A_STORE(S->done, 0);
   A_STORE(S->next, 0);
   A_STORE(S->state, ST_BUSY);   /* last: see claim_and_run */
   if (g_async && any)
      pool_go();
   else
   {
      /* A host twin, a console with no helper, or a picture with nothing
       * to do: finished before this returns. */
      if (any)
         pool_go();
      while (claim_and_run(slot))
         ;
      slot_wait(S);
   }
   return 1;
}

int upscale_help(unsigned max_us)
{
   const unsigned t0 = pspcompat_time_us();
   int n = 0, si, did;
   if (!g_async)
      return 0;
   do
   {
      did = 0;
      for (si = 0; si < UPSCALE_SLOTS; si++)
      {
         if (pspcompat_time_us() - t0 >= max_us)
         {
            A_ADD(g_st.helped, n);
            return n;
         }
         if (claim_and_run(si))
         {
            did = 1;
            n++;
         }
      }
   } while (did);
   A_ADD(g_st.helped, n);
   return n;
}

void upscale_stats(upscale_stats_t *out)
{
   const int jobs = __atomic_exchange_n(&g_st.jobs, 0, __ATOMIC_SEQ_CST);
   const int sum = __atomic_exchange_n(&g_st.sum_us, 0, __ATOMIC_SEQ_CST);
   const int stripes = __atomic_exchange_n(&g_st.stripes, 0, __ATOMIC_SEQ_CST);
   const int px = __atomic_exchange_n(&g_st.px_k, 0, __ATOMIC_SEQ_CST);
   const int scaled = __atomic_exchange_n(&g_st.scaled_k, 0, __ATOMIC_SEQ_CST);
   const int shifted = __atomic_exchange_n(&g_st.shifted, 0, __ATOMIC_SEQ_CST);
   const int prep = __atomic_exchange_n(&g_st.prep_us, 0, __ATOMIC_SEQ_CST);
   if (!out)
      return;
   out->jobs = (unsigned)jobs;
   out->avg_us = jobs ? (unsigned)(sum / jobs) : 0;
   out->max_us = (unsigned)__atomic_exchange_n(&g_st.max_us, 0,
                                               __ATOMIC_SEQ_CST);
   out->dropped = (unsigned)__atomic_exchange_n(&g_st.dropped, 0,
                                                __ATOMIC_SEQ_CST);
   out->scaled_pct = px ? (unsigned)((long long)scaled * 100 / px) : 0;
   out->slid_pct = jobs ? (unsigned)(shifted * 100 / jobs) : 0;
   out->prep_us = jobs ? (unsigned)(prep / jobs) : 0;
   out->helped_pct = stripes ? (unsigned)(
      __atomic_exchange_n(&g_st.helped, 0, __ATOMIC_SEQ_CST) * 100 / stripes)
      : 0;
   {
      int i;
      for (i = 0; i < 2; i++)
      {
         const int v = __atomic_exchange_n(&g_st.job_us[i], 0, __ATOMIC_SEQ_CST);
         out->job_part[i] = jobs ? (unsigned)(v / jobs) : 0;
      }
      out->late = (unsigned)__atomic_exchange_n(&g_st.late, 0, __ATOMIC_SEQ_CST);
      for (i = 0; i < 3; i++)
         out->late_f[i] = (unsigned)__atomic_exchange_n(&g_st.late_f[i], 0,
                                                        __ATOMIC_SEQ_CST);
      out->stepped = (unsigned)__atomic_exchange_n(&g_st.stepped, 0,
                                                   __ATOMIC_SEQ_CST);
      out->late_slid = (unsigned)__atomic_exchange_n(&g_st.late_slid, 0,
                                                     __ATOMIC_SEQ_CST);
      i = __atomic_exchange_n(&g_st.late_pct, 0, __ATOMIC_SEQ_CST);
      out->late_pct = out->late ? (unsigned)i / out->late : 0;
      for (i = 0; i < PP_N && i < UPSCALE_PREP_PARTS; i++)
      {
         out->prep_part[i] = jobs ? g_prep[i] / (unsigned)jobs : 0;
         g_prep[i] = 0;
      }
   }
   out->factor = (unsigned)A_LOAD(g_st.factor);
   out->threads = (unsigned)(g_pool > 0 ? g_pool : 0) + (g_async ? 0u : 1u);
   out->async = (unsigned)g_async;
}

static int g_xbrz_dist;

void upscale_xbrz_dist(int mode)
{
   int i;
   if (mode < 0 || mode > 2)
      mode = 0;
   for (i = 0; i < UPSCALE_SLOTS; i++)
   {
      slot_wait(&g_slot[i]);
      g_slot[i].kind = UPSCALE_NONE;     /* what `big` holds was made the other way */
   }
   if (!g_lut_ready)
      lut_build();
   xbrz_c_dist_mode(mode);
   g_xbrz_dist = mode;
}

void upscale_bench(const uint16_t *src, unsigned w, unsigned h,
                   size_t pitch_bytes, char *line, size_t cap)
{
   /* dist -1: as set */
   static const struct { int kind, f, dist; const char *name; } V[] = {
      { UPSCALE_SCALE, 2, -1, "scale2" }, { UPSCALE_SCALE, 3, -1, "scale3" },
      { UPSCALE_XBRZ, 2, 0, "xbrz2_table" }, { UPSCALE_XBRZ, 2, 1, "xbrz2_compact" },
      { UPSCALE_XBRZ, 2, 2, "xbrz2_math" },
      { UPSCALE_XBRZ, 3, 0, "xbrz3_table" }, { UPSCALE_XBRZ, 3, 1, "xbrz3_compact" },
      { UPSCALE_XBRZ, 3, 2, "xbrz3_math" },
      { UPSCALE_XBRZ, 4, 1, "xbrz4_compact" } };
   const size_t pitch = pitch_bytes / 2;
   uint16_t *cur;
   uint32_t *s32, *big;
   size_t n = 0;
   unsigned x, y, v;
   int i;

   if (line && cap)
      line[0] = '\0';
   if (!src || !w || !h || h > UP_MAX_H || pitch < w || !line || !cap)
      return;
   if (!g_lut_ready)
      lut_build();
   for (i = 0; i < UPSCALE_SLOTS; i++)
      slot_wait(&g_slot[i]);             /* the comparison is switched below */
   cur = (uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
   s32 = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
   big = (uint32_t *)malloc((size_t)w * h * 16 * sizeof(uint32_t));
   if (cur && s32 && big)
   {
      for (y = 0; y < h; y++)
         for (x = 0; x < w; x++)
         {
            cur[(size_t)w * y + x] = src[pitch * y + x];
            s32[(size_t)w * y + x] = g_lut[0][src[pitch * y + x]];
         }
      for (v = 0; v < sizeof(V) / sizeof(V[0]); v++)
      {
         unsigned t0, dt;
         if (V[v].dist >= 0)
            xbrz_c_dist_mode(V[v].dist);
         t0 = pspcompat_time_us();
         if (V[v].kind == UPSCALE_XBRZ)
            xbrz_c_scale(V[v].f, s32, big, (int)w, (int)h, 0, (int)h);
         else
            epx_rows(cur, (int)w, (int)h, V[v].f, 0, big, (size_t)V[v].f * w,
                     0, (int)h, 0, (int)w);
         dt = pspcompat_time_us() - t0;
         n += (size_t)snprintf(line + n, n < cap ? cap - n : 0, "%s%s=%u",
                               v ? " " : "", V[v].name, dt);
         if (n >= cap)
            break;
      }
      xbrz_c_dist_mode(g_xbrz_dist);
   }
   free(cur);
   free(s32);
   free(big);
}

void upscale_shutdown(void)
{
   int i;
   for (i = 0; i < UPSCALE_SLOTS; i++)
      slot_wait(&g_slot[i]);
   if (g_pool > 0)
      pool_stop();
   g_pool = -1;
   g_pool_quit = 0;
   for (i = 0; i < UPSCALE_SLOTS; i++)
   {
      free(g_slot[i].cur);
      free(g_slot[i].old);
      free(g_slot[i].src32);
      free(g_slot[i].big);
      memset(&g_slot[i], 0, sizeof(g_slot[i]));
   }
}
