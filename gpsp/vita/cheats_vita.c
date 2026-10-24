/* cheats_vita.c -- the cheat list of the running game (cheats_vita.h).
 *
 *   the player's .cht  --\
 *                         >--  one list  --(enabled, usable)-->  the core
 *   the bundled database -/
 *
 * Decoding is gpsp/cheat_engine.c; this file keeps the list, reads and
 * writes the libretro .cht, finds the game in the database (cheatdb.c) and
 * hands the enabled entries to whichever core runs:
 *
 *   GBA     compiled to the engine's operations and ROM patches and given
 *           to instance A of the core (gba_cheats_set, gpsp/cheats.c), which
 *           runs them at every frame boundary.  Instance B of the link
 *           mirror never gets any.
 *   GB/GBC  decoded to RAM writes and ROM patches and given to the
 *           single-player machine (gbcore_cheats_set).
 *
 * Seeds.  Encrypted GBA codes are decoded with the seeds the game's master
 * code sets.  The seeds are collected from EVERY entry of the list, on or
 * off, wherever it sits (cheat_gba_ctx_scan): the player switches on the
 * code he wants and need not know that "Enable Code (Must Be On)" three
 * hundred rows up is what makes it readable.
 *
 * Everything here runs on the main thread, between frames.
 */
#include "cheats_vita.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cheat_engine.h"
#include "cheatdb.h"

#ifndef CHEATS_VITA_NO_CORE
#include "gbcore/gbcore.h"
#endif

/* The engine's kinds are the contract's kinds. */
typedef char kinds_agree[((int)CE_KIND_GBA_RAW == (int)CHEAT_KIND_GBA_RAW &&
                          (int)CE_KIND_GBA_CODEBREAKER == (int)CHEAT_KIND_GBA_CODEBREAKER &&
                          (int)CE_KIND_GBA_GAMESHARK == (int)CHEAT_KIND_GBA_GAMESHARK &&
                          (int)CE_KIND_GBA_AR3 == (int)CHEAT_KIND_GBA_AR3 &&
                          (int)CE_KIND_GB_GAMESHARK == (int)CHEAT_KIND_GB_GAMESHARK &&
                          (int)CE_KIND_GB_GAMEGENIE == (int)CHEAT_KIND_GB_GAMEGENIE) ? 1 : -1];

#define OPS_MAX      8192     /* operations of all enabled GBA entries */
#define PATCHES_MAX  1024
#define GB_CODES_MAX 1024
#define FILE_MAX     (4u << 20)   /* a .cht larger than this is not one */

typedef struct entry_x
{
   char   *name_own;          /* malloc'ed, or NULL: g_list[].name points
                               * into the database block */
   char   *code_own;
   char   *extra;             /* the .cht keys of this entry we do not use,
                               * "suffix = value\n" each, written back */
   uint8_t touched;           /* a database entry the player changed: it
                               * goes into his .cht */
   uint8_t flags;             /* CE_F_* */
   int8_t  rc;                /* CHEATS_OK or why it cannot run */
} entry_x;

static cheat_entry g_list[CHEATS_MAX];
static entry_x     g_x[CHEATS_MAX];
static int         g_count;

static int          g_loaded;
static fe_console_t g_console;
static int          g_dirty;
static int          g_suspended;
static int          g_hardcore;      /* cheats_hardcore(1): RetroAchievements */
static int          g_file_existed;
static char         g_cht_path[640];
static char         g_cht_dir[600];
static char        *g_global_extra;      /* .cht lines that are no cheat's */

static char        *g_db_block;
static char         g_db_name[160];
static int          g_db_match;

static cheat_gba_ctx g_ctx;

static cheat_op      g_ops[OPS_MAX];
static cheat_patch   g_patches[PATCHES_MAX];
static unsigned      g_len[CHEATS_MAX];
static cheat_gb_code g_gb[GB_CODES_MAX];

/* ======================================================================== */
/* small things                                                             */
/* ======================================================================== */

static char *dup_n(const char *s, size_t n)
{
   char *d = (char *)malloc(n + 1);
   if (d)
   {
      memcpy(d, s, n);
      d[n] = 0;
   }
   return d;
}

static int is_code_sep(int c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '+' ||
          c == ',' || c == ';';
}

/* The same code, whatever the case and whatever sits between its parts. */
static int code_same(const char *a, const char *b)
{
   for (;;)
   {
      int ca, cb;
      while (*a && is_code_sep((unsigned char)*a))
         a++;
      while (*b && is_code_sep((unsigned char)*b))
         b++;
      if (!*a || !*b)
         return !*a && !*b;
      /* one token */
      while (*a && *b && !is_code_sep((unsigned char)*a) &&
             !is_code_sep((unsigned char)*b))
      {
         ca = (unsigned char)*a++;
         cb = (unsigned char)*b++;
         if (ca >= 'a' && ca <= 'z')
            ca -= 32;
         if (cb >= 'a' && cb <= 'z')
            cb -= 32;
         if (ca != cb)
            return 0;
      }
      if ((*a && !is_code_sep((unsigned char)*a)) ||
          (*b && !is_code_sep((unsigned char)*b)))
         return 0;
   }
}

