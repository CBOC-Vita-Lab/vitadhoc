/*
 * VitAdhoc: the FM worker thread (write log). Copyright CBOC-Vita; the core keeps the Genesis Plus GX licence (md/core/LICENSE.txt).
 *
 * The YM2612 (MAME core, the default chip) makes the FM samples of a frame from the register writes the 68000 and the Z80 sent it. With the worker
 * on, the emulating thread records those writes, the chip resets and the "run n samples" calls of sound.c in a single producer / single consumer
 * queue, in the order they happened, and the worker feeds them to the unchanged chip (sound/ym2612.c) on its own core, writing the samples into
 * the same buffer the single thread would have filled. Same calls in the same order give the same samples.
 *
 * What stays on the emulating thread, exactly as before: the YM2612 BUSY flag (fm_cycles_busy in sound.c), the position of the FM clock
 * (fm_cycles_count, which fixes how many samples each call asks for), and the timer flags of the status register: a game polls them and times
 * its music and logic by them. They come from a small copy of the timer part of the chip (YM2612Shadow*, sound/ym2612.c) that sees the same
 * writes and sample counts; with the check on the twin compares it with the real chip at every join.
 *
 * Joins: the mixer joins at the end of every frame (sound_update, before it reads the samples), and state save / load, chip options and a reset
 * of the sound run between frames with the worker idle. The wait is bounded: when the worker has not caught up after FMW_BOUND_US (or the queue is
 * full) the emulating thread takes the rest of the queue over itself, with the same code on the same chip, so the sound never drops and never differs.
 *
 * Not for the Nuked YM2612 / YM3438 option (its status needs the chip clock by clock) and not for the YM2413 of the 8 bit systems: those run inline.
 */
#ifndef FM_THREAD_H
#define FM_THREAD_H

#include "thread_ops.h"

extern int fmw_on;                                   /* 1 while the worker runs */
void fmw_set_ops(const md_thread_ops *ops, int want);   /* the platform's thread functions and whether the worker is wanted (kept across loads) */
int fmw_start(int *buffer);                          /* buffer: the FM sample buffer of sound.c; 0 on success */
void fmw_stop(void);                                 /* joins, ends the thread */
void fmw_join(void);                                 /* every queued call is done (bounded wait, then the emulating thread finishes the queue) */
void fmw_q_write(unsigned a, unsigned v);            /* YM2612Write(a, v) */
void fmw_q_run(int samples, int offset);             /* YM2612Update(buffer + offset, samples) */
void fmw_q_reset(void);                              /* YM2612ResetChip() */
int fmw_wanted(void);

#define FMW_BOUND_US 5000                            /* the longest the emulating thread waits at a join before it takes the queue over */

/* debug (the twin's --fm-check / --fm-stress / --fm-inline / --fm-tiny): bit 0 compare the timer shadow with the chip at every join, bit 1 a join
 * after every run, bit 2 always take the queue over at a join (the fallback path), bit 3 a tiny queue (the queue-full path) */
void fmw_set_debug(int flags);
unsigned long fmw_check_errors(void);

typedef struct {
	unsigned long records, writes, runs, samples, joins, join_wait_polls, takeovers, full_waits, wakes;
	unsigned long fm_us, join_wait_us;     /* the worker's busy time, the emulating thread's time waiting in joins (0 without ops->now_us) */
} fmw_census_t;
void fmw_census(fmw_census_t *c);

#endif
