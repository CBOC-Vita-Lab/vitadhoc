/* rewind_vita.h -- hold a button and the game runs backwards.
 *
 * HOW IT IS KEPT.  Every few emulated frames the machine's save state is
 * taken (fe_host_state_mem_save: 416 KiB for a GBA game, 20-190 KiB for a
 * GB/GBC one).  Only the NEWEST state is kept whole.  Every older one exists
 * as the difference to the one after it -- the two states XORed, which is
 * nearly all zeros, with the zero runs squeezed out (rw_encode below) -- in
 * one ring of bytes of a fixed size.  Going back one step is XORing the
 * newest difference into the newest state, which turns it into the one
 * before; when the ring is full the oldest difference is dropped, and the
 * only thing lost is the oldest moment.
 *
 * HOW FAR BACK.  The player picks a length (Settings > Rewind: 30 s, 1, 3
 * or 5 minutes).  The ring is a budget, not a promise, so the INTERVAL
 * between states follows from it: as short as REWIND_IV_MIN frames while
 * the differences are small, longer (up to REWIND_IV_MAX) when a busy game
 * makes them large, so that the length asked for fits.  A game too busy
 * even for that simply has less history; the chip on screen always says how
 * far back the player really is.
 *
 * WHAT A STEP COSTS THE GAME.  Taking a state is the frame thread's (the
 * core cannot run meanwhile); XORing and squeezing it is done by the I/O
 * worker thread when there is one (rewind_set_async), so the frame thread
 * pays for one state copy every interval and nothing else.  With no worker
 * (the twin's default) it all happens on the frame thread.
 *
 * THE BATTERY.  A GBA state does not hold the cartridge's battery memory.
 * Rewinding across an in-game save without it would leave the game's RAM
 * saying "not saved yet" over a cartridge that says otherwise, and what the
 * game then writes belongs to neither.  So the battery image travels with
 * the states: whenever the game has written to it between two states
 * (fe_host_battery_gen), its difference is stored with that step, and a step
 * back puts the cartridge back as well; the .sav file follows at the save
 * writer's next look.  A GB/GBC state carries its cartridge RAM itself.
 *
 * WHAT IS ON SCREEN.  A state holds the machine, not its picture.  So a
 * step back restores the state and runs ONE frame of it (no buttons, no
 * sound): that frame is the picture shown, and when the button is let go
 * the game simply carries on from it -- exactly from the frame on screen.
 *
 * Portable C against fe_host.h, plat.h and app_vita.h: no Sony call, so it
 * is the same file in the twin, where rewind_tests.py exercises it.
 */
#ifndef GBADHOC_REWIND_VITA_H
#define GBADHOC_REWIND_VITA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The memory the whole feature may take (config.ini rewind_mb): the ring
 * plus the two whole states and the battery copies beside it.  24 MiB of
 * the Vita's 128 MiB heap: a second 32 MiB cartridge for the local link,
 * both machines' buffers and the artwork still fit beside it with room
 * (heap_vita.c has the sum). */
#define REWIND_MB_DEFAULT  24
#define REWIND_MB_MIN      4
#define REWIND_MB_MAX      64
/* ...and what it always leaves of the heap, whatever the budget says: room
 * for a second 32 MiB cartridge and its machine's buffers (a cable session
 * with another game).  A heap with less than this to spare beyond the
 * minimum budget gets no rewind (plat_heap_free). */
#define REWIND_HEAP_RESERVE_MB 40
/* config.ini rewind = seconds of history asked for; 0 = off. */
#define REWIND_SECONDS_DEFAULT 60
#define REWIND_SECONDS_MAX     300

/* Frames between two states: never closer than MIN (a 416 KiB state is a
 * copy the frame thread makes), never further apart than MAX (half a
 * second: where the game continues from is that coarse). */
#define REWIND_IV_MIN  4
#define REWIND_IV_MIN_SMALL 2      /* GB/GBC: the state is a tenth the size */
#define REWIND_IV_MAX  30

/* ---- the session ----------------------------------------------------------- */

/* The player's settings: seconds of history (0 = off) and the budget in
 * MiB.  Any time; with a game running the buffers are rebuilt when either
 * changed (the history is then gone). */
