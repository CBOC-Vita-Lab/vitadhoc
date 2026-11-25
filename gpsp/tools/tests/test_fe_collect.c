/* test_fe_collect.c -- collections.txt and RetroArch playlists
 * (frontend-common/fe_collect.c):
 *
 *   1. collections: create, find, rename, delete, add, remove, names made
 *      fit, the tables' limits, the text read in pieces of any size;
 *   2. the file: round trip, a cut save (.old only) is the list, a failed
 *      write (no room for a temp file, a rename that cannot happen) leaves
 *      the old file whole, a list that did not fit is never written back;
 *   3. .lpl playlists: RetroArch's own shape, escapes, odd spacing, a file
 *      read in one byte at a time, malformed and hostile input, a file over
 *      the size cap, more items than fit, rzip files;
 *   4. matching: path, then file name, then checksum; precedence, duplicate
 *      names, normalisation of the device and slashes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../../frontend-common/fe_collect.c"

static fe_coll C;
static fe_lpl_set L;

static int slurp(const char *p, char *out, size_t cap)
{
   FILE *f = fopen(p, "rb");
   size_t n;
   if (!f)
      return -1;
   n = fread(out, 1, cap - 1, f);
   out[n] = '\0';
   fclose(f);
   return (int)n;
}

static void put(const char *p, const char *text)
{
   FILE *f = fopen(p, "wb");
   assert(f);
   fputs(text, f);
   fclose(f);
}

static void feed_bytes(const char *t)
{
   size_t i;
   for (i = 0; t[i]; i++)
      fe_coll_feed(&C, t + i, 1);
   fe_coll_feed_end(&C);
}

static void test_names(void)
{
   char o[FE_COLL_NAME_MAX];
   assert(fe_coll_clean_name("  Road trip \t", o, sizeof(o)) == 9);
   assert(!strcmp(o, "Road trip"));
   assert(fe_coll_clean_name("a\nb", o, sizeof(o)) == 3 && !strcmp(o, "a b"));
   assert(fe_coll_clean_name("   ", o, sizeof(o)) == 0);
   assert(fe_coll_clean_name("", o, sizeof(o)) == 0);
   /* cut at a character boundary: 20 two-byte letters = 40 bytes */
   assert(fe_coll_clean_name("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                             "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9",
                             o, sizeof(o)) == 38);
   assert(((unsigned char)o[36]) == 0xC3 && ((unsigned char)o[37]) == 0xA9);
}

