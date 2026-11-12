/* test_video_obj.c -- obj_attr1_x() (video_obj.h) against the 9-bit signed
 * reading of attr1 bits 0..8, for every 16-bit attr1 value.
 *
 *   gcc -O2 -fsanitize=undefined -fno-sanitize-recover=all \
 *       -o t tools/tests/test_video_obj.c && ./t           (x86, UBSan)
 *   arm-linux-gnueabihf-gcc -O2 -static -o t tools/tests/test_video_obj.c
 *       && qemu-arm ./t                                    (armhf)
 *   gcc -DOBJ_X_ORIGINAL -fsanitize=undefined -fno-sanitize-recover=all ...
 *       must FAIL: the original expression, the UB this check guards against.
 *
 * The reference reads bits 0..8 only (the other attr1 bits are size, flip
 * and affine fields) and sign-extends bit 8.  Exit 0 and "OK", or the first
 * mismatch.
 */
#include <stdio.h>
#include <stdint.h>

#ifdef OBJ_X_ORIGINAL
/* the expression as it was in video.cc, before the unsigned shift */
static int32_t obj_x_of(uint16_t attr1)
{
  return (int32_t)(attr1 << 23) >> 23;
}
#else
#include "../../video_obj.h"
static int32_t obj_x_of(uint16_t attr1)
{
  return obj_attr1_x(attr1);
}
#endif

int main(void)
{
  unsigned v;
  for (v = 0; v < 65536; v++) {
    int32_t want = (int32_t)(v & 0x1FF);
    int32_t got;
    if (want >= 256)
      want -= 512;
    got = obj_x_of((uint16_t)v);
    if (got != want) {
      printf("FAIL attr1=0x%04x: got %d, want %d\n", v, (int)got, (int)want);
      return 1;
    }
  }
  printf("OK: obj_attr1_x matches the 9-bit signed reading for all 65536 attr1 values\n");
  return 0;
}
