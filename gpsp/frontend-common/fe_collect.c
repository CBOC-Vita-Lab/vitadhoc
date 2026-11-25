/* fe_collect.c -- see fe_collect.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe_collect.h"

#define CP_TAG "#collection "

static unsigned fnv(const char *s, size_t n, int fold)
{
   unsigned h = 2166136261u;
   size_t i;
   for (i = 0; i < n; i++)
   {
      unsigned char ch = (unsigned char)s[i];
      if (fold && ch >= 'A' && ch <= 'Z')
         ch = (unsigned char)(ch + 32);
      h ^= ch;
      h *= 16777619u;
   }
   return h;
}

/* ====================== collections ====================================== */

void fe_coll_clear(fe_coll *c)
{
   c->ncoll = c->n = c->used = 0;
   c->lossy = c->unread = 0;
   c->line_n = 0;
   c->line_over = 0;
   c->cur = -1;
   memset(c->count, 0, sizeof(c->count));
}

size_t fe_coll_clean_name(const char *in, char *out, size_t cap)
{
   size_t n = 0, i, start = 0, end;
   char tmp[FE_COLL_NAME_MAX * 4];
   if (!cap)
      return 0;
   for (i = 0; in[i] && n < sizeof(tmp) - 1; i++)
      tmp[n++] = (unsigned char)in[i] < 32 || in[i] == 127 ? ' ' : in[i];
   tmp[n] = '\0';
   while (tmp[start] == ' ')
      start++;
   end = n;
   while (end > start && tmp[end - 1] == ' ')
      end--;
   n = end - start;
   if (n > FE_COLL_NAME_MAX - 1)
      n = FE_COLL_NAME_MAX - 1;
   if (n > cap - 1)
      n = cap - 1;
   /* not in the middle of a UTF-8 character */
   if (n < end - start)
      while (n && ((unsigned char)tmp[start + n] & 0xC0) == 0x80)
         n--;
   while (n && tmp[start + n - 1] == ' ')
      n--;
   memcpy(out, tmp + start, n);
   out[n] = '\0';
   return n;
}

static int ci_eq(const char *a, const char *b)
{
   for (; *a && *b; a++, b++)
   {
      char x = *a, y = *b;
      if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
      if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
      if (x != y)
         return 0;
   }
   return *a == *b;
}

int fe_coll_find(const fe_coll *c, const char *name)
{
   char clean[FE_COLL_NAME_MAX];
   unsigned i;
   if (!fe_coll_clean_name(name, clean, sizeof(clean)))
      return -1;
   for (i = 0; i < c->ncoll; i++)
      if (ci_eq(c->name[i], clean))
         return (int)i;
   return -1;
}

int fe_coll_create(fe_coll *c, const char *name)
{
   char clean[FE_COLL_NAME_MAX];
   if (!fe_coll_clean_name(name, clean, sizeof(clean)))
      return -2;
   if (fe_coll_find(c, clean) >= 0)
      return -3;
   if (c->ncoll >= FE_COLL_MAX)
      return -1;
   memcpy(c->name[c->ncoll], clean, strlen(clean) + 1);
   c->count[c->ncoll] = 0;
   return (int)c->ncoll++;
}

int fe_coll_rename(fe_coll *c, int idx, const char *name)
{
   char clean[FE_COLL_NAME_MAX];
   int other;
   if (idx < 0 || (unsigned)idx >= c->ncoll)
      return -1;
   if (!fe_coll_clean_name(name, clean, sizeof(clean)))
      return -2;
   other = fe_coll_find(c, clean);
   if (other >= 0 && other != idx)
      return -3;
   memcpy(c->name[idx], clean, strlen(clean) + 1);
   return 0;
}

/* Drop entry i: the pool closes over it, later offsets move. */
static void entry_drop(fe_coll *c, unsigned i)
{
   unsigned off = c->off[i], len = (unsigned)strlen(c->pool + off) + 1, k;
   memmove(c->pool + off, c->pool + off + len, c->used - off - len);
   c->used -= len;
   for (k = i + 1; k < c->n; k++)
   {
      c->hash[k - 1] = c->hash[k];
      c->off[k - 1] = c->off[k] - len;
      c->owner[k - 1] = c->owner[k];
   }
   c->n--;
}

