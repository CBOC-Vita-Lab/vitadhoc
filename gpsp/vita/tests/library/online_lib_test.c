/* online_lib_test.c -- the library match of the code pad's room card
 * (online_lib.c): a folder tree with a .gba, a .gb, zips and a miss;
 * several files with ONE code (the reviewer's test); the pure decision.
 *
 * Built by online_lib.mk (make online-lib-test).  The host's hooks are
 * replaced here: the identity of a file is a hash of its bytes, or, for a
 * file with a patch beside it, of the PATCH's bytes (standing for "the image
 * after the patch": the real hook is main_vita.c's). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../../online_lib.h"
#include "../../ui_host_hooks.h"

char g_app_rom_dir[APP_PATH_CAP];
static char g_extra[2][APP_PATH_CAP];
static int  g_extra_n;
int         ui_host_extra_roots(void) { return g_extra_n; }
const char *ui_host_extra_root(int i) { return g_extra[i]; }

/* zip_rom.c's extractor is linked but never called here */
unsigned long fe_crc32(unsigned long c, const void *d, unsigned long n) { (void)d; (void)n; return c; }
int stbi_zlib_decode_noheader_buffer(char *o, int ol, const char *i, int il) { (void)o; (void)ol; (void)i; (void)il; return -1; }

/* ---- counting what the walk reads ---------------------------------------- */
static int g_opens, g_rom_ids;
FILE *__real_fopen(const char *p, const char *m);
FILE *__wrap_fopen(const char *p, const char *m)
{
   g_opens++;
   return __real_fopen(p, m);
}

static unsigned hash_file(const char *path)
{
   FILE *f = __real_fopen(path, "rb");
   unsigned h = 2166136261u;
   int c;
   if (!f)
      return 0;
   while ((c = fgetc(f)) != EOF)
      h = (h ^ (unsigned)c) * 16777619u;
   fclose(f);
   return h;
}

int ui_host_online_rom_id(const char *path, char out[9])
{
   char patch[APP_PATH_CAP];
   const char *dot = strrchr(path, '.');
   struct stat st;
   unsigned h;
   g_rom_ids++;
   if (!dot)
      return -1;
   snprintf(patch, sizeof(patch), "%.*s.ips", (int)(dot - path), path);
   h = stat(patch, &st) == 0 ? hash_file(patch) : hash_file(path);
   snprintf(out, 9, "%08x", h);
   return 0;
}
int ui_host_online_available(void) { return 1; }

/* ---- fixture -------------------------------------------------------------- */
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
   printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
   printf("\n"); } } while (0)

static char root[APP_PATH_CAP];

static void mk(const char *rel)
{
   char p[APP_PATH_CAP];
   snprintf(p, sizeof(p), "%s/%s", root, rel);
   mkdir(p, 0755);
}

/* A GBA file: 0x100 bytes of `fill`, the code at 0xAC. */
static void put_gba(const char *rel, const char *code, int fill)
{
   char p[APP_PATH_CAP];
   unsigned char b[0x200];
   FILE *f;
   memset(b, fill, sizeof(b));
   memcpy(b + 0xAC, code, 4);
   snprintf(p, sizeof(p), "%s/%s", root, rel);
   f = fopen(p, "wb");
   fwrite(b, 1, sizeof(b), f);
   fclose(f);
}

static void put_gb(const char *rel, const char *title, int fill)
{
   char p[APP_PATH_CAP];
   unsigned char b[0x200];
   FILE *f;
   memset(b, fill, sizeof(b));
   memset(b + 0x134, 0, 16);
   memcpy(b + 0x134, title, strlen(title));
   snprintf(p, sizeof(p), "%s/%s", root, rel);
   f = fopen(p, "wb");
   fwrite(b, 1, sizeof(b), f);
   fclose(f);
}

static void put_text(const char *rel, const char *text)
{
   char p[APP_PATH_CAP];
   FILE *f;
   snprintf(p, sizeof(p), "%s/%s", root, rel);
   f = fopen(p, "wb");
   fputs(text, f);
   fclose(f);
}

static void w16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char *p, unsigned long v)
{
   w16(p, (unsigned)(v & 0xFFFF)); w16(p + 2, (unsigned)(v >> 16));
}

/* A one-entry zip holding `data` as `name`, stored (method 0) or "deflated"
 * (method 8: the bytes are not a real stream; only the directory is read). */