static void test_store(void)
{
   int a, b, k;
   unsigned i;
   fe_coll_clear(&C);
   assert((a = fe_coll_create(&C, "Road trip")) == 0);
   assert((b = fe_coll_create(&C, "Two at once")) == 1);
   assert(fe_coll_create(&C, "ROAD TRIP") == -3);
   assert(fe_coll_create(&C, "  ") == -2);
   assert(fe_coll_find(&C, "road trip") == 0 && fe_coll_find(&C, "nope") == -1);
   assert(fe_coll_add(&C, a, "Emerald.gba") == 0);
   assert(fe_coll_add(&C, a, "Emerald.gba") == 0 && C.count[a] == 1);   /* again: no change */
   assert(fe_coll_add(&C, b, "Emerald.gba") == 0 && fe_coll_add(&C, b, "sub/Shantae.gbc") == 0);
   assert(fe_coll_add(&C, a, "") == -1 && fe_coll_add(&C, 7, "x") == -1);
   assert(fe_coll_has(&C, a, "Emerald.gba") && fe_coll_has(&C, b, "sub/Shantae.gbc"));
   assert(!fe_coll_has(&C, a, "sub/Shantae.gbc") && !fe_coll_has(&C, a, "emerald.gba"));
   assert(C.n == 3 && C.count[a] == 1 && C.count[b] == 2);
   assert(fe_coll_remove(&C, a, "Emerald.gba") == 0 && fe_coll_remove(&C, a, "Emerald.gba") == -1);
   assert(fe_coll_has(&C, b, "Emerald.gba") && fe_coll_has(&C, b, "sub/Shantae.gbc"));
   assert(fe_coll_rename(&C, a, "Two at once") == -3);
   assert(fe_coll_rename(&C, a, "Trip") == 0 && !strcmp(C.name[a], "Trip"));
   assert(fe_coll_rename(&C, b, "TWO AT ONCE") == 0);           /* its own name, other case */
   assert(fe_coll_rename(&C, 5, "x") == -1 && fe_coll_rename(&C, a, " ") == -2);
   /* delete the first: the second moves up with its games, the pool closes */
   assert(fe_coll_add(&C, a, "Other.gba") == 0);
   assert(fe_coll_delete(&C, a) == 0 && C.ncoll == 1 && !strcmp(C.name[0], "TWO AT ONCE"));
   assert(C.n == 2 && C.count[0] == 2 && fe_coll_has(&C, 0, "sub/Shantae.gbc"));
   assert(fe_coll_has(&C, 0, "Emerald.gba") && !fe_coll_has(&C, 0, "Other.gba"));
   assert(C.used == strlen("Emerald.gba") + 1 + strlen("sub/Shantae.gbc") + 1);
   assert(fe_coll_delete(&C, 3) == -1);
   /* limits */
   fe_coll_clear(&C);
   for (k = 0; k < FE_COLL_MAX; k++)
   {
      char n[16];
      snprintf(n, sizeof(n), "C%d", k);
      assert(fe_coll_create(&C, n) == k);
   }
   assert(fe_coll_create(&C, "one more") == -1);
   for (i = 0; i < FE_COLL_ENTRIES; i++)
   {
      char p[32];
      snprintf(p, sizeof(p), "g%05u.gba", i);
      assert(fe_coll_add(&C, (int)(i % FE_COLL_MAX), p) == 0);
   }
   assert(fe_coll_add(&C, 0, "full.gba") == -1 && C.n == FE_COLL_ENTRIES);
   assert(fe_coll_has(&C, 3, "g00003.gba") && !fe_coll_has(&C, 4, "g00003.gba"));
   {
      char big[FE_COLL_PATH_MAX + 8];
      memset(big, 'a', sizeof(big) - 1);
      big[sizeof(big) - 1] = '\0';
      fe_coll_clear(&C);
      fe_coll_create(&C, "x");
      assert(fe_coll_add(&C, 0, big) == -1);
   }
}

static void test_text(void)
{
   const char *t = "#collection One\nA.gba\r\nsub/B.gbc  \n\n#collection Two\n[odd] name.gba\n"
                   "#1 starts with a hash.gba\n#collection One\nC.gba";
   fe_coll_clear(&C);
   feed_bytes(t);                          /* one byte at a time, last line without \n */
   assert(!C.lossy && C.ncoll == 2 && C.n == 5);
   assert(fe_coll_has(&C, 0, "A.gba") && fe_coll_has(&C, 0, "sub/B.gbc") && fe_coll_has(&C, 0, "C.gba"));
   assert(fe_coll_has(&C, 1, "[odd] name.gba") && fe_coll_has(&C, 1, "#1 starts with a hash.gba"));
   fe_coll_clear(&C);
   fe_coll_feed(&C, t, strlen(t));
   fe_coll_feed_end(&C);
   assert(!C.lossy && C.ncoll == 2 && C.n == 5);
   /* a path before any collection, and a path too long, are not kept and forbid a save */
   fe_coll_clear(&C);
   feed_bytes("orphan.gba\n#collection X\nok.gba\n");
   assert(C.lossy && C.ncoll == 1 && C.n == 1);
   {
      static char big[FE_COLL_PATH_MAX * 3 + 64];
      fe_coll_clear(&C);
      strcpy(big, "#collection X\n");
      memset(big + strlen(big), 'z', FE_COLL_PATH_MAX * 3);
      strcat(big, "\nfine.gba\n");
      fe_coll_feed(&C, big, strlen(big));
      fe_coll_feed_end(&C);
      assert(C.lossy && C.n == 1 && fe_coll_has(&C, 0, "fine.gba"));
   }
   /* 25 collections: the 25th does not fit */
   {
      char t2[2048] = "";
      int k;
      for (k = 0; k < FE_COLL_MAX + 1; k++)
         sprintf(t2 + strlen(t2), "#collection C%d\nx%d.gba\n", k, k);
      fe_coll_clear(&C);
      fe_coll_feed(&C, t2, strlen(t2));
      fe_coll_feed_end(&C);
      assert(C.lossy && C.ncoll == FE_COLL_MAX);
   }
   /* garbage: binary noise never crashes and never claims a collection it cannot hold */
   {
      unsigned s = 12345, i;
      static char noise[100000];
      for (i = 0; i < sizeof(noise); i++)
      {
         s = s * 1103515245u + 12345u;
         noise[i] = (char)(s >> 16);
      }
      fe_coll_clear(&C);
      fe_coll_feed(&C, noise, sizeof(noise));
      fe_coll_feed_end(&C);
      assert(C.n <= FE_COLL_ENTRIES && C.ncoll <= FE_COLL_MAX);
   }
}

