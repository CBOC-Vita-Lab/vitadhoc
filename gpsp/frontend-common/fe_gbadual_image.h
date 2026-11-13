/* fe_gbadual_image.h -- instance B's static image (GBA-LINK-MIRROR.md 19):
 * the bounds the partial-link script marks (vita/gba_instance_b.ld,
 * psp/gbalink_core_b.ld), the consistency check that the copies are made
 * with, and the copies themselves.  A header so that
 * tools/tests/test_gbadual_image.c compiles the SAME code against a partial
 * link of its own.
 *
 *   data_start .. data_end        B's .data (put back before every load)
 *   pristine .. pristine_end      the copy of it, inside B's .bss, behind the
 *                                 zeroed range (pristine_end: vita script
 *                                 only; NULL = the script does not mark it)
 *   bss_start .. bss_lend         B's zero-initialised state
 *
 * WHY THE COPIES ARE NOT memcpy/memset UNDER AddressSanitizer.  Each range
 * spans MANY globals, and with the core built -fsanitize=address every one
 * has a poisoned redzone after it, so the interceptors report the span as a
 * global-buffer-overflow although the range is exactly the section.  The
 * sizes are right (fe_bimage_check proves it, and the build checks it too);
 * what is wrong is only that this is a copy of an image rather than an access
 * to an object.  When the sanitizer runtime is linked the copies are
 * therefore plain volatile loops that no instrumentation sees (as
 * gbcore_tgbdual.c image_copy for the GB core); every other build keeps
 * memcpy/memset. */
#ifndef FE_GBADUAL_IMAGE_H
#define FE_GBADUAL_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* GCC 4.8 (-fsanitize=address) and clang know the attribute; the PSP's old
 * compiler has no sanitizer and no need for it. */
#if defined(__clang__) || (defined(__GNUC__) && \
    (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)))
#define FE_BIMAGE_NOSAN __attribute__((no_sanitize_address, noinline))
#else
#define FE_BIMAGE_NOSAN __attribute__((noinline))
#endif

typedef struct
{
   char *data_start, *data_end;
   char *pristine;
   char *pristine_end;             /* NULL: not marked by this script */
   char *bss_start;
   char *bss_lend;
} fe_bimage;

/* Defined by libasan/compiler-rt when -fsanitize=address is on the link
 * line, so a build that is not sanitized sees NULL. */
extern void __asan_init(void) __attribute__((weak));

#define FE_BIMAGE_ABSENT 1      /* no instance B in this image (no markers) */

/* 0: consistent.  FE_BIMAGE_ABSENT: none of the markers exist.  Otherwise
 * -1 and *why names what disagrees. */
static inline int fe_bimage_check(const fe_bimage *b, const char **why)
{
   const char *w = "";
   if (!b->data_start && !b->data_end && !b->pristine && !b->bss_start &&
       !b->bss_lend)
      return FE_BIMAGE_ABSENT;
   if (!b->data_start || !b->data_end || !b->pristine || !b->bss_start ||
       !b->bss_lend)
      w = "some of instance B's section markers are missing "
          "(partial link without the script?)";
   else if (b->data_end < b->data_start)
      w = "bdata_end below bdata_start";
   else if (b->bss_lend < b->bss_start)
      w = "bregion_lend below bregion_start";
   else if (b->pristine < b->bss_lend)
      w = "the pristine .data copy overlaps the zeroed range";
   else if (b->pristine_end &&
            (size_t)(b->pristine_end - b->pristine) !=
            (size_t)(b->data_end - b->data_start))
      w = "the pristine copy's room is not the size of B's .data";
   if (!*w)
      return 0;
   if (why)
      *why = w;
   return -1;
}

FE_BIMAGE_NOSAN
static void fe_bimage_raw_copy(char *d, const char *s, size_t n)
{
   volatile char *dd = (volatile char *)d;
   const volatile char *ss = (const volatile char *)s;
   while (n--)
      *dd++ = *ss++;
}

FE_BIMAGE_NOSAN
static void fe_bimage_raw_zero(char *d, size_t n)
{
   volatile char *dd = (volatile char *)d;
   while (n--)
      *dd++ = 0;
}

static inline void fe_bimage_copy(char *d, const char *s, size_t n)
{
   if (&__asan_init)
      fe_bimage_raw_copy(d, s, n);
   else
      memcpy(d, s, n);
}

static inline void fe_bimage_zero(char *d, size_t n)
{
   if (&__asan_init)
      fe_bimage_raw_zero(d, n);
   else
      memset(d, 0, n);
}

/* Boot: B's .data aside, once. */
static inline void fe_bimage_save(const fe_bimage *b)
{
   fe_bimage_copy(b->pristine, b->data_start,
                  (size_t)(b->data_end - b->data_start));
}

/* B fresh: .data back from the copy, the zeroed range cleared. */
static inline void fe_bimage_restore(const fe_bimage *b)
{
   fe_bimage_copy(b->data_start, b->pristine,
                  (size_t)(b->data_end - b->data_start));
   fe_bimage_zero(b->bss_start, (size_t)(b->bss_lend - b->bss_start));
}

#endif
