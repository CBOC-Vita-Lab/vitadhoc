/* online_lib.c -- find the game of a room in the library; see online_lib.h.
 *
 * The code pad's room card knows the host's game as a CODE (a GBA header's
 * four characters, a Game Boy title's first four letters) and, when the host
 * could say, the identity of the host's FILE (`rom`, plan 1.1).  A library can
 * hold several files with one code: two dumps, a hack, a translation.  So the
 * walk finds every file that carries the code and, for each, asks the host
 * hook for that file's identity; the files whose identity is the host's come
 * first.  What is done with the list is online_lib_decide(): a cable game is
 * never opened on its code alone.
 *
 * NOTHING HERE BLOCKS.  online_lib_step(files) looks at a few files a frame
 * (a header is four to sixteen bytes), and the one expensive thing, a file's
 * identity (about 256 KiB read), is taken once per call, so a frame never
 * waits for more than one.
 *
 * Where it looks: roms/ and then each extra folder (ui_host_extra_root), each
 * with its first level of folders: the depth link_vita.c's find_rom_in and the
 * game list use.
 * Files it reads: .gba (header at 0xAC), .gb / .gbc (title at 0x134), and a
 * .zip whose first game entry is STORED (not compressed): its header is read
 * where it lies.  A compressed .zip cannot be read without unpacking all of
 * it, which a frame cannot afford, so it is skipped (online_lib_skipped()). */
#include <stdint.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "online_lib.h"
#include "ui_host_hooks.h"
#include "zip_rom.h"
#include "../psp/room_label.h"

enum { OL_GBA = 0, OL_GB = 1,         /* the family of files that can match: found by a header's code */
       OL_NES, OL_SNES, OL_WS };      /* 2.0 "Two players": found by the whole game's CRC (below) */
#define OL_IS_PAD(f) ((f) >= OL_NES)

#define OL_DEPTH 2                    /* the folder, then one level in      */

static struct
{
   int  active;
   int  family;                       /* OL_GBA / OL_GB                     */
   char game[9];
   char rom[9];
   int  root;                         /* 0 = g_app_rom_dir, 1.. = extras    */
   int  depth;                        /* open directories                   */
   DIR *dir[OL_DEPTH];
   char path[OL_DEPTH][APP_PATH_CAP];
   int  nhit;
   online_lib_hit hit[ONLINE_LIB_MAX];
   int  skipped;                      /* compressed zips that were not read */
   int  done;
} G;

extern char g_app_rom_dir[APP_PATH_CAP];   /* main_vita.c */

static void close_dirs(void)
{
   while (G.depth > 0)
      closedir(G.dir[--G.depth]);
}

static const char *root_path(int i)
{
   if (i == 0)
      return g_app_rom_dir;
   if (i - 1 < ui_host_extra_roots())
      return ui_host_extra_root(i - 1);
   return NULL;
}

static int ext_is(const char *name, const char *ext)
{
   const char *dot = strrchr(name, '.');
   return dot && !strcasecmp(dot, ext);
}

int online_lib_begin(const char *console, const char *game, const char *rom)
{
   online_lib_end();
   if (!console || !game || !game[0])
      return -1;
   if (!strcmp(console, "gba"))
      G.family = OL_GBA;
   else if (!strcmp(console, "gb") || !strcmp(console, "gbc"))
      G.family = OL_GB;
   else if (!strcmp(console, "nes"))
      G.family = OL_NES;
   else if (!strcmp(console, "snes"))
      G.family = OL_SNES;
   else if (!strcmp(console, "ws") || !strcmp(console, "wsc"))
      G.family = OL_WS;
   else
      return -1;
   snprintf(G.game, sizeof(G.game), "%s", game);
   snprintf(G.rom, sizeof(G.rom), "%s", rom ? rom : "");
   G.active = 1;
   G.root = -1;                       /* step() opens root 0 first */
   return 0;
}

/* The code a file's header gives, in room_label.h's form: 1 and out. */
static int code_of(const uint8_t *hdr, char out[5])
{
   if (G.family == OL_GBA)
      room_code_gba((const char *)hdr, out);
   else
      room_code_gb((const char *)hdr, 16, out);
   return 1;
}