int fe_coll_delete(fe_coll *c, int idx)
{
   unsigned i, k;
   if (idx < 0 || (unsigned)idx >= c->ncoll)
      return -1;
   for (i = 0; i < c->n;)
   {
      if (c->owner[i] == idx)
         entry_drop(c, i);
      else
         i++;
   }
   for (i = 0; i < c->n; i++)
      if (c->owner[i] > idx)
         c->owner[i]--;
   for (k = (unsigned)idx + 1; k < c->ncoll; k++)
   {
      memcpy(c->name[k - 1], c->name[k], sizeof(c->name[0]));
      c->count[k - 1] = c->count[k];
   }
   c->ncoll--;
   c->count[c->ncoll] = 0;
   return 0;
}

int fe_coll_has_h(const fe_coll *c, int idx, const char *rel, unsigned h)
{
   unsigned i;
   for (i = 0; i < c->n; i++)
      if (c->hash[i] == h && c->owner[i] == idx &&
          strcmp(c->pool + c->off[i], rel) == 0)
         return 1;
   return 0;
}

int fe_coll_has(const fe_coll *c, int idx, const char *rel)
{
   return fe_coll_has_h(c, idx, rel, fnv(rel, strlen(rel), 0));
}

int fe_coll_add(fe_coll *c, int idx, const char *rel)
{
   size_t len = strlen(rel);
   unsigned h;
   if (idx < 0 || (unsigned)idx >= c->ncoll || !len || len >= FE_COLL_PATH_MAX)
      return -1;
   h = fnv(rel, len, 0);
   if (fe_coll_has_h(c, idx, rel, h))
      return 0;
   if (c->n >= FE_COLL_ENTRIES || c->used + len + 1 > FE_COLL_POOL)
      return -1;
   memcpy(c->pool + c->used, rel, len + 1);
   c->hash[c->n] = h;
   c->off[c->n] = c->used;
   c->owner[c->n] = (unsigned char)idx;
   c->used += (unsigned)len + 1;
   c->n++;
   c->count[idx]++;
   return 0;
}

int fe_coll_remove(fe_coll *c, int idx, const char *rel)
{
   unsigned i, h = fnv(rel, strlen(rel), 0);
   for (i = 0; i < c->n; i++)
      if (c->hash[i] == h && c->owner[i] == idx &&
          strcmp(c->pool + c->off[i], rel) == 0)
      {
         entry_drop(c, i);
         c->count[idx]--;
         return 0;
      }
   return -1;
}

static void line_done(fe_coll *c)
{
   unsigned n = c->line_n, a = 0;
   char *l = c->line;
   c->line_n = 0;
   if (c->line_over)
   {
      c->line_over = 0;
      c->lossy = 1;                 /* a path too long to keep: not lost */
      return;
   }
   /* a UTF-8 byte order mark that a text editor put at the head of a line */
   if (n >= 3 && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB &&
       (unsigned char)l[2] == 0xBF)
   {
      l += 3;
      n -= 3;
   }
   while (n && (l[n - 1] == '\r' || l[n - 1] == ' ' || l[n - 1] == '\t'))
      n--;
   while (a < n && (l[a] == ' ' || l[a] == '\t'))
      a++;
   l[n] = '\0';
   l += a;
   if (a >= n)
      return;
   if (strncmp(l, CP_TAG, sizeof(CP_TAG) - 1) == 0)
   {
      char clean[FE_COLL_NAME_MAX];
      int k;
      if (!fe_coll_clean_name(l + sizeof(CP_TAG) - 1, clean, sizeof(clean)))
      {
         c->cur = -1;               /* its paths have no home: say so */
         c->lossy = 1;
         return;
      }
      k = fe_coll_find(c, clean);
      if (k < 0)
         k = fe_coll_create(c, clean);
      if (k < 0)
         c->lossy = 1;
      c->cur = k;
      return;
   }
   if (c->cur < 0)
   {
      c->lossy = 1;                 /* a path before any collection */
      return;
   }
   if (fe_coll_add(c, c->cur, l) != 0)
      c->lossy = 1;
}

void fe_coll_feed(fe_coll *c, const char *text, size_t len)
{
   size_t i;
   for (i = 0; i < len; i++)
   {
      if (text[i] == '\n')
         line_done(c);
      else if (c->line_n < FE_COLL_PATH_MAX + 8)
         c->line[c->line_n++] = text[i];
      else
         c->line_over = 1;
   }
}

