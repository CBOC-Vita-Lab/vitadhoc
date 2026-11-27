/*
 * Cover-name fallbacks: the same game under the name the thumbnail
 * archive actually uses.
 *
 * The archive names its pictures after the No-Intro entry of one release
 * of a game; a ROM set often holds another release (a "(World)" cartridge
 * whose picture is filed as "(USA)", a "(Rev 1)", a language list the
 * picture's name leaves out).  The exact name is always asked for first;
 * these are what is asked next, in the order that finds a picture most
 * often (measured on the full GBA and SNES lists):
 *   1. the same name without its revision / version / language tags;
 *   2. the same title with the other regions, most common swap first;
 *   3. each of those with the " - " of a subtitle turned into a space.
 * At most AF_ALT_MAX names come back, none equal to one already in
 * `have`.  Pure string work: no I/O, no allocation.
 */
#ifndef ARTALT_H
#define ARTALT_H

#include <stdio.h>
#include <string.h>

/* a name too long for the buffers is cut short: it asks for a picture that is not there, nothing worse */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#define AF_ALT_MAX 7
#define AF_ALT_CAP 256

/* "En,Fr,De", "En", "Ja+En" ... : a language group of a No-Intro name */
static int alt_is_lang(const char *s)
{
   int n = 0;
   for (;;)
   {
      if (!(s[0] >= 'A' && s[0] <= 'Z') || !(s[1] >= 'a' && s[1] <= 'z'))
         return 0;
      s += 2;
      if (*s == '-')                 /* Zh-Hant */
      {
         s++;
         if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')))
            return 0;
         while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))
            s++;
      }
      n++;
      if (!*s)
         return n > 0;
      if (*s != ',' && *s != '+')
         return 0;
      s++;
   }
}

/* Expand GoodTools short region codes in place; returns 1 if changed. */
static int alt_expand_goodtools(char *r, size_t cap)
{
   static const struct { const char *code; const char *full; } map[] = {
      { "U",   "USA" },
      { "E",   "Europe" },
      { "J",   "Japan" },
      { "W",   "World" },
      { "JU",  "Japan, USA" },
      { "UJ",  "Japan, USA" },
      { "UE",  "USA, Europe" },
      { "EU",  "USA, Europe" },
      { "JE",  "Japan, Europe" },
      { "EJ",  "Japan, Europe" },
      { "JUE", "World" },
      { "F",   "France" },
      { "G",   "Germany" },
      { "S",   "Spain" },
      { "I",   "Italy" },
      { "A",   "Australia" },
      { "Au",  "Australia" },
      { "B",   "Brazil" },
      { "K",   "Korea" },
      { "C",   "China" },
      { "Sw",  "Sweden" },
      { "Nl",  "Netherlands" },
      { 0, 0 }
   };
   int i;
   for (i = 0; map[i].code; i++)
      if (!strcmp(r, map[i].code))
      {
         snprintf(r, cap, "%s", map[i].full);
         return 1;
      }
   return 0;
}

/* Strip GoodTools [...] bracket tags from `name` into `out` (collapsing
 * double spaces and trailing whitespace); returns 1 if anything was stripped. */
static int alt_strip_brackets(const char *name, char *out, size_t cap)
{
   size_t o = 0;
   const char *p = name;
   int stripped = 0;
   if (!cap)
      return 0;
   while (*p && o + 1 < cap)
   {
      if (*p == '[')
      {
         const char *e = strchr(p + 1, ']');
         if (e)
         {
            while (o > 0 && out[o - 1] == ' ')
               o--;
            p = e + 1;
            while (*p == ' ')
               p++;
            if (o > 0 && *p && out[o - 1] != ' ')
               out[o++] = ' ';
            stripped = 1;
            continue;
         }
      }
      out[o++] = *p++;
   }
   while (o > 0 && out[o - 1] == ' ')
      o--;
   out[o] = '\0';
   return stripped;
}

/* Split "Title (G1) (G2)" at the first " (" from which only groups follow.
 * Returns the number of groups (0: no tags); g[i] are NUL-terminated copies. */
#define ALT_GROUPS 8
static int alt_split(const char *name, char *title, size_t tcap, char g[ALT_GROUPS][64])
{
   const char *p;
   for (p = name; *p; p++)
   {
      const char *q = p;
      int n = 0, ok = 1;
      if (!(p == name || p[-1] == ' ') || *p != '(' || p == name)
         continue;
      /* the groups from here to the end */
      while (*q)
      {
         const char *e;
         size_t len;
         if (*q == ' ')
         {
            q++;
            continue;
         }
         if (*q == '[')
         {
            e = strchr(q, ']');
            if (!e)
            {
               ok = 0;
               break;
            }
            q = e + 1;
            continue;
         }
         if (*q != '(' || n >= ALT_GROUPS)
         {
            ok = 0;
            break;
         }
         e = strchr(q, ')');
         if (!e)
         {
            ok = 0;
            break;
         }
         len = (size_t)(e - q) - 1;
         if (len >= 64)
            len = 63;
         memcpy(g[n], q + 1, len);
         g[n][len] = 0;
         n++;
         q = e + 1;
      }
      if (ok && n)
      {
         size_t t = (size_t)(p - name);
         while (t && name[t - 1] == ' ')
            t--;
         if (t >= tcap)
            t = tcap - 1;
         memcpy(title, name, t);
         title[t] = 0;
         return n;
      }
   }
   snprintf(title, tcap, "%s", name);
   return 0;
}

