/* ui_vita_library.h -- the library's views beyond All, Starred and Recent:
 * the automatic ones (Most played, Not played yet), the player's own
 * collections (collections.txt, fe_collect.h) and RetroArch / Emu4Vita++
 * playlists (.lpl) shown as read-only collections.
 *
 * Included by psp/ui_psp.c under -DVITA, after the game table and the
 * Starred state: everything here is one more subset of the scan, kept in
 * g_view_idx like the Starred one, so the order, Find, the console switch
 * and the row drawing need nothing of their own.  g_browser_favs means "a
 * subset is shown" and g_view_kind says which; Recent keeps its own flag.
 *
 * NOTHING HERE READS A FILE PER FRAME.  collections.txt is read when the
 * list opens, the playlists once per run (and again at Refresh), and every
 * count and subset is built once per scan or per change (lib_mark,
 * favs_build_view), like the Starred tags.
 *
 * WHAT THE AUTOMATIC VIEWS RESTS ON.  Only what the app really knows: the
 * play log (play_log.h).  No bundled database has a player count (the game
 * database holds names, descriptions and checksums; the achievements data
 * holds achievements), so there is no "Two players" view: it would be a
 * guess. */
#ifndef UI_VITA_LIBRARY_H
#define UI_VITA_LIBRARY_H

#include "fe_collect.h"

enum { VK_ALL = 0, VK_STAR, VK_RECENT, VK_MOST, VK_UNPLAYED, VK_COLL, VK_LPL };

extern char g_dir_base[];

static int           g_view_kind;       /* VK_*                              */
static int           g_view_which;      /* the collection or playlist        */
static fe_coll       g_coll;            /* collections.txt                   */
static fe_lpl_set    g_lpl;             /* the playlists, hashes only        */
static int           g_coll_cnt[FE_COLL_MAX];   /* games each has in the scan */
static int           g_lpl_cnt[FE_LPL_MAX];     /* ...and each playlist      */
static int           g_lib_played;      /* games of the scan with a play     */
static fe_lpl_rom    g_lrom[BROWSER_MAX];       /* match keys of the scan    */
static int           g_lrom_n = -1;
static char          g_lib_rom_dir[PSP_FILE_PATH_CAP];
static int           g_lpl_read;        /* the playlists were read this run  */

static const char *const LIB_VIEW_ID[] = {
   "all", "starred", "recent", "most_played", "not_played", "collection",
   "playlist"
};

static void favs_build_view(int n_scan);       /* below, with the Starred list */