void fe_coll_feed_end(fe_coll *c)
{
   if (c->line_n || c->line_over)
      line_done(c);
}

int fe_coll_write(const fe_coll *c, int (*w)(void *, const char *, size_t),
                  void *ctx)
{
   unsigned k, i;
   int rc;
   for (k = 0; k < c->ncoll; k++)
   {
      if ((rc = w(ctx, CP_TAG, sizeof(CP_TAG) - 1)) != 0 ||
          (rc = w(ctx, c->name[k], strlen(c->name[k]))) != 0 ||
          (rc = w(ctx, "\n", 1)) != 0)
         return rc;
      for (i = 0; i < c->n; i++)
         if (c->owner[i] == k)
         {
            const char *p = c->pool + c->off[i];
            if ((rc = w(ctx, p, strlen(p))) != 0 || (rc = w(ctx, "\n", 1)) != 0)
               return rc;
         }
   }
   return 0;
}

static int side(const char *path, const char *suffix, char *out, size_t cap)
{
   int n = snprintf(out, cap, "%s%s", path, suffix);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int exists(const char *p)
{
   FILE *f = fopen(p, "rb");
   if (!f)
      return 0;
   fclose(f);
   return 1;
}

int fe_coll_load(fe_coll *c, const char *path)
{
   char old[1100], buf[4096];
   FILE *f;
   size_t n;
   int bad = 0;
   fe_coll_clear(c);
   if (side(path, ".old", old, sizeof(old)) != 0)
      return -1;
   if (!exists(path) && exists(old))
      rename(old, path);
   f = fopen(path, "rb");
   if (!f)
      return 0;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      fe_coll_feed(c, buf, n);
   if (ferror(f))
      bad = 1;
   fclose(f);
   fe_coll_feed_end(c);
   if (bad)
   {
      c->unread = 1;
      return -1;
   }
   return 0;
}

typedef struct { FILE *f; } save_ctx;

static int save_w(void *ctx, const char *s, size_t n)
{
   return fwrite(s, 1, n, ((save_ctx *)ctx)->f) == n ? 0 : -1;
}

int fe_coll_save(const fe_coll *c, const char *path)
{
   char tmp[1100], old[1100];
   save_ctx sc;
   int rc = 0, had;
   if (c->lossy || c->unread)
      return -2;
   if (side(path, ".tmp", tmp, sizeof(tmp)) != 0 ||
       side(path, ".old", old, sizeof(old)) != 0)
      return -1;
   sc.f = fopen(tmp, "wb");
   if (!sc.f)
      return -1;
   if (fe_coll_write(c, save_w, &sc) != 0)
      rc = -1;
   if (fflush(sc.f) != 0)
      rc = -1;
   if (fclose(sc.f) != 0)
      rc = -1;
   if (rc != 0)
   {
      remove(tmp);
      return -1;
   }
   had = exists(path);
   if (had)
   {
      remove(old);
      if (rename(path, old) != 0)
      {
         remove(tmp);
         return -1;
      }
   }
   if (rename(tmp, path) != 0)
   {
      if (had)
         rename(old, path);
      remove(tmp);
      return -1;
   }
   if (had)
      remove(old);
   return 0;
}

/* ====================== playlists ======================================== */

unsigned fe_lpl_hash_name(const char *name)
{
   return fnv(name, strlen(name), 1);
}

void fe_lpl_norm(const char *in, char *out, size_t cap)
{
   size_t n = 0, i = 0, k;
   if (!cap)
      return;
   while (in[i] == '.' && (in[i + 1] == '/' || in[i + 1] == '\\'))
      i += 2;
   for (; in[i] && n < cap - 1; i++)
   {
      char ch = in[i];
      if (ch == '\\')
         ch = '/';
      if (ch >= 'A' && ch <= 'Z')
         ch = (char)(ch + 32);
      if (ch == '/' && n && out[n - 1] == '/')
         continue;
      out[n++] = ch;
   }
   out[n] = '\0';
   /* "ux0:/x" is "ux0:x": a device name of letters and digits, a colon */
   for (k = 0; out[k] && k < 9; k++)
   {
      char ch = out[k];
      if (ch == ':')
      {
         if (k && out[k + 1] == '/')
            memmove(out + k + 1, out + k + 2, strlen(out + k + 2) + 1);
         break;
      }
      if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
         break;
   }
}

/* The file's name: what follows the last "/", or the "#" RetroArch puts
 * after an archive ("pack.zip#game.gbc").  Any other "#" is part of the name
 * ("Game #1.gba" and "Other #1.gba" are two games). */
static const char *base_of(const char *norm)
{
   const char *b = norm, *p;
   for (p = norm; *p; p++)
      if (*p == '/' ||
          (*p == '#' && ((p - norm >= 4 && !memcmp(p - 4, ".zip", 4)) ||
                         (p - norm >= 3 && !memcmp(p - 3, ".7z", 3)))))
         b = p + 1;
   return b;
}

void fe_lpl_rom_key(const char *full_path, fe_lpl_rom *out)
{
   char norm[1100];
   const char *b, *dot, *p;
   fe_lpl_norm(full_path, norm, sizeof(norm));
   b = base_of(norm);
   dot = NULL;
   for (p = b; *p; p++)
      if (*p == '.')
         dot = p;
   out->hp = fnv(norm, strlen(norm), 0);
   out->hb = fnv(b, strlen(b), 0);
   out->hs = (dot && dot > b && strlen(dot) <= 6) ? fnv(b, (size_t)(dot - b), 0)
                                                  : out->hb;
}

void fe_lpl_clear(fe_lpl_set *s)
{
   s->nlist = s->nitem = 0;
   s->open = 0;
}

int fe_lpl_begin(fe_lpl_set *s, const char *name)
{
   fe_lpl_list *l;
   char clean[FE_COLL_NAME_MAX];
   size_t n;
   if (s->nlist >= FE_LPL_MAX)
      return -1;
   l = &s->list[s->nlist];
   memset(l, 0, sizeof(*l));
   fe_coll_clean_name(name, clean, sizeof(clean));
   n = strlen(clean);
   if (n > 4 && strcmp(clean + n - 4, ".lpl") == 0)
      clean[n - 4] = '\0';
   if (!clean[0])
      snprintf(clean, sizeof(clean), "Playlist %u", s->nlist + 1);
   memcpy(l->name, clean, strlen(clean) + 1);
   l->first = s->nitem;
   s->open = 1;
   s->depth = s->in_items = s->in_str = s->esc = s->uni_n = 0;
   s->have_str = s->key = s->saw_items = s->bad = 0;
   s->bytes = 0;
   s->str_n = s->path_n = 0;
   s->want = 0;
   return 0;
}

enum { K_NONE = 0, K_ITEMS, K_PATH, K_CRC };

static void put_utf8(fe_lpl_set *s, unsigned cp)
{
   char b[4];
   int n = 0, i;
   if (cp < 0x80) b[n++] = (char)cp;
   else if (cp < 0x800)
   {
      b[n++] = (char)(0xC0 | (cp >> 6));
      b[n++] = (char)(0x80 | (cp & 63));
   }
   else
   {
      b[n++] = (char)(0xE0 | (cp >> 12));
      b[n++] = (char)(0x80 | ((cp >> 6) & 63));
      b[n++] = (char)(0x80 | (cp & 63));
   }
   for (i = 0; i < n; i++)
      if (s->str_n < sizeof(s->str) - 1)
         s->str[s->str_n++] = b[i];
}

static void item_emit(fe_lpl_set *s)
{
   fe_lpl_list *l = &s->list[s->nlist];
   char norm[1100];
   const char *b;
   fe_lpl_item *it;
   unsigned crc = 0, v, i;
   int hex = 0;
   if (!s->path_n)
      return;
   if (l->n >= FE_LPL_PER_LIST || s->nitem >= FE_LPL_ITEMS)
   {
      l->dropped++;
      return;
   }
   s->path[s->path_n] = '\0';
   fe_lpl_norm(s->path, norm, sizeof(norm));
   b = base_of(norm);
   it = &s->item[s->nitem++];
   it->hp = fnv(norm, strlen(norm), 0);
   it->hb = fnv(b, strlen(b), 0);
   /* "AABBCCDD|crc" -- "DETECT" and anything else is no checksum */
   for (i = 0, v = 0; s->crc[i] && i < 8; i++)
   {
      char ch = s->crc[i];
      unsigned d = ch >= '0' && ch <= '9' ? (unsigned)(ch - '0')
                 : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10)
                 : ch >= 'A' && ch <= 'F' ? (unsigned)(ch - 'A' + 10) : 99u;
      if (d == 99u)
         break;
      v = v << 4 | d;
      hex++;
   }
   if (hex == 8 && s->crc[8] == '|')
      crc = v;
   it->crc = crc;
   l->n++;
}

