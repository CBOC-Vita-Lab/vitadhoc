/* Reads No-Intro names on stdin, one per line; writes each name's fallback names (artalt.h), tab separated. */
#include "artalt.h"
#include <stdlib.h>
int main(void)
{
   char line[512];
   while (fgets(line, sizeof(line), stdin))
   {
      af_alts o;
      const char *have[1];
      int i;
      line[strcspn(line, "\r\n")] = 0;
      have[0] = line;
      alt_names(line, have, 1, &o);
      for (i = 0; i < o.n; i++)
         printf("%s%s", i ? "\t" : "", o.name[i]);
      printf("\n");
   }
   return 0;
}
