/* fe_cheats.c -- see fe_cheats.h and docs/CHEATS.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe_cheats.h"
#include "../cheat_format.h"

/* cheats.c (core, CHEATS=1).  Plain ints: see cheats_fe_parse. */
extern int cheats_fe_parse(unsigned slot, const char *code);
extern int cheats_fe_check(const char *code);
extern void cheats_fe_clear(void);
extern unsigned cheats_fe_slots(void);
extern void cheats_fe_suspend(int on);
extern const char *cheats_fe_error_name(int err);
extern unsigned cheats_fe_ram_flushes(void);
extern unsigned cheats_fe_writes(void);

static char g_buf[FE_CHEATS_FILE_MAX + 1];
static fe_cheat g_list[FE_CHEATS_MAX];
static unsigned g_n, g_found;
static int g_status = FE_CHEATS_NONE;
static int g_sys = FE_CHEATS_SYS_GBA;
static int g_truncated;
static int g_attached;
static int g_locked;

static struct { uint16_t addr; uint8_t value; } g_gb[FE_CHEATS_GB_POKES];
static unsigned g_gb_n;
static unsigned g_gb_writes;

static const char UNNAMED[] = "Unnamed cheat";

/* ---- small text helpers --------------------------------------------------- */

static int is_hex(char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F');
}

static int hex_val(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   return c - 'A' + 10;
}

static int is_blank(char c)
{
   return c == ' ' || c == '\t' || c == '\r';
}

/* Code separators: whitespace, and what lists and libretro .cht join codes
 * with.  The core's tokeniser (cheats.c cheat_is_sep) uses the same set. */
static int is_sep(char c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '+' ||
          c == ',' || c == ';';
}

/* A name as the menus can draw it: printable ASCII only (the UI font's bytes
 * 0x7F.. are button glyphs), one '?' per run of anything else, trimmed, in
 * place.  Returns UNNAMED for a name with nothing left. */
static const char *clean_name(char *s)
{
   char *r = s, *w = s;
   int bad = 0;
   while (*r == ' ' || *r == '\t')
      r++;
   for (; *r; r++)
   {
      unsigned char c = (unsigned char)*r;
      if (c >= 0x20 && c < 0x7F)
      {
         *w++ = (char)c;
         bad = 0;
      }
      else if (!bad)
      {
         *w++ = '?';
         bad = 1;
      }
   }
   while (w > s && w[-1] == ' ')
      w--;
   *w = '\0';
   return s[0] ? s : UNNAMED;
}

/* Identity of a cheat: FNV-1a over the name, a 0, and the code's digits
 * (upper-cased; ':' and '-' kept, separators dropped) -- so re-wrapping or
 * re-spacing a code does not forget that it was on, and editing it does. */
static uint32_t cheat_hash(const char *name, const char *code)
{
   uint32_t h = 2166136261u;
   const unsigned char *p;
   for (p = (const unsigned char *)name; *p; p++)
      h = (h ^ *p) * 16777619u;
   h = (h ^ 0u) * 16777619u;
   for (p = (const unsigned char *)code; *p; p++)
   {
      unsigned char c = *p;
      if (is_sep((char)c))
         continue;
      if (c >= 'a' && c <= 'z')
         c = (unsigned char)(c - 'a' + 'A');
      h = (h ^ c) * 16777619u;
   }
   return h;
}

/* ---- Game Boy codes --------------------------------------------------------
 *
 * GameShark, 8 digits: TT VV LL HH -- write VV to address HHLL once a frame.
 * Only type 01 is taken, and only into work RAM (C000-DFFF) or high RAM
 * (FF80-FFFE).  Cartridge RAM (A000-BFFF) IS the save file on a Game Boy, so
 * a code that writes there is refused, like ROM, VRAM and I/O.  Game Genie
 * codes (XXX-YYY or XXX-YYY-ZZZ) are ROM patches: refused, by name.
 * `emit` NULL just checks. */