/* A finished string and what follows it (the next structural character). */
static void value_done(fe_lpl_set *s)
{
   if (s->have_str && s->depth == 3 && s->in_items)
   {
      s->str[s->str_n] = '\0';
      if (s->key == K_PATH)
      {
         memcpy(s->path, s->str, s->str_n + 1);
         s->path_n = s->str_n;
      }
      else if (s->key == K_CRC)
         snprintf(s->crc, sizeof(s->crc), "%.39s", s->str);
   }
   s->have_str = 0;
   s->key = K_NONE;
}

static void structural(fe_lpl_set *s, char ch)
{
   switch (ch)
   {
   case ':':
      if (s->have_str)
      {
         s->str[s->str_n] = '\0';
         if (s->depth == 1 && !strcmp(s->str, "items"))
            s->key = K_ITEMS;
         else if (s->depth == 3 && !strcmp(s->str, "path"))
            s->key = K_PATH;
         else if (s->depth == 3 && !strcmp(s->str, "crc32"))
            s->key = K_CRC;
         else
            s->key = K_NONE;
      }
      s->have_str = 0;
      break;
   case ',':
      value_done(s);
      break;
   case '{':
   case '[':
      if (ch == '[' && s->depth == 1 && s->key == K_ITEMS)
      {
         s->in_items = 1;
         s->saw_items = 1;
      }
      s->depth++;
      s->have_str = 0;
      s->key = K_NONE;
      if (ch == '{' && s->in_items && s->depth == 3)
      {
         s->path_n = 0;
         s->path[0] = '\0';
         s->crc[0] = '\0';
      }
      if (ch == '[' && s->depth == 1)
         s->bad = 1;                /* a list, not an object */
      break;
   case '}':
   case ']':
      value_done(s);
      if (s->depth <= 0)
      {
         s->bad = 1;
         break;
      }
      if (ch == '}' && s->in_items && s->depth == 3)
         item_emit(s);
      if (ch == ']' && s->in_items && s->depth == 2)
         s->in_items = 0;
      s->depth--;
      break;
   default:
      break;
   }
}

