/* cheatcheck.c -- the console's cheat decoder as a filter, for
 * make_cheatdb.py (and for a quick look at what a code decodes to).
 *
 *   cc -O1 -I../.. -o cheatcheck cheatcheck.c ../../cheat_engine.c
 *
 * stdin:   G <system>      begin a game: 0 GBA, 1 GB, 2 GBC
 *          C <code>        one entry of it ('+' between lines)
 *          E               end: every entry is decoded, in the context of
 *                          the whole list (the GBA seed codes), and for each
 *                          one a line "rc kind flags" is written
 * rc is CE_OK (0) or a CE_E_*, kind a CE_KIND_*, flags CE_F_* (decimal).
 * With -v the decoded operations are printed too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheat_engine.h"

#define MAX_ENTRIES 4096

static char *entry[MAX_ENTRIES];
static unsigned n_entries;
static cheat_op ops[8192];
static cheat_patch patches[1024];

int main(int argc, char **argv)
{
   static char line[65536];
   int system = 0, verbose = argc > 1 && !strcmp(argv[1], "-v");
   unsigned i, k;

   while (fgets(line, sizeof(line), stdin))
   {
      line[strcspn(line, "\r\n")] = 0;
      if (line[0] == 'G')
      {
         system = atoi(line + 1);
         n_entries = 0;
      }
      else if (line[0] == 'C' && n_entries < MAX_ENTRIES)
         entry[n_entries++] = strdup(line[1] ? line + 2 : "");
      else if (line[0] == 'E')
      {
         cheat_gba_ctx ctx;
         cheat_gba_ctx_init(&ctx);
         if (system == 0)
            for (i = 0; i < n_entries; i++)
               cheat_gba_ctx_scan(&ctx, entry[i]);
         for (i = 0; i < n_entries; i++)
         {
            int kind = 0, rc;
            unsigned flags = 0;
            cheat_out out;
            memset(&out, 0, sizeof(out));
            out.ops = ops;
            out.cap_ops = sizeof(ops) / sizeof(ops[0]);
            out.patches = patches;
            out.cap_patches = sizeof(patches) / sizeof(patches[0]);
            if (system == 0)
            {
               rc = cheat_gba_compile(entry[i], &ctx, &out, &kind);
               flags = out.flags;
            }
            else
            {
               static cheat_gb_code gb[1024];
               unsigned n = 0;
               rc = cheat_gb_compile(entry[i], gb, 1024, &n, &kind);
               if (verbose && rc == CE_OK)
                  for (k = 0; k < n; k++)
                     printf("   %s %04X = %02X type %02X%s\n",
                            gb[k].rom ? "rom" : "ram", gb[k].address,
                            gb[k].value, gb[k].type,
                            gb[k].has_compare ? " (compare)" : "");
            }
            printf("%d %d %u\n", rc, kind, flags);
            if (verbose && system == 0 && rc == CE_OK)
            {
               for (k = 0; k < out.n_ops; k++)
                  printf("   op %2u w%u %08X %08X x%u/%u +%d +%d\n",
                         ops[k].type, ops[k].width, (unsigned)ops[k].address,
                         (unsigned)ops[k].operand, (unsigned)ops[k].repeat,
                         (unsigned)ops[k].negative_repeat,
                         (int)ops[k].address_offset, (int)ops[k].operand_offset);
               for (k = 0; k < out.n_patches; k++)
                  printf("   rom %08X = %0*X\n", (unsigned)patches[k].address,
                         patches[k].width * 2, (unsigned)patches[k].value);
            }
            free(entry[i]);
         }
         n_entries = 0;
         fflush(stdout);
      }
   }
   return 0;
}
