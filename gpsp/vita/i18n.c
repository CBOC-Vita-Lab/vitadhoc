/* i18n.c -- see i18n.h. */
#include "i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../playerkit/src/pk_utf8.h"   /* the audit reads UTF-8 */
#include "lang_en.h"      /* i18n_en[]: the English table, sorted by key */

const char *const i18n_code[I18N_LANGS] = {
   "auto", "en", "es", "fr", "de", "it", "pt-BR", "ja", "zh-Hans", "xx-long" };
/* The names of the languages in themselves (the Settings values); "Automatic"
 * is translated by the caller (settings.language.auto). */
const char *const i18n_native[I18N_LANGS] = {
   "Automatic", "English", "Espa\xc3\xb1ol", "Fran\xc3\xa7" "ais", "Deutsch",
   "Italiano", "Portugu\xc3\xaas (Brasil)",
   "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87",
   "Long test" };

#define MAX_FILE (1u << 20)        /* a catalogue past 1 MiB is not one */
#define MAX_LINE 4096

typedef struct { const char *key; const char *val; } ent;

static char  g_dir[256];
static int   g_lang = I18N_EN;
static ent  *g_ent;                /* the catalogue in force, sorted; NULL: English */
static int   g_n, g_refused;
static void *g_mem;                /* the one allocation: entries, then text */

int i18n_index(const char *code)
{
   int i;
   if (!code)
      return I18N_AUTO;
   for (i = 0; i < I18N_LANGS; i++)
   {
      const char *a = code, *b = i18n_code[i];
      while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
         a++, b++;
      if (!*a && !*b)
         return i;
   }
   return I18N_AUTO;
}

int i18n_from_system(int sce)
{
   switch (sce)
   {
      case 0:  return I18N_JA;
      case 2:  return I18N_FR;
      case 3:  return I18N_ES;
      case 4:  return I18N_DE;
      case 5:  return I18N_IT;
      case 7:  case 17: return I18N_PTBR;     /* Portugal's Portuguese reads the Brazilian text */
      case 10: case 11: return I18N_ZHS;      /* Traditional Chinese: the simplified text, the closest there is */
      default: return I18N_EN;
   }
}

void i18n_dir(const char *dir)
{
   const char *a = getenv("GBADHOC_TR_AUDIT");
   i18n_audit_on = a && *a == '1';
   snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : "");
}

/* ---- the printf conversions of a text ---------------------------------- */
/* "%-8s %lu%%" -> "slu": the type letters in order ('*' for a width taken
 * from an argument); -1 for a form this scheme refuses (positional, %n). */
static int conv_sig(const char *s, char *out, size_t n)
{
   size_t k = 0;
   for (; *s; s++)
   {
      if (*s != '%')
         continue;
      s++;
      if (*s == '%' || *s == ' ')   /* "%%"; "20 % fast" is a percent sign and a space, not the space flag */
         continue;
      for (; *s && strchr("-+ #0123456789.*hlLzjtq", *s); s++)
      {
         if (*s == '*' && k + 1 < n)
            out[k++] = '*';
         else if (strchr("hlLzjtq", *s) && k + 1 < n)
            out[k++] = *s;
      }
      if (!*s || !strchr("diouxXcsfFeEgGp", *s))
         return -1;            /* %n, %1$s, a lone % at the end... */
      if (k + 1 >= n)
         return -1;
      out[k++] = *s;
      out[k++] = ',';
   }
   out[k] = 0;
   return (int)k;
}

int i18n_same_formats(const char *a, const char *b)
{
   char x[128], y[128];
   if (conv_sig(a, x, sizeof(x)) < 0 || conv_sig(b, y, sizeof(y)) < 0)
      return 0;
   return strcmp(x, y) == 0;
}

/* ---- the file -------------------------------------------------------------- */
static int key_ok(const char *k)
{
   if (!*k)
      return 0;
   for (; *k; k++)
      if (!isalnum((unsigned char)*k) && *k != '.' && *k != '_' && *k != '-')
         return 0;
   return 1;
}

/* Unescapes in place; 0 when the text holds a form it does not know. */
static int unescape(char *s)
{
   char *w = s;
   for (; *s; s++)
   {
      if (*s == '\\')
      {
         s++;
         if (*s == 'n') *w++ = '\n';
         else if (*s == 't') *w++ = '\t';
         else if (*s == '\\') *w++ = '\\';
         else return 0;
      }
      else if ((unsigned char)*s < 0x20 && *s != '\t')
         return 0;
      else
         *w++ = *s;
   }
   *w = 0;
   return 1;
}