void fe_lpl_feed(fe_lpl_set *s, const char *buf, size_t len)
{
   size_t i;
   if (!s->open || s->bad)
      return;
   if (s->bytes >= FE_LPL_BYTES_MAX)
      return;
   if (len > FE_LPL_BYTES_MAX - s->bytes)
   {
      len = FE_LPL_BYTES_MAX - s->bytes;
      s->list[s->nlist].cut = 1;
   }
   s->bytes += (unsigned)len;
   for (i = 0; i < len && !s->bad; i++)
   {
      unsigned char ch = (unsigned char)buf[i];
      if (s->in_str)
      {
         if (s->uni_n)
         {
            unsigned d = ch >= '0' && ch <= '9' ? (unsigned)(ch - '0')
                       : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10)
                       : ch >= 'A' && ch <= 'F' ? (unsigned)(ch - 'A' + 10) : 99u;
            if (d == 99u)
               s->uni_n = 0;        /* a broken escape: the rest is text */
            else
            {
               s->uni = s->uni << 4 | d;
               if (--s->uni_n == 0)
                  put_utf8(s, s->uni >= 0xD800 && s->uni < 0xE000 ? '?'
                                                                  : s->uni);
               continue;
            }
         }
         if (s->esc)
         {
            s->esc = 0;
            if (ch == 'u')
            {
               s->uni = 0;
               s->uni_n = 4;
               continue;
            }
            ch = ch == 'n' ? '\n' : ch == 't' ? '\t' : ch == 'r' ? '\r' : ch;
         }
         else if (ch == '\\')
         {
            s->esc = 1;
            continue;
         }
         else if (ch == '"')
         {
            s->in_str = 0;
            s->have_str = 1;
            continue;
         }
         if (s->str_n < sizeof(s->str) - 1)
            s->str[s->str_n++] = (char)ch;
         continue;
      }
      if (ch == '"')
      {
         s->in_str = 1;
         s->esc = 0;
         s->uni_n = 0;
         s->str_n = 0;
         s->have_str = 0;
         continue;
      }
      structural(s, (char)ch);
   }
}

