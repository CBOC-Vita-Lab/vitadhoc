/* clip_vita.c -- the game list's previews: the last ten seconds you played.
 * clip_vita.h says what it does, how a frame is squeezed and what the file
 * looks like.
 *
 * THREADS.  As rewind_vita.c, two at most, meeting in one slot (g_slot):
 *
 *   frame thread   clip_frame copies the picture into C.pend while the slot
 *                  is NONE and sets it READY; everything else but the
 *                  squeeze (begin, end, save) after getting the slot to
 *                  NONE (slot_idle)
 *   worker thread  clip_worker: READY -> BUSY (compare-and-swap), squeeze
 *                  C.pend against C.prev into the ring, NONE
 *
 * While the slot is not NONE its taker owns the ring, its index, C.prev and
 * C.pend; the frame thread touches none of them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "clip_vita.h"

enum { OP_SKIP = 0, OP_MOVE = 1, OP_LIT = 2, OP_FILL = 3 };

#define CLIP_HDR      32
#define CLIP_IDX_CAP  1024u      /* frames in the ring: 30 s at 30 a second  */
#define CLIP_MV       8          /* the farthest a frame's motion is looked for */
#define CLIP_FRAMES_MAX 4096u    /* in a file                                */
#define CLIP_RESERVE_MB 36       /* the heap the ring never takes: a second
                                  * 32 MiB cartridge and its machine        */

/* The most bytes a frame of n pixels can take: a run of literals is 2 bytes
 * a pixel and a few for its count, and every other run is at most 2 bytes a
 * pixel too (SKIP and MOVE take >= 2 pixels for <= 4 bytes, FILL >= 3 for
 * <= 6), so the sum stays under 2 a pixel plus the header and one count. */
static size_t enc_cap(unsigned w, unsigned h)
{
   size_t n = (size_t)w * h;
   return 4 + 2 * n + n / 16 + 64;
}

/* ---- the runs ------------------------------------------------------------- */

static uint8_t *put_run(uint8_t *o, int op, unsigned n)       /* n >= 1 */
{
   unsigned m = n - 1;
   if (m < 63)
   {
      *o++ = (uint8_t)(op << 6 | m);
      return o;
   }
   *o++ = (uint8_t)(op << 6 | 63);
   m -= 63;
   while (m >= 128)
   {
      *o++ = (uint8_t)(m | 128);
      m >>= 7;
   }
   *o++ = (uint8_t)m;
   return o;
}

static uint8_t *put_px(uint8_t *o, uint16_t v)
{
   o[0] = (uint8_t)v;
   o[1] = (uint8_t)(v >> 8);
   return o + 2;
}

static uint8_t *put_lit(uint8_t *o, const uint16_t *c, unsigned n)
{
   unsigned k;
   o = put_run(o, OP_LIT, n);
   for (k = 0; k < n; k++)
      o = put_px(o, c[k]);
   return o;
}

/* Two pixels at a time where it can (the buffers are malloc'd, so an even
 * pixel is a word): a still frame is one long SKIP. */
typedef uint32_t __attribute__((may_alias)) clip_u32;

static unsigned run_skip(const uint16_t *c, const uint16_t *p, unsigned i,
                         unsigned n)
{
   unsigned k = i;
   if ((k & 1) && k < n && c[k] == p[k])
      k++;
   if (!(k & 1))
   {
      const clip_u32 *cw = (const clip_u32 *)(const void *)(c + k);
      const clip_u32 *pw = (const clip_u32 *)(const void *)(p + k);
      unsigned m = (n - k) / 2, j = 0;
      while (j < m && cw[j] == pw[j])
         j++;
      k += 2 * j;
   }
   while (k < n && c[k] == p[k])
      k++;
   return k - i;
}

static unsigned run_fill(const uint16_t *c, unsigned i, unsigned n)
{
   uint16_t v = c[i];
   unsigned k = i + 1;
   while (k < n && c[k] == v)
      k++;
   return k - i;
}

/* Pixels from i on that are the frame before's, (dx, dy) away, and lie
 * inside it: a row's stretch at a time, where the source is one offset
 * away and in bounds throughout. */