static const char *const ALT_AFTER_USA[]    = { "USA, Europe", "Europe", "World", "Japan", 0 };
static const char *const ALT_AFTER_EUROPE[] = { "USA, Europe", "USA", "World", "Japan", 0 };
static const char *const ALT_AFTER_JAPAN[]  = { "USA", "Europe", "World", "USA, Europe", 0 };
static const char *const ALT_AFTER_WORLD[]  = { "USA", "Europe", "Japan", "USA, Europe", 0 };
static const char *const ALT_AFTER_OTHER[]  = { "Europe", "USA, Europe", "USA", "Japan", "World", 0 };


static const char *const *alt_regions(const char *r)
{
   if (!strcmp(r, "USA") || !strcmp(r, "USA, Europe")) return ALT_AFTER_USA;
   if (!strcmp(r, "Europe")) return ALT_AFTER_EUROPE;
   if (!strcmp(r, "Japan")) return ALT_AFTER_JAPAN;
   if (!strcmp(r, "World")) return ALT_AFTER_WORLD;
   return ALT_AFTER_OTHER;
}

/* a region group we recognise (a plain list of countries and regions); any
 * other first group ("Beta", "Proto", "Unl") means a name we leave alone */
static int alt_is_region(const char *r)
{
   static const char *const K[] = { "USA", "Europe", "Japan", "World", "Australia", "Brazil",
      "Korea", "China", "France", "Germany", "Spain", "Italy", "Asia", "Canada", "Netherlands",
      "Sweden", "Hong Kong", "Taiwan", "Russia", "Scandinavia", 0 };
   char buf[64], *tok, *save = 0;
   int i, any = 0;
   snprintf(buf, sizeof(buf), "%s", r);
   for (tok = buf; tok; tok = save)
   {
      char *c = strstr(tok, ", ");
      int hit = 0;
      if (c)
      {
         *c = 0;
         save = c + 2;
      }
      else
         save = 0;
      for (i = 0; K[i]; i++)
         if (!strcmp(tok, K[i]))
            hit = 1;
      if (!hit)
         return 0;
      any = 1;
   }
   return any;
}

typedef struct { char name[AF_ALT_MAX][AF_ALT_CAP]; int n; } af_alts;

static void alt_add(af_alts *o, const char *have[], int nhave, const char *s)
{
   int i;
   if (o->n >= AF_ALT_MAX || strlen(s) >= AF_ALT_CAP)
      return;
   for (i = 0; i < nhave; i++)
      if (!strcmp(have[i], s))
         return;
   for (i = 0; i < o->n; i++)
      if (!strcmp(o->name[i], s))
         return;
   snprintf(o->name[o->n++], AF_ALT_CAP, "%s", s);
}

/* the fallbacks for `name`; `have` are the names already asked for */
static void alt_names(const char *name, const char *have[], int nhave, af_alts *o)
{
   char clean[AF_ALT_CAP], title[AF_ALT_CAP], spaced[AF_ALT_CAP], g[ALT_GROUPS][64];
   const char *titles[2];
   const char *const *alts;
   char one[AF_ALT_CAP];
   int ng, nt, t, a, lang, stripped;
   const char *dash;

   o->n = 0;
   stripped = alt_strip_brackets(name, clean, sizeof(clean));
   ng = alt_split(clean, title, sizeof(title), g);
   if (ng)
      alt_expand_goodtools(g[0], sizeof(g[0]));
   if (!ng || !alt_is_region(g[0]))
   {
      if (!ng && stripped && clean[0])
         alt_add(o, have, nhave, clean);
      return;
   }
   lang = ng > 1 && alt_is_lang(g[1]);
   titles[0] = title;
   nt = 1;
   dash = strstr(title, " - ");
   if (dash)
   {
      snprintf(spaced, sizeof(spaced), "%.*s %s", (int)(dash - title), title, dash + 3);
      titles[nt++] = spaced;
   }
   for (t = 0; t < nt; t++)
   {
      if (lang)
      {
         snprintf(one, sizeof(one), "%s (%s) (%s)", titles[t], g[0], g[1]);
         alt_add(o, have, nhave, one);
      }
      snprintf(one, sizeof(one), "%s (%s)", titles[t], g[0]);
      alt_add(o, have, nhave, one);
   }
   alts = alt_regions(g[0]);
   for (t = 0; t < nt; t++)
      for (a = 0; alts[a]; a++)
      {
         if (lang)
         {
            snprintf(one, sizeof(one), "%s (%s) (%s)", titles[t], alts[a], g[1]);
            alt_add(o, have, nhave, one);
         }
         snprintf(one, sizeof(one), "%s (%s)", titles[t], alts[a]);
         alt_add(o, have, nhave, one);
      }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif
