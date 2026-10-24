/* CPU-only preview decoding. The caller retains all GPU ownership. */
#ifndef GBADHOC_PREVIEW_VITA_H
#define GBADHOC_PREVIEW_VITA_H

#include <stdint.h>

typedef struct clip_preview_frame
{
   uint32_t token;
   unsigned w, h, ticks, us_per_tick, frames, sequence;
   int ended, bad;
   const uint16_t *pixels;
   unsigned slot;
} clip_preview_frame;

/* One dedicated process-lifetime worker. Failure means still-only previews. */
int clip_preview_start(void);
/* Copies path before returning. Tokens must be nonzero and never reused. */
int clip_preview_request(uint32_t token, const char *path);
void clip_preview_cancel(uint32_t token);
/* Replays the same selected clip from worker-owned memory, without reopening. */
int clip_preview_restart(uint32_t token);
/* Nonblocking. A successful take pins immutable pixels until release. */
int clip_preview_take(uint32_t token, clip_preview_frame *out);
void clip_preview_release(unsigned slot);
/* Process shutdown only: 1 stopped, 0 timed out with its storage left alive. */
int clip_preview_shutdown(unsigned timeout_us);

enum { CLIP_DIAG_IDLE, CLIP_DIAG_OPEN, CLIP_DIAG_READ, CLIP_DIAG_DECODE,
       CLIP_DIAG_SLOTS, CLIP_DIAG_CLOSE };
typedef struct clip_preview_diag {
   unsigned wanted, token, phase, age_us;
   unsigned open_us, read_us, decode_us, published, stale, bad, ready, pinned;
   int coherent;
} clip_preview_diag;
/* Bounded atomic snapshot, even while the worker is blocked in I/O. All log
 * formatting remains on main. Costs are process-lifetime maxima. */
void clip_preview_get_diag(clip_preview_diag *out);

#endif