static void test_file(void)
{
   char dir[] = "/tmp/t_fe_collect_XXXXXX", p[200], q[220], buf[4096];
   int a, b;
   assert(mkdtemp(dir));
   snprintf(p, sizeof(p), "%s/collections.txt", dir);
   fe_coll_clear(&C);
   assert(fe_coll_load(&C, p) == 0 && C.ncoll == 0);       /* missing: empty */
   a = fe_coll_create(&C, "Road trip");
   b = fe_coll_create(&C, "Two at once");
   fe_coll_add(&C, a, "Emerald.gba");
   fe_coll_add(&C, a, "sub/Shantae.gbc");
   fe_coll_add(&C, b, "Emerald.gba");
   assert(fe_coll_save(&C, p) == 0);
   assert(slurp(p, buf, sizeof(buf)) > 0);
   assert(!strcmp(buf, "#collection Road trip\nEmerald.gba\nsub/Shantae.gbc\n"
                       "#collection Two at once\nEmerald.gba\n"));
   {
      static fe_coll D;
      assert(fe_coll_load(&D, p) == 0 && D.ncoll == 2 && D.n == 3 && !D.lossy);
      assert(fe_coll_has(&D, 1, "Emerald.gba") && !strcmp(D.name[0], "Road trip"));
   }
   /* no temp or old file left behind */
   snprintf(q, sizeof(q), "%s.tmp", p);
   assert(access(q, F_OK) != 0);
   snprintf(q, sizeof(q), "%s.old", p);
   assert(access(q, F_OK) != 0);
   /* a second save replaces it */
   fe_coll_add(&C, b, "Extra.gba");
   assert(fe_coll_save(&C, p) == 0);
   assert(slurp(p, buf, sizeof(buf)) > 0 && strstr(buf, "Extra.gba"));

   /* A FAILED WRITE: the temp file cannot be made (its name is a directory)
    * -> the old file is untouched and the save says so */
   snprintf(q, sizeof(q), "%s.tmp", p);
   assert(mkdir(q, 0777) == 0);
   fe_coll_remove(&C, b, "Extra.gba");
   fe_coll_add(&C, b, "Lost if written.gba");
   assert(fe_coll_save(&C, p) == -1);
   assert(slurp(p, buf, sizeof(buf)) > 0 && strstr(buf, "Extra.gba") && !strstr(buf, "Lost if"));
   rmdir(q);
   /* a rename that cannot happen: the old name's side file is a directory
    * that cannot be removed (not empty) */
   snprintf(q, sizeof(q), "%s.old", p);
   assert(mkdir(q, 0777) == 0);
   {
      char inner[260];
      snprintf(inner, sizeof(inner), "%s/x", q);
      put(inner, "x");
      assert(fe_coll_save(&C, p) == -1);
      assert(slurp(p, buf, sizeof(buf)) > 0 && strstr(buf, "Extra.gba") && !strstr(buf, "Lost if"));
      snprintf(q, sizeof(q), "%s.tmp", p);
      assert(access(q, F_OK) != 0);                  /* its temp was removed */
      remove(inner);
      snprintf(inner, sizeof(inner), "%s.old", p);
      rmdir(inner);
   }
   /* a folder that does not exist */
   snprintf(q, sizeof(q), "%s/nope/collections.txt", dir);
   assert(fe_coll_save(&C, q) == -1);

   /* A CUT SAVE: only collections.txt.old is left -> it is the list */
   snprintf(q, sizeof(q), "%s.old", p);
   assert(rename(p, q) == 0);
   {
      static fe_coll D;
      assert(fe_coll_load(&D, p) == 0 && D.ncoll == 2 && fe_coll_has(&D, 0, "Emerald.gba"));
      assert(access(p, F_OK) == 0 && access(q, F_OK) != 0);
   }

   /* A LIST THAT DID NOT FIT (or was not read) IS NEVER WRITTEN BACK */
   put(p, "orphan.gba\n#collection X\nok.gba\n");
   {
      static fe_coll D;
      char before[4096];
      assert(fe_coll_load(&D, p) == 0 && D.lossy);
      assert(slurp(p, before, sizeof(before)) > 0);
      assert(fe_coll_save(&D, p) == -2);
      assert(slurp(p, buf, sizeof(buf)) > 0 && !strcmp(before, buf));
      D.lossy = 0;
      D.unread = 1;
      assert(fe_coll_save(&D, p) == -2);
   }
   /* a path that is a directory cannot be read: unread, no save */
   snprintf(q, sizeof(q), "%s/adir", dir);
   mkdir(q, 0777);
   {
      static fe_coll D;
      int rc = fe_coll_load(&D, q);
      /* (fopen of a directory reads nothing on some systems: either way no save may follow a failed read) */
      assert(rc == 0 || (rc == -1 && D.unread));
      if (rc == -1)
         assert(fe_coll_save(&D, q) == -2);
   }
   snprintf(q, sizeof(q), "rm -rf %s", dir);
   assert(system(q) == 0);
}

