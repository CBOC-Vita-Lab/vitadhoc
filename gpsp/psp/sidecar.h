/* Where VitAdhoc keeps everything it makes for a game other than the game's
 * battery save (VitAdhoc 1.3).
 *
 * The ROM's folder holds the ROM and its battery save (Game.sav, Game.gb.sav,
 * Game.gbc.sav) and nothing else.  The rest sits under the app's data folder
 * (ux0:data/GBAdhoc on the Vita), the way RetroArch keeps it:
 *
 *   states/<gba|gb|gbc>/<name>.st0 .. .st9      save states
 *   states/<console>/<name>.stN.thumb / .shot   their pictures
 *   states/<console>/<name>.stN.p2              a link game's second console
 *   states/<console>/<name>.rsm                 Continue (where it was left)
 *   states/<console>/<name>.rsm.pt              the patch that game had
 *   cache/boxart/<stem>.t2.565                  home-screen tile pictures
 *
 * <name> is the ROM's file name without its extension, wherever the ROM is,
 * the way RetroArch names its states: moving a game to another folder keeps
 * its states, and launchers find everything by the game's own name.  (Two
 * different games with the same file name for the same console in two
 * folders would share states -- as in RetroArch.)
 *
 * Older versions kept all of these beside the ROM.  sidecar_migrate() moves
 * what is there, once per file, never over a file already in the new place.
 * Every function here is plain C with no logging, so a worker may call it. */
#ifndef PSP_SIDECAR_H
#define PSP_SIDECAR_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "state_slots.h"

/* The folder name the states of a console live in. */
static inline const char *sidecar_console_dir(fe_console_t console)
{
   return console == FE_CONSOLE_GBC ? "gbc" :
          console == FE_CONSOLE_GB  ? "gb"  : "gba";
}

/* The cover thumbnails (artfetch.h ART_THUMB_SUFFIX) live here, below the app
 * folder; the art folders themselves keep only the player's pictures. */
#define SIDECAR_THUMB_DIR "cache/boxart"
/* Where the lazily decoded .v565 / .vh565 copies of a picture go. */
#define SIDECAR_ART_CACHE "cache"

static inline void sidecar_make_dirs(const char *root)
{
   static const char *const sub[] = { "states", "states/gba", "states/gb",
                                      "states/gbc", "cache", "cache/boxart",
                                      "cache/titles", "cache/hero" };
   char p[PSP_FILE_PATH_CAP];
   unsigned i;
   for (i = 0; i < sizeof(sub) / sizeof(sub[0]); i++)
      if (snprintf(p, sizeof(p), "%s/%s", root, sub[i]) < (int)sizeof(p))
         mkdir(p, 0777);          /* already there is the normal case */
}

/* FNV-1a of the folder's path, 24 bits. */
static inline unsigned sidecar_dir_hash(const char *s, size_t n)
{
   uint32_t h = 2166136261u;
   size_t i;
   for (i = 0; i < n; i++)
      h = (h ^ (unsigned char)s[i]) * 16777619u;
   return (h ^ (h >> 24)) & 0xFFFFFFu;
}

/* The slot-1 state path of `rom_path` booted as `console` (the base every
 * psp_state_path_for_slot / psp_state_resume_path call derives from).
 * `root` is the app folder.  0 = ok, -1 = too long. */