static int gb_code(const char *code, unsigned *n_out)
{
   const char *p = code;
   unsigned n = 0;
   while (*p)
   {
      const char *t;
      unsigned len, i, dash = 0, tt, vv, addr;
      while (*p && is_sep(*p))
         p++;
      if (!*p)
         break;
      t = p;
      while (*p && !is_sep(*p))
         p++;
      len = (unsigned)(p - t);
      for (i = 0; i < len; i++)
      {
         if (t[i] == '-')
            dash++;
         else if (!is_hex(t[i]))
            return FE_CHEAT_ERR_GB_SYNTAX;
      }
      if (dash)
         return FE_CHEAT_ERR_GB_GENIE;
      if (len != 8)
         return FE_CHEAT_ERR_GB_SYNTAX;
      tt = (unsigned)(hex_val(t[0]) << 4 | hex_val(t[1]));
      vv = (unsigned)(hex_val(t[2]) << 4 | hex_val(t[3]));
      addr = (unsigned)(hex_val(t[6]) << 12 | hex_val(t[7]) << 8 |
                        hex_val(t[4]) << 4 | hex_val(t[5]));
      if (tt != 0x01)
         return FE_CHEAT_ERR_GB_TYPE;
      if (!((addr >= 0xC000 && addr <= 0xDFFF) ||
            (addr >= 0xFF80 && addr <= 0xFFFE)))
         return FE_CHEAT_ERR_GB_REGION;
      if (n_out)
      {
         if (*n_out >= FE_CHEATS_GB_POKES)
            return FE_CHEAT_ERR_GB_TOO_MANY;
         g_gb[*n_out].addr = (uint16_t)addr;
         g_gb[*n_out].value = (uint8_t)vv;
         (*n_out)++;
      }
      n++;
   }
   return n ? FE_CHEAT_OK : FE_CHEAT_ERR_GB_SYNTAX;
}

/* ---- the plain list --------------------------------------------------------
 *
 *     # a comment
 *     Infinite money
 *     82025BC4 E0FF
 *     82025BC6 05F5
 *
 *     [Max HP]
 *     3202A4C0 00FF
 *
 * A line that reads entirely as codes is a code line; any other line is the
 * name of the next cheat ("[...]" or quotes around it are taken off, and are
 * the way to give a cheat a name that itself looks like a code).  Blank
 * lines and comments ('#' or '//') are skipped.  Codes before any name make
 * an "Unnamed cheat"; a name with no codes under it (a heading) is dropped. */

/* Does [a, b) read entirely as codes?  Tokens: 8/12/16 hex digits, 4 hex
 * digits after a longer one (a CodeBreaker value), ADDR:VALUE, or a Game
 * Genie group.  A line of ordinary words never passes: the first token must
 * be 8+ digits or carry a ':' or '-'. */
static int is_code_line(const char *a, const char *b)
{
   unsigned ntok = 0;
   const char *p = a;
   unsigned used;
   /* An explicit family tag makes this a code line even if the payload is
    * malformed. Never turn a refused tagged code into a new cheat title. */
   if (cheat_family_prefix(a, (unsigned)(b - a), &used))
      return 1;
   /* Preserve a recognizable address with a malformed/truncated value as
    * code too. Otherwise the valid prefix of an entry could be enabled
    * after the bad continuation silently became a title. */
   for (used = 0; a + used < b && is_hex(a[used]); used++) {}
   if (used >= 8 && (a + used == b || is_sep(a[used]) || a[used] == ':'))
      return 1;
   while (p < b)
   {
      const char *t;
      unsigned len, i, punct = 0;
      while (p < b && is_sep(*p))
         p++;
      if (p >= b)
         break;
      t = p;
      while (p < b && !is_sep(*p))
         p++;
      len = (unsigned)(p - t);
      for (i = 0; i < len; i++)
      {
         if (t[i] == ':' || t[i] == '-')
            punct++;
         else if (!is_hex(t[i]))
            return 0;
      }
      if (punct)
      {
         if (len < 5 || t[0] == ':' || t[0] == '-' || t[len - 1] == ':' ||
             t[len - 1] == '-')
            return 0;
      }
      else if (len == 4)
      {
         if (ntok == 0)
            return 0;
      }
      else if (len != 8 && len != 12 && len != 16)
         return 0;
      ntok++;
   }
   return ntok > 0;
}

