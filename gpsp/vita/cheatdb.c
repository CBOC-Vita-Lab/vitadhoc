/* cheatdb.c -- see cheatdb.h; the format is described in
 * tools/make_cheatdb.py.
 *
 * Plain stdio, like the rest of the app on the Vita.  The file is opened
 * for a lookup and closed again: a game boots once in a while, and nothing
 * here should hold a handle across a suspend.  A lookup is a dozen
 * 8-byte reads (a binary search of at most 5,000 entries); a load is one
 * read of at most a few tens of kilobytes.
 */
#include "cheatdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_MAGIC   "GBADCHT1"
#define DB_HEADER  64
#define KEY_MAX    256

static struct
{
   char     path[512];
   int      ok;
   uint32_t games;
   uint32_t n_crc, off_crc;
   uint32_t n_serial, off_serial;
   uint32_t n_name, off_name;
   uint32_t off_keys, size_keys;
   uint32_t off_games, off_blocks, size, cheats;
} db;

static uint32_t le32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
          ((uint32_t)p[3] << 24);
}

static int read_at(FILE *f, uint32_t offset, void *out, size_t size)
{
   if (fseek(f, (long)offset, SEEK_SET) != 0)
      return -1;
   return fread(out, 1, size, f) == size ? 0 : -1;
}

/* ---- CRC-32 (IEEE, as zlib and the No-Intro lists) ------------------------
 * Slicing by four: a 32 MiB cartridge is hashed once in its life here (the
 * result is cached by cheats_vita.c), but once is still a wait. */
static uint32_t crc_table[4][256];

static void crc_init(void)
{
   uint32_t i, j, c;
   if (crc_table[0][1])
      return;
   for (i = 0; i < 256; i++)
   {
      c = i;
      for (j = 0; j < 8; j++)
         c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
      crc_table[0][i] = c;
   }
   for (i = 0; i < 256; i++)
      for (j = 1; j < 4; j++)
         crc_table[j][i] = (crc_table[j - 1][i] >> 8) ^
                           crc_table[0][crc_table[j - 1][i] & 0xFF];
}

uint32_t cheatdb_crc32(uint32_t crc, const void *data, size_t size)
{
   const uint8_t *p = (const uint8_t *)data;
   crc_init();
   crc = ~crc;
   while (size >= 4)
   {
      crc ^= le32(p);
      crc = crc_table[3][crc & 0xFF] ^ crc_table[2][(crc >> 8) & 0xFF] ^
            crc_table[1][(crc >> 16) & 0xFF] ^ crc_table[0][crc >> 24];
      p += 4;
      size -= 4;
   }
   while (size--)
      crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 0xFF];
   return ~crc;
}

/* ---- open ------------------------------------------------------------------ */

void cheatdb_close(void)
{
   memset(&db, 0, sizeof(db));
}

int cheatdb_open(const char *path)
{
   uint8_t h[DB_HEADER];
   FILE *f;
   long end;

   cheatdb_close();
   if (!path || !*path || strlen(path) >= sizeof(db.path))
      return 0;
   f = fopen(path, "rb");
   if (!f)
      return 0;
   if (fread(h, 1, sizeof(h), f) != sizeof(h) || memcmp(h, DB_MAGIC, 8) ||
       le32(h + 8) != 1 || fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return 0;
   }
   end = ftell(f);
   fclose(f);

   db.games = le32(h + 12);
   db.n_crc = le32(h + 16);
   db.off_crc = le32(h + 20);
   db.n_serial = le32(h + 24);
   db.off_serial = le32(h + 28);
   db.n_name = le32(h + 32);
   db.off_name = le32(h + 36);
   db.off_keys = le32(h + 40);
   db.size_keys = le32(h + 44);
   db.off_games = le32(h + 48);
   db.off_blocks = le32(h + 52);
   db.size = le32(h + 56);
   db.cheats = le32(h + 60);

   /* A truncated or foreign file must not send a read anywhere odd. */
   if (end < 0 || (uint32_t)end != db.size || !db.games ||
       db.games > 0x100000 || db.n_crc > 0x1000000 ||
       db.n_serial > 0x1000000 || db.n_name > 0x1000000 ||
       (uint64_t)db.off_games + 16ull * db.games > db.size ||
       (uint64_t)db.off_crc + 8ull * db.n_crc > db.size ||
       (uint64_t)db.off_serial + 8ull * db.n_serial > db.size ||
       (uint64_t)db.off_name + 8ull * db.n_name > db.size ||
       (uint64_t)db.off_keys + db.size_keys > db.size ||
       db.off_blocks > db.size)
   {
      cheatdb_close();
      return 0;
   }
   strcpy(db.path, path);
   db.ok = 1;
   return (int)db.games;
}

