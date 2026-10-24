/* fe_rewind_pack.c -- see fe_rewind_pack.h.  Self-contained on purpose. */
#include "fe_rewind_pack.h"

#include <string.h>

#define RUN_MAX 0xFFFFu

typedef struct
{
   fe_rewind_packjob *j;
   uint32_t off;        /* ring write position */
   uint32_t skip;       /* equal words not yet accounted for in a header */
   int      ok;         /* still writing a record (0 = it did not fit) */
} enc;

/* n bytes at ring offset `off`, which do not wrap; they may straddle the
 * two pieces of memory the ring is made of. */
static void ring_store(fe_rewind_packjob *j, uint32_t off, const uint8_t *src,
                       uint32_t n)
{
   if (off < j->ring_split)
   {
      uint32_t k = j->ring_split - off;
      if (k > n)
         k = n;
      memcpy(j->ring + off, src, k);
      src += k;
      off += k;
      n -= k;
   }
   if (n)
      memcpy(j->ring2 + (off - j->ring_split), src, n);
}

static void ring_put(enc *e, const void *src, uint32_t n)
{
   fe_rewind_packjob *j = e->j;
   uint32_t first = j->ring_bytes - e->off;
   if (first > n)
      first = n;
   ring_store(j, e->off, (const uint8_t *)src, first);
   if (n > first)
      ring_store(j, 0, (const uint8_t *)src + first, n - first);
   e->off += n;
   if (e->off >= j->ring_bytes)
      e->off -= j->ring_bytes;
}

/* Make room for n more bytes of the record being written (whose bytes so
 * far are already counted in `used`).  0 = the record alone does not fit. */
static int ring_room(enc *e, uint32_t n)
{
   fe_rewind_packjob *j = e->j;
   while (j->ring_bytes - j->used < n)
   {
      if (!j->rec_n)
         return 0;
      j->used -= j->rec[j->rec_tail].len;
      j->rec_tail = (j->rec_tail + 1) % j->rec_cap;
      j->rec_n--;
      j->evicted++;
   }
   return 1;
}

static void emit_hdr(enc *e, uint32_t skip, uint32_t count)
{
   uint16_t hdr[2];
   if (!e->ok)
      return;
   if (!ring_room(e, 4))
   {
      e->ok = 0;
      return;
   }
   hdr[0] = (uint16_t)skip;
   hdr[1] = (uint16_t)count;
   e->j->used += 4;
   e->j->len  += 4;
   ring_put(e, hdr, 4);
}

/* n words that differ somewhere: cur[0..n) against ref[0..n).  Writes the
 * run and brings ref up to date. */
static void emit_run(enc *e, const uint32_t *cur, uint32_t *ref, uint32_t n)
{
   uint32_t k;
   while (e->skip > RUN_MAX)
   {
      emit_hdr(e, RUN_MAX, 0);
      e->skip -= RUN_MAX;
   }
   emit_hdr(e, e->skip, n);
   e->skip = 0;
   if (e->ok && !ring_room(e, n * 4))
      e->ok = 0;
   if (e->ok)
   {
      e->j->used += n * 4;
      e->j->len  += n * 4;
   }
   for (k = 0; k < n; )
   {
      uint32_t x[64], m = n - k, i;
      if (m > 64)
         m = 64;
      for (i = 0; i < m; i++)
      {
         x[i] = cur[k + i] ^ ref[k + i];
         ref[k + i] = cur[k + i];
      }
      if (e->ok)
         ring_put(e, x, m * 4);
      k += m;
   }
}

static void scan(enc *e, const uint32_t *cur, uint32_t *ref, uint32_t n)
{
   uint32_t i = 0;
   e->j->words_read += n;
   while (i < n)
   {
      uint32_t lit;
      /* equal stretch: eight words at a time while nothing differs */
      while (i + 8 <= n &&
             !((cur[i]     ^ ref[i])     | (cur[i + 1] ^ ref[i + 1]) |
               (cur[i + 2] ^ ref[i + 2]) | (cur[i + 3] ^ ref[i + 3]) |
               (cur[i + 4] ^ ref[i + 4]) | (cur[i + 5] ^ ref[i + 5]) |
               (cur[i + 6] ^ ref[i + 6]) | (cur[i + 7] ^ ref[i + 7])))
      {
         i += 8;
         e->skip += 8;
      }
      while (i < n && cur[i] == ref[i])
      {
         i++;
         e->skip++;
      }
      if (i >= n)
         break;
      /* a run of differing words; one equal word between two different
       * ones rides along as a zero (same size as a new header) */
      lit = i;
      while (i < n && i - lit < RUN_MAX)
      {
         if (cur[i] == ref[i] && (i + 1 >= n || cur[i + 1] == ref[i + 1]))
            break;
         i++;
      }
      emit_run(e, cur + lit, ref + lit, i - lit);
   }
}

void fe_rewind_pack_run(fe_rewind_packjob *j)
{
   enc e;
   uint32_t s, start = j->head;
   uint32_t *ref = j->ref;

   e.j = j;
   e.off = j->head;
   e.skip = 0;
   e.ok = 1;
   j->len = j->evicted = j->overflow = j->words_read = 0;

   if (j->rec_n == j->rec_cap)
   {
      j->used -= j->rec[j->rec_tail].len;
      j->rec_tail = (j->rec_tail + 1) % j->rec_cap;
      j->rec_n--;
      j->evicted++;
   }

   for (s = 0; s < j->nspans; s++)
   {
      const fe_rewind_span *sp = &j->spans[s];
      const uint32_t *cur = (const uint32_t *)sp->cur;
      uint32_t words = sp->bytes / 4;
      if (!sp->dirty || j->ignore_dirty)
         scan(&e, cur, ref, words);
      else
      {
         uint32_t pw = 1u << (sp->page_shift - 2), p, at;
         for (p = 0, at = 0; at < words; p++, at += pw)
         {
            uint32_t n = words - at < pw ? words - at : pw;
            if (sp->dirty[p])
               scan(&e, cur + at, ref + at, n);
            else
               e.skip += n;
         }
      }
      ref += words;
   }

   if (!e.ok)
   {
      /* One delta larger than the whole ring.  Nothing older survives
       * (ring_room dropped it all) and the reference is current: the
       * history starts again from this snapshot. */
      j->used = 0;
      j->head = 0;
      j->rec_n = 0;
      j->rec_tail = 0;
      j->len = 0;
      j->overflow = 1;
   }
   else
   {
      fe_rewind_rec *r = &j->rec[(j->rec_tail + j->rec_n) % j->rec_cap];
      r->base = j->base_frame;
      r->off  = start;
      r->len  = j->len;
      j->rec_n++;
      j->head = e.off;
   }
}