/* ---- playlists --------------------------------------------------------- */

static const char RA[] =
   "{\n  \"version\": \"1.5\",\n  \"default_core_path\": \"\",\n  \"default_core_name\": \"\",\n"
   "  \"label_display_mode\": 0,\n  \"right_thumbnail_mode\": 0,\n  \"left_thumbnail_mode\": 0,\n"
   "  \"sort_mode\": 0,\n  \"items\": [\n"
   "    {\n      \"path\": \"ux0:/data/retroarch/roms/gba/Emerald (USA).gba\",\n"
   "      \"label\": \"Pokemon Emerald\",\n      \"core_path\": \"DETECT\",\n      \"core_name\": \"DETECT\",\n"
   "      \"crc32\": \"1F1C08FB|crc\",\n      \"db_name\": \"Nintendo - Game Boy Advance.lpl\"\n    },\n"
   "    {\n      \"path\": \"/home/me/roms/Metroid Fusion.gba\",\n      \"label\": \"Metroid Fusion\",\n"
   "      \"crc32\": \"DEADBEEF|crc\",\n      \"db_name\": \"x.lpl\"\n    },\n"
   "    {\n      \"path\": \"C:\\\\Games\\\\Zip\\\\Pack.zip#Inner Game.gbc\",\n      \"label\": \"Inner\",\n"
   "      \"crc32\": \"00000000|crc\"\n    },\n"
   "    {\n      \"path\": \"/x/Unknown.gba\",\n      \"label\": \"Kept by checksum\",\n"
   "      \"crc32\": \"CAFE0001|crc\"\n    },\n"
   "    {\n      \"label\": \"no path at all\"\n    },\n"
   "    {\n      \"path\": \"\\u00e9t\\u00e9\\/caf\\u00e9.gba\",\n      \"crc32\": \"DETECT\"\n    }\n"
   "  ]\n}\n";

static unsigned crc_name(unsigned crc, void *ctx)
{
   (void)ctx;
   return crc == 0xCAFE0001u ? fe_lpl_hash_name("Totally Different Name (Europe)") : 0;
}

static void keys(fe_lpl_rom *r, const char *const *paths, int n)
{
   int i;
   for (i = 0; i < n; i++)
      fe_lpl_rom_key(paths[i], &r[i]);
}