static unsigned run_move(const uint16_t *c, const uint16_t *p, unsigned i,
                         unsigned n, unsigned w, unsigned h, int dx, int dy)
{
   unsigned x = i % w, k = i;
   int sy = (int)(i / w) + dy;
   unsigned xlo = dx < 0 ? (unsigned)-dx : 0;
   unsigned xhi = dx > 0 ? w - (unsigned)dx : w;
   const uint16_t *src = p + dy * (long)w + dx;
   while (k < n)
   {
      unsigned end;
      if (sy < 0 || sy >= (int)h || x < xlo || x >= xhi)
         break;
      end = k + (xhi - x);              /* this row's last usable pixel + 1 */
      while (k < end && c[k] == src[k])
         k++;
      if (k < end || xhi < w)
         break;                         /* a mismatch, or the row's end    */
      x = 0;                            /* dx <= 0: on into the next row   */
      sy++;
   }
   return k - i;
}

/* One frame: `p` NULL makes a key.  Returns its length in `out` (which
 * has enc_cap bytes). */
static size_t enc_frame(const uint16_t *c, const uint16_t *p, unsigned w,
                        unsigned h, int dx, int dy, unsigned ticks,
                        uint8_t *out)
{
   unsigned n = w * h, i = 0, lit = 0, lit_at = 0;
   int move = p && (dx || dy);
   uint8_t *o = out;

   *o++ = p ? 'D' : 'K';
   *o++ = (uint8_t)(ticks > 255 ? 255 : ticks);
   *o++ = (uint8_t)(int8_t)(p ? dx : 0);
   *o++ = (uint8_t)(int8_t)(p ? dy : 0);
   while (i < n)
   {
      unsigned rs = p ? run_skip(c, p, i, n) : 0;
      unsigned rm = move ? run_move(c, p, i, n, w, h, dx, dy) : 0;
      unsigned rf = run_fill(c, i, n), best;
      int op;
      if (rs >= 2 && rs >= rm && rs >= rf)
      {
         op = OP_SKIP;
         best = rs;
      }
      else if (rm >= 2 && rm >= rf)
      {
         op = OP_MOVE;
         best = rm;
      }
      else if (rf >= 3)
      {
         op = OP_FILL;
         best = rf;
      }
      else
      {
         if (!lit)
            lit_at = i;
         lit++;
         i++;
         continue;
      }
      if (lit)
      {
         o = put_lit(o, c + lit_at, lit);
         lit = 0;
      }
      o = put_run(o, op, best);
      if (op == OP_FILL)
         o = put_px(o, c[i]);
      i += best;
   }
   if (lit)
      o = put_lit(o, c + lit_at, lit);
   return (size_t)(o - out);
}

static int get_count(const uint8_t **s, const uint8_t *end, unsigned t,
                     unsigned *n)
{
   unsigned m = t & 63;
   if (m == 63)
   {
      unsigned v = 0, sh = 0;
      uint8_t b;
      do
      {
         if (*s >= end || sh > 21)
            return -1;
         b = *(*s)++;
         v |= (unsigned)(b & 127) << sh;
         sh += 7;
      } while (b & 128);
      m += v;
   }
   *n = m + 1;
   return 0;
}

/* One frame back into pixels.  `p` is the frame before (NULL: there is
 * none, so only a key will do).  0 = decoded; anything malformed is -1. */
