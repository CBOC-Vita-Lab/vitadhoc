/* Shared, explicit GBA format tags. No opcode or cipher guessing. */
#ifndef GPSP_CHEAT_FORMAT_H
#define GPSP_CHEAT_FORMAT_H

enum { CHEAT_GS1 = 1, CHEAT_GS1_RAW, CHEAT_AR3, CHEAT_AR3_RAW };

/* Returns the family and consumed prefix bytes, or 0. Case insensitive. */
static unsigned cheat_family_prefix(const char *s, unsigned n, unsigned *used)
{
   static const char *const tags[] = { "GS1:", "GS1RAW:", "AR3:", "AR3RAW:" };
   unsigned k, i;
   for (k = 0; k < 4; k++)
   {
      for (i = 0; tags[k][i] && i < n; i++)
      {
         char c = s[i];
         if (c >= 'a' && c <= 'z') c += 'A' - 'a';
         if (c != tags[k][i]) break;
      }
      if (!tags[k][i])
      {
         *used = i;
         return k + 1;
      }
   }
   return 0;
}
#endif