static void entry_new(const char *name)
{
   g_found++;
   if (g_n >= FE_CHEATS_MAX)
      return;
   g_list[g_n].name = name;
   g_list[g_n].code = NULL;
   g_list[g_n].hash = 0;
   g_list[g_n].enabled = 0;
   g_list[g_n].error = 0;
   g_n++;
}

static unsigned parse_plain(char *buf, size_t len)
{
   char *p = buf, *end = buf + len;
   char *code_end = NULL;        /* end of the open entry's last code line */
   int open = 0;                 /* an entry is waiting for (more) codes   */
   int listed = 0;               /* ...and it is in g_list (not past MAX)  */

   while (p < end)
   {
      char *ls = p, *le = (char *)memchr(p, '\n', (size_t)(end - p)), *a, *b;
      if (!le)
         le = end;
      p = le < end ? le + 1 : end;
      a = ls;
      b = le;
      while (a < b && is_blank(*a))
         a++;
      while (b > a && is_blank(b[-1]))
         b--;
      if (a == b)
         continue;
      if (a[0] == '#' || (a[0] == '/' && b - a >= 2 && a[1] == '/'))
      {
         memset(ls, ' ', (size_t)(le - ls));   /* a comment between codes */
         continue;
      }
      if (is_code_line(a, b))
      {
         if (!open)
         {
            entry_new(UNNAMED);
            listed = g_found <= FE_CHEATS_MAX;
            open = 1;
            code_end = NULL;
         }
         if (listed && !g_list[g_n - 1].code)
            g_list[g_n - 1].code = a;
         code_end = b;
         continue;
      }
      /* A name.  Close the entry before it; one that got no code was a
       * heading, and its slot is reused. */
      if (open)
      {
         if (code_end)
            *code_end = '\0';
         else
         {
            g_found--;
            if (listed)
               g_n--;
         }
      }
      if ((a[0] == '[' && b[-1] == ']') || (a[0] == '"' && b[-1] == '"'))
      {
         if (b - a >= 2)
         {
            a++;
            b--;
         }
      }
      *b = '\0';
      entry_new(a);
      listed = g_found <= FE_CHEATS_MAX;
      open = 1;
      code_end = NULL;
   }
   if (open)
   {
      if (code_end)
         *code_end = '\0';
      else
      {
         g_found--;
         if (listed)
            g_n--;
      }
   }
   return g_n;
}

/* ---- libretro .cht ---------------------------------------------------------
 *
 *     cheats = 2
 *     cheat0_desc = "Infinite HP"
 *     cheat0_code = "820240AC 0063+820240B0 0063"
 *     cheat0_enable = false
 *
 * Values may be quoted.  `cheats = N` is not trusted (the entries are the
 * cheatN_code keys the file actually has) and `cheatN_enable` is not read: a
 * downloaded file does not get to switch cheats on.  RetroArch's own keys
 * (cheatN_address, _handler, ...) are ignored. */

static char *trim(char *s, char *e)
{
   while (s < e && is_blank(*s))
      s++;
   while (e > s && is_blank(e[-1]))
      e--;
   *e = '\0';
   return s;
}

/* "cheat<N>_<field>" -> N and field, else -1. */
static long cht_key(const char *k, const char **field)
{
   unsigned long n;
   char *e;
   if (strncmp(k, "cheat", 5) != 0 || k[5] < '0' || k[5] > '9')
      return -1;
   n = strtoul(k + 5, &e, 10);
   if (*e != '_' || n >= 1000000ul)
      return -1;
   *field = e + 1;
   return (long)n;
}