int fe_lpl_end(fe_lpl_set *s)
{
   fe_lpl_list *l = &s->list[s->nlist];
   /* a file read only up to the cap stops anywhere, inside a string too */
   int ok = s->open && !s->bad && s->saw_items &&
            (l->cut || (!s->in_str && s->depth == 0));
   s->open = 0;
   if (!ok)
   {
      s->nitem = l->first;          /* as if it had not been there */
      return -1;
   }
   s->nlist++;
   return (int)l->n;
}

int fe_lpl_load(fe_lpl_set *s, const char *path, const char *name)
{
   FILE *f;
   char buf[4096];
   size_t n;
   int first = 1;
   if (s->nlist >= FE_LPL_MAX)
      return -2;
   f = fopen(path, "rb");
   if (!f)
      return -1;
   if (fe_lpl_begin(s, name) != 0)
   {
      fclose(f);
      return -2;
   }
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
   {
      if (first)
      {
         first = 0;
         if (n >= 6 && memcmp(buf, "#RZIPv", 6) == 0)
         {
            fclose(f);
            s->open = 0;
            return -1;              /* compressed: not read */
         }
      }
      fe_lpl_feed(s, buf, n);
      if (s->bad)
         break;
      if (s->bytes >= FE_LPL_BYTES_MAX)
      {
         if (fgetc(f) != EOF)
            s->list[s->nlist].cut = 1;   /* more than is read */
         break;
      }
   }
   if (ferror(f))
      s->bad = 1;
   fclose(f);
   return fe_lpl_end(s);
}

typedef struct { unsigned h; unsigned short i; } rkey;

static int rkey_cmp(const void *a, const void *b)
{
   const rkey *x = a, *y = b;
   if (x->h != y->h)
      return x->h < y->h ? -1 : 1;
   return (int)x->i - (int)y->i;
}

static rkey g_kp[FE_LPL_ROMS_MAX], g_kb[FE_LPL_ROMS_MAX], g_ks[FE_LPL_ROMS_MAX];
static unsigned char g_hit[FE_LPL_ROMS_MAX];

/* mark every game with hash h in the sorted keys; returns how many */
static int mark(const rkey *k, int n, unsigned h)
{
   int lo = 0, hi = n, hits = 0;
   while (lo < hi)
   {
      int mid = (lo + hi) / 2;
      if (k[mid].h < h)
         lo = mid + 1;
      else
         hi = mid;
   }
   for (; lo < n && k[lo].h == h; lo++, hits++)
      g_hit[k[lo].i] = 1;
   return hits;
}

unsigned fe_lpl_match(const fe_lpl_set *s, int list, const fe_lpl_rom *roms,
                      int n, unsigned short *out, unsigned cap,
                      unsigned (*crc_name)(unsigned, void *), void *ctx)
{
   const fe_lpl_list *l;
   unsigned i, got = 0;
   int k;
   if (list < 0 || (unsigned)list >= s->nlist || n <= 0)
      return 0;
   if (n > FE_LPL_ROMS_MAX)
      n = FE_LPL_ROMS_MAX;
   l = &s->list[list];
   for (k = 0; k < n; k++)
   {
      g_kp[k].h = roms[k].hp; g_kp[k].i = (unsigned short)k;
      g_kb[k].h = roms[k].hb; g_kb[k].i = (unsigned short)k;
      g_ks[k].h = roms[k].hs; g_ks[k].i = (unsigned short)k;
      g_hit[k] = 0;
   }
   qsort(g_kp, (size_t)n, sizeof(rkey), rkey_cmp);
   qsort(g_kb, (size_t)n, sizeof(rkey), rkey_cmp);
   qsort(g_ks, (size_t)n, sizeof(rkey), rkey_cmp);
   for (i = l->first; i < l->first + l->n; i++)
   {
      const fe_lpl_item *it = &s->item[i];
      unsigned h;
      if (mark(g_kp, n, it->hp))
         continue;
      if (mark(g_kb, n, it->hb))
         continue;
      if (it->crc && crc_name && (h = crc_name(it->crc, ctx)) != 0)
         mark(g_ks, n, h);
   }
   for (k = 0; k < n; k++)
      if (g_hit[k])
      {
         if (out && got < cap)
            out[got] = (unsigned short)k;
         got++;
      }
   return got;
}