/* Trim, turn line ends into '+', keep everything else as typed.  Returns a
 * malloc'ed copy cut to CHEATS_CODE_CAP - 1. */
static char *code_clean(const char *s, size_t n)
{
   char *d = (char *)malloc(n + 1);
   size_t i, o = 0;
   if (!d)
      return NULL;
   for (i = 0; i < n; i++)
   {
      char c = s[i];
      if (c == '\r' || c == '\n')
      {
         /* a line end is a '+', once, and not at either end */
         while (o && (d[o - 1] == ' ' || d[o - 1] == '\t'))
            o--;
         if (o && d[o - 1] != '+')
            d[o++] = '+';
         continue;
      }
      if ((c == ' ' || c == '\t') && (!o || d[o - 1] == '+'))
         continue;                          /* leading blanks of a line */
      if (c == '"' || (unsigned char)c < 0x20)
         continue;                          /* would break the .cht line */
      d[o++] = c;
   }
   while (o && (d[o - 1] == ' ' || d[o - 1] == '\t' || d[o - 1] == '+'))
      o--;
   if (o > CHEATS_CODE_CAP - 1)
      o = CHEATS_CODE_CAP - 1;
   d[o] = 0;
   return d;
}

/* A name: one line, no quotes (the .cht has no way to escape one), at most
 * CHEATS_NAME_CAP - 1 bytes and not ending inside a UTF-8 character. */
static char *name_clean(const char *s, size_t n)
{
   char *d = (char *)malloc(n + 1);
   size_t i, o = 0;
   if (!d)
      return NULL;
   for (i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7F)
         c = ' ';
      if (c == '"')
         c = '\'';
      if (c == ' ' && (!o || d[o - 1] == ' '))
         continue;
      d[o++] = (char)c;
   }
   while (o && d[o - 1] == ' ')
      o--;
   if (o > CHEATS_NAME_CAP - 1)
   {
      o = CHEATS_NAME_CAP - 1;
      while (o && ((unsigned char)d[o] & 0xC0) == 0x80)
         o--;                               /* not in the middle of one */
   }
   d[o] = 0;
   return d;
}

static int map_error(int ce)
{
   switch (ce)
   {
   case CE_OK:            return CHEATS_OK;
   case CE_E_UNSUPPORTED: return CHEATS_E_UNSUPPORTED;
   case CE_E_TOO_LONG:    return CHEATS_E_TOO_LONG;
   case CE_E_PLACEHOLDER: return CHEATS_E_NEEDS_VALUE;
   case CE_E_ENCRYPTED:   return CHEATS_E_ENCRYPTED;
   default:               return CHEATS_E_FORMAT;
   }
}

/* ======================================================================== */
/* the cores                                                                */
/* ======================================================================== */

#ifdef CHEATS_VITA_NO_CORE
/* Unit tests without an emulator: remember what would have been pushed. */
unsigned cheats_test_pushed_ops, cheats_test_pushed_patches,
         cheats_test_pushed_gb, cheats_test_pushes;
static int core_gba_set(const cheat_op *ops, const unsigned *len, unsigned n,
                        const cheat_patch *p, unsigned np)
{
   unsigned i;
   (void)ops;
   (void)p;
   cheats_test_pushed_ops = 0;
   for (i = 0; i < n; i++)
      cheats_test_pushed_ops += len[i];
   cheats_test_pushed_patches = np;
   cheats_test_pushes++;
   return 0;
}
static int core_gb_set(const cheat_gb_code *codes, unsigned n)
{
   (void)codes;
   cheats_test_pushed_gb = n;
   cheats_test_pushes++;
   return 0;
}
static void core_button(int down) { (void)down; }
#else
static int core_gba_set(const cheat_op *ops, const unsigned *len, unsigned n,
                        const cheat_patch *p, unsigned np)
{
   return gba_cheats_set(ops, len, n, p, np);
}
static int core_gb_set(const cheat_gb_code *codes, unsigned n)
{
   /* cheat_gb_code and gbcore_cheat_t are the same seven fields; copied one
    * by one so neither header has to know the other. */
   static gbcore_cheat_t out[GB_CODES_MAX];
   unsigned i;
   for (i = 0; i < n; i++)
   {
      out[i].rom = codes[i].rom;
      out[i].type = codes[i].type;
      out[i].value = codes[i].value;
      out[i].compare = codes[i].compare;
      out[i].has_compare = codes[i].has_compare;
      out[i].pad = 0;
      out[i].address = codes[i].address;
   }
   /* NULL: this instance's running machine (none during a link session's
    * setup: then there is nothing to cheat on, and -1 says so). */
   return gbcore_cheats_set(NULL, out, n);
}
static void core_button(int down)
{
   gba_cheats_button(down);
}
#endif

/* Hand the enabled, usable entries to the core.  Returns the index of the
 * first enabled entry that did not fit, or -1. */
static int g_rom_failed;       /* ROM patches the core could not place */

