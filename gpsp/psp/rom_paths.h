/* Bounds and checked builders for paths derived from browser ROM names.
 * A browser entry can contain up to 319 relative-path bytes; with the PSP
 * install path and sidecar suffixes, 512 bytes leaves room for all products. */
#ifndef PSP_ROM_PATHS_H
#define PSP_ROM_PATHS_H

#include <stdio.h>
#include <string.h>

#define PSP_ROM_REL_PATH_CAP 320
#define PSP_FILE_PATH_CAP    512
#define PSP_BACKUP_PATH_CAP  (PSP_FILE_PATH_CAP + 4) /* ".bak" + NUL */

#ifdef VITA
/* A browser entry from an EXTRA ROM FOLDER (a folder other than roms/, see
 * ui_host_extra_roots in vita/ui_host_hooks.h) is named by its whole path,
 * "ux0:ROMS/GBA/Advance Wars (U).gba", where an entry from roms/ is named
 * relative to it.  The two never meet: FAT forbids ':' in a file name, so a
 * ':' in the first path component (the device, "ux0:") can only be a path
 * that carries its own device, and a relative name found by a scan never has
 * one.
 *
 * On the Linux twin (PSPCOMPAT_PLAT) a leading '/' is a whole path as well,
 * so a test can name an extra folder by its host path.  That cannot take a
 * roms/ entry for one: a scan builds those from directory entries, and no
 * directory entry begins with '/'.  The console's build does not have this
 * rule: there a '/' name is simply relative, as it always was. */
static inline int psp_rom_name_is_absolute(const char *name)
{
   const char *p;
   if (!name)
      return 0;
#ifdef PSPCOMPAT_PLAT
   if (name[0] == '/')
      return 1;
#endif
   for (p = name; *p && *p != '/'; p++)
      if (*p == ':')
         return 1;
   return 0;
}
#endif

static inline int psp_rom_path_join(char *out, size_t out_sz,
                                    const char *root, const char *relative)
{
   int n;
   if (!out || !out_sz || !root || !relative)
      return -1;
#ifdef VITA
   /* an extra folder's game: its name is already the whole path */
   if (psp_rom_name_is_absolute(relative))
      n = snprintf(out, out_sz, "%s", relative);
   else
#endif
   n = snprintf(out, out_sz, "%s/%s", root, relative);
   if (n < 0 || (size_t)n >= out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

/* Replace the final extension with suffix. Source and destination may alias.
 * This is the plain form: the file is named beside `path`. */
static inline int psp_rom_path_suffix_plain(char *out, size_t out_sz,
                                            const char *path,
                                            const char *suffix)
{
   size_t n, suffix_len;
   const char *dot;
   if (!out || !out_sz || !path || !suffix)
      return -1;
   n = strlen(path);
   dot = strrchr(path, '.');
   if (dot)
      n = (size_t)(dot - path);
   suffix_len = strlen(suffix);
   if (n + suffix_len + 1 > out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   if (out != path)
      memmove(out, path, n);
   memcpy(out + n, suffix, suffix_len + 1);
   return 0;
}

/* CENTRAL SAVES (Vita).  When psp_saves_root is set ("ux0:saves"), a path
 * built for a SAVE file -- a battery (.sav .srm), a state (.st0 .. .st9) or
 * the resume state (.rsm) -- lands in <root>/<CONSOLE>/<game name><suffix>
 * instead of beside the ROM.  CONSOLE is read from the suffix, which always
 * carries it (.gb.sav, .md.st0, ...; no console tag means GBA).  A path that
 * is already under the root only has its suffix replaced, so a slot path
 * derived from the slot-1 path stays where slot 1 is.  Patches, art and
 * every other sidecar keep following the ROM.  NULL = off (the old layout).
 * A weak definition: every translation unit sees the one variable, and a
 * test that never sets it keeps the old behaviour. */
const char *psp_saves_root __attribute__((weak));

static inline int psp_suffix_ends(const char *s, const char *tail)
{
   size_t n = strlen(s), t = strlen(tail);
   return n >= t && !strcmp(s + n - t, tail);
}

static inline int psp_suffix_is_save(const char *s)
{
   size_t n = strlen(s);
   if (psp_suffix_ends(s, ".sav") || psp_suffix_ends(s, ".srm") ||
       psp_suffix_ends(s, ".rsm"))
      return 1;
   return n >= 4 && s[n - 4] == '.' && s[n - 3] == 's' && s[n - 2] == 't' &&
          s[n - 1] >= '0' && s[n - 1] <= '9';
}

static inline const char *psp_saves_console_dir(const char *s)
{
   if (!strncmp(s, ".gbc.", 5))  return "GBC";
   if (!strncmp(s, ".gb.", 4))   return "GB";
   if (!strncmp(s, ".md.", 4))   return "MD";
   if (!strncmp(s, ".nes.", 5))  return "NES";
   if (!strncmp(s, ".snes.", 6)) return "SNES";
   return "GBA";
}

static inline int psp_rom_path_suffix(char *out, size_t out_sz,
                                      const char *path, const char *suffix)
{
   if (out && out_sz && path && suffix && psp_saves_root &&
       psp_saves_root[0] && psp_suffix_is_save(suffix))
   {
      size_t rl = strlen(psp_saves_root);
      if (strncmp(path, psp_saves_root, rl) != 0 || path[rl] != '/')
      {
         char tmp[PSP_FILE_PATH_CAP + 16];
         const char *slash = strrchr(path, '/');
         const char *base = slash ? slash + 1 : path;
         const char *dot = strrchr(base, '.');
         size_t stem = dot ? (size_t)(dot - base) : strlen(base);
         int n = snprintf(tmp, sizeof(tmp), "%s/%s/%.*s%s", psp_saves_root,
                          psp_saves_console_dir(suffix), (int)stem, base,
                          suffix);
         if (n < 0 || (size_t)n >= sizeof(tmp) || (size_t)n + 1 > out_sz)
         {
            out[0] = '\0';
            return -1;
         }
         memcpy(out, tmp, (size_t)n + 1);
         return 0;
      }
   }
   return psp_rom_path_suffix_plain(out, out_sz, path, suffix);
}

#endif
