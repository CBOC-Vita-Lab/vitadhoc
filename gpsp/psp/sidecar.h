/* Where VitAdhoc keeps everything it makes for a game other than the game's
 * battery save (VitAdhoc 1.3).
 *
 * The ROM's folder holds the ROM and its battery save (Game.sav, Game.gb.sav,
 * Game.gbc.sav) and nothing else.  The rest sits under the app's data folder
 * (ux0:data/GBAdhoc on the Vita), the way RetroArch keeps it:
 *
 *   states/<console>/<xx>/<name>.st0 .. .st9    save states
 *   states/<console>/<xx>/<name>.stN.thumb / .shot   their pictures
 *   states/<console>/<xx>/<name>.stN.p2         a link game's second console
 *   states/<console>/<xx>/<name>.rsm            Continue (where it was left)
 *   states/<console>/<xx>/<name>.rsm.pt         the patch that game had
 *   states/<console>/<xx>/<name>.<ext>st0.mig   this game was migrated
 *
 * <xx> (VitAdhoc 1.4, the layout the SNES player's playerkit uses too) is the
 * first two hex digits of the CRC-32 of <name>: 256 small folders instead of
 * one with thousands of files, which the Vita's card is slow to search.
 * Games are moved there lazily, the first time the app looks at them
 * (sidecar_migrate_game: from beside the ROM, 1.2, and from the flat
 * states/<console>/ folder, 1.3), once (the .mig marker); never over a file
 * already there, never losing one (a file that cannot be moved stays and is
 * counted as failed, and the marker is not written). 
 *   cache/boxart/<stem>.t2.565                  home-screen tile pictures
 * (the list above is the 1.4 one)
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
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#if defined(VITA) || defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <errno.h>
#endif
#include "state_slots.h"

/* The folder name the states of a console live in. */
static inline const char *sidecar_console_dir(fe_console_t console)
{
   return console == FE_CONSOLE_GBC ? "gbc" :
          console == FE_CONSOLE_GB  ? "gb"  :
          console == FE_CONSOLE_NES ? "nes" :
          console == FE_CONSOLE_WSWAN ? "ws" :
          console == FE_CONSOLE_SNES ? "snes" :          /* snes.bin's (pk_states) */
          console == FE_CONSOLE_MD ? "md" :              /* md.bin's (pk_states) */
          console == FE_CONSOLE_SMS ? "sms" :
          console == FE_CONSOLE_GG ? "gg" :
          console == FE_CONSOLE_SG ? "sg" :
          console == FE_CONSOLE_SCD ? "scd" : "gba";
}

/* The cover thumbnails (artfetch.h ART_THUMB_SUFFIX) live here, below the app
 * folder; the art folders themselves keep only the player's pictures. */
#define SIDECAR_THUMB_DIR "cache/boxart"
/* Where the lazily decoded .v565 / .vh565 copies of a picture go. */
#define SIDECAR_ART_CACHE "cache"