static int push(void)
{
   int i, overflow = -1;

   g_rom_failed = 0;
   if (!g_loaded)
      return -1;
   if (g_console == FE_CONSOLE_GBA)
   {
      unsigned n_ops = 0, n_patches = 0, n_entries = 0;
      for (i = 0; i < g_count && !g_suspended && !g_hardcore; i++)
      {
         cheat_out out;
         if (!g_list[i].enabled || !g_list[i].usable)
            continue;
         out.ops = g_ops + n_ops;
         out.cap_ops = OPS_MAX - n_ops;
         out.patches = g_patches + n_patches;
         out.cap_patches = PATCHES_MAX - n_patches;
         out.n_ops = out.n_patches = 0;
         out.flags = 0;
         if (cheat_gba_compile(g_list[i].code, &g_ctx, &out, NULL) != CE_OK)
         {
            if (overflow < 0)
               overflow = i;
            continue;
         }
         g_len[n_entries++] = out.n_ops;
         n_ops += out.n_ops;
         n_patches += out.n_patches;
      }
      g_rom_failed = core_gba_set(g_ops, g_len, n_entries, g_patches,
                                  n_patches);
   }
   else
   {
      unsigned n = 0;
      for (i = 0; i < g_count && !g_suspended && !g_hardcore; i++)
      {
         unsigned k = 0;
         if (!g_list[i].enabled || !g_list[i].usable)
            continue;
         if (cheat_gb_compile(g_list[i].code, g_gb + n, GB_CODES_MAX - n, &k,
                              NULL) != CE_OK)
         {
            if (overflow < 0)
               overflow = i;
            continue;
         }
         n += k;
      }
      /* > 0: codes the machine had no room for (512 RAM writes, 4096
       * patched ROM bytes) */
      if (core_gb_set(g_gb, n) > 0 && overflow < 0)
         overflow = g_count;
   }
   return overflow;
}

/* ======================================================================== */
/* the list                                                                 */
/* ======================================================================== */

static void validate_one(int i)
{
   int kind = CE_KIND_UNKNOWN, rc;
   unsigned flags = 0;

   if (g_console == FE_CONSOLE_GBA)
   {
      cheat_out out;
      out.ops = g_ops;
      out.cap_ops = OPS_MAX;
      out.patches = g_patches;
      out.cap_patches = PATCHES_MAX;
      out.n_ops = out.n_patches = 0;
      out.flags = 0;
      rc = cheat_gba_compile(g_list[i].code, &g_ctx, &out, &kind);
      flags = out.flags;
   }
   else
   {
      unsigned n = 0;
      rc = cheat_gb_compile(g_list[i].code, g_gb, GB_CODES_MAX, &n, &kind);
   }
   g_list[i].kind = kind;
   g_list[i].usable = rc == CE_OK;
   g_x[i].rc = (int8_t)map_error(rc);
   g_x[i].flags = (uint8_t)flags;
}

/* After anything that changes which codes the list has: the seeds, then
 * every entry's verdict. */
static void validate_all(void)
{
   int i;
   cheat_gba_ctx_init(&g_ctx);
   if (g_console == FE_CONSOLE_GBA)
      for (i = 0; i < g_count; i++)
         cheat_gba_ctx_scan(&g_ctx, g_list[i].code);
   for (i = 0; i < g_count; i++)
      validate_one(i);
}

static void entry_free(int i)
{
   free(g_x[i].name_own);
   free(g_x[i].code_own);
   free(g_x[i].extra);
   memset(&g_x[i], 0, sizeof(g_x[i]));
   memset(&g_list[i], 0, sizeof(g_list[i]));
}

/* Insert at `at` an entry that takes ownership of name/code/extra (malloc'ed
 * or NULL with a borrowed pointer given instead).  -1 when the list is
 * full (and then the strings are freed). */
static int entry_insert(int at, char *name_own, const char *name,
                        char *code_own, const char *code, char *extra,
                        int enabled, int builtin)
{
   if (g_count >= CHEATS_MAX || (!name_own && !name) || (!code_own && !code))
   {
      free(name_own);
      free(code_own);
      free(extra);
      return -1;
   }
   memmove(&g_list[at + 1], &g_list[at], (size_t)(g_count - at) * sizeof(g_list[0]));
   memmove(&g_x[at + 1], &g_x[at], (size_t)(g_count - at) * sizeof(g_x[0]));
   memset(&g_list[at], 0, sizeof(g_list[at]));
   memset(&g_x[at], 0, sizeof(g_x[at]));
   g_x[at].name_own = name_own;
   g_x[at].code_own = code_own;
   g_x[at].extra = extra;
   g_list[at].name = name_own ? name_own : name;
   g_list[at].code = code_own ? code_own : code;
   g_list[at].enabled = enabled ? 1 : 0;
   g_list[at].builtin = builtin ? 1 : 0;
   g_count++;
   return at;
}

/* The player's entries come first: where they end. */
static int own_end(void)
{
   int i = 0;
   while (i < g_count && !g_list[i].builtin)
      i++;
   return i;
}

/* ======================================================================== */
/* the .cht file                                                            */
/* ======================================================================== */

static char *file_read(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   long len;
   char *buf;

   *size = 0;
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
       (unsigned long)len > FILE_MAX || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   buf = (char *)malloc((size_t)len + 1);
   if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len)
   {
      free(buf);
      fclose(f);
      return NULL;
   }
   fclose(f);
   buf[len] = 0;
   *size = (size_t)len;
   return buf;
}

