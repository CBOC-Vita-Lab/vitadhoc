/* gamedb_test.c -- gamedb.c against the real assets/gamedb.txt, and its
 * title keys against make_gamedb.py's (the pairs in keys.txt, written by
 * the Python side: "title<TAB>key" a line).
 *
 *    cc -I../.. gamedb_test.c ../../gamedb.c -o gamedb_test &&
 *    ./gamedb_test ../../assets/gamedb.txt keys.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gamedb.h"

static int fails;
/* the file's size (assets/gamedb.txt): update with the generator's output */
#define EXPECT_RECORDS 51351
#define EXPECT_C_RECORDS 23933

static void expect(int cond, const char *what)
{
   if (!cond)
   {
      printf("FAIL %s\n", what);
      fails++;
   }
}

/* ---- Sega (2.0): the SMD and copier-header rules ---- */

static unsigned long long rng = 0x1234567ull;
static unsigned char next_byte(void)
{
   rng = rng * 6364136223846793005ull + 1442695040888963407ull;
   return (unsigned char)(rng >> 56);
}

static int write_file(const char *path, const unsigned char *d, size_t n)
{
   FILE *t = fopen(path, "wb");
   if (!t)
      return -1;
   fwrite(d, 1, n, t);
   fclose(t);
   return 0;
}

/* plain ROM -> SMD: a 512-byte header (blocks, 3, ..., AA BB, 6), then each
 * 16 KiB block with the odd bytes first and the even bytes second: the
 * inverse of Genesis Plus GX's deinterleave_block, written here on its own */
static size_t make_smd(const unsigned char *plain, size_t n, unsigned char *out)
{
   size_t b, i;
   memset(out, 0, 512);
   out[0] = (unsigned char)(n / 16384);
   out[1] = 3;
   out[8] = 0xAA;
   out[9] = 0xBB;
   out[10] = 6;
   for (b = 0; b < n / 16384; b++)
      for (i = 0; i < 8192; i++)
      {
         out[512 + b * 16384 + i] = plain[b * 16384 + i * 2 + 1];
         out[512 + b * 16384 + 8192 + i] = plain[b * 16384 + i * 2];
      }
   return 512 + n;
}

