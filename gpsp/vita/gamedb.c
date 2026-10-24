/* gamedb.c -- see gamedb.h. */
#include "gamedb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *g_text;               /* the whole file, NUL-terminated lines */
static char **g_line;              /* each record's start, in file order   */
static int g_lines;
static const char **g_names;       /* the C and S records' values, sorted  */
static int g_nnames;

/* gamedb_open_later: the file is read by the first lookup instead of at
 * boot (on the console that was the 907 KB read and a sort of ten thousand
 * names before the game list could show; every lookup is made by a worker
 * thread anyway).  DB_LATER until a lookup claims the read, DB_READING
 * while it reads (the other lookups wait for it), DB_READY after. */
enum { DB_NONE = 0, DB_LATER, DB_READING, DB_READY };
static int g_db_state;
static char g_db_path[1024];
static int g_db_records = -1;
static int db_load(const char *path);

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
static void db_nap(void) { sceKernelDelayThread(1000); }
#else
#include <time.h>
static void db_nap(void)
{
   struct timespec t = { 0, 1000000 };
   nanosleep(&t, NULL);
}
#endif

int gamedb_open_later(const char *path)
{
   if (!path || strlen(path) >= sizeof(g_db_path))
      return -1;
   gamedb_close();
   strcpy(g_db_path, path);
   g_db_records = -1;
   __atomic_store_n(&g_db_state, DB_LATER, __ATOMIC_RELEASE);
   return 0;
}

int gamedb_records(void)
{
   return __atomic_load_n(&g_db_state, __ATOMIC_ACQUIRE) == DB_READY
          ? g_db_records : -1;
}

