/* video_vita_priv.h -- shared between video_vita.c and video_vita_text.c.
 * Not an API: frontends include psp/video_psp.h and vita/video_vita.h. */
#ifndef VIDEO_VITA_PRIV_H
#define VIDEO_VITA_PRIV_H

#include <stdint.h>
#include "vback.h"

/* The PSP's logical screen is 480x272 and the Vita's panel exactly twice
 * that: one logical pixel is VV_S native pixels. */
#define VV_S 2

/* Text (video_vita_text.c).  x, y are NATIVE pixels: the top-left of the
 * line box.  Widths come back in LOGICAL pixels and equal the PSP's. */
int  vv_text_init(void);
void vv_text_term(void);
int  vv_text_width(int hd, const char *s);
void vv_text_draw(int hd, int nx, int ny, const char *s, uint32_t tint);
void vv_text_cap(int *top, int *h);          /* logical rows of 'H' (ui)  */
/* Self-check: the worst per-glyph gap between the natural advance at the
 * native size and the advance used (2 x the PSP's), in native pixels. */
float vv_text_worst_dev(int hd, int *ch, float *mean_abs);

/* video_vita.c: called by anything about to draw into the frame. */
void vv_frame_need(void);

#endif /* VIDEO_VITA_PRIV_H */