static int dec_frame(const uint8_t *src, size_t len, const uint16_t *p,
                     uint16_t *out, unsigned w, unsigned h, unsigned *ticks)
{
   const uint8_t *s = src, *end = src + len;
   unsigned n = w * h, i = 0;
   int key, dx, dy;

   if (len < 4)
      return -1;
   key = s[0] == 'K';
   if ((!key && s[0] != 'D') || (!key && !p))
      return -1;
   if (ticks)
      *ticks = s[1];
   dx = (int8_t)s[2];
   dy = (int8_t)s[3];
   s += 4;
   while (i < n)
   {
      unsigned t, cnt, k;
      if (s >= end)
         return -1;
      t = *s++;
      if (get_count(&s, end, t, &cnt) != 0 || cnt > n - i)
         return -1;
      switch (t >> 6)
      {
      case OP_SKIP:
         if (key)
            return -1;
         memcpy(out + i, p + i, (size_t)cnt * 2);
         break;
      case OP_MOVE:
      {
         unsigned x = i % w;
         int y = (int)(i / w);
         if (key)
            return -1;
         for (k = 0; k < cnt; k++)
         {
            int sx = (int)x + dx, sy = y + dy;
            if (sx < 0 || sx >= (int)w || sy < 0 || sy >= (int)h)
               return -1;
            out[i + k] = p[(unsigned)sy * w + (unsigned)sx];
            if (++x == w)
            {
               x = 0;
               y++;
            }
         }
         break;
      }
      case OP_LIT:
         if ((size_t)(end - s) < (size_t)cnt * 2)
            return -1;
         for (k = 0; k < cnt; k++, s += 2)
            out[i + k] = (uint16_t)(s[0] | s[1] << 8);
         break;
      default:
      {
         uint16_t v;
         if (end - s < 2)
            return -1;
         v = (uint16_t)(s[0] | s[1] << 8);
         s += 2;
         for (k = 0; k < cnt; k++)
            out[i + k] = v;
         break;
      }
      }
      i += cnt;
   }
   return s == end ? 0 : -1;
}

/* ---- the motion of a frame ------------------------------------------------- */

#define MV_SAMPLES 1400

/* Which way did the picture move?  Only the sample pixels that CHANGED
 * are asked (the ones that stayed are SKIP's already), and the answer is
 * the shift that explains most of them: the last frame's motion first
 * (a walk keeps walking), then straight lines up to CLIP_MV, and the whole
 * square only when nothing straight explains half.  (0, 0): nothing moved,
 * or nothing explains it. */
static void motion(const uint16_t *c, const uint16_t *p, unsigned w,
                   unsigned h, int *mdx, int *mdy)
{
   static uint32_t at[MV_SAMPLES];
   unsigned n = 0, x, y, k, step_y;
   unsigned best = 0;
   int bdx = 0, bdy = 0, d, dir, dx, dy;
   const unsigned m = CLIP_MV + 1;

   if (w < 4 * m || h < 4 * m)
   {
      *mdx = *mdy = 0;
      return;
   }
   step_y = (h - 2 * m) / 16;
   if (step_y < 1)
      step_y = 1;
   for (y = m; y + m < h && n < MV_SAMPLES; y += step_y)
      for (x = m + (y & 1); x + m < w && n < MV_SAMPLES; x += 3)
         if (c[y * w + x] != p[y * w + x])
            at[n++] = y * w + x;
   if (n < 8)
   {
      *mdx = *mdy = 0;
      return;
   }

#define MV_TRY(DX, DY)                                                     \
   do {                                                                     \
      long off_ = (long)(DY) * (long)w + (DX);                              \
      unsigned s_ = 0, left_ = n;                                           \
      for (k = 0; k < n && s_ + left_ > best; k++, left_--)                 \
         s_ += c[at[k]] == p[(long)at[k] + off_];                           \
      if (s_ > best) { best = s_; bdx = (DX); bdy = (DY); }                 \
   } while (0)

   if (*mdx || *mdy)
      MV_TRY(*mdx, *mdy);
   for (d = 1; d <= CLIP_MV; d++)
      for (dir = 0; dir < 4; dir++)
      {
         dx = dir == 0 ? d : dir == 1 ? -d : 0;
         dy = dir == 2 ? d : dir == 3 ? -d : 0;
         MV_TRY(dx, dy);
      }
   if (best * 2 < n)
      for (dy = -CLIP_MV; dy <= CLIP_MV; dy++)
         for (dx = -CLIP_MV; dx <= CLIP_MV; dx++)
            if (dx && dy)
               MV_TRY(dx, dy);
#undef MV_TRY
   /* a shift that explains too little is not worth a MOVE */
   if (best * 8 < n)
      bdx = bdy = 0;
   *mdx = bdx;
   *mdy = bdy;
}