/* Before any lookup: the data is there, or there is none to wait for. */
static void db_ready(void)
{
   int st = __atomic_load_n(&g_db_state, __ATOMIC_ACQUIRE);
   if (st == DB_READY || st == DB_NONE)
      return;
   if (st == DB_LATER &&
       __atomic_compare_exchange_n(&g_db_state, &st, DB_READING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
   {
      g_db_records = db_load(g_db_path);
      __atomic_store_n(&g_db_state, DB_READY, __ATOMIC_RELEASE);
      return;
   }
   while (__atomic_load_n(&g_db_state, __ATOMIC_ACQUIRE) == DB_READING)
      db_nap();
}

static void db_free(void)
{
   free(g_names);
   free(g_line);
   free(g_text);
   g_names = NULL;
   g_line = NULL;
   g_text = NULL;
   g_lines = g_nnames = 0;
}

void gamedb_close(void)
{
   free(g_names);
   free(g_line);
   free(g_text);
   g_names = NULL;
   g_line = NULL;
   g_text = NULL;
   g_lines = g_nnames = 0;
   __atomic_store_n(&g_db_state, DB_NONE, __ATOMIC_RELEASE);
}

static int name_cmp(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int gamedb_open(const char *path)
{
   int n;
   gamedb_close();
   n = db_load(path);
   g_db_records = n;
   __atomic_store_n(&g_db_state, DB_READY, __ATOMIC_RELEASE);
   return n;
}

static int db_load(const char *path)
{
   FILE *f;
   long size;
   char *p, *end;
   int n = 0, k = 0;

   f = fopen(path, "rb");
   if (!f)
      return 0;
   if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
       size > 16L * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0 ||
       !(g_text = (char *)malloc((size_t)size + 1)))
   {
      fclose(f);
      return 0;
   }
   if (fread(g_text, 1, (size_t)size, f) != (size_t)size)
   {
      fclose(f);
      db_free();
      return 0;
   }
   fclose(f);
   g_text[size] = '\0';
   end = g_text + size;
   for (p = g_text; p < end; p++)
      if (*p == '\n')
         n++;
   g_line = (char **)malloc(((size_t)n + 1) * sizeof(char *));
   g_names = (const char **)malloc(((size_t)n + 1) * sizeof(char *));
   if (!g_line || !g_names)
   {
      db_free();
      return 0;
   }
   /* each line: "<key>\t<value>" -> "<key>\0<value>\0" */
   for (p = g_text; p < end;)
   {
      char *nl = memchr(p, '\n', (size_t)(end - p));
      char *tab;
      if (!nl)
         nl = end;
      *nl = '\0';
      tab = strchr(p, '\t');
      if (tab && tab > p)
      {
         *tab = '\0';
         g_line[g_lines++] = p;
         if (*p == 'C' || *p == 'S')
            g_names[k++] = tab + 1;
      }
      p = nl + 1;
   }
   g_nnames = k;
   qsort(g_names, (size_t)g_nnames, sizeof(char *), name_cmp);
   return g_lines;
}

static const char *lookup(const char *key)
{
   db_ready();
   int lo = 0, hi = g_lines - 1;
   while (lo <= hi)
   {
      int mid = (lo + hi) / 2;
      int c = strcmp(g_line[mid], key);
      if (!c)
         return g_line[mid] + strlen(g_line[mid]) + 1;
      if (c < 0)
         lo = mid + 1;
      else
         hi = mid - 1;
   }
   return NULL;
}

static void copy_out(char *out, size_t cap, const char *s)
{
   size_t n;
   if (!out || !cap)
      return;
   n = strlen(s);
   if (n >= cap)
      n = cap - 1;
   memcpy(out, s, n);
   out[n] = '\0';
}

int gamedb_get(const char *key, char *out, size_t cap)
{
   const char *v = key ? lookup(key) : NULL;
   if (!v)
      return 0;
   copy_out(out, cap, v);
   return 1;
}

int gamedb_known_name(const char *name)
{
   int lo, hi;
   db_ready();
   lo = 0;
   hi = g_nnames - 1;
   if (!name)
      return 0;
   while (lo <= hi)
   {
      int mid = (lo + hi) / 2;
      int c = strcmp(g_names[mid], name);
      if (!c)
         return 1;
      if (c < 0)
         lo = mid + 1;
      else
         hi = mid - 1;
   }
   return 0;
}

void gamedb_base_title(const char *name, char *out, size_t cap)
{
   char t[256];
   const char *par;
   size_t n, i;

   if (!out || !cap)
      return;
   out[0] = '\0';
   if (!name)
      return;
   par = strstr(name, " (");
   n = par ? (size_t)(par - name) : strlen(name);
   if (n >= sizeof(t))
      n = sizeof(t) - 1;
   memcpy(t, name, n);
   t[n] = '\0';
   while (n && t[n - 1] == ' ')
      t[--n] = '\0';
   {
      const char *s = t;
      while (*s == ' ')
         s++;
      memmove(t, s, strlen(s) + 1);
      n = strlen(t);
   }
   /* the first ", The" / ", A" / ", An" that ends the title or a " - "
    * part of it goes back in front */
   for (i = 0; i + 2 < n; i++)
   {
      static const char *const art[3] = { "The", "A", "An" };
      int a;
      if (t[i] != ',' || t[i + 1] != ' ')
         continue;
      for (a = 0; a < 3; a++)
      {
         size_t l = strlen(art[a]);
         const char *after = t + i + 2 + l;
         if (strncmp(t + i + 2, art[a], l) == 0 &&
             (*after == '\0' || strncmp(after, " - ", 3) == 0))
         {
            char r[300];
            t[i] = '\0';
            snprintf(r, sizeof(r), "%s %s%s", art[a], t, after);
            copy_out(out, cap, r);
            return;
         }
      }
   }
   copy_out(out, cap, t);
}

/* U+00C0..U+00FF: the letter an accented one is, as NFKD takes it apart;
 * 0 for those it leaves whole (Æ, Ð, ×, Ø, Þ, ß, æ, ð, ÷, ø, þ). */
static const char g_fold[65] =
   "AAAAAA\0CEEEEIIII\0NOOOOO\0\0UUUUY\0\0aaaaaa\0ceeeeiiii\0nooooo\0\0uuuuy\0y";

void gamedb_title_key(const char *title, char *out, size_t cap)
{
   char k[300];
   size_t n = 0;
   const unsigned char *p = (const unsigned char *)title;

   if (!out || !cap)
      return;
   out[0] = '\0';
   if (!title)
      return;
   while (*p && n + 8 < sizeof(k))
   {
      unsigned c = *p++;
      char ch = 0;
      if (c == '&')
      {
         if (n && k[n - 1] != ' ')
            k[n++] = ' ';
         k[n++] = 'a'; k[n++] = 'n'; k[n++] = 'd';
         k[n++] = ' ';
         continue;
      }
      if (c >= 0x80)
      {
         /* UTF-8: a Latin-1 letter folds, anything else is a gap */
         if (c == 0xC3 && (*p & 0xC0) == 0x80)
         {
            ch = g_fold[(*p & 0x3F) | 0x00];
            p++;
         }
         else
            while ((*p & 0xC0) == 0x80)
               p++;
      }
      else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))
         ch = (char)c;
      else if (c >= 'A' && c <= 'Z')
         ch = (char)(c - 'A' + 'a');
      if (ch >= 'A' && ch <= 'Z')
         ch = (char)(ch - 'A' + 'a');
      if (ch)
         k[n++] = ch;
      else if (n && k[n - 1] != ' ')
         k[n++] = ' ';
   }
   while (n && k[n - 1] == ' ')
      n--;
   k[n] = '\0';
   {
      const char *s = k;
      size_t len = n;
      if (len >= 4 && strncmp(s, "the ", 4) == 0)
      {
         s += 4;
         len -= 4;
      }
      if (len >= 4 && strcmp(s + len - 4, " the") == 0)
         len -= 4;
      if (len >= cap)
         len = cap - 1;
      memmove(out, s, len);
      out[len] = '\0';
   }
}

