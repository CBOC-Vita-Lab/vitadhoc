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
   /* loose titles (N records), every console */
   expect(gamedb_name_loose('D', "Act Raiser", out, sizeof(out)) &&
          !strcmp(out, "ActRaiser (USA)"), "SNES: 'Act Raiser' is ActRaiser");
   expect(gamedb_name_loose('D', "7th Saga", out, sizeof(out)) &&
          !strcmp(out, "7th Saga, The (USA)"), "SNES: '7th Saga' is 'The 7th Saga'");
   expect(gamedb_name_loose('D', "Mega Man X 2", out, sizeof(out)) &&
          !strcmp(out, "Mega Man X2 (USA)"), "SNES: 'Mega Man X 2' is Mega Man X2");
   expect(gamedb_name_loose('D', "Zelda - A Link to the Past", out, sizeof(out)) &&
          !strcmp(out, "Legend of Zelda, The - A Link to the Past (USA)"),
          "SNES: 'Zelda - A Link to the Past' finds the Legend of Zelda");
   expect(gamedb_name_loose('A', "Pocket Dogs (U)", out, sizeof(out)) &&
          !strcmp(out, "Pocket Dogs (USA)"), "GBA: a GoodTools '(U)' name");
   expect(!gamedb_name_loose('A', "Act Raiser", out, sizeof(out)),
          "the console matters: no GBA ActRaiser");
   expect(gamedb_name_by_crc(0x2D206BF7u, out, sizeof(out)) && !strcmp(out, "Chrono Trigger (USA)"),
          "SNES cartridges by CRC");
   /* NES (E) and Disk System (F): loose titles by console, the CRC taken without
    * the 16-byte iNES header and without a trainer */
   expect(gamedb_name_loose('E', "Super Mario Bros", out, sizeof(out)) &&
          !strcmp(out, "Super Mario Bros. (World)"), "NES: 'Super Mario Bros'");
   expect(!gamedb_name_loose('A', "Duck Hunt", out, sizeof(out)) && gamedb_name_loose('E', "Duck Hunt", out, sizeof(out)),
          "the console matters: Duck Hunt is NES only");
   {
      static unsigned char body[16384 + 8192], file[16 + 512 + 16384 + 8192 + 100];
      uint32_t plain = 0, headed = 0, trained = 0;
      FILE *t;
      size_t i;
      for (i = 0; i < sizeof(body); i++) body[i] = (unsigned char)(i * 7 + i / 251);
      memset(file, 0, sizeof(file));
      memcpy(file, "NES\x1a\x01\x01\x00\x00", 8);
      memcpy(file + 16, body, sizeof(body));
      t = fopen("nes_plain.tmp", "wb"); fwrite(body, 1, sizeof(body), t); fclose(t);
      t = fopen("nes_headed.tmp", "wb"); fwrite(file, 1, 16 + sizeof(body) + 100, t); fclose(t);
      file[6] = 0x04;
      memmove(file + 16 + 512, body, sizeof(body));
      memset(file + 16, 0x55, 512);
      memcpy(file + 16 + 512, body, sizeof(body));
      t = fopen("nes_trained.tmp", "wb"); fwrite(file, 1, 16 + 512 + sizeof(body), t); fclose(t);
      expect(gamedb_file_crc("nes_plain.tmp", &plain, NULL) == 0, "crc of the plain body");
      expect(gamedb_nes_crc_cancel("nes_headed.tmp", &headed, NULL) == 0 && headed == plain,
             "NES CRC skips the 16-byte header (and ignores trailing bytes)");
      expect(gamedb_nes_crc_cancel("nes_trained.tmp", &trained, NULL) == 0 && trained == plain,
             "NES CRC skips the 512-byte trainer when flag 6 says there is one");
      remove("nes_plain.tmp"); remove("nes_headed.tmp"); remove("nes_trained.tmp");
   }
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