typedef struct cht_slot
{
   unsigned long index;       /* the N of cheatN_ */
   char *desc, *code, *extra;
   int   enable;
} cht_slot;

static void append_text(char **dst, const char *a, size_t na, const char *b,
                        size_t nb)
{
   size_t old = *dst ? strlen(*dst) : 0;
   char *d = (char *)realloc(*dst, old + na + nb + 2);
   if (!d)
      return;
   memcpy(d + old, a, na);
   memcpy(d + old + na, b, nb);
   d[old + na + nb] = '\n';
   d[old + na + nb + 1] = 0;
   *dst = d;
}

static int slot_cmp(const void *a, const void *b)
{
   const cht_slot *x = (const cht_slot *)a, *y = (const cht_slot *)b;
   return x->index < y->index ? -1 : x->index > y->index;
}

static int truth(const char *v, size_t n)
{
   return (n == 4 && (!memcmp(v, "true", 4) || !memcmp(v, "TRUE", 4) ||
                      !memcmp(v, "True", 4))) ||
          (n == 1 && v[0] == '1') ||
          (n == 3 && (!memcmp(v, "yes", 3) || !memcmp(v, "Yes", 3))) ||
          (n == 2 && (!memcmp(v, "on", 2) || !memcmp(v, "On", 2)));
}

/* Read the player's file into the (empty) list.  A file that is missing,
 * unreadable or not a .cht at all gives an empty list. */
static void cht_read(const char *path)
{
   size_t size, n_slots = 0, i;
   char *text = file_read(path, &size);
   char *p, *end;
   cht_slot *slots;

   g_file_existed = text != NULL;
   if (!text)
      return;
   slots = (cht_slot *)calloc(CHEATS_MAX, sizeof(cht_slot));
   if (!slots)
   {
      free(text);
      return;
   }
   p = text;
   end = text + size;
   if (size >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3))
      p += 3;                               /* a byte order mark */

   while (p < end)
   {
      char *line = p, *eol = p, *key, *kend, *val, *vend;
      const char *raw_val;
      size_t raw_len;
      while (eol < end && *eol != '\n' && *eol != '\r')
         eol++;
      p = eol;
      while (p < end && (*p == '\n' || *p == '\r'))
         p++;

      key = line;
      while (key < eol && (*key == ' ' || *key == '\t'))
         key++;
      if (key == eol || *key == '#')
         continue;
      kend = key;
      while (kend < eol && *kend != '=')
         kend++;
      if (kend == eol)
      {
         /* not "key = value": keep it, it is somebody's */
         append_text(&g_global_extra, key, (size_t)(eol - key), "", 0);
         continue;
      }
      val = kend + 1;
      while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t'))
         kend--;
      while (val < eol && (*val == ' ' || *val == '\t'))
         val++;
      vend = eol;
      raw_val = val;
      if (val < eol && *val == '"')
      {
         /* quoted: up to the LAST quote of the line -- or, when the line
          * has no other, of a later one (a code written on several lines) */
         char *q = eol;
         val++;
         while (q > val && q[-1] != '"')
            q--;
         if (q > val)
            vend = q - 1;
         else
         {
            char *close = val;
            while (close < end && *close != '"')
               close++;
            vend = close;
            p = close < end ? close + 1 : end;
            while (p < end && *p != '\n' && *p != '\r')
               p++;
            while (p < end && (*p == '\n' || *p == '\r'))
               p++;
         }
         raw_len = (size_t)((vend < end ? vend + 1 : vend) - raw_val);
      }
      else
      {
         while (vend > val && (vend[-1] == ' ' || vend[-1] == '\t'))
            vend--;
         raw_len = (size_t)(vend - raw_val);
      }

      if ((size_t)(kend - key) > 5 && !memcmp(key, "cheat", 5) &&
          key[5] >= '0' && key[5] <= '9')
      {
         unsigned long index = 0;
         char *s = key + 5, *suffix;
         cht_slot *slot = NULL;
         while (s < kend && *s >= '0' && *s <= '9' && index < 100000000ul)
            index = index * 10 + (unsigned long)(*s++ - '0');
         if (s < kend && *s == '_')
         {
            suffix = s + 1;
            for (i = 0; i < n_slots; i++)
               if (slots[i].index == index)
                  slot = &slots[i];
            if (!slot && n_slots < CHEATS_MAX)
            {
               slot = &slots[n_slots++];
               slot->index = index;
            }
            if (!slot)
               continue;                    /* more than the list holds */
            if (kend - suffix == 4 && !memcmp(suffix, "desc", 4))
            {
               free(slot->desc);
               slot->desc = name_clean(val, (size_t)(vend - val));
            }
            else if (kend - suffix == 4 && !memcmp(suffix, "code", 4))
            {
               free(slot->code);
               slot->code = code_clean(val, (size_t)(vend - val));
            }
            else if (kend - suffix == 6 && !memcmp(suffix, "enable", 6))
               slot->enable = truth(val, (size_t)(vend - val));
            else
            {
               /* RetroArch's own keys (cheatN_handler, _address, ...) */
               char head[96];
               int n = snprintf(head, sizeof(head), "%.*s = ",
                                (int)(kend - suffix > 64 ? 64 : kend - suffix),
                                suffix);
               append_text(&slot->extra, head, (size_t)n, raw_val, raw_len);
            }
            continue;
         }
      }
      if (kend - key == 6 && !memcmp(key, "cheats", 6))
         continue;                          /* the count: we count ourselves */
      {
         char head[96];
         int n = snprintf(head, sizeof(head), "%.*s = ",
                          (int)(kend - key > 64 ? 64 : kend - key), key);
         append_text(&g_global_extra, head, (size_t)n, raw_val, raw_len);
      }
   }

   qsort(slots, n_slots, sizeof(slots[0]), slot_cmp);
   for (i = 0; i < n_slots; i++)
   {
      cht_slot *s = &slots[i];
      char fallback[32];
      if (!s->code)
         s->code = dup_n("", 0);
      if (!s->desc || !s->desc[0])
      {
         free(s->desc);
         snprintf(fallback, sizeof(fallback), "Cheat %d", g_count + 1);
         s->desc = dup_n(fallback, strlen(fallback));
      }
      entry_insert(g_count, s->desc, NULL, s->code, NULL, s->extra, s->enable, 0);
   }
   free(slots);
   free(text);
}

