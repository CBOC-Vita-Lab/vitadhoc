/*
 * Temporal frame blend for presenting the WonderSwan's 75.4717 Hz output on a 59.94 Hz display (portable C with a
 * NEON path; no Vita calls, so the Linux twin and qemu-arm test it). Copyright CBOC-Vita. MIT licence.
 *
 * Model: emulated frame j is on the (virtual) LCD during [j*Te, (j+1)*Te), a display refresh k shows the average of the
 * LCD over [k*Td, (k+1)*Td). The weight of frame j in refresh k is therefore the overlap of those two intervals divided by
 * Td: EXACT rational arithmetic (64 bit integer ticks of 1/(emu_num*disp_num) s), no accumulated float error, so the
 * pattern repeats forever. Weights are then rounded to 8 bits (sum 256, largest remainder) and applied per colour channel
 * with round-to-nearest. With 3072000/40704 Hz and 60000/1001 Hz a refresh covers 1.259 emulated frames: it blends
 * 2 frames, or 3 when it straddles two boundaries.
 *
 * Pacing contract: before each refresh, call wsb_frames_needed() and run that many emulated frames (1 or 2, 1.259 on
 * average), wsb_submit() each, then wsb_present().
 */
#ifndef WSWAN_BLEND_H
#define WSWAN_BLEND_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define WSB_MAX_FRAMES 4

typedef struct wsb {
	uint64_t te, td;        /* ticks per emulated frame / per display refresh */
	uint64_t t;             /* start of the next refresh, in ticks */
	uint64_t emu_done;      /* frames submitted so far */
	unsigned pixels;        /* w * h */
	uint16_t *ring;         /* WSB_MAX_FRAMES frames, j % 4 */
	uint16_t *prev;         /* last output, for the optional LCD persistence */
	int have_prev;
	int ghost;              /* 0 = off .. 255: share of the previous output kept (LCD persistence) */
} wsb;

/* emu rate emu_num/emu_den Hz (3072000 / 40704), display disp_num/disp_den Hz (60000 / 1001). Returns 0, else 1. */
int wsb_init(wsb *b, unsigned w, unsigned h, uint64_t emu_num, uint64_t emu_den, uint64_t disp_num, uint64_t disp_den);
void wsb_free(wsb *b);
void wsb_reset(wsb *b);                       /* forget the phase (after a pause / a state load) */
int wsb_frames_needed(const wsb *b);          /* emulated frames to run before the next refresh (0..3) */
void wsb_submit(wsb *b, const uint16_t *frame);   /* copies the frame (w*h RGB565) */
/* The frames and 8 bit weights of the next refresh: idx[i] = frame number, w[i] sums to 256; returns the count (1..3). */
int wsb_weights(const wsb *b, uint64_t idx[3], unsigned w[3]);
/* Blend into out (w*h) and advance one refresh. Returns the number of frames mixed. Missing frames (the emulator fell
 * behind) are replaced by the newest one. */
int wsb_present(wsb *b, uint16_t *out);

/* kernels, public for the tests: out = (a*wa + b*wb + c*wc + 128) >> 8 per channel (wa + wb + wc must be 256) */
void wsb_mix3_scalar(uint16_t *out, const uint16_t *a, const uint16_t *b, const uint16_t *c, unsigned wa, unsigned wb, unsigned wc, size_t n);
void wsb_mix3(uint16_t *out, const uint16_t *a, const uint16_t *b, const uint16_t *c, unsigned wa, unsigned wb, unsigned wc, size_t n);
int wsb_has_neon(void);

#ifdef __cplusplus
}
#endif
#endif
