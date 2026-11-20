/* diskspace.h: the device of a path, 64-bit sizes, and the honest line. */
#include <stdio.h>
#include <stdlib.h>
#include "../../diskspace.h"

static uint64_t g_max, g_free;
static int g_ok = 1;
static char g_asked[16];
static int fake(const char *dev, uint64_t *mx, uint64_t *fr)
{
   snprintf(g_asked, sizeof(g_asked), "%s", dev);
   if (!g_ok) return 0;
   *mx = g_max; *fr = g_free;
   return 1;
}
static int fails;
static void check(int ok, const char *why)
{
   if (!ok) { fprintf(stderr, "FAIL: %s\n", why); fails++; }
}

int main(void)
{
   char dev[16], line[160], sz[24];
   uint64_t fr, mx;
   static const struct { const char *path, *dev; } d[] = {
      { "ux0:data/VitAdhoc/saves/A.sav", "ux0:" }, { "uma0:roms/A.gba", "uma0:" },
      { "imc0:data/x", "imc0:" }, { "ur0:data/x", "ur0:" }, { "grw0:x", "grw0:" },
      { "ux0:", "ux0:" }, { "roms/A.gba", "ux0:" }, { "ux0:a:b", "ux0:" },
   };
   unsigned i;
   ds_set_query(fake);
   for (i = 0; i < sizeof(d) / sizeof(d[0]); i++)
   {
      check(ds_device_of(d[i].path, dev, sizeof(dev)) && !strcmp(dev, d[i].dev), d[i].path);
   }
   /* 150 GB free on a 256 GB card: a 32-bit byte count wraps */
   g_max = 256ull << 30; g_free = 150ull << 30;
   check(ds_free_for("uma0:roms/A.gba", &fr, &mx) && fr == (150ull << 30) && !strcmp(g_asked, "uma0:"),
         "150 GB free is read in full from the path's own device");
   check((uint64_t)(uint32_t)g_free != g_free, "(the fixture really wraps in 32 bits)");
   ds_size_text(g_free, sz, sizeof(sz));
   check(!strcmp(sz, "150.0 GB"), "150 GB prints as such");
   ds_size_text(g_free + (300ull << 20), sz, sizeof(sz));
   check(!strcmp(sz, "150.2 GB"), "150.29 GB prints 150.2");
   ds_size_text(5ull << 30, sz, sizeof(sz)); check(!strcmp(sz, "5.0 GB"), "5 GB");
   ds_size_text(420ull << 20, sz, sizeof(sz)); check(!strcmp(sz, "420 MB"), "420 MB");
   ds_size_text(3 << 10, sz, sizeof(sz)); check(!strcmp(sz, "3 KB"), "3 KB");
   ds_failed_line("Save", "ux0:data/VitAdhoc/saves/A.sav", line, sizeof(line));
   check(strstr(line, "150.0 GB free") && strstr(line, "read-only") && !strstr(line, "is full"),
         "a failed write on a card with 150 GB free does not say the card is full");
   g_free = 1ull << 20;
   ds_failed_line("Save", "uma0:saves/A.sav", line, sizeof(line));
   check(strstr(line, "uma0: is full") && strstr(line, "1 MB free"), "a nearly empty card says full, and which");
   g_free = DS_LOW_BYTES;
   ds_failed_line("Save", "ux0:a", line, sizeof(line));
   check(!strstr(line, "is full"), "exactly the threshold is not full");
   g_free = 4ull << 40; g_max = 8ull << 40;
   check(ds_free_for("ux0:a", &fr, NULL) && fr == (4ull << 40), "terabyte card");
   g_free = g_max + 1;
   check(!ds_free_for("ux0:a", &fr, NULL), "free above size is a bad answer, not a number");
   g_ok = 0;
   ds_failed_line("Save", "ur0:a", line, sizeof(line));
   check(strstr(line, "could not be checked") && !strstr(line, "full"), "an unreadable device is not called full");
   if (fails) return 1;
   puts("PASS: diskspace (device per path, 64-bit sizes, honest failure line)");
   return 0;
}