/* ---- the recorder --------------------------------------------------------- */

typedef struct clip_ent
{
   uint32_t off, len;
   uint16_t ticks;
   uint16_t act;            /* pixels unlike the frame before's            */
   uint8_t key;
   uint8_t input;           /* the player pressed something since the last */
} clip_ent;

enum { SLOT_NONE = 0, SLOT_READY, SLOT_BUSY };
static volatile int g_slot;

static struct
{
   int on;
   unsigned w, h;
   uint8_t *ring;
   size_t cap, head;
   clip_ent idx[CLIP_IDX_CAP];
   unsigned first, count;
   unsigned ticks;               /* the frames' ticks, summed              */
   size_t bytes;                 /* ...and their lengths                    */
   unsigned keys;
   uint16_t *prev, *pend;
   int have_prev;
   unsigned pend_ticks;
   int pend_cut;
   int pend_input;
   unsigned keys_seen;           /* buttons since the last frame kept       */
   uint8_t *scratch;
   unsigned since_key;
   unsigned tick;                /* core frames since the last one kept     */
   int cut;
   int mdx, mdy;
   void (*wake)(void);
   clip_stats st;
} C;

static clip_ent *ent(unsigned i)                 /* i-th oldest */
{
   return &C.idx[(C.first + i) % CLIP_IDX_CAP];
}

static void ring_clear(void)
{
   C.first = C.count = 0;
   C.head = 0;
   C.ticks = 0;
   C.bytes = 0;
   C.keys = 0;
   C.have_prev = 0;
   C.since_key = 0;
}

static void drop_oldest(unsigned frames)
{
   while (frames-- && C.count)
   {
      clip_ent *e = ent(0);
      C.ticks -= e->ticks;
      C.bytes -= e->len;
      C.keys -= e->key;
      C.first = (C.first + 1) % CLIP_IDX_CAP;
      C.count--;
   }
   if (!C.count)
      C.head = 0;
}

/* The oldest two seconds: how many frames, how many ticks.  0 = it is the
 * only segment there is. */
static int seg_first(unsigned *frames, unsigned *ticks)
{
   unsigned i, t;
   if (!C.count)
      return 0;
   t = ent(0)->ticks;
   for (i = 1; i < C.count; i++)
   {
      if (ent(i)->key)
      {
         *frames = i;
         *ticks = t;
         return 1;
      }
      t += ent(i)->ticks;
   }
   return 0;
}

/* Where `len` bytes go without touching a frame still kept: 1 = at *at. */
static int ring_place(size_t len, size_t *at)
{
   size_t tail;
   if (!C.count)
   {
      C.head = 0;
      *at = 0;
      return len <= C.cap;
   }
   tail = ent(0)->off;
   if (C.head > tail)
   {
      if (C.head + len <= C.cap)
         *at = C.head;
      else if (len <= tail)
         *at = 0;                /* round to the start; the end is idle */
      else
         return 0;
      return 1;
   }
   if (C.head + len <= tail)
   {
      *at = C.head;
      return 1;
   }
   return 0;
}

/* Into the ring, letting the oldest segments go for room.  -1: a frame
 * said against the one before would need its own segment let go. */
static int ring_put(const uint8_t *d, size_t len, int key, unsigned ticks,
                    unsigned act, int input)
{
   size_t at = 0;
   clip_ent *e;
   for (;;)
   {
      unsigned f, t;
      if (C.count < CLIP_IDX_CAP && ring_place(len, &at))
         break;
      if (seg_first(&f, &t))
         drop_oldest(f);
      else if (key)
         drop_oldest(C.count);
      else
         return -1;
   }
   memcpy(C.ring + at, d, len);
   e = &C.idx[(C.first + C.count) % CLIP_IDX_CAP];
   e->off = (uint32_t)at;
   e->len = (uint32_t)len;
   e->ticks = (uint16_t)ticks;
   e->act = (uint16_t)(act > 65535 ? 65535 : act);
   e->key = (uint8_t)(key ? 1 : 0);
   e->input = (uint8_t)(input ? 1 : 0);
   C.count++;
   C.head = at + len;
   C.ticks += ticks;
   C.bytes += len;
   C.keys += e->key;
   /* the oldest two seconds go once the rest is the history on its own,
    * and a second to spare */
   for (;;)
   {
      unsigned f, t;
      if (!seg_first(&f, &t) || C.ticks - t < CLIP_HISTORY_TICKS + 60)
         break;
      drop_oldest(f);
   }
   return 0;
}