static inline void sidecar_make_dirs(const char *root)
{
   static const char *const sub[] = { "states", "states/gba", "states/gb",
                                      "states/gbc", "states/nes", "states/ws", "states/snes", "states/md", "states/sms",
                                      "states/gg", "states/sg", "states/scd", "cache", "cache/boxart",
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

/* CRC-32 (the zlib one) of a name: its first byte, two hex digits, is the
 * game's state folder. */
static inline uint32_t sidecar_crc32(const char *s, size_t n)
{
   uint32_t c = 0xFFFFFFFFu;
   size_t i;
   int k;
   for (i = 0; i < n; i++)
   {
      c ^= (unsigned char)s[i];
      for (k = 0; k < 8; k++)
         c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
   }
   return c ^ 0xFFFFFFFFu;
}

/* A Game Boy game's saves and states belong to its FILE, not to the
 * hardware it runs on: a ".gb" file the header calls a Color game (gbkind.h)
 * keeps the "gb" folder it always had, so deciding by the header cannot
 * strand an old save state.  Other consoles, and names with another
 * extension, are as given. */
static inline fe_console_t sidecar_file_console(const char *rom_path, fe_console_t console)
{
   size_t n = rom_path ? strlen(rom_path) : 0;
   if (console != FE_CONSOLE_GB && console != FE_CONSOLE_GBC)
      return console;
   if (n > 4 && !strcasecmp(rom_path + n - 4, ".gbc"))
      return FE_CONSOLE_GBC;
   if (n > 3 && !strcasecmp(rom_path + n - 3, ".gb"))
      return FE_CONSOLE_GB;
   return console;
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
   console = sidecar_file_console(rom_path, console);
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
   n = snprintf(out, out_sz, "%s/states/%s/%02x/%.*s%s", root,
                sidecar_console_dir(console),
                (unsigned)(sidecar_crc32(name, stem) >> 24), (int)stem, name,
                psp_state_slot1_suffix(console));
   if (n < 0 || (size_t)n >= out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

/* The slot-1 path of the 1.3 layout: states/<console>/<name>.st0 (no
 * bucket), read only to move what is there. */
static inline int sidecar_flat_of(char *out, size_t out_sz, const char *b)
{
   const char *slash;
   size_t r;
   if (!b)
      return -1;
   slash = strrchr(b, '/');                 /* .../states/<c>/<xx>/<file> */
   r = slash ? (size_t)(slash - b) : 0;
   if (r < 3 || b[r - 3] != '/')
      return -1;
   if (snprintf(out, out_sz, "%.*s%s", (int)(r - 3), b, slash) >= (int)out_sz)
      return -1;
   return 0;
}

static inline int sidecar_flat_state1(char *out, size_t out_sz, const char *root,
                                      const char *rom_path, fe_console_t console)
{
   char b[PSP_FILE_PATH_CAP];
   if (sidecar_state1(b, sizeof(b), root, rom_path, console) != 0)
      return -1;
   return sidecar_flat_of(out, out_sz, b);
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
   console = sidecar_file_console(rom_path, console);
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
   {
      /* the bucket folder is made the first time something goes in it */
      char d[PSP_FILE_PATH_CAP];
      const char *sl = strrchr(to, '/');
      if (sl && (size_t)(sl - to) < sizeof(d))
      {
         memcpy(d, to, (size_t)(sl - to));
         d[sl - to] = '\0';
         mkdir(d, 0777);
         if (rename(from, to) == 0)
         {
            m->moved++;
            return;
         }
      }
   }
   if (sidecar_copy_verified(from, to) == 0 && remove(from) == 0)
      m->copied++;
   else
      m->failed++;                /* the old one stays and is read from */
}

#if defined(VITA) || defined(__unix__) || defined(__APPLE__)
/* Map a legacy sidecar suffix starting at '.' to its bit index in [0, 52]:
 *   0..49  : .st<0..9> + { "", ".thumb", ".shot", ".p2", ".old" }
 *   50..52 : .rsm      + { "", ".pt", ".old" }
 * Returns -1 for anything else. */
static inline int sidecar_suffix_idx(const char *s)
{
   if (!s || s[0] != '.')
      return -1;
   if ((s[1] | 0x20) == 's' && (s[2] | 0x20) == 't' &&
       s[3] >= '0' && s[3] <= '9')
   {
      int slot0 = s[3] - '0';
      const char *rest = s + 4;
      int t = -1;
      if (!rest[0])                       t = 0;
      else if (!strcasecmp(rest, ".thumb")) t = 1;
      else if (!strcasecmp(rest, ".shot"))  t = 2;
      else if (!strcasecmp(rest, ".p2"))    t = 3;
      else if (!strcasecmp(rest, ".old"))   t = 4;
      return t >= 0 ? slot0 * 5 + t : -1;
   }
   if ((s[1] | 0x20) == 'r' && (s[2] | 0x20) == 's' &&
       (s[3] | 0x20) == 'm')
   {
      const char *rest = s + 4;
      if (!rest[0])                    return 50;
      if (!strcasecmp(rest, ".pt"))    return 51;
      if (!strcasecmp(rest, ".old"))   return 52;
   }
   return -1;
}

/* Non-zero if `name` is any legacy .st0..st9 / .rsm sidecar file (for any
 * game), used to decide whether a scanned directory can be cached as clean. */
static inline int sidecar_name_is_state(const char *name)
{
   const char *d1 = strrchr(name, '.');
   if (!d1)
      return 0;
   if (sidecar_suffix_idx(d1) >= 0)
      return 1;
   if (d1 > name)
   {
      const char *p = d1 - 1;
      while (p > name && *p != '.')
         p--;
      if (*p == '.' && sidecar_suffix_idx(p) >= 0)
         return 1;
   }
   return 0;
}
#endif

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
   uint64_t match_mask = ~0ull;
   memset(m, 0, sizeof(*m));
   if (!old1 || !new1 || !strcmp(old1, new1))
      return;
#if defined(VITA) || defined(__unix__) || defined(__APPLE__)
   /* On Vita FAT32, 53 blind stat() calls in a 1000-file ROM folder stall for
    * 10-45 s per new game.  Scan old_dir once with opendir()/readdir() and
    * remember up to 16 directories that contain zero legacy sidecar files so
    * subsequent games in those folders issue 0 syscalls. */
   {
      enum { SIDECAR_CLEAN_CAP = 16 };
      static struct
      {
         unsigned hash;
         size_t   len;
         time_t   mtime;
         off_t    size;
         char     dir[PSP_FILE_PATH_CAP];
      } clean[SIDECAR_CLEAN_CAP];
      static unsigned clean_n, clean_next;

      const char *sl = strrchr(old1, '/');
      const char *fname, *dot;
      char old_dir[PSP_FILE_PATH_CAP];
      size_t dir_len = 0, stem_len = 0;

      if (!sl)
         sl = strrchr(old1, ':');
      if (sl)
      {
         if (*sl == ':')
            dir_len = (size_t)(sl - old1) + 1;
         else if (sl == old1)
            dir_len = 1;
         else
            dir_len = (size_t)(sl - old1);
         fname = sl + 1;
      }
      else
      {
         old_dir[0] = '.';
         old_dir[1] = '\0';
         dir_len = 1;
         fname = old1;
      }
      dot = strrchr(fname, '.');
      if (dot && dot > fname && dir_len < sizeof(old_dir))
      {
         unsigned h, i;
         struct stat dst;
         int have_dst;
         DIR *d;
         if (sl)
         {
            memcpy(old_dir, old1, dir_len);
            old_dir[dir_len] = '\0';
         }
         stem_len = (size_t)(dot - fname);
         h = sidecar_dir_hash(old_dir, dir_len);
         have_dst = stat(old_dir, &dst) == 0;
         if (!have_dst && (errno == ENOENT || errno == ENOTDIR))
            return;
         if (have_dst)
         {
            for (i = 0; i < clean_n; i++)
               if (clean[i].hash == h && clean[i].len == dir_len &&
                   clean[i].mtime == dst.st_mtime && clean[i].size == dst.st_size &&
                   !memcmp(clean[i].dir, old_dir, dir_len))
                  return;
         }
         d = opendir(old_dir);
         if (d)
         {
            struct dirent *ent;
            int dir_has_any = 0, rd_err = 0;
            uint64_t scanned_mask = 0ull;
            errno = 0;
            while ((ent = readdir(d)) != NULL)
            {
               const char *nm = ent->d_name;
               size_t nlen;
               if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))
                  continue;
               nlen = strlen(nm);
               if (nlen > stem_len && nm[stem_len] == '.' &&
                   !strncasecmp(nm, fname, stem_len))
               {
                  int bit = sidecar_suffix_idx(nm + stem_len);
                  if (bit >= 0)
                  {
                     scanned_mask |= (1ull << (unsigned)bit);
                     dir_has_any = 1;
                  }
               }
               if (!dir_has_any && sidecar_name_is_state(nm))
                  dir_has_any = 1;
               errno = 0;
            }
            rd_err = errno;
            closedir(d);
            if (!rd_err)
            {
               match_mask = scanned_mask;
               if (!dir_has_any && have_dst)
               {
                  unsigned dst_slot = clean_n;
                  for (i = 0; i < clean_n; i++)
                     if (clean[i].hash == h && clean[i].len == dir_len &&
                         !memcmp(clean[i].dir, old_dir, dir_len))
                     {
                        dst_slot = i;
                        break;
                     }
                  if (dst_slot == clean_n)
                  {
                     dst_slot = clean_next;
                     clean_next = (clean_next + 1u) % SIDECAR_CLEAN_CAP;
                     if (clean_n < SIDECAR_CLEAN_CAP)
                        clean_n++;
                  }
                  clean[dst_slot].hash = h;
                  clean[dst_slot].len = dir_len;
                  clean[dst_slot].mtime = dst.st_mtime;
                  clean[dst_slot].size = dst.st_size;
                  memcpy(clean[dst_slot].dir, old_dir, dir_len + 1);
               }
               if (!match_mask)
                  return;
            }
         }
         else if (errno == ENOENT || errno == ENOTDIR)
            return;
      }
   }
#endif
   for (slot = 1; slot <= PSP_STATE_SLOT_COUNT; slot++)
   {
      if ((match_mask & (0x1Full << ((slot - 1u) * 5u))) == 0)
         continue;
      if (psp_state_path_for_slot(sa, sizeof(sa), old1, slot) != 0 ||
          psp_state_path_for_slot(sb, sizeof(sb), new1, slot) != 0)
         continue;
      for (t = 0; t < sizeof(tail) / sizeof(tail[0]); t++)
         if ((match_mask & (1ull << ((slot - 1u) * 5u + t))) &&
             snprintf(a, sizeof(a), "%s%s", sa, tail[t]) < (int)sizeof(a) &&
             snprintf(b, sizeof(b), "%s%s", sb, tail[t]) < (int)sizeof(b))
            sidecar_move_file(a, b, m);
   }
   if ((match_mask & (0x7ull << 50u)) &&
       psp_state_resume_path(sa, sizeof(sa), old1) == 0 &&
       psp_state_resume_path(sb, sizeof(sb), new1) == 0)
      for (t = 0; t < sizeof(rtail) / sizeof(rtail[0]); t++)
         if ((match_mask & (1ull << (50u + t))) &&
             snprintf(a, sizeof(a), "%s%s", sa, rtail[t]) < (int)sizeof(a) &&
             snprintf(b, sizeof(b), "%s%s", sb, rtail[t]) < (int)sizeof(b))
            sidecar_move_file(a, b, m);
}

/* Everything of one game into its bucket (the 1.4 layout), once: from
 * beside the ROM (`legacy1`, 1.2) and from the flat 1.3 folder (`flat1`).
 * The marker <new1>.mig says it was done: one stat a game afterwards.
 * Returns 1 when the marker was there (nothing looked at). */
static inline int sidecar_migrate_game(const char *legacy1, const char *flat1,
                                       const char *new1, sidecar_mig *m)
{
   char mk[PSP_FILE_PATH_CAP + 8];
   sidecar_mig a;
   FILE *f;
   memset(m, 0, sizeof(*m));
   if (!new1 || snprintf(mk, sizeof(mk), "%s.mig", new1) >= (int)sizeof(mk))
      return 0;
   if (sidecar_exists(mk))
      return 1;
   if (legacy1 && legacy1[0])
   {
      sidecar_migrate(legacy1, new1, &a);
      m->moved += a.moved; m->copied += a.copied;
      m->failed += a.failed; m->kept += a.kept;
   }
   if (flat1 && flat1[0])
   {
      sidecar_migrate(flat1, new1, &a);
      m->moved += a.moved; m->copied += a.copied;
      m->failed += a.failed; m->kept += a.kept;
   }
   if (m->failed)
      return 0;                     /* looked at again next time */
   f = fopen(mk, "wb");
   if (!f)
   {
      char d[PSP_FILE_PATH_CAP];
      const char *sl = strrchr(mk, '/');
      if (sl && (size_t)(sl - mk) < sizeof(d))
      {
         memcpy(d, mk, (size_t)(sl - mk));
         d[sl - mk] = '\0';
         mkdir(d, 0777);
         f = fopen(mk, "wb");
      }
   }
   if (f)
      fclose(f);
   return 0;
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
