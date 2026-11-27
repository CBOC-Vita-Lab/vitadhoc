/*
 * Fast forward at normal pitch: the game runs `factor` times as fast but the sound does not.
 *
 * The audio of the emulated frames of one display refresh arrives as `factor` chunks (one per emulated frame). Only the last chunk of the
 * group is played, so the speaker gets one frame's worth of sound per refresh (the audio pacer sees the same number of samples as at normal
 * speed) and it plays at the game's own pitch. The other chunks are dropped, and the seam is smoothed: the sound that would have
 * followed the last thing played is the head of the group's FIRST chunk, and the head of the played chunk is faded in from it over
 * PK_FFN_XFADE samples. The output therefore starts exactly where the previous output ended (no step) and ends in the played
 * chunk's own stream; the only difference to a straight cut is the short fade. Plain C so the app (C) and the players (C++) share it.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_FFNORM_H
#define PK_FFNORM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PK_FFN_XFADE 128   /* samples (frames) faded over; about 3 ms at 44.1 kHz */

typedef struct pk_ffnorm {
	int factor;                        /* 2 or 3 chunks per group */
	int idx;                           /* chunks of this group seen */
	int head_n;                        /* frames kept of the group's first chunk */
	int16_t head[PK_FFN_XFADE * 2];
} pk_ffnorm;

static inline void pk_ffnorm_reset(pk_ffnorm *s, int factor)
{
	s->factor = factor < 2 ? 2 : factor > 8 ? 8 : factor;
	s->idx = 0;
	s->head_n = 0;
}

/* One emulated frame's audio (`frames` stereo L,R int16 pairs) in place. Returns the number of frames to play: 0 for a chunk that
 * is dropped, `frames` for the one that is played. */
static inline size_t pk_ffnorm_chunk(pk_ffnorm *s, int16_t *lr, size_t frames)
{
	if (s->idx < s->factor - 1) {
		if (s->idx == 0) {
			s->head_n = (int) (frames < PK_FFN_XFADE ? frames : PK_FFN_XFADE);
			memcpy(s->head, lr, (size_t) s->head_n * 2 * sizeof(int16_t));
		}
		s->idx++;
		return 0;
	}
	{
		int n = s->head_n < (int) frames ? s->head_n : (int) frames, i;
		for (i = 0; i < n; i++) {
			/* weight of the new chunk: i / n (0 at the seam: exactly the continuation of what was played) */
			int32_t w = i, v = n - i;
			lr[i * 2 + 0] = (int16_t) (((int32_t) s->head[i * 2 + 0] * v + (int32_t) lr[i * 2 + 0] * w) / n);
			lr[i * 2 + 1] = (int16_t) (((int32_t) s->head[i * 2 + 1] * v + (int32_t) lr[i * 2 + 1] * w) / n);
		}
	}
	s->idx = 0;
	s->head_n = 0;
	return frames;
}

#endif