int cheatdb_games(void)
{
   return db.ok ? (int)db.games : 0;
}

int cheatdb_cheats(void)
{
   return db.ok ? (int)db.cheats : 0;
}

/* ---- the two fixed-key indexes ----------------------------------------------
 * Entries are 8 bytes: a 4-byte key and the game.  `be` says how the key
 * orders: the CRC as a number (little-endian on disk), the serial as text. */
static int find8(uint32_t offset, uint32_t count, uint32_t key, int be,
                 uint32_t *value)
{
   FILE *f;
   uint32_t lo = 0, hi = count;
   int found = -1;

   if (!db.ok || !count)
      return -1;
   f = fopen(db.path, "rb");
   if (!f)
      return -1;
   while (lo < hi)
   {
      uint32_t mid = lo + (hi - lo) / 2, k;
      uint8_t e[8];
      if (read_at(f, offset + 8 * mid, e, 8) != 0)
         break;
      k = be ? ((uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 |
                (uint32_t)e[2] << 8 | e[3])
             : le32(e);
      if (k == key)
      {
         *value = le32(e + 4);
         found = 0;
         break;
      }
      if (k < key)
         lo = mid + 1;
      else
         hi = mid;
   }
   fclose(f);
   return found;
}

int cheatdb_find_crc(uint32_t crc)
{
   uint32_t g;
   if (find8(db.off_crc, db.n_crc, crc, 0, &g) != 0 || g >= db.games)
      return -1;
   return (int)g;
}

int cheatdb_find_serial(const char code[4], int *unique)
{
   uint32_t g, key = (uint32_t)(uint8_t)code[0] << 24 |
                     (uint32_t)(uint8_t)code[1] << 16 |
                     (uint32_t)(uint8_t)code[2] << 8 | (uint8_t)code[3];
   if (unique)
      *unique = 0;
   if (find8(db.off_serial, db.n_serial, key, 1, &g) != 0 ||
       (g & 0x7FFFFFFF) >= db.games)
      return -1;
   if (unique)
      *unique = (int)(g >> 31);
   return (int)(g & 0x7FFFFFFF);
}

/* ---- names -------------------------------------------------------------------
 * The same reading of a name as make_cheatdb.py's parse_name / norm_title:
 * change one, change the other (tests/cheats checks them against each
 * other through the built database). */

/* sorted: a region set printed in table order is in sorted order */
static const char *const regions[] = {
   "asia", "australia", "brazil", "canada", "china", "denmark", "europe",
   "finland", "france", "germany", "greece", "hong kong", "italy", "japan",
   "korea", "latin america", "netherlands", "norway", "portugal", "russia",
   "scandinavia", "spain", "sweden", "taiwan", "uk", "usa", "world"
};
#define N_REGIONS (sizeof(regions) / sizeof(regions[0]))

static const struct { const char *tag; const char *names; } goodtools[] = {
   { "u", "usa" }, { "e", "europe" }, { "j", "japan" }, { "g", "germany" },
   { "f", "france" }, { "s", "spain" }, { "i", "italy" }, { "w", "world" },
   { "a", "australia" }, { "k", "korea" }, { "c", "china" },
   { "ue", "usa,europe" }, { "eu", "usa,europe" }, { "ju", "japan,usa" },
   { "uj", "japan,usa" }, { "jue", "japan,usa,europe" },
   { "je", "japan,europe" }
};

static int lower(int c)
{
   return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int alnum(int c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z');
}

static int region_index(const char *s, size_t len)
{
   unsigned i;
   for (i = 0; i < N_REGIONS; i++)
      if (strlen(regions[i]) == len && !memcmp(regions[i], s, len))
         return (int)i;
   return -1;
}

/* A comma-separated list of region names, every part known: its bits, else
 * 0.  "unknown" is a name that stands for nothing. */
static uint32_t region_list(const char *s, size_t len, int *all_known)
{
   uint32_t mask = 0;
   size_t i = 0;
   *all_known = 1;
   while (i <= len)
   {
      size_t a = i, b;
      while (i < len && s[i] != ',')
         i++;
      b = i++;
      while (a < b && s[a] == ' ')
         a++;
      while (b > a && s[b - 1] == ' ')
         b--;
      if (b - a == 7 && !memcmp(s + a, "unknown", 7))
         continue;
      {
         int r = region_index(s + a, b - a);
         if (r < 0)
         {
            *all_known = 0;
            return 0;
         }
         mask |= 1u << r;
      }
   }
   return mask;
}

void cheatdb_name_keys(char system, const char *file_name, char *full,
                       char *base, size_t cap)
{
   const char *name = file_name, *p, *end;
   char title[KEY_MAX], rev[8] = "";
   size_t n = 0, i;
   uint32_t mask = 0;

   if (cap)
      full[0] = base[0] = 0;
   if (cap < 8)
      return;

   /* the file's own name: after the last / \ or : */
   for (p = file_name; *p; p++)
      if (*p == '/' || *p == '\\' || *p == ':')
         name = p + 1;
   end = name + strlen(name);
   /* ... without its extension */
   for (p = end; p > name && end - p <= 5; p--)
      if (p[-1] == '.')
      {
         const char *q;
         int ok = p < end;
         for (q = p; q < end; q++)
            if (!alnum((unsigned char)*q))
               ok = 0;
         if (ok)
            end = p - 1;
         break;
      }

   /* title: up to the first bracket, letters and digits only */
   for (p = name; p < end && *p != '(' && *p != '['; p++)
      if (alnum((unsigned char)*p) && n + 1 < sizeof(title))
         title[n++] = (char)lower((unsigned char)*p);
   title[n] = 0;

   /* the (tags) */
   for (; p < end; p++)
   {
      char tag[64];
      size_t len = 0, k;
      const char *q;
      int known;
      if (*p != '(')
         continue;
      for (q = p + 1; q < end && *q != ')'; q++)
         if (len + 1 < sizeof(tag))
            tag[len++] = (char)lower((unsigned char)*q);
      if (q >= end)
         break;
      p = q;
      while (len && tag[len - 1] == ' ')
         len--;
      for (k = 0; k < len && tag[k] == ' '; k++)
         ;
      memmove(tag, tag + k, len - k);
      len -= k;
      tag[len] = 0;

      if (len > 4 && !memcmp(tag, "rev ", 4))
      {
         size_t r = 0;
         int ok = 1;
         for (k = 4; k < len; k++)
            if (!alnum((unsigned char)tag[k]))
               ok = 0;
         if (ok)
         {
            for (k = 4; k < len && r + 1 < sizeof(rev); k++)
               rev[r++] = tag[k];
            rev[r] = 0;
            continue;
         }
      }
      if (len == 4 && tag[0] == 'v' && tag[1] == '1' && tag[2] == '.' &&
          tag[3] >= '0' && tag[3] <= '9')
      {
         rev[0] = tag[3] != '0' ? tag[3] : 0;
         rev[1] = 0;
         continue;
      }
      for (k = 0; k < sizeof(goodtools) / sizeof(goodtools[0]); k++)
         if (!strcmp(tag, goodtools[k].tag))
            break;
      if (k < sizeof(goodtools) / sizeof(goodtools[0]))
      {
         mask |= region_list(goodtools[k].names, strlen(goodtools[k].names),
                             &known);
         continue;
      }
      {
         uint32_t m = region_list(tag, len, &known);
         if (known)
            mask |= m;
      }
   }

   snprintf(base, cap, "%cb%s", system, title);
   n = (size_t)snprintf(full, cap, "%cf%s(", system, title);
   {
      int first = 1;
      for (i = 0; i < N_REGIONS && n < cap; i++)
         if (mask & (1u << i))
         {
            n += (size_t)snprintf(full + n, cap - n, "%s%s", first ? "" : ",",
                                  regions[i]);
            first = 0;
         }
   }
   if (n < cap)
      n += (size_t)snprintf(full + n, cap - n, ")%s%s", rev[0] ? "r" : "", rev);
   if (n >= cap)
      full[0] = 0;              /* does not fit: no such key in the index */
}

/* The name index entry at `i`: its key (NUL-terminated, cut at KEY_MAX) and
 * game. */
static int name_entry(FILE *f, uint32_t i, char *key, uint32_t *game)
{
   uint8_t e[8];
   uint32_t off, room;
   size_t got;

   if (read_at(f, db.off_name + 8 * i, e, 8) != 0)
      return -1;
   off = le32(e);
   *game = le32(e + 4);
   if (off >= db.size_keys)
      return -1;
   room = db.size_keys - off;
   if (room > KEY_MAX - 1)
      room = KEY_MAX - 1;
   if (fseek(f, (long)(db.off_keys + off), SEEK_SET) != 0)
      return -1;
   got = fread(key, 1, room, f);
   key[got] = 0;
   return 0;
}

/* First entry whose key is >= `want`. */
static uint32_t name_lower_bound(FILE *f, const char *want)
{
   uint32_t lo = 0, hi = db.n_name;
   char key[KEY_MAX];
   while (lo < hi)
   {
      uint32_t mid = lo + (hi - lo) / 2, g;
      if (name_entry(f, mid, key, &g) != 0)
         return db.n_name;
      if (strcmp(key, want) < 0)
         lo = mid + 1;
      else
         hi = mid;
   }
   return lo;
}

static int name_exact(FILE *f, const char *want)
{
   char key[KEY_MAX];
   uint32_t g, i;
   if (!want[0])
      return -1;
   i = name_lower_bound(f, want);
   if (i >= db.n_name || name_entry(f, i, key, &g) != 0 ||
       strcmp(key, want) || g >= db.games)
      return -1;
   return (int)g;
}

int cheatdb_find_name(char system, const char *file_name, int *exact)
{
   char full[KEY_MAX], base[KEY_MAX];
   FILE *f;
   int g;

   if (exact)
      *exact = 0;
   if (!db.ok || !db.n_name || !file_name)
      return -1;
   cheatdb_name_keys(system, file_name, full, base, sizeof(full));
   if (strlen(base) < 3)        /* no title at all */
      return -1;
   f = fopen(db.path, "rb");
   if (!f)
      return -1;
   g = name_exact(f, full);
   if (g >= 0 && exact)
      *exact = 1;
   if (g < 0)
      g = name_exact(f, base);
   fclose(f);
   return g;
}

int cheatdb_find_title(char system, const char *header_title)
{
   char want[KEY_MAX], key[KEY_MAX];
   size_t n = 2, len;
   uint32_t i, g;
   int found = -1;
   FILE *f;
   const char *p;

   if (!db.ok || !db.n_name || !header_title)
      return -1;
   want[0] = system;
   want[1] = 'b';
   for (p = header_title; *p && n + 1 < sizeof(want); p++)
      if (alnum((unsigned char)*p))
         want[n++] = (char)lower((unsigned char)*p);
   want[n] = 0;
   len = n;
   if (len < 2 + 4)             /* "ZOO": too little to go on */
      return -1;

   f = fopen(db.path, "rb");
   if (!f)
      return -1;
   for (i = name_lower_bound(f, want); i < db.n_name; i++)
   {
      if (name_entry(f, i, key, &g) != 0 || strncmp(key, want, len) ||
          g >= db.games)
         break;
      if (!key[len])
      {
         found = (int)g;        /* the whole title: that game */
         break;
      }
      if (found >= 0 && found != (int)g)
      {
         found = -1;            /* two games begin like this */
         break;
      }
      found = (int)g;
   }
   fclose(f);
   return found;
}

/* ---- one game ------------------------------------------------------------------ */

char *cheatdb_load(int game, unsigned *cheats, size_t *size)
{
   uint8_t e[16];
   uint32_t off, len, n, crc, i, strings = 0;
   char *block;
   FILE *f;

   if (cheats)
      *cheats = 0;
   if (size)
      *size = 0;
   if (!db.ok || game < 0 || (uint32_t)game >= db.games)
      return NULL;
   f = fopen(db.path, "rb");
   if (!f)
      return NULL;
   if (read_at(f, db.off_games + 16u * (uint32_t)game, e, 16) != 0)
   {
      fclose(f);
      return NULL;
   }
   off = le32(e);
   len = le32(e + 4);
   n = (uint32_t)e[8] | ((uint32_t)e[9] << 8);
   crc = le32(e + 12);
   if (off < db.off_blocks || (uint64_t)off + len > db.size || !len ||
       len > 0x400000)
   {
      fclose(f);
      return NULL;
   }
   block = (char *)malloc(len + 1);
   if (!block || read_at(f, off, block, len) != 0)
   {
      free(block);
      fclose(f);
      return NULL;
   }
   fclose(f);
   block[len] = 0;
   for (i = 0; i < len; i++)
      if (!block[i])
         strings++;
   /* the name and two strings a cheat, each ended, and the bytes intact */
   if (block[len - 1] || strings != 1 + 2 * n ||
       cheatdb_crc32(0, block, len) != crc)
   {
      free(block);
      return NULL;
   }
   if (cheats)
      *cheats = n;
   if (size)
      *size = len;
   return block;
}
