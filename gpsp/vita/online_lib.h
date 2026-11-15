/* online_lib.h -- find the game of a room in the library, without blocking
 * (2.0 online play).  Owner: package U (online_lib.c).  Match by cartridge
 * code first, then by file identity.  Plan: ONLINE-2.0-PLAN.md section 2.5. */
#ifndef ONLINE_LIB_H
#define ONLINE_LIB_H

/* APP_PATH_CAP is app_vita.h's (512); not included here so that the host
 * bench and the unit test can use this header on its own. */
#ifndef APP_PATH_CAP
#define APP_PATH_CAP 512
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_LIB_MAX 8
typedef struct online_lib_hit { char path[APP_PATH_CAP]; char rom[9]; int rom_match; int patched; } online_lib_hit;
/* console "gba" "gb" "gbc" (milestone B: "nes" "snes" "ws", where `game` is the CRC).  `rom` may be "". */
int  online_lib_begin(const char *console, const char *game, const char *rom);
/* Look at up to `files` more files (8 a frame).  0 searching, 1 finished. */
int  online_lib_step(int files);
int  online_lib_hits(online_lib_hit *out, int max);   /* code matches found so far, hash matches first */
/* Compressed .zip files the walk could not read (their header lies inside
 * deflated data): the page may say so.  Valid until online_lib_end. */
int  online_lib_skipped(void);
void online_lib_end(void);
/* pure, unit-tested: what the card page does with the hits.
 * ONLINE_PICK_NONE: nothing has that code.  ONLINE_PICK_OPEN: open hits[0] without asking.
 * ONLINE_PICK_ASK: show the list.   cable = kind is a cable mode (1, 2, 5). */
enum { ONLINE_PICK_NONE = 0, ONLINE_PICK_OPEN, ONLINE_PICK_ASK };
int  online_lib_decide(const online_lib_hit *hits, int n, int cable);

#ifdef __cplusplus
}
#endif

#endif /* ONLINE_LIB_H */
