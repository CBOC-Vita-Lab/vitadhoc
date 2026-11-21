/* Quick response (run-ahead by one frame) in the app: the decision, with playerkit's pk_ahead.h (MIT) as the governor.  The frames
 * themselves are fe_host.c's (fe_host_ahead_set / gb_ahead_frame): Game Boy, Game Boy Color, NES and WonderSwan.
 *
 * Off never; On whenever nothing blocks it; Auto while the emulating thread has the time (it starts when the projected cost of a
 * run-ahead frame is at most half a frame, stops over 70 % or after 3 late frames in 120, and waits before trying again).
 * Blocked = off at once: a link of either kind, rewind, fast forward, Steady pacing's half mode, no machine that can. */
#ifndef GBADHOC_AHEAD_GLUE_H
#define GBADHOC_AHEAD_GLUE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { AHEAD_MODE_OFF = 0, AHEAD_MODE_AUTO = 1, AHEAD_MODE_ON = 2 };

/* A game starts: the mode, one frame in microseconds, and the cost numbers the console starts from (ratio_x100: a run-ahead frame
 * costs this share of a plain one, the first guess until measured; overhead_us: the state copy) */
void ahead_begin(int mode, uint32_t frame_us, int ratio_x100, uint32_t overhead_us);
void ahead_set_mode(int mode);
int  ahead_mode(void);
/* Before a pass of play: 1 = run this frame ahead.  `blocked` as above. */
int  ahead_decide(int blocked);
/* After a normal pass (not fast forward, not blocked): the emulating thread's time for the frame (microseconds), and whether it ran ahead */
void ahead_cost(uint32_t emu_us, int ran_ahead);
/* once a second: "ahead: mode=... on=.. frames=a/b plain_ms=.. ahead_ms=.. ratio=.. starts=.. stops=.." and the census restarts; 0 when off and idle */
int  ahead_census_line(char *buf, size_t n);
int  ahead_on(void);

#ifdef __cplusplus
}
#endif
#endif