static unsigned parse_cht(char *buf, size_t len)
{
   /* File index -> slot, in first-seen order, then sorted: the menu lists
    * entries as the file numbers them. */
   static long idx_of[FE_CHEATS_MAX];
   unsigned n = 0, i, j, overflow = 0;
   char *p = buf, *end = buf + len;

   while (p < end)
   {
      char *line = p, *eol = (char *)memchr(p, '\n', (size_t)(end - p));
      char *eq, *k, *v;
      const char *field;
      long idx;
      if (!eol)
         eol = end;
      p = eol < end ? eol + 1 : end;
      eq = (char *)memchr(line, '=', (size_t)(eol - line));
      if (!eq)
         continue;
      k = trim(line, eq);
      v = trim(eq + 1, eol);
      if (v[0] == '"')
      {
         char *q = strrchr(v + 1, '"');
         v++;
         if (q)
            *q = '\0';
      }
      idx = cht_key(k, &field);
      if (idx < 0)
         continue;
      for (i = 0; i < n; i++)
         if (idx_of[i] == idx)
            break;
      if (i == n)
      {
         if (n >= FE_CHEATS_MAX)
         {
            if (!strcmp(field, "code"))
               overflow++;
            continue;
         }
         idx_of[n] = idx;
         g_list[n].name = UNNAMED;
         g_list[n].code = NULL;
         g_list[n].hash = 0;
         g_list[n].enabled = 0;
         g_list[n].error = 0;
         n++;
      }
      if (!strcmp(field, "desc"))
         g_list[i].name = v;
      else if (!strcmp(field, "code"))
         g_list[i].code = v;
   }
   /* Sort by file index (insertion sort, once per load). */
   for (i = 1; i < n; i++)
      for (j = i; j > 0 && idx_of[j - 1] > idx_of[j]; j--)
      {
         fe_cheat t = g_list[j];
         long ti = idx_of[j];
         g_list[j] = g_list[j - 1];
         idx_of[j] = idx_of[j - 1];
         g_list[j - 1] = t;
         idx_of[j - 1] = ti;
      }
   /* Drop entries without a code (a desc line alone). */
   for (i = j = 0; i < n; i++)
      if (g_list[i].code && g_list[i].code[0])
         g_list[j++] = g_list[i];
   g_n = j;
   g_found = j + overflow;
   return j;
}

/* The first line that is not blank or a comment decides: `cheats = N` or a
 * `cheatN_field =` key means .cht. */
static int looks_like_cht(const char *buf, size_t len)
{
   const char *p = buf, *end = buf + len;
   while (p < end)
   {
      const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
      const char *a = p;
      if (!eol)
         eol = end;
      while (a < eol && is_blank(*a))
         a++;
      if (a < eol && *a != '#' && !(a[0] == '/' && eol - a >= 2 && a[1] == '/'))
         return eol - a > 6 && !strncmp(a, "cheat", 5) &&
                memchr(a, '=', (size_t)(eol - a)) != NULL &&
                (a[5] == 's' || (a[5] >= '0' && a[5] <= '9'));
      p = eol < end ? eol + 1 : end;
   }
   return 0;
}

/* ---- the list and the core ------------------------------------------------ */

static int entry_check(const fe_cheat *e)
{
   if (g_sys == FE_CHEATS_SYS_GB)
      return gb_code(e->code, NULL);
   return cheats_fe_check(e->code);
}

/* Hand every enabled entry to the core, from slot 0.  The whole list every
 * time: a toggle is a menu action, and rebuilding means the core can never
 * disagree with what the menu shows.  An entry that does not fit (too many
 * on, or the record pool is full) is switched off; `watch`'s reason comes
 * back in *watch_err. */
