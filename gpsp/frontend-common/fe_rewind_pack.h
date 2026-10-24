/* fe_rewind_pack.h -- the "pack" half of a rewind snapshot, as one pure
 * function over one job record (fe_rewind.h; docs/REWIND.md).
 *
 * Pack compares the machine's current state, given as a list of spans,
 * against the reference image, appends the difference to the delta ring as
 * one record, and brings the reference up to date IN PLACE (only the words
 * that differ are written).  It reads and writes nothing but what the job
 * points at -- no globals, no libc beyond memcpy, no clock -- so the same
 * file can be compiled for a second CPU and handed the job there.
 *
 * Record format: a sequence of
 *     u16 skip      words equal in both images (skipped pages included)
 *     u16 count     words that follow
 *     u32 xor[count]
 * ending wherever the record's length says.  Offsets are words of the image,
 * which is the spans laid end to end. */
#ifndef FE_REWIND_PACK_H
#define FE_REWIND_PACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One piece of the state image. */
typedef struct fe_rewind_span
{
   uint32_t       bytes;        /* a multiple of 4 */
   /* Where this piece's CURRENT content is when a snapshot is taken: the
    * machine's own memory (nothing is copied), or a buffer the gather
    * callback refreshes.  Word-aligned. */
   const void    *cur;
   /* Optional page map, valid at pack time: dirty[p] == 0 means page p of
    * this span has not changed since the last snapshot and is not even
    * read.  NULL = compare everything.  A map that says 0 for a page that
    * did change loses that change: it must be exact or conservative. */
   const uint8_t *dirty;
   uint32_t       page_shift;   /* log2(bytes per page), >= 2 */
} fe_rewind_span;

typedef struct fe_rewind_rec
{
   uint32_t base;   /* frame of the snapshot this delta leads back TO */
   uint32_t off;    /* ring offset of its first byte */
   uint32_t len;    /* bytes */
} fe_rewind_rec;

typedef struct fe_rewind_packjob
{
   /* in */
   const fe_rewind_span *spans;
   uint32_t       nspans;
   uint32_t      *ref;          /* the reference image, updated in place */
   uint8_t       *ring;
   uint32_t       ring_bytes;
   /* The ring may be in two pieces of memory: bytes [0, ring_split) at
    * `ring`, the rest at `ring2`.  ring_split == ring_bytes = one piece. */
   uint8_t       *ring2;
   uint32_t       ring_split;
   fe_rewind_rec *rec;          /* index ring, oldest at rec_tail */
   uint32_t       rec_cap;
   uint32_t       base_frame;   /* frame of the reference before this pack */
   /* 1 = read every page whatever the maps say.  For a pack whose reference
    * was moved (stepped back) without the machine following it: the maps
    * describe writes since the machine and the reference last agreed, which
    * is then not the question. */
   uint32_t       ignore_dirty;
   /* in / out */
   uint32_t       head, used, rec_tail, rec_n;
   /* out */
   uint32_t       len;          /* bytes of the record written */
   uint32_t       evicted;      /* old records dropped to make room */
   uint32_t       overflow;     /* 1 = the record alone exceeded the ring:
                                 * the ring is empty, the reference is
                                 * current, the history starts here */
   uint32_t       words_read;   /* words actually compared (cost counter) */
} fe_rewind_packjob;

void fe_rewind_pack_run(fe_rewind_packjob *j);

#ifdef __cplusplus
}
#endif

#endif /* FE_REWIND_PACK_H */