static int lib_coll_path(char *out, size_t cap)
{
   int n = snprintf(out, cap, "%s/collections.txt", g_dir_base);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* The tab's words for the views that are not a collection. */
static const char *lib_view_name(int kind, int which)
{
   switch (kind)
   {
   case VK_ALL:      return "Games";
   case VK_STAR:     return "Starred";
   case VK_RECENT:   return "Recent";
   case VK_MOST:     return "Most played";
   case VK_UNPLAYED: return "Not played yet";
   case VK_COLL:
      return which >= 0 && (unsigned)which < g_coll.ncoll ? g_coll.name[which]
                                                          : "";
   case VK_LPL:
      return which >= 0 && (unsigned)which < g_lpl.nlist ? g_lpl.list[which].name
                                                         : "";
   }
   return "";
}

/* ---- reading -------------------------------------------------------------- */

static void lib_load_collections(void)
{
   char p[PSP_FILE_PATH_CAP];
   int rc = -1;
   if (lib_coll_path(p, sizeof(p)) == 0)
      rc = fe_coll_load(&g_coll, p);
   else
      fe_coll_clear(&g_coll);
   fe_evt("collections_load n=%u games=%u rc=%d lossy=%d", g_coll.ncoll,
          g_coll.n, rc, g_coll.lossy);
}

static int lib_name_cmp(const void *a, const void *b)
{
   return strcasecmp((const char *)a, (const char *)b);
}

/* The folders RetroArch and Emu4Vita++ keep their .lpl files in. */
static int lib_playlist_dirs(char dirs[][PSP_FILE_PATH_CAP], int cap)
{
   int n = 0;
#ifdef __vita__
   static const char *const D[] = {
      "ux0:data/retroarch/playlists", "uma0:data/retroarch/playlists",
      "ux0:data/EMU4VITAPLUS/playlists", "uma0:data/EMU4VITAPLUS/playlists",
   };
   for (; n < (int)(sizeof(D) / sizeof(D[0])) && n < cap; n++)
      snprintf(dirs[n], PSP_FILE_PATH_CAP, "%s", D[n]);
#else
   /* the twin keeps them beside its own data, where its save import looks
    * for RetroArch's */
   if (n < cap)
      snprintf(dirs[n++], PSP_FILE_PATH_CAP, "%s/retroarch/playlists", g_dir_base);
   if (n < cap)
      snprintf(dirs[n++], PSP_FILE_PATH_CAP, "%s/EMU4VITAPLUS/playlists", g_dir_base);
#endif
   return n;
}

/* Every .lpl in those folders, once (a name already read is not read
 * again).  Never opened for writing. */
static void lib_playlists_read(void)
{
   static char dirs[4][PSP_FILE_PATH_CAP];
   static char names[24][64];
   int nd = lib_playlist_dirs(dirs, 4), d;
   fe_lpl_clear(&g_lpl);
   for (d = 0; d < nd; d++)
   {
      SceUID fd = sceIoDopen(dirs[d]);
      SceIoDirent ent;
      int nn = 0, i;
      if (fd < 0)
         continue;
      memset(&ent, 0, sizeof(ent));
      while (nn < 24 && sceIoDread(fd, &ent) > 0)
      {
         size_t nl = strlen(ent.d_name);
         int is_dir = (ent.d_stat.st_mode & 0x1000) != 0 ||
                      (ent.d_stat.st_attr & 0x0010) != 0;
         if (!is_dir && nl > 4 && nl < sizeof(names[0]) &&
             !strcasecmp(ent.d_name + nl - 4, ".lpl"))
            snprintf(names[nn++], sizeof(names[0]), "%s", ent.d_name);
         memset(&ent, 0, sizeof(ent));
      }
      sceIoDclose(fd);
      qsort(names, (size_t)nn, sizeof(names[0]), lib_name_cmp);
      for (i = 0; i < nn; i++)
      {
         char path[PSP_FILE_PATH_CAP], stem[64];
         unsigned k;
         int dup = 0, rc;
         snprintf(stem, sizeof(stem), "%s", names[i]);
         stem[strlen(stem) - 4] = '\0';
         for (k = 0; k < g_lpl.nlist; k++)
            if (!strcasecmp(g_lpl.list[k].name, stem))
               dup = 1;
         if (dup)
            continue;
         if (snprintf(path, sizeof(path), "%s/%s", dirs[d], names[i]) >=
             (int)sizeof(path))
            continue;
         rc = fe_lpl_load(&g_lpl, path, stem);
         fe_evt("playlist_load file=%s items=%d", names[i], rc);
      }
   }
   g_lpl_read = 1;
   fe_evt("playlists_read n=%u items=%u", g_lpl.nlist, g_lpl.nitem);
}

static void lib_load(const char *rom_dir)
{
   snprintf(g_lib_rom_dir, sizeof(g_lib_rom_dir), "%s", rom_dir);
   lib_load_collections();
   if (!g_lpl_read)
      lib_playlists_read();
}

/* ---- matching playlists to the scan -------------------------------------- */

/* The bundled No-Intro table's name for a checksum, hashed the way the
 * matcher hashes a game's file name (lower case, no extension): the game's
 * file is never read. */
static unsigned lib_crc_name(unsigned crc, void *ctx)
{
   char name[160];
   (void)ctx;
   if (!ui_host_name_by_crc(crc, name, sizeof(name)))
      return 0;
   return fe_lpl_hash_name(name);
}

static void lib_keys(int n_scan)
{
   int i;
   char full[PSP_FILE_PATH_CAP];
   if (g_lrom_n == n_scan)
      return;
   for (i = 0; i < n_scan && i < BROWSER_MAX; i++)
   {
      if (psp_rom_path_join(full, sizeof(full), g_lib_rom_dir,
                            g_roms[i].name) != 0)
         snprintf(full, sizeof(full), "%s", g_roms[i].name);
      fe_lpl_rom_key(full, &g_lrom[i]);
   }
   g_lrom_n = n_scan;
}

/* After every scan (favs_mark): the counts the tabs are offered by. */
static void lib_mark(int n_scan)
{
   static unsigned h[BROWSER_MAX];
   int i, k;
   g_lrom_n = -1;
   g_lib_played = 0;
   for (i = 0; i < n_scan; i++)
      g_lib_played += g_roms[i].play_last != 0;
   for (k = 0; k < FE_COLL_MAX; k++)
      g_coll_cnt[k] = 0;
   for (k = 0; k < FE_LPL_MAX; k++)
      g_lpl_cnt[k] = 0;
   if (g_coll.n)
   {
      for (i = 0; i < n_scan; i++)
         h[i] = fe_favs_hash(g_roms[i].name);
      for (k = 0; k < (int)g_coll.ncoll; k++)
         if (g_coll.count[k])
            for (i = 0; i < n_scan; i++)
               g_coll_cnt[k] += fe_coll_has_h(&g_coll, k, g_roms[i].name, h[i]);
   }
   if (g_lpl.nlist && n_scan > 0)
   {
      lib_keys(n_scan);
      for (k = 0; k < (int)g_lpl.nlist; k++)
         g_lpl_cnt[k] = (int)fe_lpl_match(&g_lpl, k, g_lrom, n_scan, NULL, 0,
                                          lib_crc_name, NULL);
   }
   /* a view whose collection is gone: back to all games */
   if ((g_view_kind == VK_COLL && g_view_which >= (int)g_coll.ncoll) ||
       (g_view_kind == VK_LPL && g_view_which >= (int)g_lpl.nlist))
   {
      g_view_kind = VK_ALL;
      g_view_which = 0;
      g_browser_favs = 0;
   }
}

/* The subset of the scan the view shows, into g_view_idx. */
static void lib_view_build(int n_scan)
{
   int i;
   g_view_n = 0;
   switch (g_view_kind)
   {
   case VK_MOST:
      for (i = 0; i < n_scan; i++)
         if (g_roms[i].play_last)
            g_view_idx[g_view_n++] = (unsigned short)i;
      break;
   case VK_UNPLAYED:
      for (i = 0; i < n_scan; i++)
         if (!g_roms[i].play_last)
            g_view_idx[g_view_n++] = (unsigned short)i;
      break;
   case VK_COLL:
      for (i = 0; i < n_scan; i++)
         if (fe_coll_has(&g_coll, g_view_which, g_roms[i].name))
            g_view_idx[g_view_n++] = (unsigned short)i;
      break;
   case VK_LPL:
      lib_keys(n_scan);
      g_view_n = (int)fe_lpl_match(&g_lpl, g_view_which, g_lrom, n_scan,
                                   g_view_idx, BROWSER_MAX, lib_crc_name, NULL);
      if (g_view_n > BROWSER_MAX)
         g_view_n = BROWSER_MAX;
      break;
   default:
      for (i = 0; i < n_scan; i++)
         if (g_roms[i].is_fav)
            g_view_idx[g_view_n++] = (unsigned short)i;
      break;
   }
}

/* ---- the tabs ------------------------------------------------------------ */

typedef struct { signed char kind, which; } lib_tab;

/* The views SELECT steps through, in order: Games, Starred, Recent (the Home
 * only, once something was played), Most played, Not played yet (once
 * something was played and something was not), then every collection and
 * playlist that has a game on this console.  The view on screen is always
 * in the list, so a stepped-to empty one can be left again. */
static int lib_tabs(int n_scan, lib_tab *t, int cap)
{
   int n = 0, k;
#define LIB_ADD(kd, wh) \
   do { if (n < cap) { t[n].kind = (signed char)(kd); t[n].which = (signed char)(wh); n++; } } while (0)
   LIB_ADD(VK_ALL, 0);
   LIB_ADD(VK_STAR, 0);
   if (g_ui_home && g_recent_n)
      LIB_ADD(VK_RECENT, 0);
   if (g_lib_played > 0)
      LIB_ADD(VK_MOST, 0);
   if (g_lib_played > 0 && g_lib_played < n_scan)
      LIB_ADD(VK_UNPLAYED, 0);
   for (k = 0; k < (int)g_coll.ncoll; k++)
      if (g_coll_cnt[k] > 0 || (g_view_kind == VK_COLL && g_view_which == k))
         LIB_ADD(VK_COLL, k);
   for (k = 0; k < (int)g_lpl.nlist; k++)
      if (g_lpl_cnt[k] > 0 || (g_view_kind == VK_LPL && g_view_which == k))
         LIB_ADD(VK_LPL, k);
#undef LIB_ADD
   return n;
}

static int lib_tab_next(int n_scan, int *kind, int *which)
{
   lib_tab t[2 + 1 + 2 + FE_COLL_MAX + FE_LPL_MAX];
   int n = lib_tabs(n_scan, t, (int)(sizeof(t) / sizeof(t[0]))), i;
   for (i = 0; i < n; i++)
      if (t[i].kind == g_view_kind && (t[i].kind < VK_COLL ||
                                       t[i].which == g_view_which))
         break;
   if (i == n)
      i = -1;
   i = (i + 1) % n;
   *kind = t[i].kind;
   *which = t[i].which;
   return n;
}

/* The view the browser shows, and the log line the tests read. */
static void lib_view_set(int kind, int which, int n_scan)
{
   g_view_kind = kind;
   g_view_which = which;
   g_browser_favs = kind != VK_ALL && kind != VK_RECENT;
   g_browser_recent = kind == VK_RECENT;
   favs_build_view(n_scan);        /* the subset, then Find and the order  */
}

static void lib_view_evt(int shown)
{
   fe_evt("library view=%s name=\"%s\" shown=%d", LIB_VIEW_ID[g_view_kind],
          lib_view_name(g_view_kind, g_view_which), shown);
}

/* 1 when the shown subset is the Starred one (a star toggled there makes the
 * row leave it). */
#define LIB_STAR_VIEW() (g_browser_favs && g_view_kind == VK_STAR)

#endif /* UI_VITA_LIBRARY_H */