/* UTF-8 that a font can draw: well-formed, no overlong forms, no surrogates. */
static int utf8_ok(const unsigned char *s)
{
   while (*s)
   {
      int n;
      unsigned cp;
      if (*s < 0x80) { s++; continue; }
      if ((*s & 0xe0) == 0xc0) { n = 1; cp = *s & 0x1f; }
      else if ((*s & 0xf0) == 0xe0) { n = 2; cp = *s & 0x0f; }
      else if ((*s & 0xf8) == 0xf0) { n = 3; cp = *s & 0x07; }
      else return 0;
      for (s++; n; n--, s++)
      {
         if ((*s & 0xc0) != 0x80)
            return 0;
         cp = (cp << 6) | (*s & 0x3f);
      }
      if (cp < 0x80 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
         return 0;
      if (cp < 0x800 && (s[-2] & 0xe0) != 0xc0)
         return 0;
   }
   return 1;
}

int i18n_parse(char *buf, size_t len, i18n_line_fn fn, void *user)
{
   char *p = buf, *end = buf + len;
   int refused = 0;
   if (len >= 3 && !memcmp(p, "\xef\xbb\xbf", 3))
      p += 3;
   while (p < end)
   {
      char *e = memchr(p, '\n', (size_t)(end - p)), *tab, *line = p;
      size_t ll;
      if (!e)
         e = end;
      p = e + 1;
      ll = (size_t)(e - line);
      if (ll && line[ll - 1] == '\r')
         ll--;
      line[ll] = 0;       /* e is a '\n' or the end: the buffer has a spare byte (i18n_use) */
      if (!ll || line[0] == '#')
         continue;
      if (ll > MAX_LINE || !(tab = strchr(line, '\t')))
      {
         refused++;
         continue;
      }
      *tab = 0;
      if (!key_ok(line) || !tab[1] || !unescape(tab + 1) || !tab[1] || !utf8_ok((unsigned char *)tab + 1))
      {
         refused++;
         continue;
      }
      fn(user, line, tab + 1);
   }
   return refused;
}

static int cmp_ent(const void *a, const void *b)
{
   return strcmp(((const ent *)a)->key, ((const ent *)b)->key);
}

static const char *en_find(const char *key)
{
   int lo = 0, hi = (int)(sizeof(i18n_en) / sizeof(i18n_en[0])) - 1;
   while (lo <= hi)
   {
      int mid = (lo + hi) / 2, c = strcmp(key, i18n_en[mid].key);
      if (!c)
         return i18n_en[mid].val;
      if (c < 0) hi = mid - 1; else lo = mid + 1;
   }
   return NULL;
}

/* The Vita's button glyphs the text layers bake after ASCII (video_psp.h
 * VID_GLYPH_*): a catalogue writes them {tri} {o} {x} {sq} {star} {staro}. */
static const struct { const char *name; char byte; } glyph[] = {
   { "{tri}", '\x7f' }, { "{o}", '\x80' }, { "{x}", '\x81' }, { "{sq}", '\x82' },
   { "{staro}", '\x84' }, { "{star}", '\x83' } };

static void expand_glyphs(char *s)
{
   char *w = s;
   while (*s)
   {
      size_t i, n = 0;
      if (*s == '{')
         for (i = 0; i < sizeof(glyph) / sizeof(glyph[0]); i++)
         {
            n = strlen(glyph[i].name);
            if (!strncmp(s, glyph[i].name, n))
               break;
            n = 0;
         }
      if (n)
      {
         *w++ = glyph[i].byte;
         s += n;
      }
      else
         *w++ = *s++;
   }
   *w = 0;
}

typedef struct { ent *e; int n, cap, dropped; } collect;

static void on_line(void *u, const char *key, const char *text)
{
   collect *c = (collect *)u;
   const char *en = en_find(key);
   if (!en)
   {
      c->dropped++;            /* a key this build has no use for */
      return;
   }
   if (!i18n_same_formats(en, text))
   {
      c->dropped++;
      return;
   }
   if (c->n < c->cap)
   {
      expand_glyphs((char *)text);
      c->e[c->n].key = key;
      c->e[c->n].val = text;
      c->n++;
   }
}

static void drop(void)
{
   free(g_mem);
   g_mem = NULL;
   g_ent = NULL;
   g_n = g_refused = 0;
}

/* A file into one allocation: [entries][text].  Entries point into the text,
 * whose lines are cut and unescaped in place. */
static int load_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   long sz;
   size_t cap_e = sizeof(i18n_en) / sizeof(i18n_en[0]), tbytes;
   char *text;
   collect c;
   if (!f)
      return 0;
   if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) <= 0 || (unsigned long)sz > MAX_FILE || fseek(f, 0, SEEK_SET))
   {
      fclose(f);
      return 0;
   }
   tbytes = (size_t)sz + 2;
   g_mem = malloc(cap_e * sizeof(ent) + tbytes);
   if (!g_mem)
   {
      fclose(f);
      return 0;
   }
   text = (char *)g_mem + cap_e * sizeof(ent);
   if (fread(text, 1, (size_t)sz, f) != (size_t)sz)
   {
      fclose(f);
      drop();
      return 0;
   }
   fclose(f);
   text[sz] = 0;
   c.e = (ent *)g_mem;
   c.n = 0;
   c.cap = (int)cap_e;
   c.dropped = 0;
   g_refused = i18n_parse(text, (size_t)sz, on_line, &c) + c.dropped;
   if (c.n <= 0)
   {
      drop();
      return 0;
   }
   qsort(c.e, (size_t)c.n, sizeof(ent), cmp_ent);
   g_ent = c.e;
   g_n = c.n;
   return 1;
}