static int read_at(FILE *f, long off, uint8_t *buf, size_t n)
{
   return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* The header of the file at `path`: 4 bytes (GBA, 0xAC) or 16 (GB, 0x134).
 * 1 = read; 0 = not a file of this family or unreadable; -1 = a zip that is
 * compressed (counted, not an error). */
static int read_header(const char *path, uint8_t hdr[16])
{
   FILE *f;
   int ok = 0;
   long base = 0;
   size_t n = G.family == OL_GBA ? 4 : 16;
   long off = G.family == OL_GBA ? 0xAC : 0x134;

   if (ext_is(path, ".zip"))
   {
      zip_rom_entry e;
      uint8_t lh[30];
      if (zip_rom_peek(path, &e) != 0 || e.console < 0 ||
          (G.family == OL_GBA ? e.console != 0 : (e.console != 1 && e.console != 2)))
         return 0;
      if (e.method != 0)
         return -1;
      if (e.usize < (unsigned long)(off + (long)n))
         return 0;
      f = fopen(path, "rb");
      if (!f)
         return 0;
      /* local header: 30 bytes, then the name and the extra field, then
       * the data (stored: the file's bytes themselves) */
      if (read_at(f, (long)e.local, lh, sizeof(lh)) && lh[0] == 'P' &&
          lh[1] == 'K' && lh[2] == 3 && lh[3] == 4)
         base = (long)e.local + 30 + (lh[26] | (lh[27] << 8)) +
                (lh[28] | (lh[29] << 8));
      else
      {
         fclose(f);
         return 0;
      }
   }
   else
   {
      if (G.family == OL_GBA ? !ext_is(path, ".gba")
                             : !(ext_is(path, ".gb") || ext_is(path, ".gbc")))
         return 0;
      f = fopen(path, "rb");
      if (!f)
         return 0;
   }
   ok = read_at(f, base + off, hdr, n);
   fclose(f);
   return ok;
}

/* ---- Two players games: the game is its CRC-32 ----------------------------------
 * The room's `game` is the 8 hex digits of the game's CRC, the same text on both consoles (link_vita.c pad_game8):
 * the NES core's CRC of PRG + CHR (a file without its 16 byte header and trainer), the WonderSwan's of the whole file,
 * the SNES program's of the whole file without a 512 byte copier header.  Reading a file whole is the expensive thing
 * here (a few MiB), so the walk does one per call. */
static uint32_t ol_crc_table[256];

static uint32_t ol_crc(uint32_t crc, const uint8_t *p, size_t n)
{
   if (!ol_crc_table[1])
   {
      uint32_t i, k, c;
      for (i = 0; i < 256; i++)
      {
         for (c = i, k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
         ol_crc_table[i] = c;
      }
   }
   crc = ~crc;
   while (n--)
      crc = ol_crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
   return ~crc;
}

static int pad_ext_ok(const char *path)
{
   if (G.family == OL_NES)
      return ext_is(path, ".nes");
   if (G.family == OL_SNES)
      return ext_is(path, ".sfc") || ext_is(path, ".smc");
   return ext_is(path, ".ws") || ext_is(path, ".wsc");
}

/* The id of the file at `path` as 8 upper-case hex digits; 1 = read, 0 = not a game of this family. */
static int pad_id_of(const char *path, char out[9])
{
   FILE *f = fopen(path, "rb");
   uint8_t buf[8192];
   long size, from = 0, upto;
   uint32_t crc = 0;
   size_t n;
   if (!f)
      return 0;
   if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0)
   {
      fclose(f);
      return 0;
   }
   upto = size;
   if (G.family == OL_SNES && size % 1024 == 512)
      from = 512;
   else if (G.family == OL_NES)
   {
      uint8_t h[16];
      if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 16, f) != 16 || h[0] != 'N' || h[1] != 'E' || h[2] != 'S' ||
          h[3] != 0x1A)
      {
         fclose(f);
         return 0;
      }
      from = 16 + ((h[6] & 4) ? 512 : 0);
      upto = from + (long)h[4] * 16384 + (long)h[5] * 8192;
      if (upto > size)
         upto = size;
   }
   if (fseek(f, from, SEEK_SET) != 0)
   {
      fclose(f);
      return 0;
   }
   while (from < upto && (n = fread(buf, 1, upto - from < (long)sizeof(buf) ? (size_t)(upto - from) : sizeof(buf), f)) > 0)
   {
      crc = ol_crc(crc, buf, n);
      from += (long)n;
   }
   fclose(f);
   snprintf(out, 9, "%08X", (unsigned)crc);
   return 1;
}

/* A patch beside the game (rom_patch.h: Game.ips / .bps / .ups). */
static int has_patch(const char *path)
{
   static const char *const ext[] = { ".ips", ".bps", ".ups" };
   char p[APP_PATH_CAP];
   const char *dot = strrchr(path, '.');
   struct stat st;
   size_t stem, i;
   if (!dot || ext_is(path, ".zip"))
      return 0;
   stem = (size_t)(dot - path);
   for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
   {
      if (stem + 5 > sizeof(p))
         return 0;
      memcpy(p, path, stem);
      strcpy(p + stem, ext[i]);
      if (stat(p, &st) == 0)
         return 1;
   }
   return 0;
}

/* A new code match: its identity (one file's read), then into the list,
 * the files that ARE the host's before the others. */