static void apply_all(unsigned watch, int *watch_err)
{
   unsigned i, slot = 0, slots;
   if (watch_err)
      *watch_err = 0;
   if (!g_attached)
      return;
   g_gb_n = 0;
   cheats_fe_clear();
   slots = cheats_fe_slots();
   for (i = 0; i < g_n; i++)
   {
      int err;
      if (!g_list[i].enabled)
         continue;
      if (g_sys == FE_CHEATS_SYS_GB)
         err = gb_code(g_list[i].code, &g_gb_n);
      else if (slot >= slots)
         err = 1;                       /* CheatErrorTooMany */
      else
         err = cheats_fe_parse(slot, g_list[i].code);
      if (err)
      {
         g_list[i].enabled = 0;         /* a refused code is never left "on" */
         if (g_sys == FE_CHEATS_SYS_GB)
         {
            /* gb_code may have emitted part of this cheat: rebuild without
             * it rather than run half a code. */
            unsigned k;
            g_gb_n = 0;
            for (k = 0; k < i; k++)
               if (g_list[k].enabled)
                  gb_code(g_list[k].code, &g_gb_n);
         }
         if (i == watch && watch_err)
            *watch_err = err;
      }
      else
         slot++;
   }
}

char *fe_cheats_buffer(void)
{
   return g_buf;
}

void fe_cheats_reset(void)
{
   if (g_attached)
      cheats_fe_clear();
   g_attached = 0;
   g_gb_n = 0;
   g_n = g_found = 0;
   g_truncated = 0;
   g_status = FE_CHEATS_NONE;
}