void rewind_configure(int seconds, int budget_mb);
/* A game is running (after fe_host_boot): take the memory.  Without it --
 * the setting is off, or the heap said no -- every call below does
 * nothing. */
void rewind_game_begin(void);
/* The game is going away (before fe_host_shutdown, the worker stopped). */
void rewind_game_end(void);
/* Compress on another thread: `wake` makes rewind_worker() run there soon
 * (plat_worker_wake).  NULL = on the frame thread, inside rewind_capture.
 * Set it once the worker runs; clear it AFTER the worker has stopped (what
 * it had not finished is then finished here). */
void rewind_set_async(void (*wake)(void));
/* The worker thread's part: call from its step function, every pass. */
void rewind_worker(void);

/* Forget the history: a save state was loaded, a link session began --
 * anything after which "a moment ago" is not this timeline. */
void rewind_clear(void);

/* ---- the game loop --------------------------------------------------------- */

/* After every emulated frame of ordinary play (fast forward included). */
void rewind_capture(void);

/* Once per iteration, menu closed, before the core would run.
 *   want    the rewind control is held
 *   locked  rewinding is not possible now (a link session, the local link)
 * Returns nonzero while it owns the iteration: the game went back a step
 * (the restored frame was run and drawn through fe_host's sinks, with
 * g_app_keys cleared) or stands still between two steps (the last picture
 * was drawn again).  The caller presents and does NOT run the core.  0 =
 * play on.  The top-right chip (osd_chip_ff) is this function's while it
 * returns nonzero; it is cleared when the rewind ends. */
int  rewind_frame(int want, int locked);
/* 1 while the game is running backwards: its frames are silent. */
int  rewind_active(void);
/* Something the player should be told, once: "Rewind is off during a link
 * session", "This is as far back as it goes".  NULL = nothing. */
const char *rewind_notice(void);

/* ---- numbers ---------------------------------------------------------------- */

typedef struct rewind_stats
{
   int      on;              /* buffers exist                                */
   unsigned seconds;         /* history asked for                            */
   unsigned ring_bytes;      /* the ring                                     */
   unsigned total_bytes;     /* everything allocated                         */
   unsigned state_bytes;     /* one whole state                              */
   unsigned interval;        /* frames between states, now                   */
   unsigned entries;         /* steps held                                   */
   unsigned frames;          /* emulated frames they span                    */
   unsigned used_bytes;      /* ring bytes they occupy                       */
   unsigned captures;        /* states taken since the game began            */
   unsigned skipped;         /* ...put off a frame: the worker was in the
                              * middle of the one before                     */
   unsigned late;            /* states the frame thread packed itself: the
                              * worker had not begun them an interval later  */
   unsigned dropped;         /* steps that fell off the ring's old end       */
   uint64_t raw_bytes;       /* state bytes that went in                     */
   uint64_t packed_bytes;    /* ring bytes that came out                     */
   unsigned battery_steps;   /* steps that carry a battery difference        */
   uint64_t capture_us;      /* frame thread: taking states (plat_time_us)   */
   unsigned capture_max_us;
   uint64_t pack_us;         /* XOR + squeeze (the worker's, when async)     */
   unsigned pack_max_us;
   unsigned rewinds;         /* times the player rewound                     */
   unsigned steps_back;      /* steps taken backwards                        */
   unsigned landings;        /* states restored into the machine             */
   uint64_t landing_us;      /* restoring + the one frame, in total          */
   unsigned landing_max_us;
} rewind_stats;
void rewind_get_stats(rewind_stats *out);

/* ---- the squeeze (also the unit test's entry points) ------------------------
 * rw_encode writes the difference of two n-byte buffers: byte-wise XOR, as
 * [skip][count][count XOR bytes]... with both numbers as LEB128, runs of
 * fewer than three equal bytes folded into the literal run around them.
 * Never more than RW_BOUND(n) bytes; returns the length (0 only for n = 0;
 * two equal buffers encode to nothing... see the .c), or -1 when `cap` is
 * too small.  rw_apply XORs an encoded difference into `buf`, which turns
 * either of the two buffers into the other; -1 if the stream is damaged
 * (`buf` may then be half changed). */
#define RW_BOUND(n) ((n) + (n) / 64 + 16)
long rw_encode(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *out,
               size_t cap);
int  rw_apply(uint8_t *buf, size_t n, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_REWIND_VITA_H */