static void put_zip(const char *rel, const char *name, const unsigned char *data,
                    size_t len, int method)
{
   unsigned char lh[30], cd[46], eo[22];
   size_t nl = strlen(name), cd_off;
   char p[APP_PATH_CAP];
   FILE *f;
   memset(lh, 0, sizeof(lh)); memset(cd, 0, sizeof(cd)); memset(eo, 0, sizeof(eo));
   w32(lh, 0x04034b50ul); w16(lh + 8, (unsigned)method);
   w32(lh + 18, len); w32(lh + 22, len); w16(lh + 26, (unsigned)nl);
   cd_off = 30 + nl + len;
   w32(cd, 0x02014b50ul); w16(cd + 10, (unsigned)method);
   w32(cd + 20, len); w32(cd + 24, len); w16(cd + 28, (unsigned)nl);
   w32(cd + 42, 0);
   w32(eo, 0x06054b50ul); w16(eo + 8, 1); w16(eo + 10, 1);
   w32(eo + 12, 46 + nl); w32(eo + 16, cd_off);
   snprintf(p, sizeof(p), "%s/%s", root, rel);
   f = fopen(p, "wb");
   fwrite(lh, 1, 30, f); fwrite(name, 1, nl, f); fwrite(data, 1, len, f);
   fwrite(cd, 1, 46, f); fwrite(name, 1, nl, f); fwrite(eo, 1, 22, f);
   fclose(f);
}

static void gba_bytes(unsigned char *b, size_t n, const char *code, int fill)
{
   memset(b, fill, n);
   memcpy(b + 0xAC, code, 4);
}

static char *idof(const char *rel, char out[9])
{
   char p[APP_PATH_CAP];
   snprintf(p, sizeof(p), "%s/%s", root, rel);
   ui_host_online_rom_id(p, out);
   return out;
}

/* Run a whole search; returns the hits. */
static int search(const char *console, const char *game, const char *rom,
                  online_lib_hit *h, int per_call)
{
   int guard = 0, n;
   if (online_lib_begin(console, game, rom) != 0)
      return -1;
   while (!online_lib_step(per_call) && ++guard < 10000)
      ;
   n = online_lib_hits(h, ONLINE_LIB_MAX);
   return n;
}

static const char *base(const char *p)
{
   const char *s = strrchr(p, '/');
   return s ? s + 1 : p;
}

