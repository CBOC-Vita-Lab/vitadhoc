/* audio_vita.h -- ring buffer, resampler and rate follow between the core's
 * sound and the Vita's audio port.
 *
 *   emulation thread:  audio_push() from the core's audio callback,
 *                      audio_frame() once per emulated frame
 *   audio thread:      renders AUDIO_GRAIN output frames from the ring and
 *                      blocks in the port write (plat_audio_write)
 *
 * The core produces exactly 1/59.7275 s of sound per emulated frame, at
 * whatever rate fe_host_sample_rate() reports (32768 Hz by default).  The
 * port plays 48000 Hz off its own crystal, and the frontend runs one
 * emulated frame per display refresh (59.94 or 60.00 Hz), so sound arrives
 * 0.36-0.46 % faster than a literal 32768:48000 resampler drains it.
 * audio_frame() closes that loop: it steers the resampler's step by the
 * ring's fill, at most AUDIO_FOLLOW_PPM either way, which is far below what
 * an ear places as pitch.  (psp/main_psp.c solves the same mismatch by
 * dropping a displayed frame every few seconds, and uses a rate follow like
 * this one only in GB link sessions.)
 */
#ifndef GBADHOC_AUDIO_VITA_H
#define GBADHOC_AUDIO_VITA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_OUT_RATE    48000u   /* the port's rate                        */
#define AUDIO_GRAIN       512u     /* frames per port write: 10.7 ms         */
#define AUDIO_TARGET_MS   60u      /* ring fill the follow holds: the latency
                                    * cushion, about 3.5 emulated frames     */
#define AUDIO_TARGET_MAX_MS 120u   /* ...grown to this after gaps: each gap
                                    * adds AUDIO_TARGET_STEP_MS, and half a
                                    * minute without one takes 5 ms back    */
#define AUDIO_TARGET_STEP_MS 20u
#define AUDIO_FOLLOW_PPM  10000    /* +-1 %: covers 60.00 Hz vs 59.7275
                                    * (4560 ppm) with room to pull back      */

/* Open the port and start the audio thread.  in_rate = the core's sample
 * rate; speed_x1e6 = how fast the emulation is paced relative to the GBA's
 * own 59.7275 Hz, in millionths (1003560 when locked to a 59.94 Hz panel,
 * 1000000 when paced by the clock): the resampler's starting point, so the
 * follow only has to correct what is left.  0 = ok; on failure the game
 * runs silent and audio_push drops everything. */
int  audio_start(unsigned in_rate, unsigned speed_x1e6);
void audio_stop(void);

/* The core's samples, interleaved L/R.  Never blocks: a full ring drops the
 * tail (counted). */
void audio_push(const int16_t *stereo_lr, size_t frames);
/* Once per emulated frame, after the core ran: update the rate follow. */
void audio_frame(void);
/* Forget what is queued (after a state load or a resume: it would play
 * stale).  Safe from the emulation thread. */
void audio_flush(void);

typedef struct audio_stats
{
   unsigned in_rate;      /* Hz                                             */
   unsigned level;        /* input frames queued now                        */
   unsigned level_avg;    /* the follow's smoothed level                    */
   unsigned target;       /* input frames the follow aims at (it grows after
                           * gaps, AUDIO_TARGET_MAX_MS)                     */
   int      follow_ppm;   /* current correction, +fast / -slow              */
   unsigned underruns;    /* times the ring ran dry (one gap each)          */
   unsigned starved;      /* output frames of silence played for those gaps */
   unsigned dropped;      /* input frames dropped on a full ring            */
   unsigned trims;        /* times a backlog was cut back to the target     */
   unsigned port_errors;  /* failed port writes (the port is then reopened) */
} audio_stats;
void audio_get_stats(audio_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_AUDIO_VITA_H */