/* ---- the pseudo-locale ---------------------------------------------------------- */
void i18n_pseudo(const char *en, char *out, size_t n)
{
   static const char *const acc[26] = {   /* a..z: an accented form, or "" */
      "\xc3\xa0", "", "", "", "\xc3\xa9", "", "", "", "\xc3\xad", "", "", "",
      "", "\xc3\xb1", "\xc3\xb6", "", "", "", "", "", "\xc3\xbc", "", "", "", "", "" };
   static const char *const pad[4] = { "\xc3\xab", "\xc3\xb6", "\xc3\xbc", "\xc3\xaf" };
   size_t k = 0;
   int run = 0, pi = 0;
   if (!n)
      return;
   while (*en && k + 8 < n)
   {
      unsigned char c = (unsigned char)*en;
      if (c == '%')
      {
         if (en[1] == '%')
         {
            out[k++] = *en++;
            out[k++] = *en++;
            continue;
         }
         out[k++] = *en++;                 /* a conversion: untouched */
         while (*en && strchr("-+ #0123456789.*hlLzjtq", *en))
            out[k++] = *en++;
         if (*en)
            out[k++] = *en++;
         run = 0;
         continue;
      }
      if (isalpha(c))
      {
         const char *a = c >= 'a' && c <= 'z' ? acc[c - 'a'] : "";
         if (*a)
            for (; *a; a++)
               out[k++] = *a;
         else
            out[k++] = (char)c;
         en++;
         if (++run % 3 == 0)
         {
            const char *p = pad[pi++ & 3];
            out[k++] = p[0];
            out[k++] = p[1];
         }
         continue;
      }
      if (c >= 0x80)                        /* already beyond ASCII: copy the whole character */
      {
         out[k++] = *en++;
         while (((unsigned char)*en & 0xc0) == 0x80 && k + 4 < n)
            out[k++] = *en++;
         run = 0;
         continue;
      }
      run = 0;
      out[k++] = *en++;
   }
   out[k] = 0;
}

static int load_pseudo(void)
{
   size_t i, n = sizeof(i18n_en) / sizeof(i18n_en[0]), tb = 0;
   char *t;
   ent *e;
   for (i = 0; i < n; i++)
      tb += strlen(i18n_en[i].val) * 3 + 1;
   g_mem = malloc(n * sizeof(ent) + tb);
   if (!g_mem)
      return 0;
   e = (ent *)g_mem;
   t = (char *)g_mem + n * sizeof(ent);
   for (i = 0; i < n; i++)
   {
      size_t room = strlen(i18n_en[i].val) * 3 + 1;
      e[i].key = i18n_en[i].key;
      e[i].val = t;
      i18n_pseudo(i18n_en[i].val, t, room);
      t += strlen(t) + 1;
   }
   g_ent = e;
   g_n = (int)n;
   return 1;
}

int i18n_use(int lang, int system)
{
   char path[300];
   drop();
   if (lang == I18N_AUTO)
      lang = i18n_from_system(system);
   if (lang <= I18N_AUTO || lang >= I18N_LANGS)
      lang = I18N_EN;
   g_lang = I18N_EN;
   if (lang == I18N_PSEUDO)
   {
      if (load_pseudo())
         g_lang = I18N_PSEUDO;
   }
   else if (lang != I18N_EN)
   {
      snprintf(path, sizeof(path), "%s%slang/%s.txt", g_dir,
               g_dir[0] && g_dir[strlen(g_dir) - 1] != '/' && g_dir[strlen(g_dir) - 1] != ':' ? "/" : "",
               i18n_code[lang]);
      if (load_file(path))
         g_lang = lang;
   }
   return g_lang;
}

int i18n_audit_on;

