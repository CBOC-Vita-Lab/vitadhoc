/* Instance B's boot copies against the section bounds (frontend-common/
 * fe_gbadual_image.h; run_gbadual_image_test.sh builds it).
 *
 * The program links a stand-in core that was partially linked with
 * vita/gba_instance_b.ld and renamed gbab_*, built with the sanitizers,
 * takes the same copies fe_gbadual_boot / b_fresh take, and proves
 *   - the markers are consistent and the copies leave no byte of B's .data
 *     or .bss unrestored (and touch nothing outside them),
 *   - the consistency check refuses every way the markers can disagree,
 *   - (--naive) that a plain memcpy over the same range IS what
 *     AddressSanitizer reports (global-buffer-overflow in memcpy), the
 *     report that this header's copies exist to avoid.
 * Exit 0 = pass. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../frontend-common/fe_gbadual_image.h"

extern char gbab_bdata_start[], gbab_bdata_end[], gbab_bdata_pristine[],
            gbab_bdata_pristine_end[], gbab_bregion_start[],
            gbab_bregion_lend[];
extern uint32_t gbab_core_sum(void);
extern void gbab_core_scribble(void);

/* Reads of a range that spans redzones (the test looks at the image as the
 * copies do). */
__attribute__((no_sanitize_address, noinline))
static int all_zero(const char *p, size_t n)
{
   const volatile char *v = p;
   while (n--) if (*v++) return 0;
   return 1;
}
__attribute__((no_sanitize_address, noinline))
static unsigned char peek(const char *p) { return *(const volatile unsigned char *)p; }

static fe_bimage image(void)
{
   fe_bimage b;
   b.data_start = gbab_bdata_start;   b.data_end = gbab_bdata_end;
   b.pristine = gbab_bdata_pristine;  b.pristine_end = gbab_bdata_pristine_end;
   b.bss_start = gbab_bregion_start;  b.bss_lend = gbab_bregion_lend;
   return b;
}

static int bad(fe_bimage b)
{
   const char *why = "";
   return fe_bimage_check(&b, &why) == -1 && *why;
}

int main(int argc, char **argv)
{
   fe_bimage b = image(), x;
   const char *why = "";
   uint32_t sum0, sum1;
   size_t n = (size_t)(b.data_end - b.data_start), z = (size_t)(b.bss_lend - b.bss_start);
   unsigned char guard_before, guard_after;

   if (argc > 1 && !strcmp(argv[1], "--naive"))
   {   /* what fe_gbadual_boot used to do */
      memcpy(b.pristine, b.data_start, n);
      puts("naive memcpy over B's .data: NOT reported");
      return 0;
   }

   printf("B image: .data %zu, zeroed %zu, pristine room %zu\n", n, z,
          (size_t)(b.pristine_end - b.pristine));
   assert(n > 100 && z > 1000);
   assert(fe_bimage_check(&b, &why) == 0);
   assert((size_t)(b.pristine_end - b.pristine) == n);
   assert(b.pristine >= b.bss_lend);

   sum0 = gbab_core_sum();
   fe_bimage_save(&b);                  /* boot */
   gbab_core_scribble();                /* a session */
   sum1 = gbab_core_sum();
   assert(sum1 != sum0);
   /* the copy's neighbours: the byte after the room is B's padding or the
    * next object's, and must not change */
   guard_before = peek(b.pristine_end);
   fe_bimage_restore(&b);               /* b_fresh */
   guard_after = peek(b.pristine_end);
   assert(guard_before == guard_after);
   /* .data is what boot saw; .bss is zero (the stand-in's .bss started zero) */
   assert(gbab_core_sum() == sum0);
   assert(all_zero(b.bss_start, z));
   gbab_core_scribble(); fe_bimage_restore(&b);
   assert(gbab_core_sum() == sum0);

   /* every disagreement is refused */
   x = b; x.pristine_end = b.pristine_end + 1;          assert(bad(x));
   x = b; x.pristine_end = b.pristine_end - 1;          assert(bad(x));
   x = b; x.pristine = b.bss_lend - 1;                  assert(bad(x));
   x = b; x.data_end = b.data_start - 1;                assert(bad(x));
   x = b; x.bss_lend = b.bss_start - 1;                 assert(bad(x));
   x = b; x.bss_start = NULL;                           assert(bad(x));
   x = b; x.pristine = NULL;                            assert(bad(x));
   x = b; x.pristine_end = NULL;                        assert(fe_bimage_check(&x, NULL) == 0);
   memset(&x, 0, sizeof(x));        assert(fe_bimage_check(&x, NULL) == FE_BIMAGE_ABSENT);
   puts("test_gbadual_image: ok");
   return 0;
}