static int cht_write(const char *path)
{
   FILE *f;
   int i, n = 0, ok;

   for (i = 0; i < g_count; i++)
      if (!g_list[i].builtin || g_x[i].touched)
         n++;
   if (!n && !g_file_existed && !g_global_extra)
      return CHEATS_OK;                     /* nothing of the player's */

   f = fopen(path, "wb");
   if (!f)
   {
      /* the cheats directory of a fresh installation */
      mkdir(g_cht_dir, 0777);
      f = fopen(path, "wb");
   }
   if (!f)
      return CHEATS_E_IO;
   fprintf(f, "cheats = %d\n", n);
   n = 0;
   for (i = 0; i < g_count; i++)
   {
      if (g_list[i].builtin && !g_x[i].touched)
         continue;
      fprintf(f, "\ncheat%d_desc = \"%s\"\ncheat%d_code = \"%s\"\n"
                 "cheat%d_enable = %s\n",
              n, g_list[i].name, n, g_list[i].code, n,
              g_list[i].enabled ? "true" : "false");
      if (g_x[i].extra)
      {
         /* "suffix = value" lines: put the entry's new number back on */
         const char *s = g_x[i].extra;
         while (*s)
         {
            const char *e = strchr(s, '\n');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            fprintf(f, "cheat%d_%.*s\n", n, (int)len, s);
            s += len + (e ? 1 : 0);
         }
      }
      n++;
   }
   if (g_global_extra)
      fprintf(f, "\n%s", g_global_extra);
   ok = !ferror(f);
   if (fclose(f) != 0)
      ok = 0;
   if (!ok)
      return CHEATS_E_IO;
   g_file_existed = 1;
   return CHEATS_OK;
}

/* ======================================================================== */
/* which game is this                                                       */
/* ======================================================================== */

static const char *base_name(const char *path)
{
   const char *b = path, *p;
   for (p = path; *p; p++)
      if (*p == '/' || *p == '\\' || *p == ':')
         b = p + 1;
   return b;
}

/* The ROM's CRC-32, from <root>/cheats/index.cache when this file (by name
 * and size) has been hashed before: a 32 MiB cartridge takes seconds to
 * read from a memory card, and it is read for this once in its life. */
static int rom_crc(const char *rom_path, uint32_t *crc_out)
{
   static char cache_path[640];
   static uint8_t buf[32768];
   const char *name = base_name(rom_path);
   FILE *f;
   long size;
   uint32_t crc = 0;
   size_t got;

   f = fopen(rom_path, "rb");
   if (!f)
      return -1;
   if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
       fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return -1;
   }

   snprintf(cache_path, sizeof(cache_path), "%s/index.cache", g_cht_dir);
   {
      FILE *c = fopen(cache_path, "rb");
      if (c)
      {
         char line[700];
         int hit = 0;
         while (fgets(line, sizeof(line), c))
         {
            unsigned long csize = 0;
            unsigned int ccrc = 0;
            int at = 0;
            line[strcspn(line, "\r\n")] = 0;
            if (sscanf(line, "%8x %lu %n", &ccrc, &csize, &at) >= 2 && at &&
                csize == (unsigned long)size && !strcmp(line + at, name))
            {
               crc = ccrc;      /* the last line for a name wins */
               hit = 1;
            }
         }
         fclose(c);
         if (hit)
         {
            fclose(f);
            *crc_out = crc;
            return 0;
         }
      }
   }

   while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
      crc = cheatdb_crc32(crc, buf, got);
   if (ferror(f))
   {
      fclose(f);
      return -1;
   }
   fclose(f);

   {
      FILE *c = fopen(cache_path, "ab");
      if (!c)
      {
         mkdir(g_cht_dir, 0777);
         c = fopen(cache_path, "ab");
      }
      if (c)
      {
         /* start over rather than grow for ever (some 4,000 games) */
         fseek(c, 0, SEEK_END);
         if (ftell(c) > 256 * 1024)
         {
            fclose(c);
            c = fopen(cache_path, "wb");
         }
         if (c)
         {
            fprintf(c, "%08X %lu %s\n", (unsigned)crc, (unsigned long)size, name);
            fclose(c);
         }
      }
   }
   *crc_out = crc;
   return 0;
}

