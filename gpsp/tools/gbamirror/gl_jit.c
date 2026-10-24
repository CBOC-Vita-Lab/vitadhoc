/* gl_jit.c -- where the test rig puts the translation caches (ARM rig only;
 * Makefile.armhf links it INSTEAD of the core's memmap.c).
 *
 * The core asks for its code memory with map_jit_block(size) from
 * retro_init (libretro.c, MMAP_JIT_CACHE): one block, the ROM cache followed
 * by the RAM cache.  memmap.c answers with a block next to .text.  On the
 * PS Vita the block comes from sceKernelAllocMemBlockForVM and can be
 * anywhere, so this file answers the way the Vita does, on demand:
 *
 *   GL_JIT=far    (default) every block more than GL_JIT_FAR_MIN from .text
 *                 and from every block handed out before (instance A's and
 *                 instance B's caches are far from the program AND from each
 *                 other, as two VM blocks are);
 *   GL_JIT=split  far, and gl_host then moves each instance's RAM cache into
 *                 a block of its own, again more than GL_JIT_FAR_MIN from
 *                 everything else (gl_jit_split_ram).  ROM-cache code and
 *                 RAM-cache code can then not reach each other with a 24-bit
 *                 branch: only an ARM_FAR_STUBS core runs this, through its
 *                 veneers (arm/arm_emit.h);
 *   GL_JIT=near   next to .text, as memmap.c does it.
 *
 * map_jit_block is an UNDEFINED symbol of the core relocatable, so instance
 * A and the renamed instance B both call this one copy -- the same sharing
 * the Vita frontend's allocator has.  Each call therefore returns a new
 * block; nothing here is per instance.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "gl_jit.h"

#define MB (1024u * 1024u)
/* 24-bit ARM branches reach +-32 MiB; twice that leaves no doubt. */
#define GL_JIT_FAR_MIN (64u * MB)
#define GL_JIT_STEP    (96u * MB)

#define MAX_BLOCKS 8
static struct { uintptr_t lo, hi; } blk[MAX_BLOCKS];
static unsigned nblk;
static int mode = -1;   /* 0 near, 1 far, 2 split */

static uintptr_t text_ref(void) { return (uintptr_t)&map_jit_block; }

int gl_jit_mode(void)
{
   if (mode < 0)
   {
      const char *e = getenv("GL_JIT");
      mode = 1;
      if (e && !strcmp(e, "near")) mode = 0;
      else if (e && !strcmp(e, "split")) mode = 2;
      else if (e && strcmp(e, "far"))
      {
         fprintf(stderr, "GL_JIT=%s? (near|far|split)\n", e);
         exit(2);
      }
   }
   return mode;
}

static uintptr_t gap(uintptr_t lo, uintptr_t hi, uintptr_t lo2, uintptr_t hi2)
{
   if (hi <= lo2) return lo2 - hi;
   if (hi2 <= lo) return lo - hi2;
   return 0;
}

/* Smallest gap from [lo,hi) to .text and to every block handed out. */
static uintptr_t clearance(uintptr_t lo, uintptr_t hi)
{
   uintptr_t t = text_ref(), best = gap(lo, hi, t, t + 1);
   unsigned i;
   for (i = 0; i < nblk; i++)
   {
      uintptr_t g = gap(lo, hi, blk[i].lo, blk[i].hi);
      if (g < best) best = g;
   }
   return best;
}

static void *map_at(uintptr_t hint, unsigned size)
{
   void *p = mmap((void *)hint, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
   return p == MAP_FAILED ? NULL : p;
}

static void *map_far(unsigned size, const char *what)
{
   uintptr_t base = (text_ref() & ~(uintptr_t)(MB - 1));
   unsigned i;
   /* Above .text first, then below, in steps that also keep the blocks
    * apart from each other. */
   for (i = 1; i < 40; i++)
   {
      int64_t off = (int64_t)((i + 1) / 2) * GL_JIT_STEP * ((i & 1) ? 1 : -1);
      int64_t at = (int64_t)base + off;
      void *p;
      if (at < (int64_t)(16 * MB) || at + size > (int64_t)0xB0000000u)
         continue;
      if (clearance((uintptr_t)at, (uintptr_t)at + size) < GL_JIT_FAR_MIN)
         continue;
      p = map_at((uintptr_t)at, size);
      if (!p)
         continue;
      {
         uintptr_t c = clearance((uintptr_t)p, (uintptr_t)p + size);
         if (c >= GL_JIT_FAR_MIN && nblk < MAX_BLOCKS)
         {
            blk[nblk].lo = (uintptr_t)p;
            blk[nblk].hi = (uintptr_t)p + size;
            nblk++;
            printf("jit: %s %u KiB at %p, %ld MiB from .text, >= %lu MiB from "
                   "anything else\n", what, size / 1024, p,
                   (long)(((int64_t)(uintptr_t)p - (int64_t)text_ref()) /
                          (int64_t)MB), (unsigned long)(c / MB));
            return p;
         }
      }
      munmap(p, size);
   }
   fprintf(stderr, "jit: no far block for %s\n", what);
   return NULL;
}

void *map_jit_block(unsigned size)
{
   if (gl_jit_mode() == 0)
   {
      /* memmap.c's search: 4 MiB steps either side of .text */
      uintptr_t base = text_ref() & ~(uintptr_t)(4 * MB - 1);
      unsigned i;
      for (i = 0; i < 1024; i++)
      {
         int64_t at = (int64_t)base +
                      (int64_t)((i & 1) ? 1 : -1) * (int64_t)(i >> 1) * (int64_t)(4 * MB);
         void *p;
         if (at <= 0)
            continue;
         p = map_at((uintptr_t)at, size);
         if (p)
         {
            printf("jit: caches %u KiB at %p, %ld KiB from .text (near)\n",
                   size / 1024, p,
                   (long)(((int64_t)(uintptr_t)p - (int64_t)text_ref()) / 1024));
            return p;
         }
      }
      return NULL;
   }
   return map_far(size, "caches");
}

void unmap_jit_block(void *bufptr, unsigned size)
{
   munmap(bufptr, size);
}

/* GL_JIT=split: give one instance's RAM cache a block of its own. */
int gl_jit_split_ram(unsigned char **ram_cache, unsigned ram_size)
{
   void *p;
   if (gl_jit_mode() != 2)
      return 0;
   p = map_far(ram_size, "RAM cache (split)");
   if (!p)
      return -1;
   *ram_cache = (unsigned char *)p;
   return 1;
}
