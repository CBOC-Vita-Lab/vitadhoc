/* online_lib.c -- find the game of a room in the library; see online_lib.h.
 *
 * W0 scaffold: nothing is found.  Package U writes the walk of the ROM
 * folders and the pure decision of the plan's 2.5. */
#include "online_lib.h"

int online_lib_begin(const char *console, const char *game, const char *rom)
{
   (void)console; (void)game; (void)rom;
   return -1;
}

int online_lib_step(int files)
{
   (void)files;
   return 1;
}

int online_lib_hits(online_lib_hit *out, int max)
{
   (void)out; (void)max;
   return 0;
}

void online_lib_end(void) { }

int online_lib_decide(const online_lib_hit *hits, int n, int cable)
{
   (void)hits; (void)n; (void)cable;
   return ONLINE_PICK_NONE;
}
