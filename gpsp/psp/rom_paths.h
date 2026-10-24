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

/* Replace the final extension with suffix. Source and destination may alias. */
static inline int psp_rom_path_suffix(char *out, size_t out_sz,
                                      const char *path, const char *suffix)
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

#endif
