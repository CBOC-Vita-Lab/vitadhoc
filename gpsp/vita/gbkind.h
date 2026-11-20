/* gbkind.h -- which Game Boy a game file is: the cartridge header decides,
 * the file name only when the header cannot be read, and the player can
 * overrule both for one file.
 *
 * Header byte 0x143 is the Color flag: 0x80 (works on a Color and on the old
 * Game Boy) and 0xC0 (Color only) are Game Boy Color cartridges; anything
 * else is a plain Game Boy one.  A Color hack named ".gb" used to be listed
 * and played as a Game Boy (grey, with the Color features missing), and a
 * plain game named ".gbc" as a Color; both now go where the cartridge says.
 *
 * <app folder>/gb_override.txt, one game a line, overrules the header:
 *     gbc  Pokemon Blue Color Hack.gb      (play it as a Game Boy Color)
 *     gb   Some Game.gbc                   (play it as a Game Boy)
 * The file name is the game's own (no folder), compared without regard to
 * case; "#" starts a comment.
 *
 * Header only; the app's list scan and its direct launch both call it.
 * Licensed MIT OR GPL-2.0-or-later (VitAdhoc's own). */
#ifndef GBKIND_H
#define GBKIND_H

#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { GBK_NONE = 0, GBK_GB = 1, GBK_GBC = 2 };

/* the cartridge's Color flag */
static inline int gbk_from_flag(unsigned char flag)
{
   return flag == 0x80 || flag == 0xC0 ? GBK_GBC : GBK_GB;
}

/* GBK_GB / GBK_GBC from the file's header; GBK_NONE when it cannot be read
 * (too short, missing). */
static inline int gbk_header_kind(const char *path)
{
   FILE *f = path ? fopen(path, "rb") : NULL;
   unsigned char hdr[0x150];
   size_t n;
   if (!f)
      return GBK_NONE;
   n = fread(hdr, 1, sizeof(hdr), f);
   fclose(f);
   return n >= sizeof(hdr) ? gbk_from_flag(hdr[0x143]) : GBK_NONE;
}

/* the extension's own idea: ".gbc" Color, ".gb" Game Boy, else none */
static inline int gbk_ext_kind(const char *name)
{
   size_t n = name ? strlen(name) : 0;
   if (n > 4 && !strcasecmp(name + n - 4, ".gbc"))
      return GBK_GBC;
   if (n > 3 && !strcasecmp(name + n - 3, ".gb"))
      return GBK_GB;
   return GBK_NONE;
}

/* the player's per-file overrides */
#define GBK_OVR_MAX  64
#define GBK_OVR_NAME 120
static struct { char name[GBK_OVR_NAME]; int kind; } gbk_ovr[GBK_OVR_MAX];
static int gbk_ovr_n;

static inline void gbk_overrides_clear(void) { gbk_ovr_n = 0; }

static inline void gbk_overrides_parse(const char *text)
{
   const char *p = text;
   gbk_ovr_n = 0;
   while (p && *p && gbk_ovr_n < GBK_OVR_MAX)
   {
      const char *eol = strchr(p, '\n'), *q = p;
      size_t len = eol ? (size_t)(eol - p) : strlen(p), k = 0;
      char line[GBK_OVR_NAME + 16];
      int kind = GBK_NONE;
      if (len >= sizeof(line))
         len = sizeof(line) - 1;
      memcpy(line, p, len);
      line[len] = '\0';
      p = eol ? eol + 1 : p + len;
      if (len && line[len - 1] == '\r')
         line[--len] = '\0';
      while (line[k] == ' ' || line[k] == '\t')
         k++;
      if (line[k] == '#' || !line[k])
         continue;
      q = line + k;
      if (!strncasecmp(q, "gbc", 3) && (q[3] == ' ' || q[3] == '\t'))
         kind = GBK_GBC, q += 3;
      else if (!strncasecmp(q, "gb", 2) && (q[2] == ' ' || q[2] == '\t'))
         kind = GBK_GB, q += 2;
      else
         continue;
      while (*q == ' ' || *q == '\t')
         q++;
      if (!*q || strlen(q) >= GBK_OVR_NAME)
         continue;
      snprintf(gbk_ovr[gbk_ovr_n].name, GBK_OVR_NAME, "%s", q);
      gbk_ovr[gbk_ovr_n++].kind = kind;
   }
}

static inline int gbk_overrides_load(const char *path)
{
   static char text[GBK_OVR_MAX * (GBK_OVR_NAME + 16)];
   FILE *f = path ? fopen(path, "rb") : NULL;
   size_t n;
   gbk_ovr_n = 0;
   if (!f)
      return 0;
   n = fread(text, 1, sizeof(text) - 1, f);
   fclose(f);
   text[n] = '\0';
   gbk_overrides_parse(text);
   return gbk_ovr_n;
}

static inline int gbk_override_for(const char *name)
{
   const char *slash = name ? strrchr(name, '/') : NULL;
   int i;
   if (slash)
      name = slash + 1;
   for (i = 0; name && i < gbk_ovr_n; i++)
      if (!strcasecmp(gbk_ovr[i].name, name))
         return gbk_ovr[i].kind;
   return GBK_NONE;
}

/* The answer: the override, else the header, else the extension. */
static inline int gbk_decide(const char *path)
{
   int k = gbk_override_for(path);
   if (k != GBK_NONE)
      return k;
   k = gbk_header_kind(path);
   return k != GBK_NONE ? k : gbk_ext_kind(path);
}

#endif