static void sega_files(void)
{
   enum { PLAIN = 5 * 16384 };
   static unsigned char rom[PLAIN], smd[512 + PLAIN], junk[PLAIN + 512];
   uint32_t a = 0, b = 0, c = 0;
   size_t i;
   for (i = 0; i < PLAIN; i++)
      rom[i] = next_byte();
   memcpy(rom + 0x100, "SEGA MEGA DRIVE ", 16);
   make_smd(rom, PLAIN, smd);
   write_file("md_plain.md", rom, PLAIN);
   write_file("md_plain.bin", rom, PLAIN);
   write_file("md_smd.smd", smd, sizeof(smd));
   write_file("md_smd.bin", smd, sizeof(smd));
   expect(gamedb_file_crc("md_plain.md", &a, NULL) == 0, "crc of the plain Genesis ROM");
   expect(gamedb_md_crc_cancel("md_plain.md", &b, NULL) == 0 && b == a,
          "Genesis: a plain .md is hashed whole");
   expect(gamedb_md_crc_cancel("md_plain.bin", &b, NULL) == 0 && b == a,
          "Genesis: a plain .bin is hashed whole");
   expect(gamedb_md_crc_cancel("md_smd.smd", &c, NULL) == 0 && c == a,
          "Genesis: an SMD file (.smd) has the plain ROM's CRC");
   expect(gamedb_md_crc_cancel("md_smd.bin", &c, NULL) == 0 && c == a,
          "Genesis: an SMD file (.bin) has the plain ROM's CRC");
   expect(gamedb_file_crc("md_smd.smd", &b, NULL) == 0 && b != a,
          "the SMD file's own CRC is not the ROM's");
   expect(gamedb_md_rom_kind("md_plain.md") == GAMEDB_MD_PLAIN, "kind: plain");
   expect(gamedb_md_rom_kind("md_smd.bin") == GAMEDB_MD_SMD, "kind: SMD");
   expect(gamedb_md_accept("md_plain.bin") && gamedb_md_accept("md_smd.bin"),
          ".bin with SEGA at 0x100, or with an SMD header, is a Genesis game");
   /* an .smd with a header that lacks AA BB is still SMD (the extension says so) */
   smd[8] = smd[9] = 0;
   write_file("md_smd_nomark.smd", smd, sizeof(smd));
   write_file("md_smd_nomark.bin", smd, sizeof(smd));
   expect(gamedb_md_crc_cancel("md_smd_nomark.smd", &c, NULL) == 0 && c == a,
          ".smd without AA BB: still de-interleaved");
   expect(gamedb_md_rom_kind("md_smd_nomark.bin") == GAMEDB_MD_NONE &&
          !gamedb_md_accept("md_smd_nomark.bin"),
          ".bin of that shape without AA BB is rejected");
   /* a track file of a disc is not opened at all: even one that would pass for a Genesis ROM by its bytes is not listed */
   write_file("Game (Track 01).bin", junk, 3 * 16384);
   junk[0x100] = 'S'; junk[0x101] = 'E'; junk[0x102] = 'G'; junk[0x103] = 'A';
   write_file("Game (Track 02).bin", junk, 3 * 16384);
   expect(!gamedb_md_accept("Game (Track 02).bin") && !gamedb_md_accept("Game (Track 01).bin"),
          "a '(Track NN).bin' file is never a Genesis game");
   /* the track rule is anchored to the end of the file's own name: "(Track " elsewhere in the name or in a folder name does not exclude a file (the
    * content decides, as for any .bin); only "(Track NN).bin" does. Junk has SEGA at 0x100 here, so these are Genesis ROMs by content. */
   mkdir("Hacks (Track 1 fix)", 0755);
   write_file("Hacks (Track 1 fix)/Game.bin", junk, 3 * 16384);
   write_file("Game (Track 2 demo).bin", junk, 3 * 16384);
   expect(gamedb_md_accept("Hacks (Track 1 fix)/Game.bin"), "a folder named '(Track 1 fix)' does not exclude the ROM inside");
   expect(gamedb_md_accept("Game (Track 2 demo).bin"), "'(Track 2 demo).bin' is a candidate (kind by content)");
   expect(!gamedb_md_accept("Game (Track 02).bin"), "'(Track 02).bin' is still never a candidate");
   junk[0x100] = 0;
   write_file("Game (Track 2 demo).bin", junk, 3 * 16384);
   expect(!gamedb_md_accept("Game (Track 2 demo).bin"), "'(Track 2 demo).bin' without SEGA is refused by its content");
   junk[0x100] = 'S';
   /* a .bin that is no Genesis ROM: no SEGA at 0x100, no SMD header */
   for (i = 0; i < sizeof(junk); i++)
      junk[i] = next_byte();
   junk[0x100] = 'X';
   write_file("md_junk.bin", junk, 3 * 16384);
   write_file("md_junk512.bin", junk, 3 * 16384 + 512);
   expect(gamedb_md_rom_kind("md_junk.bin") == GAMEDB_MD_NONE &&
          !gamedb_md_accept("md_junk.bin"), "a .bin without SEGA is rejected");
   junk[8] = 0xAA; junk[9] = 0xBB;
   write_file("md_junk512.bin", junk, 3 * 16384 + 512);
   expect(gamedb_md_accept("md_junk512.bin"),
          "a .bin of 512 + n x 16 KiB with AA BB is an SMD");
   junk[8] = 0xAA; junk[9] = 0xBB;
   write_file("md_junk_size.bin", junk, 3 * 16384 + 100);
   expect(!gamedb_md_accept("md_junk_size.bin"), "AA BB at the wrong size is not an SMD");
   memcpy(junk + 0x100, "SEGA 32X        ", 16);
   write_file("md_32x.bin", junk, 4 * 16384);
   expect(gamedb_md_rom_kind("md_32x.bin") == GAMEDB_MD_32X && !gamedb_md_accept("md_32x.bin"),
          "a 32X .bin is not a Genesis game");
   expect(gamedb_md_accept("md_32x.bin") == 0 && !gamedb_md_accept("x.sms") &&
          !gamedb_md_accept("x.zip") && gamedb_md_accept("nothing.GEN") &&
          gamedb_md_accept("nothing.MD") && gamedb_md_accept("nothing.smd"),
          "extensions: .md .gen .smd always (any case); nothing else");
   expect(gamedb_md_accept("raw.68k") && gamedb_md_accept("RAW.68K"), "a .68k (raw Genesis image) is listed");
   {
      /* the same test on the first bytes of a file that is not on the card (a .bin in a .zip) */
      static unsigned char h[0x110];
      memset(h, 0, sizeof(h));
      expect(!gamedb_md_head_ok(h, sizeof(h), 0x20000), "zipped .bin: no SEGA, not Genesis");
      memcpy(h + 0x100, "SEGA MEGA DRIVE ", 16);
      expect(gamedb_md_head_ok(h, sizeof(h), 0x20000), "zipped .bin: SEGA at 0x100 is Genesis");
      expect(!gamedb_md_head_ok(h, 0x100, 0x20000), "zipped .bin: too few bytes to say");
      memcpy(h + 0x100, "SEGA 32X        ", 16);
      expect(!gamedb_md_head_ok(h, sizeof(h), 0x20000), "zipped .bin: 32X is not Genesis");
      memset(h, 0, sizeof(h));
      h[8] = 0xAA; h[9] = 0xBB;
      expect(gamedb_md_head_ok(h, sizeof(h), 512 + 2 * 16384), "zipped .bin: SMD header at the right size");
      expect(!gamedb_md_head_ok(h, sizeof(h), 0x20000), "zipped .bin: AA BB at the wrong size is not SMD");
   }
   expect(gamedb_md_crc_cancel("no_such_file.md", &b, NULL) != 0, "a missing file fails");
   /* Master System family: a 512-byte copier header is left out */
   {
      static unsigned char sms[32768], hd[32768 + 512], odd[24576 + 512];
      uint32_t p = 0, h = 0, o = 0, w = 0;
      for (i = 0; i < sizeof(sms); i++)
         sms[i] = next_byte();
      memset(hd, 0x55, 512);
      memcpy(hd + 512, sms, sizeof(sms));
      write_file("sms_plain.sms", sms, sizeof(sms));
      write_file("sms_headed.sms", hd, sizeof(hd));
      memcpy(odd + 512, sms, 24576);          /* 24 KiB (SG-1000 size): size % 16384 = 8192 + 512 */
      write_file("sg_headed.sg", odd, sizeof(odd));
      write_file("sg_plain.sg", sms, 24576);
      expect(gamedb_file_crc("sms_plain.sms", &p, NULL) == 0 &&
             gamedb_sms_crc_cancel("sms_plain.sms", &w, NULL) == 0 && w == p,
             "SMS: a plain image is hashed whole");
      expect(gamedb_sms_crc_cancel("sms_headed.sms", &h, NULL) == 0 && h == p,
             "SMS: a 512-byte copier header (size 512 over 16 KiB) is left out");
      expect(gamedb_file_crc("sg_plain.sg", &p, NULL) == 0 &&
             gamedb_sms_crc_cancel("sg_headed.sg", &o, NULL) == 0 && o != p,
             "SG-1000: 24 KiB + 512 is not the 16 KiB-multiple rule (left whole)");
      expect(gamedb_sms_crc_cancel("sg_plain.sg", &o, NULL) == 0 && o == p,
             "SG-1000: a 24 KiB image is hashed whole");
   }
   /* every real ROM listed in GAMEDB_MD_ROMLIST (one path a line; private, not
    * in the repository): its SMD copy has the same CRC */
   {
      const char *list = getenv("GAMEDB_MD_ROMLIST");
      FILE *l = list ? fopen(list, "r") : NULL;
      char path[1024];
      int n = 0;
      while (l && fgets(path, sizeof(path), l))
      {
         static unsigned char *buf, *sm;
         long sz;
         FILE *r;
         uint32_t x = 0, y = 0;
         path[strcspn(path, "\r\n")] = '\0';
         r = fopen(path, "rb");
         if (!r)
            continue;
         fseek(r, 0, SEEK_END);
         sz = ftell(r);
         fseek(r, 0, SEEK_SET);
         if (sz % 16384)          /* pad to whole blocks, as an SMD tool does */
            sz += 16384 - sz % 16384;
         buf = calloc(1, (size_t)sz);
         sm = calloc(1, (size_t)sz + 512);
         if (fread(buf, 1, (size_t)sz, r) != (size_t)sz)
            sz = 0;
         fclose(r);
         write_file("md_real_plain.md", buf, (size_t)sz);
         make_smd(buf, (size_t)sz, sm);
         write_file("md_real.smd", sm, (size_t)sz + 512);
         expect(gamedb_md_crc_cancel("md_real_plain.md", &x, NULL) == 0 &&
                gamedb_md_crc_cancel("md_real.smd", &y, NULL) == 0 && x == y,
                "a real ROM and its SMD copy have one CRC");
         free(buf);
         free(sm);
         n++;
      }
      if (l)
         fclose(l);
      printf("  %d real ROMs compared with their SMD copies\n", n);
   }
   remove("md_plain.md"); remove("md_plain.bin"); remove("md_smd.smd");
   remove("md_smd.bin"); remove("md_smd_nomark.smd"); remove("md_smd_nomark.bin");
   remove("md_junk.bin"); remove("md_junk512.bin"); remove("md_junk_size.bin");
   remove("md_32x.bin"); remove("sms_plain.sms"); remove("sms_headed.sms");
   remove("sg_headed.sg"); remove("sg_plain.sg"); remove("md_real_plain.md");
   remove("Game (Track 01).bin"); remove("Game (Track 02).bin");
   remove("Game (Track 2 demo).bin"); remove("Hacks (Track 1 fix)/Game.bin"); rmdir("Hacks (Track 1 fix)");
   remove("md_real.smd");
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
   /* 2.0: NES, WonderSwan and the Sega consoles have descriptions; a title two
    * consoles' games share (U records) gives each its own game's article */
   expect(gamedb_blurb_sys('E', "Strider (USA)", out, sizeof(out)) &&
          strstr(out, "Nintendo Entertainment System"), "NES Strider");
   expect(gamedb_blurb_sys('I', "Strider (USA)", out, sizeof(out)) &&
          strstr(out, "arcades") && !strstr(out, "Nintendo"), "Genesis Strider is the arcade game");
   expect(gamedb_blurb_sys('I', "Sonic the Hedgehog (USA, Europe)", out, sizeof(out)) &&
          strstr(out, "Sega Genesis"), "Genesis Sonic");
   expect(gamedb_blurb_sys('J', "Sonic the Hedgehog (Europe)", out, sizeof(out)) &&
          strstr(out, "Master System"), "Master System Sonic");
   expect(gamedb_blurb_sys('K', "Sonic the Hedgehog (USA, Europe)", out, sizeof(out)) &&
          strstr(out, "Game Gear"), "Game Gear Sonic");
   expect(gamedb_blurb_sys('E', "Mega Man 2 (USA)", out, sizeof(out)) &&
          strstr(out, "Mega Man 2"), "NES Mega Man 2");
   expect(gamedb_blurb_sys('I', "Streets of Rage 2 (USA)", out, sizeof(out)) &&
          strstr(out, "Streets of Rage 2"), "Genesis Streets of Rage 2");
   expect(gamedb_blurb_sys('G', "Buffers Evolution (Japan)", out, sizeof(out)) &&
          strstr(out, "WonderSwan"), "WonderSwan Buffers Evolution");
   expect(gamedb_blurb_sys('L', "Flicky (Japan) (Rev 1)", out, sizeof(out)) &&
          strstr(out, "Flicky"), "SG-1000 Flicky");
   /* a description only for the game it is about: these titles' articles are
    * about a game of another console (or a series) and are left out */
   expect(!gamedb_blurb_sys('H', "Final Fantasy II (Japan)", out, sizeof(out)),
          "no Super NES Final Fantasy IV text for the WonderSwan Color game");
   expect(!gamedb_blurb_sys('K', "Tempo Jr. (World)", out, sizeof(out)),
          "no Sega 32X Tempo text for the Game Gear game");
   expect(!gamedb_blurb_sys('J', "Golden Axe (USA, Europe, Brazil) (En)", out, sizeof(out)),
          "no series text");
   /* a Game Boy game is found for the Color too, not the other way round for
    * a console that has no such partner */
   expect(gamedb_blurb_sys('C', "Tetris (World) (Rev 1)", out, sizeof(out)) &&
          strstr(out, "Game Boy"), "a Game Boy game's text on the Color");
   expect(!gamedb_blurb_sys('I', "Alex Kidd in Miracle World (USA, Europe)", out, sizeof(out)),
          "a Master System game has no Genesis text");
   expect(!gamedb_blurb_sys('I', "gbadhoc-test", out, sizeof(out)),
          "no description for a title that is not in the database");
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
   /* every C record resolves to its own name (crcs.txt is written from the same
    * file by the Makefile), Sega's catalogues among them */
   f = fopen(argc > 3 ? argv[3] : "crcs.txt", "r");
   if (f)
   {
      int bad = 0, total = 0;
      while (fgets(line, sizeof(line), f))
      {
         char *tab = strchr(line, '\t'), *nl;
         if (!tab)
            continue;
         *tab = '\0';
         nl = strchr(tab + 1, '\n');
         if (nl)
            *nl = '\0';
         total++;
         if (!gamedb_name_by_crc((uint32_t)strtoul(line, NULL, 16), out, sizeof(out)) ||
             strcmp(out, tab + 1))
         {
            if (bad++ < 5)
               printf("FAIL crc %s -> \"%s\", want \"%s\"\n", line, out, tab + 1);
            fails++;
         }
      }
      fclose(f);
      printf("%d CRC records resolved to their names\n", total - bad);
      expect(total == EXPECT_C_RECORDS, "the file has the C records expected");
   }
   expect(n == EXPECT_RECORDS, "the file has the records expected");
   /* Sega: loose titles by console (N records I J K L), checksums, names */
   expect(gamedb_name_by_crc(0xF9394E97u, out, sizeof(out)) &&
          !strcmp(out, "Sonic The Hedgehog (USA, Europe)"), "Genesis: Sonic 1 by CRC");
   expect(gamedb_name_by_crc(0x17A40E29u, out, sizeof(out)) &&
          !strcmp(out, "Alex Kidd in Miracle World (USA, Europe)"), "SMS: Alex Kidd by CRC");
   expect(gamedb_name_by_crc(0x95A18EC7u, out, sizeof(out)) &&
          !strcmp(out, "Sonic The Hedgehog 2 (World)"), "Game Gear: Sonic 2 by CRC");
   expect(gamedb_name_by_crc(0x1898F274u, out, sizeof(out)) &&
          !strcmp(out, "Girl's Garden (Japan, Australia) (Ja)"), "SG-1000: Girl's Garden by CRC");
   expect(gamedb_name_loose('I', "Sonic the Hedgehog", out, sizeof(out)) &&
          !strcmp(out, "Sonic The Hedgehog (USA, Europe)"), "Genesis: loose 'Sonic the Hedgehog'");
   expect(gamedb_name_loose('J', "Alex Kidd in Miracle World", out, sizeof(out)) &&
          !strcmp(out, "Alex Kidd in Miracle World (USA, Europe)"), "SMS: loose Alex Kidd");
   expect(gamedb_name_loose('K', "Sonic The Hedgehog 2", out, sizeof(out)) &&
          !strcmp(out, "Sonic The Hedgehog 2 (World)"), "Game Gear: loose Sonic 2");
   expect(gamedb_name_loose('L', "Girls Garden", out, sizeof(out)) &&
          !strncmp(out, "Girl's Garden", 13), "SG-1000: loose 'Girls Garden'");
   expect(!gamedb_name_loose('L', "Sonic The Hedgehog 2", out, sizeof(out)),
          "the console matters: no SG-1000 Sonic 2");
   expect(gamedb_known_name("Sonic & Knuckles (World)"), "a Genesis name with '&' is known");
   sega_files();
   printf("%d keys compared\n%s: %d failed\n", keys, fails ? "FAIL" : "ok",
          fails);
   gamedb_close();
   return fails != 0;
}