int main(void)
{
   online_lib_hit h[ONLINE_LIB_MAX], syn[3];
   char id[9], rel[APP_PATH_CAP];
   unsigned char zb[0x200];
   int n, i, calls, per_max, guard;

   snprintf(root, sizeof(root), "/tmp/online_lib_test.%d", (int)getpid());
   mkdir(root, 0755);
   mk("roms"); mk("roms/sub"); mk("roms/sub/deep"); mk("extra"); mk("extra/Hacks");
   snprintf(g_app_rom_dir, sizeof(g_app_rom_dir), "%s/roms", root);
   snprintf(g_extra[0], sizeof(g_extra[0]), "%s/extra", root);
   g_extra_n = 1;

   /* -- the tree: Emerald three times (one is the host's), others, a miss -- */
   put_gba("roms/Emerald (USA).gba", "BPEE", 0x11);            /* a dump      */
   put_gba("roms/sub/Emerald (Europe).gba", "BPEE", 0x22);     /* another     */
   put_gba("extra/Hacks/Emerald host copy.gba", "BPEE", 0x33); /* the host's  */
   put_gba("roms/Ruby.gba", "AXVE", 0x44);
   put_gba("roms/sub/deep/Emerald too deep.gba", "BPEE", 0x55); /* depth 3: not seen */
   put_gb("roms/Tetris.gb", "TETRIS", 0x66);
   put_gb("roms/Tetris (color).gbc", "TETRIS", 0x67);
   put_text("roms/readme.txt", "BPEE");
   gba_bytes(zb, sizeof(zb), "BPEE", 0x77);
   put_zip("roms/Stored.zip", "Stored Emerald.gba", zb, sizeof(zb), 0);
   gba_bytes(zb, sizeof(zb), "AXVE", 0x78);
   put_zip("roms/Packed.zip", "Packed Ruby.gba", zb, sizeof(zb), 8);

   /* ---- 1. a code with three files and the host's bytes among them ------ */
   idof("extra/Hacks/Emerald host copy.gba", id);
   n = search("gba", "BPEE", id, h, 8);
   CHECK(n == 4, "BPEE: %d hits (3 files + the stored zip), not the deep one", n);
   CHECK(n > 0 && h[0].rom_match && !strcmp(base(h[0].path), "Emerald host copy.gba"),
         "the host's bytes come first: %s", n > 0 ? base(h[0].path) : "-");
   for (i = 1; i < n; i++)
      CHECK(!h[i].rom_match, "only one file is the host's (%s)", base(h[i].path));
   CHECK(online_lib_decide(h, n, 1) == ONLINE_PICK_OPEN, "cable, host bytes found: OPEN");
   CHECK(online_lib_decide(h, n, 0) == ONLINE_PICK_OPEN, "adapter, host bytes found: OPEN");
   CHECK(online_lib_skipped() == 1, "the deflated zip was counted: %d", online_lib_skipped());
   for (i = 0; i < n; i++)
      CHECK(strstr(h[i].path, "too deep") == NULL, "depth 3 is not walked");
   online_lib_end();

   /* ---- 2. the same, no file is the host's ------------------------------ */
   n = search("gba", "BPEE", "deadbeef", h, 8);
   CHECK(n == 4, "BPEE, other bytes: %d hits", n);
   for (i = 0; i < n; i++)
      CHECK(!h[i].rom_match, "no match for deadbeef");
   CHECK(online_lib_decide(h, n, 1) == ONLINE_PICK_ASK, "cable, several, none the host's: ASK");
   CHECK(online_lib_decide(h, n, 0) == ONLINE_PICK_ASK, "adapter, several: ASK");
   online_lib_end();

   /* ---- 3. one hit only: cable asks, the adapter opens ------------------- */
   n = search("gba", "AXVE", "deadbeef", h, 8);
   CHECK(n == 1 || n == 1, "AXVE: %d hits (the deflated zip is skipped)", n);
   CHECK(online_lib_decide(h, n, 1) == ONLINE_PICK_ASK,
         "cable game, one hit that is not known to be the host's: ASK (never opened on its code)");
   CHECK(online_lib_decide(h, n, 0) == ONLINE_PICK_OPEN, "adapter, one hit: OPEN");
   online_lib_end();

   /* ---- 4. the card's rom is empty --------------------------------------- */
   n = search("gba", "AXVE", "", h, 8);
   CHECK(n == 1 && !h[0].rom_match && h[0].rom[0] == '\0', "empty rom: no match, no id");
   CHECK(online_lib_decide(h, n, 1) == ONLINE_PICK_ASK, "empty rom, cable: ASK");
   CHECK(g_rom_ids >= 0, "(counter)");
   online_lib_end();
   {
      int before = g_rom_ids;
      n = search("gba", "BPEE", "", h, 8);
      CHECK(g_rom_ids == before, "an empty rom asks the hook for nothing");
      CHECK(online_lib_decide(h, n, 1) == ONLINE_PICK_ASK, "empty rom, three files, cable: ASK");
      online_lib_end();
   }

   /* ---- 5. a miss -------------------------------------------------------- */
   n = search("gba", "ZZZZ", "00000000", h, 8);
   CHECK(n == 0 && online_lib_decide(h, n, 1) == ONLINE_PICK_NONE, "no such code: NONE (%d)", n);
   online_lib_end();

   /* ---- 6. Game Boy: the title's first four letters ---------------------- */
   n = search("gb", "TETR", "", h, 8);
   CHECK(n == 2, "TETR: the .gb and the .gbc, %d hits", n);
   n = search("gbc", "TETR", "", h, 8);
   CHECK(n == 2, "gbc room: both files (a cable links either), %d hits", n);
   online_lib_end();
   n = search("gb", "GAME", "", h, 8);
   CHECK(n == 0, "GAME is a fallback, not a title here (%d)", n);
   online_lib_end();

   /* ---- 7. a patched file whose patched image is the host's -------------- */
   put_gba("roms/Patched.gba", "BXYZ", 0x88);
   put_text("roms/Patched.ips", "PATCH body that yields the host's image");
   snprintf(rel, sizeof(rel), "%s/roms/Patched.ips", root);
   snprintf(id, sizeof(id), "%08x", hash_file(rel));        /* host's identity */
   n = search("gba", "BXYZ", id, h, 8);
   CHECK(n == 1 && h[0].patched && h[0].rom_match, "patched file: patched=%d match=%d",
         n ? h[0].patched : -1, n ? h[0].rom_match : -1);
   CHECK(online_lib_decide(h, n, 1) == ONLINE_PICK_OPEN, "its patched image is the host's: OPEN");
   online_lib_end();
   put_gba("roms/Plain.gba", "BXYW", 0x89);
   n = search("gba", "BXYW", "00000001", h, 8);
   CHECK(n == 1 && !h[0].patched, "no patch beside it: patched=%d", n ? h[0].patched : -1);
   online_lib_end();

   /* ---- 8. the budget: never more than `files` files per call ----------- */
   {
      int total = 0;
      mk("roms/many");
      for (i = 0; i < 20; i++)
      {
         snprintf(rel, sizeof(rel), "roms/many/F%02d.gba", i);
         put_gba(rel, "NOPE", 0x90 + i);
      }
      CHECK(online_lib_begin("gba", "QQQQ", "") == 0, "begin");
      per_max = 0; calls = 0; guard = 0;
      for (;;)
      {
         int d;
         g_opens = 0;
         d = online_lib_step(3);
         calls++;
         total += g_opens;
         if (g_opens > per_max)
            per_max = g_opens;
         if (d || ++guard > 1000)
            break;
      }
      CHECK(per_max <= 3, "3 files a call: the most opened in one call was %d", per_max);
      CHECK(calls > 5, "the walk took %d calls (it is spread over frames)", calls);
      online_lib_end();
   }
   {
      /* the identity is one file per call even with a big budget */
      int per_call_ids = 0, d;
      CHECK(online_lib_begin("gba", "BPEE", idof("extra/Hacks/Emerald host copy.gba", id)) == 0, "begin");
      guard = 0;
      do
      {
         int before = g_rom_ids;
         d = online_lib_step(100000);
         if (g_rom_ids - before > per_call_ids)
            per_call_ids = g_rom_ids - before;
      } while (!d && ++guard < 1000);
      CHECK(per_call_ids == 1, "at most one file identity per call (%d)", per_call_ids);
      online_lib_end();
   }

   /* ---- 9. the list holds ONLINE_LIB_MAX ---------------------------------- */
   for (i = 0; i < 12; i++)
   {
      snprintf(rel, sizeof(rel), "extra/Dup%02d.gba", i);
      put_gba(rel, "DUPL", 0xA0 + i);
   }
   n = search("gba", "DUPL", "", h, 8);
   CHECK(n == ONLINE_LIB_MAX, "twelve copies, the list stops at %d", n);
   online_lib_end();

   /* ---- 10. the pure decision, with hits made by hand --------------------- */
   memset(syn, 0, sizeof(syn));
   CHECK(online_lib_decide(syn, 0, 1) == ONLINE_PICK_NONE, "no hits: NONE");
   CHECK(online_lib_decide(NULL, 0, 0) == ONLINE_PICK_NONE, "NULL: NONE");
   syn[0].rom_match = 1; syn[1].rom_match = 1;
   CHECK(online_lib_decide(syn, 2, 1) == ONLINE_PICK_OPEN, "two with the host's bytes (the same bytes): OPEN");
   memset(syn, 0, sizeof(syn));
   syn[1].rom_match = 1;
   CHECK(online_lib_decide(syn, 3, 0) == ONLINE_PICK_ASK, "a match that is not first: the caller must not open hits[0]");
   CHECK(online_lib_decide(syn, 3, 1) == ONLINE_PICK_ASK, "(cable too)");
   memset(syn, 0, sizeof(syn));
   CHECK(online_lib_decide(syn, 1, 1) == ONLINE_PICK_ASK, "cable, one, no match: ASK");
   CHECK(online_lib_decide(syn, 1, 0) == ONLINE_PICK_OPEN, "adapter, one: OPEN");
   CHECK(online_lib_decide(syn, 2, 0) == ONLINE_PICK_ASK, "adapter, two: ASK");
   CHECK(online_lib_decide(syn, 3, 1) == ONLINE_PICK_ASK, "cable, three: ASK");

   /* unsupported consoles and bad input */
   CHECK(online_lib_begin("nes", "ABCD", "") != 0, "milestone B consoles: not yet");
   CHECK(online_lib_begin("gba", "", "") != 0, "an empty code");
   CHECK(online_lib_step(8) == 1, "no search: finished");
   online_lib_end();

   {
      char cmd[APP_PATH_CAP + 16];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
      if (system(cmd)) { }
   }
   printf("online_lib_test: %d checks, %d failed\n", checks, fails);
   return fails ? 1 : 0;
}
