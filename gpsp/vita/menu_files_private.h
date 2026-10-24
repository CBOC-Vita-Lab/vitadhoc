#ifndef GBADHOC_MENU_FILES_PRIVATE_H
#define GBADHOC_MENU_FILES_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#define MENU_FILES_BUDGET (48u * 1024u * 1024u)
enum { MF_OPEN, MF_STAT, MF_FSTAT, MF_READ, MF_SEEK, MF_CLOSE };

/* Independent atomic counters, not a transactional snapshot. Only the menu
 * worker performs loading; main may sample these without logging from it. */
typedef struct menu_files_diag
{
   uint32_t calls, stat_calls, fstat_calls, open_calls, read_calls, seek_calls;
   uint32_t close_calls, bytes, stat_us, read_us, meta_us, decode_us;
   uint32_t resample_us, max_io_us, peak_bytes, live_bytes, thread_tag;
} menu_files_diag;
void menu_files_get_diag(menu_files_diag *out);

#ifdef MENU_FILES_TEST
/* The hook can delay an operation or return an errno; never in production. */
void menu_files_test_hook(int (*hook)(unsigned, const char *));
void menu_files_test_limit(size_t bytes);
#endif
#endif
