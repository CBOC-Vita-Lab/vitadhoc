/* video_obj.h -- the OBJ X coordinate of an OAM entry (attr1, bits 0..8).
 *
 * X is a 9-bit two's complement number, -256..255.  The sign is extended by
 * shifting it to the top of a 32-bit word and back.  The shift is done on an
 * UNSIGNED value: shifting a non-negative int so that its sign bit is set is
 * undefined behaviour in C (-fsanitize=undefined reports "left shift of 16484
 * by 23 places cannot be represented in type 'int'", video.cc).  The unsigned
 * shift is defined; the conversion back to a signed type is implementation-
 * defined (modular on every compiler this builds with) and the arithmetic
 * right shift of a negative value is implementation-defined too.  Neither is
 * undefined, and the result is the same on every input as the original
 * expression, which tools/tests/test_video_obj.c checks over all 65536 attr1
 * values.
 */
#ifndef GPSP_VIDEO_OBJ_H
#define GPSP_VIDEO_OBJ_H

#include <stdint.h>

static inline int32_t obj_attr1_x(uint32_t attr1)
{
  return (int32_t)((uint32_t)attr1 << 23) >> 23;
}

#endif