int gamedb_blurb(const char *name, char *out, size_t cap)
{
   /* a file named the way people say it ("Pokemon - Emerald") misses the
    * word No-Intro adds: tried with each of them, and without */
   static const char *const more[4] = { "", " version", " edition", NULL };
   char base[256], key[300], art[256], wkey[300];
   int t;

   db_ready();
   if (!name || !g_lines)
      return 0;
   gamedb_base_title(name, base, sizeof(base));
   key[0] = 'T';
   gamedb_title_key(base, key + 1, sizeof(key) - 16);
   if (!key[1])
      return 0;
   for (t = 0; more[t]; t++)
   {
      size_t n = strlen(key);
      strcat(key, more[t]);
      if (gamedb_get(key, art, sizeof(art)))
      {
         snprintf(wkey, sizeof(wkey), "W%s", art);
         return gamedb_get(wkey, out, cap);
      }
      key[n] = '\0';
   }
   return 0;
}

int gamedb_name_by_code(const char code[4], char *out, size_t cap)
{
   char key[8];
   int i;
   key[0] = 'S';
   for (i = 0; i < 4; i++)
      key[1 + i] = code[i];
   key[5] = '\0';
   return gamedb_get(key, out, cap);
}

int gamedb_name_by_crc(uint32_t crc, char *out, size_t cap)
{
   char key[12];
   snprintf(key, sizeof(key), "C%08X", (unsigned)crc);
   return gamedb_get(key, out, cap);
}

int gamedb_read_code(const char *path, char code[4])
{
   unsigned char b[4];
   FILE *f = path ? fopen(path, "rb") : NULL;
   int i, ok;
   if (!f)
      return 0;
   ok = fseek(f, 0xAC, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4;
   fclose(f);
   if (!ok)
      return 0;
   for (i = 0; i < 4; i++)
   {
      if (!((b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= '0' && b[i] <= '9')))
         return 0;
      code[i] = (char)b[i];
   }
   return 1;
}

int gamedb_name_by_header(const char *path, char *out, size_t cap)
{
   char code[4], key[8], size[24];
   long have = -1;
   FILE *f;
   if (!gamedb_read_code(path, code))
      return 0;
   snprintf(key, sizeof(key), "Z%.4s", code);
   if (!gamedb_get(key, size, sizeof(size)))
      return 0;
   f = fopen(path, "rb");
   if (f)
   {
      if (fseek(f, 0, SEEK_END) == 0)
         have = ftell(f);
      fclose(f);
   }
   if (have < 0 || (unsigned long)have * 2 < strtoul(size, NULL, 10))
      return 0;
   return gamedb_name_by_code(code, out, cap);
}

static uint32_t g_crc_tab[256];

static int file_crc(const char *path, uint32_t *crc, volatile const int *abort,
                     int (*cancel)(void))
{
   static unsigned char buf[64 * 1024];
   uint32_t c = 0xFFFFFFFFu;
   FILE *f;
   size_t got;

   if (!g_crc_tab[1])
   {
      uint32_t i, j;
      for (i = 0; i < 256; i++)
      {
         uint32_t v = i;
         for (j = 0; j < 8; j++)
            v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
         g_crc_tab[i] = v;
      }
   }
   f = path ? fopen(path, "rb") : NULL;
   if (!f)
      return -1;
   for (;;)
   {
      size_t i;
      if ((abort && *abort) || (cancel && cancel()))
      {
         fclose(f);
         return -1;
      }
      got = fread(buf, 1, sizeof(buf), f);
      if (!got)
         break;
      for (i = 0; i < got; i++)
         c = g_crc_tab[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
   }
   if (ferror(f))
   {
      fclose(f);
      return -1;
   }
   fclose(f);
   *crc = c ^ 0xFFFFFFFFu;
   return 0;
}

int gamedb_file_crc(const char *path, uint32_t *crc, volatile const int *abort)
{
   return file_crc(path, crc, abort, NULL);
}

int gamedb_file_crc_cancel(const char *path, uint32_t *crc, int (*abort)(void))
{
   return file_crc(path, crc, NULL, abort);
}
