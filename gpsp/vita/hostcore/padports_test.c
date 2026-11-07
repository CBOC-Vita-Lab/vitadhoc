/* padports_test.c -- which sceCtrl port is player 2 (../pad_port_pick.h):
 * the table of setups the rule is written for.  Exit 0 = all agree. */
#include <stdio.h>
#include "../pad_port_pick.h"

enum { NONE = 0, PHY = 1, VIRT = 2, DS3 = 4, DS4 = 8 };

static const struct
{
   const char *what;
   unsigned char type[5];
   int tv, want;
} T[] = {
   { "handheld, nothing paired",              { PHY, 0, 0, 0, 0 },        0, 0 },
   { "handheld, one DS4 on port 1",           { PHY, DS4, 0, 0, 0 },      0, 1 },
   { "handheld, one DS3 on port 1",           { PHY, DS3, 0, 0, 0 },      0, 1 },
   { "handheld, two pads on 1 and 2",         { PHY, DS4, DS4, 0, 0 },    0, 2 },
   { "handheld, two pads on 1 and 3",         { PHY, DS4, 0, DS3, 0 },    0, 3 },
   { "handheld, a plugin's pad on 2 only",    { PHY, 0, DS4, 0, 0 },      0, 2 },
   { "PS TV, no pad",                         { VIRT, 0, 0, 0, 0 },       1, 0 },
   { "PS TV, one DS4 (port 0 and 1 the same)",{ VIRT, DS4, 0, 0, 0 },     1, 0 },
   { "PS TV, one DS3",                        { VIRT, DS3, 0, 0, 0 },     1, 0 },
   { "PS TV, two DS4",                        { VIRT, DS4, DS4, 0, 0 },   1, 2 },
   { "PS TV, DS3 + DS4",                      { VIRT, DS3, DS4, 0, 0 },   1, 2 },
   { "PS TV, four pads",                      { VIRT, DS4, DS4, DS4, DS4 },1, 2 },
   { "PS TV, pad 2 gone, pad 3 still on",     { VIRT, DS4, 0, DS4, 0 },   1, 3 },
   { "PS TV, only port 4 paired",             { VIRT, DS4, 0, 0, DS4 },   1, 4 },
};

int main(void)
{
   unsigned i, bad = 0;
   for (i = 0; i < sizeof(T) / sizeof(T[0]); i++)
   {
      int got = plat_pick_pad2_port(T[i].type, T[i].tv);
      printf("%s %s -> port %d\n", got == T[i].want ? "ok  " : "FAIL",
             T[i].what, got);
      bad += got != T[i].want;
   }
   return bad != 0;
}
