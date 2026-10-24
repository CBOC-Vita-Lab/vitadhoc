/* clip_vita.h -- the game list's previews: the last ten seconds you played.
 *
 * WHAT IT IS.  While a game runs, every second frame the core presents is
 * kept (30 a second) in a ring of memory, squeezed, for the last twenty
 * seconds.  When the game is left, ten seconds of it go to
 * clips/<game>.clip in the data folder: the last ten -- or, when the game
 * stood still for a while before the player quit (a pause screen, a
 * dialogue left open), the ten up to where it went still and a second of
 * the calm after it ("something happened": the player pressed a button,
 * or more than a fiftieth of the picture changed).  The clip sits next to the scene the home screen
 * already shows (scenes/<game>.shot).  When the home screen's row rests on
 * a game for about a second, that clip fades in where the still picture was
 * and plays, then fades back into the still -- which is the clip's own last
 * frame, or nearly -- and starts again a little later.  Real frames of the player's own game, not a trailer: what the
 * Vita's own LiveArea cannot do.
 *
 * HOW IT IS SQUEEZED.  A frame is said in terms of the one before it.  A
 * game's picture mostly either stays where it was (the status bar, a menu,
 * the player's sprite in the middle of a scrolling map) or moves all at
 * once (the map, scrolled a pixel or two); so each frame names ONE motion
 * for the whole picture (found by trying the likely ones on a sample of
 * pixels) and then is a list of runs:
 *
 *    SKIP n     the next n pixels are where they were
 *    MOVE n     ...are the frame before's, (dx, dy) away
 *    LIT  n     n pixels follow, as they are
 *    FILL n     one pixel follows, n times
 *
 * Every two seconds a frame is said whole (a key: LIT and FILL only), so
 * that the oldest two seconds can be let go of as a block when the ring is
 * full or holds more than it needs.  Lossless: a frame decodes to exactly
 * the core's pixels (libretro RGB565).  A walk across a Pokemon map is a few
 * KiB a frame; a clip is typically 0.5-2 MiB.
 *
 * THREADS.  As rewind_vita.c: the frame thread only copies the frame into
 * one slot (77 KiB, a fraction of a millisecond); the I/O worker squeezes it
 * into the ring.  With no worker it is all done on the frame thread.  A
 * frame that finds the slot still full is simply not kept (the next one
 * says how long it stood for).
 *
 * THE FILE (all little-endian):
 *    0   "VCL1"
 *    4   u16 width, u16 height                  (at most 256 x 224)
 *    8   u32 frames
 *   12   u32 microseconds per tick              (a tick is one core frame)
 *   16   u32 bytes of frame data
 *   20   12 bytes, zero
 *   32   u32 offsets[frames + 1]                (into the data; [0] = 0)
 *        the data: each frame is
 *          u8 kind ('K' or 'D'), u8 ticks it stands for, s8 dx, s8 dy,
 *          then the runs: a byte (op << 6 | n - 1, with n - 1 = 63 meaning
 *          63 + a LEB128 number more), and LIT's / FILL's pixels as u16.
 *
 * Portable C (stdio, plat.h on the recording side only): the same file in
 * the twin, where app_tests.py records and plays a clip.
 */
#ifndef GBADHOC_CLIP_VITA_H
#define GBADHOC_CLIP_VITA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLIP_W_MAX     256
#define CLIP_H_MAX     224
#define CLIP_TICKS     600     /* what a clip keeps: 10 s of core frames   */
#define CLIP_TICKS_MIN 180     /* a shorter recording leaves the old clip  */
#define CLIP_HISTORY_TICKS 1200 /* what the ring keeps to choose from: 20 s */
#define CLIP_ACTIVE_DIV 50     /* a frame with more than 1/50 of its pixels
                                * changed is one in which something happened */
#define CLIP_CALM_TICKS 60     /* the calm a clip keeps after the last one */
#define CLIP_EVERY     2       /* a frame kept every 2 core frames: 30/s   */
#define CLIP_KEY_EVERY 60      /* a key every 60 kept frames: 2 s          */
#define CLIP_RING_MB   6       /* the ring: the 20 s of most games easily  */
#define CLIP_FILE_MAX  (12u * 1024u * 1024u)

/* ---- recording (the frame thread, but clip_worker) ------------------------- */

/* A game starts / ends.  `on` = config.ini previews.  begin takes the
 * ring (6 MiB) when the heap can spare it; end gives everything back
 * (whatever was not saved is gone). */
void clip_game_begin(int on);
void clip_game_end(void);

/* Every frame the core ran and presented, in real time (not in fast
 * forward, not while the game runs backwards: those frames are not the
 * game as it is played).  `pix` is the console's picture, libretro RGB565;
 * `keys` the buttons the game was given (a frame the player pressed
 * something in is one in which something happened, however little of the
 * picture changed).  A gap (a cut) is fine: the next frame kept is said
 * against the last. */
void clip_frame(const uint16_t *pix, unsigned w, unsigned h,
                size_t pitch_bytes, unsigned keys);

/* Something happened that the frames since are not a continuation of (a
 * state loaded, the game reset): the next frame kept is a key. */
void clip_cut(void);

/* The I/O worker's part, and how to call it (NULL = none: the frame thread
 * squeezes). */
void clip_set_async(void (*wake)(void));
void clip_worker(void);

/* The last ten seconds to `path` (written whole or not at all).  0 = a
 * clip was written; 1 = too little was recorded to replace what is there;
 * -1 = it could not be written.  `us_per_tick`: the core's frame time. */
int  clip_save(const char *path, unsigned us_per_tick);

/* What the recorder holds (tests and the log). */
typedef struct clip_stats
{
   unsigned frames, keys;     /* in the ring now                           */
   unsigned ticks;            /* the time they cover, in core frames       */
   size_t   bytes;            /* their squeezed size                       */
   unsigned kept, skipped;    /* frames squeezed / let go (slot busy)      */
   unsigned enc_us_max;       /* the slowest squeeze                       */
   uint64_t enc_us_sum;
   unsigned saved;            /* frames in the last clip_save's file       */
   unsigned calm_cut;         /* ...and the ticks of calm left off its end */
} clip_stats;
void clip_get_stats(clip_stats *out);

/* ---- playing (the UI thread) ------------------------------------------------ */

typedef struct clip_player clip_player;

/* Opens a clip: its header and offsets are read and checked here, its
 * frames by clip_load as the screen has time.  NULL = no clip there, or
 * not one this reads. */
clip_player *clip_open(const char *path);
void clip_close(clip_player *p);

/* Reads up to `budget` more bytes of the file.  1 = all of it is in,
 * 0 = more to come, -1 = it went wrong (close it). */
int  clip_load(clip_player *p, size_t budget);

unsigned clip_w(const clip_player *p);
unsigned clip_h(const clip_player *p);
unsigned clip_frames(const clip_player *p);
unsigned clip_us_per_tick(const clip_player *p);

/* The next frame, decoded: its pixels (libretro RGB565, clip_w() to a
 * row) and how many ticks it stands for.  NULL with *ended = 0: not read
 * in yet; NULL with *ended = 1: the clip is over (or broken). */
const uint16_t *clip_next(clip_player *p, unsigned *ticks, int *ended);
/* Back to the first frame. */
void clip_restart(clip_player *p);
/* The frame index clip_next returns next. */
unsigned clip_pos(const clip_player *p);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_CLIP_VITA_H */
