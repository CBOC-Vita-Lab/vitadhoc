/* fe_linkmode.h -- Settings > Link mode: the per-game override of the link
 * table (link_games.h), stored in roms/linkmode.txt beside favourites.txt.
 *
 * PER GAME, BY ROM FILE.  The override exists for hacks.  Hacks keep the
 * base game's header code (Unbound is BPEE, Radical Red BPRE), so a key by
 * game code would also hit retail Emerald, and one global switch would
 * silently change every other game's hosting.  The ROM's name (relative to
 * roms/, the key favourites.txt and last_rom use) names the hack and nothing
 * else.  Renaming the ROM loses the override, as it loses the star.
 *
 *     # GBAdhoc link mode, one game per line
 *     Pokemon Unbound.gba = cable
 *     hacks/Radical Red.gba = adapter
 *
 * Absent = auto (the table decides).  Join ignores the override entirely:
 * Join follows the host's room.
 *
 * STATIC, like fe_favs: the heap is gone once a game has loaded.  ~13 KB the
 * caller places in BSS.  Pure C, no platform calls; tools/test_link_mode.c
 * exercises it. */
#ifndef FE_LINKMODE_H
#define FE_LINKMODE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_LINKMODE_MAX   128
#define FE_LINKMODE_NAME  100

enum { FE_LM_AUTO = 0, FE_LM_CABLE = 1, FE_LM_ADAPTER = 2 };

typedef struct
{
   unsigned n;
   struct
   {
      unsigned hash;
      unsigned char mode;                 /* FE_LM_CABLE / FE_LM_ADAPTER */
      char name[FE_LINKMODE_NAME];
   } e[FE_LINKMODE_MAX];
} fe_linkmode;

void fe_linkmode_clear(fe_linkmode *m);

/* Parse the file's text: "<name> = cable|adapter" per line (split at the
 * LAST '='), '#' comments, CR and surrounding blanks dropped; unknown values,
 * over-long names and entries past FE_LINKMODE_MAX are ignored; a repeated
 * name keeps its last value.  Returns the entries kept. */
unsigned fe_linkmode_parse(fe_linkmode *m, const char *text, size_t len);

/* FE_LM_AUTO when absent. */
int fe_linkmode_get(const fe_linkmode *m, const char *name);

/* FE_LM_AUTO removes the entry.  0 = done (or nothing to do), -1 = the
 * table is full or the name is empty / too long. */
int fe_linkmode_set(fe_linkmode *m, const char *name, int mode);

/* The file's text.  Returns the bytes the whole text needs; writes at most
 * cap-1 and NUL-terminates when cap > 0. */
size_t fe_linkmode_serialise(const fe_linkmode *m, char *out, size_t cap);

/* The Host row's mode for a game: the table's (LINK_CABLE / LINK_ADAPTER /
 * LINK_BOTH) unless the override names one. */
int fe_linkmode_effective(int table_mode, int override_mode);

#ifdef __cplusplus
}
#endif

#endif /* FE_LINKMODE_H */