int fe_cheats_parse(size_t len, int sys, int truncated)
{
   unsigned i;
   char *buf = g_buf;
   int was_attached = g_attached;

   fe_cheats_reset();
   g_attached = was_attached;
   g_sys = sys == FE_CHEATS_SYS_GB ? FE_CHEATS_SYS_GB : FE_CHEATS_SYS_GBA;
   if (len > FE_CHEATS_FILE_MAX)
   {
      len = FE_CHEATS_FILE_MAX;
      truncated = 1;
   }
   if (truncated)
   {
      /* The last line may be cut in the middle of a code: drop it. */
      while (len > 0 && buf[len - 1] != '\n')
         len--;
      g_truncated = 1;
   }
   /* NUL bytes would end a name or a code early; nothing sane contains one. */
   for (i = 0; i < len; i++)
      if (buf[i] == '\0')
         buf[i] = ' ';
   buf[len] = '\0';
   if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
       (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
      buf[0] = buf[1] = buf[2] = ' ';       /* UTF-8 byte order mark */

   if (looks_like_cht(buf, len))
      parse_cht(buf, len);
   else
      parse_plain(buf, len);

   for (i = 0; i < g_n; i++)
   {
      /* The strings are in g_buf, which this module owns. */
      g_list[i].name = g_list[i].name == UNNAMED
                          ? UNNAMED : clean_name((char *)g_list[i].name);
      if (!g_list[i].code)
         g_list[i].code = "";
      g_list[i].hash = cheat_hash(g_list[i].name, g_list[i].code);
      g_list[i].enabled = 0;
      g_list[i].error = (unsigned char)entry_check(&g_list[i]);
   }
   g_status = g_n ? FE_CHEATS_LOADED : FE_CHEATS_EMPTY;
   apply_all(0, NULL);            /* attached: the core is now all-off too */
   return g_status;
}

int fe_cheats_load_text(const char *text, size_t len, int sys)
{
   int trunc = len > FE_CHEATS_FILE_MAX;
   if (trunc)
      len = FE_CHEATS_FILE_MAX;
   if (len)
      memcpy(g_buf, text, len);
   return fe_cheats_parse(len, sys, trunc);
}

void fe_cheats_state_parse(const char *text, size_t len)
{
   const char *p = text, *end = text + len;
   unsigned i;
   for (i = 0; i < g_n; i++)
      g_list[i].enabled = 0;
   while (p < end)
   {
      const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
      const char *a = p;
      if (!eol)
         eol = end;
      p = eol < end ? eol + 1 : end;
      while (a < eol && is_blank(*a))
         a++;
      if (eol - a >= 8)
      {
         uint32_t h = 0;
         for (i = 0; i < 8 && is_hex(a[i]); i++)
            h = h << 4 | (uint32_t)hex_val(a[i]);
         if (i == 8 && (a + 8 == eol || is_blank(a[8])))
            for (i = 0; i < g_n; i++)
               if (g_list[i].hash == h && !g_list[i].error)
                  g_list[i].enabled = 1;
      }
   }
   apply_all(0, NULL);
}

size_t fe_cheats_state_serialise(char *out, size_t cap)
{
   static const char head[] =
      "# GBAdhoc: the cheats switched on for this game.  Written by the\n"
      "# Cheats menu; delete this file to switch them all off.\n";
   size_t n = sizeof(head) - 1;
   unsigned i;
   if (cap <= n)
      return cap;
   memcpy(out, head, n);
   for (i = 0; i < g_n; i++)
   {
      int w;
      if (!g_list[i].enabled)
         continue;
      w = snprintf(out + n, cap - n, "%08lX %.*s\n",
                   (unsigned long)g_list[i].hash, FE_CHEATS_NAME_SHOWN,
                   g_list[i].name);
      if (w < 0 || (size_t)w >= cap - n)
         return cap;
      n += (size_t)w;
   }
   return n;
}

void fe_cheats_attach(int on)
{
   if (!on && g_attached)
   {
      cheats_fe_clear();
      g_gb_n = 0;
   }
   g_attached = on ? 1 : 0;
   if (on)
      apply_all(0, NULL);
}

int fe_cheats_status(void)        { return g_status; }
int fe_cheats_system(void)        { return g_sys; }
int fe_cheats_truncated(void)     { return g_truncated; }
unsigned fe_cheats_found(void)    { return g_found; }
unsigned fe_cheats_count(void)    { return g_n; }

const fe_cheat *fe_cheats_get(unsigned i)
{
   return i < g_n ? &g_list[i] : NULL;
}

unsigned fe_cheats_enabled_count(void)
{
   unsigned i, c = 0;
   for (i = 0; i < g_n; i++)
      c += g_list[i].enabled;
   return c;
}

unsigned fe_cheats_usable_count(void)
{
   unsigned i, c = 0;
   for (i = 0; i < g_n; i++)
      c += !g_list[i].error;
   return c;
}

int fe_cheats_set(unsigned i, int on)
{
   int err = 0;
   if (i >= g_n)
      return -1;
   if (on && g_list[i].error)
      return g_list[i].error;
   g_list[i].enabled = on ? 1 : 0;
   apply_all(i, &err);
   return err;
}

void fe_cheats_all_off(void)
{
   unsigned i;
   for (i = 0; i < g_n; i++)
      g_list[i].enabled = 0;
   apply_all(0, NULL);
}

void fe_cheats_lock(int on)
{
   on = on ? 1 : 0;
   if (on == g_locked)
      return;
   g_locked = on;
   cheats_fe_suspend(on);
}

int fe_cheats_locked(void)
{
   return g_locked;
}

unsigned fe_cheats_running(void)
{
   return g_attached && !g_locked ? fe_cheats_enabled_count() : 0;
}

void fe_cheats_gb_frame(int (*poke)(void *ctx, uint16_t addr, uint8_t value),
                        void *ctx)
{
   unsigned i;
   if (!g_attached || g_locked || g_sys != FE_CHEATS_SYS_GB || !poke)
      return;
   for (i = 0; i < g_gb_n; i++)
      if (poke(ctx, g_gb[i].addr, g_gb[i].value) == 0)
         g_gb_writes++;
}

unsigned fe_cheats_gb_writes(void)
{
   return g_gb_writes;
}

const char *fe_cheats_error_name(int err)
{
   switch (err)
   {
   case FE_CHEAT_ERR_GB_GENIE:    return "Game Genie";
   case FE_CHEAT_ERR_GB_TYPE:     return "not a 01 code";
   case FE_CHEAT_ERR_GB_REGION:   return "not a RAM code";
   case FE_CHEAT_ERR_GB_SYNTAX:   return "not a code";
   case FE_CHEAT_ERR_GB_TOO_MANY: return "too many cheats on";
   default:                       return cheats_fe_error_name(err);
   }
}

unsigned fe_cheats_ram_flushes(void)
{
   return cheats_fe_ram_flushes();
}

unsigned fe_cheats_core_writes(void)
{
   return cheats_fe_writes();
}