static void test_lpl(void)
{
   char a[200], b[200];
   fe_lpl_clear(&L);
   assert(fe_lpl_begin(&L, "Nintendo - Game Boy Advance.lpl") == 0);
   fe_lpl_feed(&L, RA, strlen(RA));
   assert(fe_lpl_end(&L) == 5);                   /* the no-path item is not one */
   assert(L.nlist == 1 && !strcmp(L.list[0].name, "Nintendo - Game Boy Advance"));
   assert(L.item[0].crc == 0x1F1C08FBu && L.item[1].crc == 0xDEADBEEFu && L.item[2].crc == 0);
   assert(L.item[4].crc == 0);

   /* the same file a byte at a time gives the same items */
   {
      fe_lpl_set *M = calloc(1, sizeof(*M));
      size_t i;
      assert(M && fe_lpl_begin(M, "x") == 0);
      for (i = 0; RA[i]; i++)
         fe_lpl_feed(M, RA + i, 1);
      assert(fe_lpl_end(M) == 5 && !memcmp(M->item, L.item, 5 * sizeof(fe_lpl_item)));
      free(M);
   }

   /* normalisation */
   fe_lpl_norm("UX0:/Data//Roms\\GBA/Game.GBA", a, sizeof(a));
   assert(!strcmp(a, "ux0:data/roms/gba/game.gba"));
   fe_lpl_norm("ux0:data/roms/gba/game.gba", b, sizeof(b));
   assert(!strcmp(a, b));
   fe_lpl_norm("./a//b", a, sizeof(a));
   assert(!strcmp(a, "a/b"));
   fe_lpl_norm("/abs//x", a, sizeof(a));
   assert(!strcmp(a, "/abs/x"));

   /* matching */
   {
      const char *lib[] = {
         "ux0:data/retroarch/roms/GBA/Emerald (USA).gba",      /* 0: by path (case, slash)   */
         "ux0:data/GBAdhoc/roms/Metroid Fusion.GBA",           /* 1: by file name            */
         "ux0:data/GBAdhoc/roms/Inner Game.gbc",               /* 2: by name inside a zip    */
         "ux0:data/GBAdhoc/roms/Totally Different Name (Europe).gba", /* 3: by checksum     */
         "ux0:data/GBAdhoc/roms/Unrelated.gba",                /* 4: not in it               */
         "ux0:data/GBAdhoc/roms/sub/Emerald (USA).gba",        /* 5: same name elsewhere     */
         "ux0:data/GBAdhoc/roms/\xc3\xa9t\xc3\xa9/caf\xc3\xa9.gba", /* 6: escapes, by path tail name */
      };
      fe_lpl_rom r[7];
      unsigned short out[16];
      unsigned n;
      keys(r, lib, 7);
      n = fe_lpl_match(&L, 0, r, 7, out, 16, crc_name, NULL);
      /* item 0 has a path match (game 0): the other "Emerald (USA).gba" (5)
       * is NOT added; 1 by file name, 2 by the name after '#', 3 by checksum
       * (its file name is not the item's), 6 by the name of its file */
      assert(n == 5);
      assert(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 3 && out[4] == 6);
      /* no checksum table: game 3 is not found */
      assert(fe_lpl_match(&L, 0, r, 7, out, 16, NULL, NULL) == 4);
      /* count only */
      assert(fe_lpl_match(&L, 0, r, 7, NULL, 0, crc_name, NULL) == 5);
      /* a small out: the count is still whole, nothing is written past cap */
      out[2] = 0xFFFF;
      assert(fe_lpl_match(&L, 0, r, 7, out, 2, crc_name, NULL) == 5 && out[2] == 0xFFFF);
      assert(fe_lpl_match(&L, 3, r, 7, out, 16, crc_name, NULL) == 0);     /* no such list */
      assert(fe_lpl_match(&L, 0, r, 0, out, 16, crc_name, NULL) == 0);
      /* a name that is in two folders is both, when no path says which */
      fe_lpl_begin(&L, "two.lpl");
      {
         const char *two = "{\"items\":[{\"path\":\"/other/Emerald (USA).gba\"}]}";
         fe_lpl_feed(&L, two, strlen(two));
      }
      assert(fe_lpl_end(&L) == 1);
      n = fe_lpl_match(&L, 1, r, 7, out, 16, crc_name, NULL);
      assert(n == 2 && out[0] == 0 && out[1] == 5);
   }
   assert(L.nlist == 2 && L.nitem == 6);
}

static void feed_str(fe_lpl_set *s, const char *name, const char *t)
{
   assert(fe_lpl_begin(s, name) == 0);
   fe_lpl_feed(s, t, strlen(t));
}