/* The cartridge header: the GBA's 4-character game code and 12-character
 * title, the Game Boy's title. */
static void rom_header(const char *rom_path, fe_console_t console,
                       char code[5], char title[20])
{
   uint8_t h[0x150];
   FILE *f = fopen(rom_path, "rb");
   size_t got, i, n = 0;

   code[0] = title[0] = 0;
   if (!f)
      return;
   got = fread(h, 1, sizeof(h), f);
   fclose(f);
   if (console == FE_CONSOLE_GBA)
   {
      if (got < 0xC0)
         return;
      for (i = 0; i < 4; i++)
      {
         uint8_t c = h[0xAC + i];
         if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return;
         code[i] = (char)c;
      }
      code[4] = 0;
      for (i = 0; i < 12 && h[0xA0 + i] >= 0x20 && h[0xA0 + i] < 0x7F; i++)
         title[n++] = (char)h[0xA0 + i];
   }
   else
   {
      /* a Game Boy Color cartridge keeps the last five bytes of the old
       * 16-character field for a maker's code and its flag */
      size_t len;
      if (got < 0x150)
         return;
      len = (h[0x143] & 0x80) ? 11 : 16;
      for (i = 0; i < len && h[0x134 + i] >= 0x20 && h[0x134 + i] < 0x7F; i++)
         title[n++] = (char)h[0x134 + i];
   }
   title[n] = 0;
}

/* Find the game in the database.  Order: the ROM's CRC-32; the header's
 * game code / title; the file's name.  For the GBA the game code is asked
 * FIRST when the database says every release with that code has the same
 * list -- the answer is the one the CRC would give, without hashing up to
 * 32 MiB. */
static int db_find(const char *rom_path, fe_console_t console, int *match)
{
   char sys = console == FE_CONSOLE_GBA ? 'A' : 'G';
   char code[5], title[20];
   int game = -1, by_code = -1, unique = 0, exact = 0;
   uint32_t crc;

   *match = CHEATS_MATCH_NONE;
   if (!cheatdb_games())
      return -1;
   rom_header(rom_path, console, code, title);

   if (console == FE_CONSOLE_GBA && code[0])
   {
      by_code = cheatdb_find_serial(code, &unique);
      if (by_code >= 0 && unique)
      {
         *match = CHEATS_MATCH_CODE;
         return by_code;
      }
   }
   if (rom_crc(rom_path, &crc) == 0 && (game = cheatdb_find_crc(crc)) >= 0)
   {
      *match = CHEATS_MATCH_CRC;
      return game;
   }
   if (by_code >= 0)
   {
      *match = CHEATS_MATCH_CODE;
      return by_code;
   }
   /* the file's full name before the header's title: "(Europe)" in a name
    * says more than twelve capital letters do */
   game = cheatdb_find_name(sys, rom_path, &exact);
   if (game >= 0 && exact)
   {
      *match = CHEATS_MATCH_NAME;
      return game;
   }
   if (title[0])
   {
      int t = cheatdb_find_title(sys, title);
      if (t >= 0)
      {
         *match = CHEATS_MATCH_TITLE;
         return t;
      }
   }
   if (game >= 0)
      *match = CHEATS_MATCH_TITLE;
   return game;
}

/* ======================================================================== */
/* the contract                                                             */
/* ======================================================================== */

int cheats_db_open(const char *path)
{
   return cheatdb_open(path);
}

const char *cheats_db_game(void)
{
   return g_db_name;
}

int cheats_db_match(void)
{
   return g_db_match;
}

void cheats_unload(void)
{
   int i;
   if (g_loaded)
   {
      g_suspended = 1;        /* an empty program for the core */
      push();
   }
   for (i = 0; i < g_count; i++)
      entry_free(i);
   g_count = 0;
   free(g_db_block);
   g_db_block = NULL;
   free(g_global_extra);
   g_global_extra = NULL;
   g_db_name[0] = 0;
   g_db_match = CHEATS_MATCH_NONE;
   g_cht_path[0] = 0;
   g_loaded = 0;
   g_dirty = 0;
   g_suspended = 0;
   g_hardcore = 0;
   g_file_existed = 0;
}

int cheats_load(const char *root, const char *rom_path, fe_console_t console)
{
   const char *name, *dot;
   int game, stem;

   cheats_unload();
   if (!root || !rom_path)
      return 0;
   name = base_name(rom_path);
   dot = strrchr(name, '.');
   stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
   snprintf(g_cht_dir, sizeof(g_cht_dir), "%s/cheats", root);
   snprintf(g_cht_path, sizeof(g_cht_path), "%s/%.*s.cht", g_cht_dir, stem, name);
   g_console = console;
   g_loaded = 1;

   cht_read(g_cht_path);

   game = db_find(rom_path, console, &g_db_match);
   if (game >= 0)
   {
      unsigned n = 0, k;
      size_t size = 0;
      g_db_block = cheatdb_load(game, &n, &size);
      if (g_db_block)
      {
         const char *p = g_db_block;
         int own = g_count, j;
         snprintf(g_db_name, sizeof(g_db_name), "%s", p);
         p += strlen(p) + 1;
         for (k = 0; k < n; k++)
         {
            const char *ename = p, *ecode = p + strlen(p) + 1;
            int have = 0;
            p = ecode + strlen(ecode) + 1;
            for (j = 0; j < own && !have; j++)
               have = code_same(g_list[j].code, ecode);
            if (!have)
               entry_insert(g_count, NULL, ename, NULL, ecode, NULL, 0, 1);
         }
      }
      else
         g_db_match = CHEATS_MATCH_NONE;
   }

   validate_all();
   push();
   return g_count;
}