/* Pixels unlike the frame before's, at the same place (the frame's
 * activity), counted up to `limit` + 1: whether something happened is all
 * that is asked of it.  Two at a time: n is even for every console's
 * picture. */
static unsigned changed(const uint16_t *a, const uint16_t *b, unsigned n,
                        unsigned limit)
{
   const clip_u32 *aw = (const clip_u32 *)(const void *)a;
   const clip_u32 *bw = (const clip_u32 *)(const void *)b;
   unsigned k, c = 0;
   for (k = 0; k < n / 2; k++)
      if (aw[k] != bw[k])
      {
         c += (a[2 * k] != b[2 * k]) + (a[2 * k + 1] != b[2 * k + 1]);
         if (c > limit)
            return c;
      }
   if (n & 1)
      c += a[n - 1] != b[n - 1];
   return c;
}

static void commit(void)
{
   uint64_t t0 = plat_time_us();
   unsigned w = C.w, h = C.h, us;
   unsigned act = C.have_prev ? changed(C.pend, C.prev, w * h,
                                        w * h / CLIP_ACTIVE_DIV)
                              : w * h;
   int key = !C.have_prev || C.pend_cut || C.since_key >= CLIP_KEY_EVERY;
   int dx = 0, dy = 0;
   size_t len;
   uint16_t *sw;

   if (!key)
   {
      /* (a frame that changed nothing has no motion to look for; the last
       * one stays the next frame's first guess) */
      dx = act ? C.mdx : 0;
      dy = act ? C.mdy : 0;
      if (act)
         motion(C.pend, C.prev, w, h, &dx, &dy);
      len = enc_frame(C.pend, C.prev, w, h, dx, dy, C.pend_ticks, C.scratch);
      /* more than half a raw picture: a new scene, as costly as a key, and
       * a key lets what came before it go */
      if (len > (size_t)w * h)
         key = 1;
      else if (act)
      {
         C.mdx = dx;
         C.mdy = dy;
      }
   }
   if (key)
      len = enc_frame(C.pend, NULL, w, h, 0, 0, C.pend_ticks, C.scratch);
   if (ring_put(C.scratch, len, key, C.pend_ticks, act, C.pend_input) != 0)
   {
      /* the ring cannot hold even this segment: start again from here */
      ring_clear();
      key = 1;
      len = enc_frame(C.pend, NULL, w, h, 0, 0, C.pend_ticks, C.scratch);
      (void)ring_put(C.scratch, len, 1, C.pend_ticks, act, C.pend_input);
   }
   C.since_key = key ? 1 : C.since_key + 1;
   /* this frame is the next one's "before"; the old one is the next slot */
   sw = C.prev;
   C.prev = C.pend;
   C.pend = sw;
   C.have_prev = 1;
   C.st.kept++;
   us = (unsigned)(plat_time_us() - t0);
   C.st.enc_us_sum += us;
   if (us > C.st.enc_us_max)
      C.st.enc_us_max = us;
}

