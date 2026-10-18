/* xbrz_c.cpp -- xBRZ behind two C functions (vita/upscale_vita.c). */
#include "xbrz.h"

extern "C" void xbrz_c_init(void)
{
	xbrz::init();
}

extern "C" void xbrz_c_dist_mode(int mode)
{
	xbrz::setDistMode(mode);
}

/* Rows [y0, y1) of the w x h source into the factor-times-larger target.
 * Pixels are R,G,B,A bytes; the target comes out opaque.  Slices of one
 * picture may run on different threads (xbrz.h). */
extern "C" void xbrz_c_scale(int factor, const uint32_t *src, uint32_t *dst,
                             int w, int h, int y0, int y1)
{
	xbrz::scale(static_cast<size_t>(factor), src, dst, w, h,
	            xbrz::ColorFormat::RGB, xbrz::ScalerCfg(), y0, y1);
}

/* ...and columns [x0, x1) of those rows only. */
extern "C" void xbrz_c_scale_rect(int factor, const uint32_t *src,
                                  uint32_t *dst, int w, int h, int y0, int y1,
                                  int x0, int x1)
{
	xbrz::scaleRect(static_cast<size_t>(factor), src, dst, w, h, y0, y1, x0, x1);
}

/* ...written at `at` (source pixel (x0, y0)'s block), `pitch` pixels a row:
 * a rectangle placed anywhere in a larger target. */
extern "C" void xbrz_c_scale_rect_to(int factor, const uint32_t *src, int w,
                                     int h, int y0, int y1, int x0, int x1,
                                     uint32_t *at, int pitch)
{
	xbrz::scaleRect(static_cast<size_t>(factor), src, at, w, h, y0, y1, x0, x1,
	                at, pitch);
}