int cheats_count(void)
{
   return g_count;
}

const cheat_entry *cheats_get(int i)
{
   return i >= 0 && i < g_count ? &g_list[i] : NULL;
}

int cheats_enabled_count(void)
{
   int i, n = 0;
   for (i = 0; i < g_count; i++)
      n += g_list[i].enabled;
   return n;
}

int cheats_set_enabled(int i, int on)
{
   int overflow;
   if (i < 0 || i >= g_count)
      return CHEATS_E_RANGE;
   on = on ? 1 : 0;
   if (on && (g_suspended || g_hardcore))
      return CHEATS_E_SUSPENDED;
   if (on && !g_list[i].usable)
      return g_x[i].rc ? g_x[i].rc : CHEATS_E_UNSUPPORTED;
   if (g_list[i].enabled == on)
      return CHEATS_OK;
   g_list[i].enabled = on;
   overflow = push();
   if (on && overflow >= 0)
   {
      /* more operations than the core is given room for */
      g_list[i].enabled = 0;
      push();
      return CHEATS_E_FULL;
   }
   if (on && g_rom_failed > 0 && (g_x[i].flags & CE_F_ROM_PATCH))
   {
      /* a ROM patch on a cartridge that is not held whole in memory */
      g_list[i].enabled = 0;
      push();
      return CHEATS_E_UNSUPPORTED;
   }
   if (g_list[i].builtin)
      g_x[i].touched = 1;
   g_dirty = 1;
   return CHEATS_OK;
}

void cheats_disable_all(void)
{
   int i, any = 0;
   for (i = 0; i < g_count; i++)
      if (g_list[i].enabled)
      {
         g_list[i].enabled = 0;
         if (g_list[i].builtin)
            g_x[i].touched = 1;
         any = 1;
      }
   if (any)
   {
      g_dirty = 1;
      push();
   }
}

int cheats_validate(const char *code, fe_console_t console)
{
   int kind = CE_KIND_UNKNOWN, rc;
   char *clean;

   if (!code)
      return CHEATS_E_FORMAT;
   if (strlen(code) >= CHEATS_CODE_CAP)
      return CHEATS_E_TOO_LONG;
   clean = code_clean(code, strlen(code));
   if (!clean)
      return CHEATS_E_TOO_LONG;
   if (console == FE_CONSOLE_GBA)
   {
      /* with the running game's seeds, and its own if it brings any */
      static cheat_gba_ctx ctx;
      cheat_out out;
      if (g_loaded && g_console == FE_CONSOLE_GBA)
         ctx = g_ctx;
      else
         cheat_gba_ctx_init(&ctx);
      cheat_gba_ctx_scan(&ctx, clean);
      out.ops = g_ops;
      out.cap_ops = OPS_MAX;
      out.patches = g_patches;
      out.cap_patches = PATCHES_MAX;
      out.n_ops = out.n_patches = 0;
      out.flags = 0;
      rc = cheat_gba_compile(clean, &ctx, &out, &kind);
   }
   else
   {
      unsigned n = 0;
      rc = cheat_gb_compile(clean, g_gb, GB_CODES_MAX, &n, &kind);
   }
   free(clean);
   if (rc != CE_OK)
      return map_error(rc);
   return kind > 0 ? kind : CHEATS_E_FORMAT;
}

int cheats_add(const char *name, const char *code)
{
   char fallback[32];
   char *cname, *ccode;
   int rc, at, i;

   if (!g_loaded)
      return CHEATS_E_RANGE;
   if (g_count >= CHEATS_MAX)
      return CHEATS_E_FULL;
   rc = cheats_validate(code, g_console);
   if (rc < 0)
      return rc;
   ccode = code_clean(code, strlen(code));
   if (!ccode)
      return CHEATS_E_TOO_LONG;

   /* The same code again?  Then it is that entry, switched on. */
   for (i = 0; i < g_count; i++)
      if (code_same(g_list[i].code, ccode))
      {
         free(ccode);
         rc = cheats_set_enabled(i, 1);
         return rc == CHEATS_OK ? i : rc;
      }

   if (!name || !name[0])
   {
      int n = 0;
      for (i = 0; i < g_count; i++)
         n += !g_list[i].builtin;
      snprintf(fallback, sizeof(fallback), "Cheat %d", n + 1);
      name = fallback;
   }
   cname = name_clean(name, strlen(name));
   if (cname && !cname[0])
   {
      free(cname);
      cname = dup_n("Cheat", 5);
   }
   at = entry_insert(own_end(), cname, NULL, ccode, NULL, NULL, 0, 0);
   if (at < 0)
      return CHEATS_E_FULL;
   g_dirty = 1;
   validate_all();            /* it may be a master code: new seeds */
   if (!g_suspended && !g_hardcore)
   {
      g_list[at].enabled = 1;
      if (push() >= 0)
      {
         g_list[at].enabled = 0;
         push();
      }
   }
   return at;
}