static void add_hit(const char *path)
{
   online_lib_hit h;
   int at;
   if (G.nhit >= ONLINE_LIB_MAX)
      return;
   memset(&h, 0, sizeof(h));
   snprintf(h.path, sizeof(h.path), "%s", path);
   h.patched = has_patch(path);
   /* The host hook knows how a file is identified, patch included: for a
    * patched file it answers with the PATCHED image's identity or fails
    * (then the file simply is not known to be the host's). */
   if (OL_IS_PAD(G.family))
   {
      /* found by the game's CRC: it IS the host's file (a patch beside it is not part of what was hashed) */
      snprintf(h.rom, sizeof(h.rom), "%s", G.game);
      h.rom_match = !h.patched;
   }
   else if (G.rom[0] && ui_host_online_rom_id(path, h.rom) == 0 && h.rom[0])
      h.rom_match = strcmp(h.rom, G.rom) == 0;
   else
      h.rom[0] = '\0';
   at = h.rom_match ? 0 : G.nhit;
   if (h.rom_match)
      while (at < G.nhit && G.hit[at].rom_match)
         at++;
   memmove(&G.hit[at + 1], &G.hit[at], (size_t)(G.nhit - at) * sizeof(G.hit[0]));
   G.hit[at] = h;
   G.nhit++;
}

/* Open the directory for depth `d` (G.path[d] is set): 1 = open. */
static int open_dir(int d)
{
   G.dir[d] = opendir(G.path[d]);
   if (!G.dir[d])
      return 0;
   G.depth = d + 1;
   return 1;
}

/* The next directory entry of the walk, or 0 when everything has been seen. */
static int next_entry(char *path, size_t cap, int *is_dir)
{
   for (;;)
   {
      struct dirent *e;
      struct stat st;
      if (G.depth == 0)
      {
         const char *r;
         G.root++;
         r = root_path(G.root);
         if (!r)
            return 0;
         if (!r[0] || snprintf(G.path[0], sizeof(G.path[0]), "%s", r) >=
                      (int)sizeof(G.path[0]))
            continue;
         if (!open_dir(0))
            continue;
      }
      e = readdir(G.dir[G.depth - 1]);
      if (!e)
      {
         closedir(G.dir[--G.depth]);
         continue;
      }
      if (e->d_name[0] == '.')
         continue;
      if (snprintf(path, cap, "%s/%s", G.path[G.depth - 1], e->d_name) >= (int)cap ||
          stat(path, &st) != 0)
         continue;
      *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
      return 1;
   }
}

int online_lib_step(int files)
{
   char path[APP_PATH_CAP];
   int is_dir;
   if (!G.active || G.done)
      return 1;
   while (files > 0 && G.nhit < ONLINE_LIB_MAX)
   {
      if (!next_entry(path, sizeof(path), &is_dir))
      {
         G.done = 1;
         return 1;
      }
      if (is_dir)
      {
         /* the first level of folders only */
         if (G.depth < OL_DEPTH)
         {
            snprintf(G.path[G.depth], sizeof(G.path[0]), "%s", path);
            open_dir(G.depth);
         }
         continue;
      }
      {
         uint8_t hdr[16];
         char code[5];
         int rc;
         files--;
         if (OL_IS_PAD(G.family))
         {
            /* the whole file is the identity: a zip cannot be read in a frame, so it is counted and left */
            char id[9];
            if (ext_is(path, ".zip"))
            {
               G.skipped++;
               continue;
            }
            if (!pad_ext_ok(path) || !pad_id_of(path, id) || strcasecmp(id, G.game) != 0)
               continue;
            add_hit(path);
            break;
         }
         rc = read_header(path, hdr);
         if (rc < 0)
            G.skipped++;
         if (rc <= 0)
            continue;
         code_of(hdr, code);
         if (strcmp(code, G.game) != 0)
            continue;
         add_hit(path);
         break;                      /* a hit costs a file read: one a call */
      }
   }
   if (G.nhit >= ONLINE_LIB_MAX)
      G.done = 1;
   if (G.done)
      close_dirs();
   return G.done;
}

int online_lib_hits(online_lib_hit *out, int max)
{
   int n = G.nhit < max ? G.nhit : max;
   if (out && n > 0)
      memcpy(out, G.hit, (size_t)n * sizeof(out[0]));
   return n > 0 ? n : 0;
}

/* Compressed zips the walk had to leave unread (for the page's note). */
int online_lib_skipped(void)
{
   return G.skipped;
}

void online_lib_end(void)
{
   close_dirs();
   memset(&G, 0, sizeof(G));
}

int online_lib_decide(const online_lib_hit *hits, int n, int cable)
{
   int i, any_match = 0;
   if (!hits || n <= 0)
      return ONLINE_PICK_NONE;
   for (i = 0; i < n; i++)
      if (hits[i].rom_match)
         any_match = 1;
   /* the host's own bytes (online_lib_hits puts them first: hits[0] is what
    * the caller opens, so it must be one) */
   if (any_match)
      return hits[0].rom_match ? ONLINE_PICK_OPEN : ONLINE_PICK_ASK;
   /* the same game, but not known to be the host's file: a cable game (the
    * two machines run one program in lock step) is never opened on that */
   if (cable)
      return ONLINE_PICK_ASK;
   /* the Wireless Adapter: each console runs its own cartridge, the code
    * is what matters */
   return n == 1 ? ONLINE_PICK_OPEN : ONLINE_PICK_ASK;
}