static inline int sidecar_state1(char *out, size_t out_sz, const char *root,
                                 const char *rom_path, fe_console_t console)
{
   const char *slash, *name, *dot;
   char rooms[PSP_FILE_PATH_CAP];
   size_t dirlen, stem;
   int n;
   if (!out || !out_sz || !root || !rom_path)
      return -1;
   slash = strrchr(rom_path, '/');
   if (!slash)
      slash = strrchr(rom_path, ':');
   name = slash ? slash + 1 : rom_path;
   dirlen = slash ? (size_t)(slash - rom_path) : 0;
   dot = strrchr(name, '.');
   stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
   if (!stem)
      return -1;
   (void)dirlen; (void)rooms;
   n = snprintf(out, out_sz, "%s/states/%s/%.*s%s", root,
                sidecar_console_dir(console), (int)stem, name,
                psp_state_slot1_suffix(console));
   if (n < 0 || (size_t)n >= out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

/* A game name with the "~<6 hex>" of sidecar_state1 cut off its end. */
static inline void sidecar_strip_hash(char *name)
{
   size_t n = strlen(name), i;
   if (n < 8 || name[n - 7] != '~')
      return;
   for (i = n - 6; i < n; i++)
      if (!((name[i] >= '0' && name[i] <= '9') ||
            (name[i] >= 'a' && name[i] <= 'f')))
         return;
   name[n - 7] = '\0';
}

/* The slot-1 path older versions used: beside the ROM. */
static inline int sidecar_legacy_state1(char *out, size_t out_sz,
                                        const char *rom_path,
                                        fe_console_t console)
{
   return psp_rom_path_suffix(out, out_sz, rom_path,
                              psp_state_slot1_suffix(console));
}

/* ---- moving what older versions left beside the ROM ------------------------ */

typedef struct sidecar_mig
{
   unsigned moved;      /* renamed */
   unsigned copied;     /* copied, checked, the old one removed */
   unsigned failed;     /* still beside the ROM: read from there */
   unsigned kept;       /* both exist: neither is touched */
} sidecar_mig;

static inline int sidecar_exists(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0;
}

/* Copy `from` to `to` (a temp name, then renamed), reading both back to
 * check.  0 = `to` is a complete copy. */
static inline int sidecar_copy_verified(const char *from, const char *to)
{
   char tmp[PSP_FILE_PATH_CAP + 8], a[4096], b[4096];
   FILE *s = NULL, *d = NULL;
   size_t n;
   int ok = 1;
   if (snprintf(tmp, sizeof(tmp), "%s.mv", to) >= (int)sizeof(tmp))
      return -1;
   s = fopen(from, "rb");
   d = s ? fopen(tmp, "wb") : NULL;
   if (!s || !d)
      ok = 0;
   while (ok && (n = fread(a, 1, sizeof(a), s)) > 0)
      if (fwrite(a, 1, n, d) != n)
         ok = 0;
   if (s)
      fclose(s);
   if (d && fclose(d) != 0)
      ok = 0;
   if (ok)
   {
      s = fopen(from, "rb");
      d = fopen(tmp, "rb");
      if (!s || !d)
         ok = 0;
      while (ok && (n = fread(a, 1, sizeof(a), s)) > 0)
         if (fread(b, 1, n, d) != n || memcmp(a, b, n))
            ok = 0;
      if (ok && fread(b, 1, 1, d) != 0)
         ok = 0;
      if (s)
         fclose(s);
      if (d)
         fclose(d);
   }
   if (ok && rename(tmp, to) != 0)
      ok = 0;
   if (!ok)
      remove(tmp);
   return ok ? 0 : -1;
}

/* One file.  Nothing there, or the new one already there: nothing is done
 * (the new file is never overwritten, the old one never removed unread). */
static inline void sidecar_move_file(const char *from, const char *to,
                                     sidecar_mig *m)
{
   if (!sidecar_exists(from))
      return;
   if (sidecar_exists(to))
   {
      m->kept++;
      return;
   }
   if (rename(from, to) == 0)
   {
      m->moved++;
      return;
   }
   if (sidecar_copy_verified(from, to) == 0 && remove(from) == 0)
      m->copied++;
   else
      m->failed++;                /* the old one stays and is read from */
}

/* Everything older versions made for one game and console: all ten slots with
 * their pictures, the link pair file and the old-state copy, and Continue
 * with its patch note.  `old1` / `new1` are the two slot-1 paths. */
static inline void sidecar_migrate(const char *old1, const char *new1,
                                   sidecar_mig *m)
{
   static const char *const tail[] = { "", ".thumb", ".shot", ".p2", ".old" };
   static const char *const rtail[] = { "", ".pt", ".old" };
   char a[PSP_FILE_PATH_CAP + 16], b[PSP_FILE_PATH_CAP + 16];
   char sa[PSP_FILE_PATH_CAP], sb[PSP_FILE_PATH_CAP];
   unsigned slot, t;
   memset(m, 0, sizeof(*m));
   if (!old1 || !new1 || !strcmp(old1, new1))
      return;
   for (slot = 1; slot <= PSP_STATE_SLOT_COUNT; slot++)
   {
      if (psp_state_path_for_slot(sa, sizeof(sa), old1, slot) != 0 ||
          psp_state_path_for_slot(sb, sizeof(sb), new1, slot) != 0)
         continue;
      for (t = 0; t < sizeof(tail) / sizeof(tail[0]); t++)
         if (snprintf(a, sizeof(a), "%s%s", sa, tail[t]) < (int)sizeof(a) &&
             snprintf(b, sizeof(b), "%s%s", sb, tail[t]) < (int)sizeof(b))
            sidecar_move_file(a, b, m);
   }
   if (psp_state_resume_path(sa, sizeof(sa), old1) == 0 &&
       psp_state_resume_path(sb, sizeof(sb), new1) == 0)
      for (t = 0; t < sizeof(rtail) / sizeof(rtail[0]); t++)
         if (snprintf(a, sizeof(a), "%s%s", sa, rtail[t]) < (int)sizeof(a) &&
             snprintf(b, sizeof(b), "%s%s", sb, rtail[t]) < (int)sizeof(b))
            sidecar_move_file(a, b, m);
}

/* The path to READ for `want`: itself, or -- when it is not there and a
 * migration could not move it -- the same file beside the ROM.  `old1` /
 * `new1` as above; `want` is `new1` with its last extension replaced. */
static inline const char *sidecar_read_path(const char *want, const char *old1,
                                            const char *new1, char *buf,
                                            size_t buf_sz)
{
   const char *sx, *ox;
   size_t nl, ol;
   if (!old1 || !old1[0] || !new1 || sidecar_exists(want))
      return want;
   sx = strrchr(new1, '.');
   ox = strrchr(old1, '.');
   if (!sx || !ox)
      return want;
   nl = (size_t)(sx - new1);
   ol = (size_t)(ox - old1);
   if (strncmp(want, new1, nl) ||
       snprintf(buf, buf_sz, "%.*s%s", (int)ol, old1, want + nl) >= (int)buf_sz)
      return want;
   return sidecar_exists(buf) ? buf : want;
}

#endif
