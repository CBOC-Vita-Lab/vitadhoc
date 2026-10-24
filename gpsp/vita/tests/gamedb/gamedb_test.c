/* gamedb_test.c -- gamedb.c against the real assets/gamedb.txt, and its
 * title keys against make_gamedb.py's (the pairs in keys.txt, written by
 * the Python side: "title<TAB>key" a line).
 *
 *    cc -I../.. gamedb_test.c ../../gamedb.c -o gamedb_test &&
 *    ./gamedb_test ../../assets/gamedb.txt keys.txt
 */
#include <stdio.h>
#include <string.h>

#include "gamedb.h"

static int fails;

static void expect(int cond, const char *what)
{
   if (!cond)
   {
      printf("FAIL %s\n", what);
      fails++;
   }
}

int main(int argc, char **argv)
{
   char out[512], line[1024];
   int n = gamedb_open(argc > 1 ? argv[1] : "../../assets/gamedb.txt");
   FILE *f;
   int keys = 0;

   printf("records %d\n", n);
   expect(n > 10000, "the file has its records");
   expect(gamedb_known_name("Pokemon - Emerald Version (USA, Europe)"),
          "a No-Intro name is known");
   expect(!gamedb_known_name("Pokemon Emerald"), "a made-up name is not");
   expect(gamedb_name_by_code("BPEE", out, sizeof(out)) &&
          !strcmp(out, "Pokemon - Emerald Version (USA, Europe)"),
          "BPEE -> Emerald");
   expect(gamedb_name_by_crc(0x1F1C08FBu, out, sizeof(out)) &&
          !strcmp(out, "Pokemon - Emerald Version (USA, Europe)"),
          "CRC -> Emerald");
   expect(gamedb_blurb("Pokemon - Emerald Version (USA, Europe)", out,
                       sizeof(out)) && strstr(out, "Game Boy Advance"),
          "Emerald's blurb");
   printf("  %s\n", out);
   expect(gamedb_blurb("Pokemon - Emerald", out, sizeof(out)),
          "a file name without 'Version' still finds it");
   expect(gamedb_blurb("Legend of Zelda, The - The Minish Cap (USA)", out,
                       sizeof(out)) && strstr(out, "Minish Cap"),
          "', The' goes in front");
   gamedb_base_title("Legend of Zelda, The - The Minish Cap (USA)", out,
                     sizeof(out));
   expect(!strcmp(out, "The Legend of Zelda - The Minish Cap"), out);
   gamedb_base_title("Bug's Life, A (USA)", out, sizeof(out));
   expect(!strcmp(out, "A Bug's Life"), out);
   expect(!gamedb_blurb("gbadhoc-test", out, sizeof(out)),
          "a test cartridge has none");
   /* the port's test cartridge says AGDE (Lufia's code) but is 1/8 of its
    * size: not taken for it */
   expect(!gamedb_name_by_header("../../hostcore/testrom/gbadhoc-test.gba",
                                 out, sizeof(out)),
          "a small homebrew file with a borrowed code is not that game");

   f = fopen(argc > 2 ? argv[2] : "keys.txt", "r");
   if (f)
   {
      while (fgets(line, sizeof(line), f))
      {
         char *tab = strchr(line, '\t'), *nl;
         if (!tab)
            continue;
         *tab = '\0';
         nl = strchr(tab + 1, '\n');
         if (nl)
            *nl = '\0';
         gamedb_title_key(line, out, sizeof(out));
         if (strcmp(out, tab + 1))
         {
            printf("FAIL key of \"%s\": C \"%s\", Python \"%s\"\n", line, out,
                   tab + 1);
            fails++;
         }
         keys++;
      }
      fclose(f);
   }
   printf("%d keys compared\n%s: %d failed\n", keys, fails ? "FAIL" : "ok",
          fails);
   gamedb_close();
   return fails != 0;
}
