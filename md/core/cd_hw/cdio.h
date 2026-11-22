/* VitAdhoc: the disc file layer of the Sega CD (md/core/cd_hw/cdio.c). Copyright CBOC-Vita. MIT licence for this file.
 *
 * What cdd.c reads a disc with: .cue / .bin / .iso / .wav / .ogg track files through stdio-like calls (cdStream* in osd.h), and a .chd through
 * libchdr one hunk at a time. This layer
 *   - reads with positioned reads (pread / sceIoPread: a stream has no hidden state to lose on a seek, and no buffer a seek throws away),
 *   - keeps a small cache of fixed blocks per streamed file (16 KB) and per CHD (8 hunks),
 *   - and, when the platform gave the core a thread (md_host_set_threads with MD_THREAD_DISC), reads the next blocks of every stream, and
 *     decodes the next CHD hunks (LZMA / zlib / zstd / FLAC, CRC check, ECC), on a helper thread: after sector n is served, the next
 *     blocks are fetched in the background and the emulating thread only copies out of a finished block. It waits for the helper only when
 *     the block it needs is being read at that very moment; a block that is not there is read by the emulating thread itself, exactly the
 *     bytes asked for (a "miss", counted).
 * The bytes the emulating thread gets are the bytes of the file in every case: a cache block is a copy of the file's bytes at a fixed offset
 * and a CHD hunk is what libchdr's chd_read returns (cdio_debug_check compares every cached answer with a direct read).
 * Without a helper thread (no ops, or the thread could not start) a miss fills a whole block on the emulating thread, as a buffered stream does. */
#ifndef CDIO_H
#define CDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct md_thread_ops;
typedef struct cdio_file cdio_file;

/* stdio-like; every function accepts a NULL stream (a track whose FILE line was not understood reads nothing) */
cdio_file *cdio_open(const char *path);
int cdio_close(cdio_file *f);
size_t cdio_read(void *p, size_t size, size_t n, cdio_file *f);
int cdio_seek(cdio_file *f, long off, int whence);
long cdio_tell(cdio_file *f);
char *cdio_gets(char *s, int n, cdio_file *f);
/* the file is a track that is played / streamed: read ahead on the helper (when there is one) */
void cdio_stream(cdio_file *f);

/* CHD: opens the CHD on the stream (which stays the track file the core closes) and a second decoder of the same file for the helper */
struct _chd_file;
int cdio_chd_open(cdio_file *f, struct _chd_file **out);        /* libchdr's chd_error, 0 = ok */
int cdio_chd_read(struct _chd_file *chd, uint32_t hunknum, uint8_t *dst);
void cdio_chd_close(struct _chd_file *chd);                      /* NULL is fine */

/* the helper thread: ops from md_host_set_threads (NULL = none). begin after the disc is loaded, end before it is closed */
void cdio_set_ops(const struct md_thread_ops *ops, int enabled);
void cdio_begin(void);
void cdio_end(void);
int cdio_helper_running(void);

/* debug: check = compare every block served from the cache with a fresh read (mismatches are counted); stress = the helper sleeps for a
 * random while before and after every block, and the emulating thread sometimes ignores a finished block (more races, more waits) */
void cdio_debug(int check, int stress);

/* census since the disc was loaded:
 * [0] reads (calls that touched a streamed file or the CHD)  [1] served from a finished block  [2] misses (read by the emulating thread)
 * [3] waits for a block the helper was reading  [4] us the emulating thread spent in misses and waits
 * [5] blocks the helper finished  [6] us the helper was busy (reading, decoding)  [7] blocks read ahead and never used  [8] check mismatches
 * [9] timeouts: waits for the helper given up after about 2 ms (the emulating thread read the bytes itself), plus closes that left a stuck helper's cache to it */
void cdio_census(unsigned long out[10]);

#ifdef __cplusplus
}
#endif
#endif
