/* me_host.h — user-side driver for the Media Engine (ADR-0080).
 *
 * The kernel PRX (psp/me/gbadhoc_me.prx, shipped beside the EBOOT) boots
 * the ME into a mailbox dispatcher; this is the app side: load, handshake,
 * post jobs, watch the heartbeat.  Everything here is NON-BLOCKING by
 * design — the emulation thread checks and drops, never waits (the frame
 * loop's cardinal rule, and the entire point of the offload).
 */
#ifndef ME_HOST_H
#define ME_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the PRX from <base_dir>/gbadhoc_me.prx, start it with the mailbox,
 * and wait up to ~250 ms for the ME's boot handshake.  Returns 0 on a live
 * ME, negative on any failure (missing prx, load refused, no handshake) —
 * failure is ALWAYS non-fatal to the app, which continues single-core.
 * Emits EVT me_init state=... either way. */
int  me_host_init(const char *base_dir);
/* Standby needs the ME's kernel-side sleep handler installed; without it the
 * console cold-boots on wake.  Call with 1 before me_host_init to make that
 * install MANDATORY -- the module then refuses to start rather than booting
 * into a configuration that silently cannot sleep.  Default 0: attempt it,
 * ignore failure, behave exactly as before on any model. */
void me_host_require_sysevent(int on);
int  me_host_up(void);
/* Module lifetime, independent of watchdog health. A failed watchdog does
 * not reset the ME or release its buffers. Remains true if shutdown fails. */
int  me_host_loaded(void);
void me_host_shutdown(void);
/* The watchdog declared the engine dead (it fell back to the CPU). */
int  me_host_was_dead(void);

/* Stage-0 microbenchmark: times ME copies of `len` bytes across access
 * modes against the same copy on the main CPU, verifies checksums, and
 * emits EVT me_bench lines.  Call only at boot (allocates/frees ~2*len).
 * Safe to call with the ME down (emits nothing, returns -1). */
int  me_host_bench(unsigned len, unsigned iters);

/* Non-blocking job interface (Stage 1 video staging).
 * me_host_post_pitch_copy: returns 0 if posted, -1 if the ME is busy or
 * down (caller drops the frame).  me_host_idle: 1 when the last posted
 * job has completed. */
int  me_host_post_pitch_copy(const void *src, void *dst,
                             unsigned rows, unsigned row_bytes,
                             unsigned src_pitch, unsigned dst_pitch);
int  me_host_idle(void);

/* Post ME_CMD_RENDER (renderer offload) with an ME_UNCACHED me_render_desc addr;
 * caller writes back the snapshot buffers first.  me_host_result() = last
 * command's return (the frame checksum for a render). */
int  me_host_post_render(unsigned int desc_uncached);
unsigned int me_host_result(void);
int  me_host_input_done(void);   /* v2: ME consumed the live inputs */
/* v3: the render that just completed was REFUSED -- the ME found no usable
 * work area in its desc and drew nothing (me/me_mbox.h).  Ask once the
 * command is idle, before presenting its stage. */
int  me_host_render_refused(void);
/* v3: have the ME write back and drop its whole D-cache, and wait for it
 * (bounded by timeout_us).  Returns 0 when it is done, -1 if the engine is
 * down, busy or did not answer in time -- in which case the caller must NOT
 * free memory the ME has been writing.  Never called from the frame loop. */
int  me_host_sync(unsigned timeout_us);

/* Watchdog: call once per frame while using the ME.  Returns 0 while
 * healthy; nonzero once the ME has missed its deadline enough times that
 * the caller must fall back to the main-CPU path for the session.  Emits
 * EVT me_watchdog on the transition. */
int  me_host_watchdog_frame(void);

/* Diagnostics only (stall watch): raw mailbox words, read uncached.  Any
 * pointer may be NULL; all outputs read 0 while the ME is down. */
void me_host_diag(unsigned *cmd_seq, unsigned *done_seq, unsigned *input_seq,
                  unsigned *heartbeat);

#ifdef __cplusplus
}
#endif

#endif /* ME_HOST_H */