static int slot_take(void)
{
   int want = SLOT_READY;
   return __atomic_compare_exchange_n(&g_slot, &want, SLOT_BUSY, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void clip_worker(void)
{
   if (slot_take())
   {
      commit();
      __atomic_store_n(&g_slot, SLOT_NONE, __ATOMIC_SEQ_CST);
   }
}

static void slot_idle(void)
{
   clip_worker();
   while (__atomic_load_n(&g_slot, __ATOMIC_SEQ_CST) != SLOT_NONE)
      plat_thread_yield();
}

void clip_set_async(void (*wake)(void))
{
   if (!wake && C.wake)
      slot_idle();
   C.wake = wake;
}

void clip_game_end(void)
{
   void (*wake)(void) = C.wake;
   if (!C.on && !C.ring)
      return;                    /* nothing held */
   slot_idle();
   free(C.ring);
   free(C.prev);
   free(C.pend);
   free(C.scratch);
   memset(&C, 0, sizeof(C));
   C.wake = wake;
   __atomic_store_n(&g_slot, SLOT_NONE, __ATOMIC_SEQ_CST);
}

void clip_game_begin(int on)
{
   size_t px = (size_t)CLIP_W_MAX * CLIP_H_MAX;
   size_t ring = (size_t)CLIP_RING_MB << 20;
   size_t need = ring + 2 * px * 2 + enc_cap(CLIP_W_MAX, CLIP_H_MAX);
   clip_game_end();
   if (!on || plat_heap_free() < need + ((size_t)CLIP_RESERVE_MB << 20))
      return;
   C.ring = (uint8_t *)malloc(ring);
   C.prev = (uint16_t *)malloc(px * 2);
   C.pend = (uint16_t *)malloc(px * 2);
   C.scratch = (uint8_t *)malloc(enc_cap(CLIP_W_MAX, CLIP_H_MAX));
   if (!C.ring || !C.prev || !C.pend || !C.scratch)
   {
      clip_game_end();
      return;
   }
   C.cap = ring;
   C.on = 1;
}

void clip_cut(void)
{
   C.cut = 1;
}

void clip_frame(const uint16_t *pix, unsigned w, unsigned h,
                size_t pitch_bytes, unsigned keys)
{
   unsigned y;
   if (!C.on || !pix || !w || !h || w > CLIP_W_MAX || h > CLIP_H_MAX ||
       pitch_bytes < (size_t)w * 2)
      return;
   C.keys_seen |= keys;
   if (++C.tick < CLIP_EVERY)
      return;
   if (__atomic_load_n(&g_slot, __ATOMIC_SEQ_CST) != SLOT_NONE)
   {
      if (C.tick == CLIP_EVERY)
         C.st.skipped++;          /* (counted once per frame not kept) */
      return;
   }
   if (w != C.w || h != C.h)
   {
      ring_clear();               /* the slot is NONE: the ring is ours */
      C.w = w;
      C.h = h;
   }
   for (y = 0; y < h; y++)
      memcpy(C.pend + (size_t)y * w,
             (const uint8_t *)pix + (size_t)y * pitch_bytes, (size_t)w * 2);
   C.pend_ticks = C.tick > 65535 ? 65535 : C.tick;
   C.pend_cut = C.cut;
   C.pend_input = C.keys_seen != 0;
   C.keys_seen = 0;
   C.tick = 0;
   C.cut = 0;
   __atomic_store_n(&g_slot, SLOT_READY, __ATOMIC_SEQ_CST);
   if (C.wake)
      C.wake();
   else
      clip_worker();
}

void clip_get_stats(clip_stats *out)
{
   *out = C.st;
   out->frames = C.count;
   out->keys = C.keys;
   out->ticks = C.ticks;
   out->bytes = C.bytes;
}

static void put32(uint8_t *o, uint32_t v)
{
   o[0] = (uint8_t)v;
   o[1] = (uint8_t)(v >> 8);
   o[2] = (uint8_t)(v >> 16);
   o[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *s)
{
   return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 |
          (uint32_t)s[3] << 24;
}

int clip_save(const char *path, unsigned us_per_tick)
{
   unsigned first, last, n, i, t = 0, k;
   size_t px, first_len, total;
   uint16_t *a = NULL, *b = NULL;
   uint8_t hdr[CLIP_HDR], *offs = NULL;
   FILE *f;
   int ok;

   slot_idle();
   C.st.saved = C.st.calm_cut = 0;
   if (!C.on || !C.count)
      return 1;
   /* Where it ends: the last frame in which something happened, and a
    * second of calm after it -- a game left standing for a while before
    * the player quit is remembered up to where it went still.  (Nothing
    * happened at all: the newest frames, as they are.) */
   last = C.count - 1;
   for (i = C.count; i-- > 0;)
      if (ent(i)->input || ent(i)->act > C.w * C.h / CLIP_ACTIVE_DIV)
      {
         for (last = i; last + 1 < C.count &&
                        t + ent(last + 1)->ticks <= CLIP_CALM_TICKS; last++)
            t += ent(last + 1)->ticks;
         break;
      }
   for (i = last + 1; i < C.count; i++)
      C.st.calm_cut += ent(i)->ticks;
   if (C.st.calm_cut <= CLIP_CALM_TICKS)
   {
      last = C.count - 1;          /* nothing worth cutting */
      C.st.calm_cut = 0;
   }
   /* ...and the frames before it that fit in ten seconds (the first one's
    * own wait does not count: it is shown from the start) */
   t = 0;
   first = last;
   while (first > 0 && t + ent(first)->ticks <= CLIP_TICKS)
      t += ent(first--)->ticks;
   if (t < CLIP_TICKS_MIN)
      return 1;
   n = last - first + 1;
   px = (size_t)C.w * C.h;

   /* The first frame has to be a key.  When it is not, the segment it is
    * in is decoded up to it and it is said again, whole. */
   a = (uint16_t *)malloc(px * 2);
   b = (uint16_t *)malloc(px * 2);
   offs = (uint8_t *)malloc(((size_t)n + 1) * 4);
   if (!a || !b || !offs)
      goto fail;
   for (k = first; !ent(k)->key; k--)
      ;
   for (i = k; i <= first; i++)
   {
      uint16_t *sw;
      if (dec_frame(C.ring + ent(i)->off, ent(i)->len, i == k ? NULL : a, b,
                    C.w, C.h, NULL) != 0)
         goto fail;
      sw = a;
      a = b;
      b = sw;
   }
   first_len = enc_frame(a, NULL, C.w, C.h, 0, 0, CLIP_EVERY, C.scratch);

   total = first_len;
   put32(offs, 0);
   put32(offs + 4, (uint32_t)first_len);
   for (i = 1; i < n; i++)
   {
      total += ent(first + i)->len;
      put32(offs + 4 * ((size_t)i + 1), (uint32_t)total);
   }
   if (total > CLIP_FILE_MAX)
      goto fail;
   memset(hdr, 0, sizeof(hdr));
   memcpy(hdr, "VCL1", 4);
   hdr[4] = (uint8_t)C.w;
   hdr[5] = (uint8_t)(C.w >> 8);
   hdr[6] = (uint8_t)C.h;
   hdr[7] = (uint8_t)(C.h >> 8);
   put32(hdr + 8, n);
   put32(hdr + 12, us_per_tick);
   put32(hdr + 16, (uint32_t)total);

   f = fopen(path, "wb");
   if (!f)
      goto fail;
   C.st.saved = n;
   ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
        fwrite(offs, 4, (size_t)n + 1, f) == (size_t)n + 1 &&
        fwrite(C.scratch, 1, first_len, f) == first_len;
   for (i = 1; ok && i < n; i++)
      ok = fwrite(C.ring + ent(first + i)->off, 1, ent(first + i)->len, f) ==
           ent(first + i)->len;
   if (fclose(f) != 0)
      ok = 0;
   if (!ok)
      remove(path);
   free(a);
   free(b);
   free(offs);
   return ok ? 0 : -1;
fail:
   free(a);
   free(b);
   free(offs);
   return -1;
}

/* ---- the player ------------------------------------------------------------ */

struct clip_player
{
   FILE *f;                 /* open while the frames are being read in */
   unsigned w, h, n, us;
   uint32_t *off;           /* n + 1 */
   uint8_t *data;
   size_t len, have;
   uint16_t *buf[2];
   int cur;
   unsigned pos;
   int bad;
};

void clip_close(clip_player *p)
{
   if (!p)
      return;
   if (p->f)
      fclose(p->f);
   free(p->off);
   free(p->data);
   free(p->buf[0]);
   free(p->buf[1]);
   free(p);
}

clip_player *clip_open(const char *path)
{
   uint8_t hdr[CLIP_HDR], *raw = NULL;
   clip_player *p;
   unsigned i;

   p = (clip_player *)calloc(1, sizeof(*p));
   if (!p)
      return NULL;
   p->f = fopen(path, "rb");
   if (!p->f || fread(hdr, 1, sizeof(hdr), p->f) != sizeof(hdr) ||
       memcmp(hdr, "VCL1", 4) != 0)
      goto fail;
   p->w = (unsigned)hdr[4] | (unsigned)hdr[5] << 8;
   p->h = (unsigned)hdr[6] | (unsigned)hdr[7] << 8;
   p->n = get32(hdr + 8);
   p->us = get32(hdr + 12);
   p->len = get32(hdr + 16);
   if (!p->w || !p->h || p->w > CLIP_W_MAX || p->h > CLIP_H_MAX || !p->n ||
       p->n > CLIP_FRAMES_MAX || p->len > CLIP_FILE_MAX ||
       p->us < 1000 || p->us > 100000)
      goto fail;
   raw = (uint8_t *)malloc(((size_t)p->n + 1) * 4);
   p->off = (uint32_t *)malloc(((size_t)p->n + 1) * sizeof(uint32_t));
   p->data = (uint8_t *)malloc(p->len ? p->len : 1);
   p->buf[0] = (uint16_t *)malloc((size_t)p->w * p->h * 2);
   p->buf[1] = (uint16_t *)malloc((size_t)p->w * p->h * 2);
   if (!raw || !p->off || !p->data || !p->buf[0] || !p->buf[1] ||
       fread(raw, 4, (size_t)p->n + 1, p->f) != (size_t)p->n + 1)
      goto fail;
   for (i = 0; i <= p->n; i++)
   {
      p->off[i] = get32(raw + 4 * (size_t)i);
      if ((i == 0 && p->off[0] != 0) ||
          (i > 0 && p->off[i] < p->off[i - 1] + 4))
         goto fail;
   }
   if (p->off[p->n] != p->len)
      goto fail;
   free(raw);
   return p;
fail:
   free(raw);
   clip_close(p);
   return NULL;
}

int clip_load(clip_player *p, size_t budget)
{
   size_t want, got;
   if (!p || p->bad)
      return -1;
   if (!p->f)
      return 1;
   want = p->len - p->have;
   if (want > budget)
      want = budget;
   got = want ? fread(p->data + p->have, 1, want, p->f) : 0;
   p->have += got;
   if (got < want)
   {
      p->bad = 1;
      return -1;
   }
   if (p->have == p->len)
   {
      fclose(p->f);
      p->f = NULL;
      return 1;
   }
   return 0;
}

unsigned clip_w(const clip_player *p) { return p ? p->w : 0; }
unsigned clip_h(const clip_player *p) { return p ? p->h : 0; }
unsigned clip_frames(const clip_player *p) { return p ? p->n : 0; }
unsigned clip_us_per_tick(const clip_player *p) { return p ? p->us : 0; }
unsigned clip_pos(const clip_player *p) { return p ? p->pos : 0; }

void clip_restart(clip_player *p)
{
   if (p)
      p->pos = 0;
}

const uint16_t *clip_next(clip_player *p, unsigned *ticks, int *ended)
{
   int nb;
   unsigned t = 0;
   *ended = 0;
   if (!p || p->bad || p->pos >= p->n)
   {
      *ended = 1;
      return NULL;
   }
   if (p->off[p->pos + 1] > p->have)
      return NULL;
   nb = p->cur ^ 1;
   if (dec_frame(p->data + p->off[p->pos], p->off[p->pos + 1] - p->off[p->pos],
                 p->pos ? p->buf[p->cur] : NULL, p->buf[nb], p->w, p->h,
                 &t) != 0)
   {
      p->bad = 1;
      *ended = 1;
      return NULL;
   }
   p->cur = nb;
   p->pos++;
   if (ticks)
      *ticks = t;
   return p->buf[p->cur];
}