/* A drawn text with half a character in it (a cut at a byte count) or a byte
 * that is no UTF-8: reported once per text, the way the shipped text layers
 * would show a '?' or nothing for it.  The button glyphs 0x80..0x84 are the
 * app's own and are fine. */
static void audit_utf8(const char *s)
{
   const char *p = s;
   static char seen[16][40];
   static int nseen;
   int i;
   while (*p)
   {
      int n;
      uint32_t cp = pk_u8_next(p, &n);
      if (cp >= PK_U8_RAW && !(cp - PK_U8_RAW >= 0x80 && cp - PK_U8_RAW <= 0x84))
      {
         for (i = 0; i < nseen; i++)
            if (!strncmp(seen[i], s, sizeof(seen[i]) - 1))
               return;
         if (nseen < 16)
            snprintf(seen[nseen++], sizeof(seen[0]), "%s", s);
         /* (the bytes in hex: the text itself is the very thing that is not valid) */
         const char *from = p - s >= 3 ? p - 3 : s;
         char hex[40];
         int h = 0;
         for (i = 0; i < 8 && from[i] && h + 3 < (int)sizeof(hex); i++)
            h += snprintf(hex + h, sizeof(hex) - (size_t)h, "%02X", (unsigned char)from[i]);
         fprintf(stderr, "EVT i18n: %s drawn at byte %d (bytes from %d: %s)\n",
                 pk_u8_cut_tail(p) ? "cut character" : "bad UTF-8", (int)(p - s), (int)(from - s), hex);
         return;
      }
      p += n;
   }
}

/* The code points no font has (why 0) or that found no free cell in the face's
 * atlas (why 1), drawn as '?': once each. */
void i18n_audit_glyph(unsigned cp, int why)
{
   static unsigned seen[64];
   static int nseen;
   int i;
   cp |= (unsigned)why << 24;
   for (i = 0; i < nseen; i++)
      if (seen[i] == cp)
         return;
   if (nseen < 64)
      seen[nseen++] = cp;
   fprintf(stderr, "EVT i18n: %s U+%04X\n", why ? "no free glyph cell for" : "glyph missing", cp & 0xFFFFFFu);
}

void i18n_audit(const char *drawn)
{
   if (drawn)
      audit_utf8(drawn);
   if (drawn && drawn[0] >= 'a' && drawn[0] <= 'z' && strchr(drawn, '.') && !strchr(drawn, ' ') &&
       en_find(drawn))
      fprintf(stderr, "EVT i18n: key shown %s\n", drawn);
}

int i18n_current(void) { return g_lang; }
int i18n_loaded_count(void) { return g_n; }
int i18n_refused_count(void) { return g_refused; }

const char *tr_en(const char *key)
{
   const char *v = en_find(key);
   return v ? v : key;
}

const char *tr_english_of(const char *text)
{
   int i;
   size_t n = sizeof(i18n_en) / sizeof(i18n_en[0]);
   if (!text)
      return text;
   for (i = 0; i < g_n; i++)
      if (g_ent[i].val == text)
         return tr_en(g_ent[i].key);
   for (i = 0; i < (int)n; i++)
      if (i18n_en[i].val == text)
         return text;
   return text;
}

void i18n_date(char *out, size_t n, int year, int mon, int day)
{
   static const char *const mk[12] = { "ui.jan", "ui.feb", "ui.mar", "ui.apr", "ui.may", "ui.jun",
                                       "ui.jul", "ui.aug", "ui.sep", "ui.oct", "ui.nov", "ui.dec" };
   const char *m = tr(mk[(mon < 1 || mon > 12 ? 1 : mon) - 1]);
   const char *o = tr("ui.date_order");
   if (!strcmp(o, "mdy"))
      snprintf(out, n, "%s %d, %d", m, day, year);
   else if (!strcmp(o, "ymd"))
   {
      size_t ml = strlen(m);
      /* Japanese and Chinese month names end in 月: "2026年10月3日" */
      if (ml >= 3 && !strcmp(m + ml - 3, "\xe6\x9c\x88"))
         snprintf(out, n, "%d\xe5\xb9\xb4%s%d\xe6\x97\xa5", year, m, day);
      else
         snprintf(out, n, "%d %s %d", year, m, day);
   }
   else
      snprintf(out, n, "%d %s %d", day, m, year);
}

const char *tr(const char *key)
{
   if (g_ent)
   {
      int lo = 0, hi = g_n - 1;
      while (lo <= hi)
      {
         int mid = (lo + hi) / 2, c = strcmp(key, g_ent[mid].key);
         if (!c)
            return g_ent[mid].val;
         if (c < 0) hi = mid - 1; else lo = mid + 1;
      }
   }
   return tr_en(key);
}