int cheats_remove(int i)
{
   if (i < 0 || i >= g_count)
      return CHEATS_E_RANGE;
   if (g_list[i].builtin)
      return CHEATS_E_BUILTIN;  /* it would be back at the next boot: the
                                 * database is not the player's to edit */
   entry_free(i);
   memmove(&g_list[i], &g_list[i + 1], (size_t)(g_count - i - 1) * sizeof(g_list[0]));
   memmove(&g_x[i], &g_x[i + 1], (size_t)(g_count - i - 1) * sizeof(g_x[0]));
   g_count--;
   memset(&g_list[g_count], 0, sizeof(g_list[0]));
   memset(&g_x[g_count], 0, sizeof(g_x[0]));
   g_dirty = 1;
   validate_all();
   push();
   return CHEATS_OK;
}

int cheats_rename(int i, const char *name)
{
   char *cname;
   if (i < 0 || i >= g_count)
      return CHEATS_E_RANGE;
   if (!name || !name[0])
      return CHEATS_E_FORMAT;
   cname = name_clean(name, strlen(name));
   if (!cname || !cname[0])
   {
      free(cname);
      return CHEATS_E_FORMAT;
   }
   free(g_x[i].name_own);
   g_x[i].name_own = cname;
   g_list[i].name = cname;
   if (g_list[i].builtin)
      g_x[i].touched = 1;
   g_dirty = 1;
   return CHEATS_OK;
}

void cheats_apply(void)
{
   push();
}

void cheats_suspend(int on)
{
   on = on ? 1 : 0;
   if (g_suspended == on)
      return;
   g_suspended = on;
   push();
}

int cheats_suspended(void)
{
   return g_suspended ? 1 : g_hardcore ? 2 : 0;
}

void cheats_hardcore(int on)
{
   on = on ? 1 : 0;
   if (g_hardcore == on)
      return;
   g_hardcore = on;
   push();
}

int cheats_flush(void)
{
   int rc;
   if (!g_loaded || !g_dirty)
      return CHEATS_OK;
   rc = cht_write(g_cht_path);
   if (rc == CHEATS_OK)
      g_dirty = 0;
   return rc;
}

void cheats_button(int down)
{
   core_button(down);
}

const char *cheats_note(int i)
{
   if (i < 0 || i >= g_count)
      return "";
   if (!g_list[i].usable)
      return cheats_error_text(g_x[i].rc);
   if (g_x[i].flags & CE_F_NOOP)
      return "An enable code. Not needed on this console; harmless when on.";
   if (g_x[i].flags & CE_F_BUTTON)
      return "Acts while the cheat button is held.";
   if (g_x[i].flags & CE_F_ROM_PATCH)
      return "Changes the game's program.";
   return "";
}

const char *cheats_error_text(int rc)
{
   switch (rc)
   {
   case CHEATS_OK:            return "";
   case CHEATS_E_FULL:        return "No room for more cheats.";
   case CHEATS_E_FORMAT:      return "This is not a code for this console.";
   case CHEATS_E_UNSUPPORTED: return "This kind of code cannot be used here.";
   case CHEATS_E_TOO_LONG:    return "The code is too long.";
   case CHEATS_E_IO:          return "The cheat file could not be saved.";
   case CHEATS_E_RANGE:       return "No such cheat.";
   case CHEATS_E_NEEDS_VALUE: return "Incomplete: replace ?? / XX with the value you want.";
   case CHEATS_E_ENCRYPTED:   return "Encrypted: add this game's master code first.";
   case CHEATS_E_SUSPENDED:   return "Cheats are off while playing over a link.";
   case CHEATS_E_BUILTIN:     return "A built-in cheat cannot be deleted. Switch it off instead.";
   default:                   return "The cheat could not be used.";
   }
}

const char *cheats_kind_name(int kind)
{
   switch (kind)
   {
   case CHEAT_KIND_GBA_RAW:         return "Raw";
   case CHEAT_KIND_GBA_CODEBREAKER: return "CodeBreaker";
   case CHEAT_KIND_GBA_GAMESHARK:   return "GameShark";
   case CHEAT_KIND_GBA_AR3:         return "Action Replay";
   case CHEAT_KIND_GB_GAMESHARK:    return "GameShark";
   case CHEAT_KIND_GB_GAMEGENIE:    return "Game Genie";
   default:                         return "Unknown";
   }
}

const char *cheats_format_hint(fe_console_t console)
{
   if (console == FE_CONSOLE_GBA)
      return "8 digits, space, 4 digits (CodeBreaker) or 8 digits "
             "(GameShark, Action Replay). + between lines.";
   return "8 digits (GameShark) or 3-3-3 digits (Game Genie). + between codes.";
}