static void test_lpl_bad(void)
{
   static fe_lpl_set S;
   char dir[] = "/tmp/t_fe_lpl_XXXXXX", p[200], cmd[260];
   int k;
   const char *bad[] = {
      "", "not json at all", "[1,2,3]", "{\"items\": [ {\"path\": \"/a.gba\"} ",   /* cut short */
      "{\"version\": \"1.5\"}",                                                    /* no items */
      "{\"items\": [ {\"path\": \"/a.gba\" ]}}}}", "}}}}", "{\"items\": [ {\"path\": \"/a.gb",
      "\xff\xfe\x00\x01{\"items\":[", "{{{{{{{{{{{{{{{{{{{{{{",
   };
   fe_lpl_clear(&S);
   for (k = 0; k < (int)(sizeof(bad) / sizeof(bad[0])); k++)
   {
      feed_str(&S, "bad", bad[k]);
      assert(fe_lpl_end(&S) == -1);
      assert(S.nlist == 0 && S.nitem == 0);           /* dropped whole */
   }
   /* an empty playlist is a playlist */
   feed_str(&S, "empty", "{\"items\": []}");
   assert(fe_lpl_end(&S) == 0 && S.nlist == 1);
   /* an item with a hostile checksum / huge strings / escapes only */
   feed_str(&S, "odd", "{\"items\":[{\"path\":\"/a.gba\",\"crc32\":\"12345\"},"
            "{\"path\":\"/b.gba\",\"crc32\":\"zzzzzzzz|crc\"},{\"path\":\"/c.gba\",\"crc32\":7},"
            "{\"path\":\"\\u12\\ud800x\"}, 5, \"str\", null, {\"items\":[{\"path\":\"/deep.gba\"}]}]}");
   assert(fe_lpl_end(&S) == 4);
   assert(S.item[S.list[1].first].crc == 0 && S.item[S.list[1].first + 1].crc == 0);
   /* a path longer than the buffer is cut, not overrun */
   {
      static char huge[20000];
      strcpy(huge, "{\"items\":[{\"path\":\"/");
      memset(huge + strlen(huge), 'q', 15000);
      strcat(huge, ".gba\"}]}");
      feed_str(&S, "huge", huge);
      assert(fe_lpl_end(&S) == 1);
   }
   /* more items than a playlist keeps: kept up to the cap, the rest counted */
   {
      static char big[FE_LPL_PER_LIST * 40 + 64];
      int i;
      size_t n = 0;
      n += (size_t)sprintf(big + n, "{\"items\":[");
      for (i = 0; i < FE_LPL_PER_LIST + 100; i++)
         n += (size_t)sprintf(big + n, "%s{\"path\":\"/r/g%d.gba\"}", i ? "," : "", i);
      strcpy(big + n, "]}");
      fe_lpl_clear(&S);
      feed_str(&S, "many", big);
      assert(fe_lpl_end(&S) == FE_LPL_PER_LIST && S.list[0].dropped == 100);
   }
   /* the table of playlists: the 17th does not begin */
   fe_lpl_clear(&S);
   for (k = 0; k < FE_LPL_MAX; k++)
   {
      assert(fe_lpl_begin(&S, "p") == 0);
      fe_lpl_feed(&S, "{\"items\":[]}", 12);
      assert(fe_lpl_end(&S) == 0);
   }
   assert(fe_lpl_begin(&S, "one too many") == -1);

   /* files: a missing one, rzip, a file over the size cap, a good one */
   assert(mkdtemp(dir));
   fe_lpl_clear(&S);
   snprintf(p, sizeof(p), "%s/none.lpl", dir);
   assert(fe_lpl_load(&S, p, "none.lpl") == -1 && S.nlist == 0);
   snprintf(p, sizeof(p), "%s/z.lpl", dir);
   put(p, "#RZIPv\x01\x31 binary follows");
   assert(fe_lpl_load(&S, p, "z.lpl") == -1 && S.nlist == 0);
   snprintf(p, sizeof(p), "%s/ok.lpl", dir);
   put(p, RA);
   assert(fe_lpl_load(&S, p, "ok.lpl") == 5 && S.nlist == 1 && !strcmp(S.list[0].name, "ok"));
   {
      /* 9 MiB: items first, then padding the size cap stops short of; what came before the cut is kept */
      FILE *f;
      size_t i;
      static char pad[65536];
      snprintf(p, sizeof(p), "%s/huge.lpl", dir);
      f = fopen(p, "wb");
      assert(f);
      fputs("{\"items\":[{\"path\":\"/first.gba\"}", f);
      memset(pad, ' ', sizeof(pad));
      for (i = 0; i < (9u << 20) / sizeof(pad); i++)
         fwrite(pad, 1, sizeof(pad), f);
      fputs(",{\"path\":\"/never_read.gba\"}]}", f);
      fclose(f);
      assert(fe_lpl_load(&S, p, "huge.lpl") == 1 && S.list[1].cut);
      assert(S.bytes <= FE_LPL_BYTES_MAX);
   }
   snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
   assert(system(cmd) == 0);
}

int main(void)
{
   test_names();
   test_store();
   test_text();
   test_file();
   test_lpl();
   test_lpl_bad();
   printf("test_fe_collect: ok\n");
   return 0;
}
