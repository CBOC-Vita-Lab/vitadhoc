/* play_log.h -- what has been played, when, and for how long.
 *
 * <data>/playlog.txt, one line per game ever played:
 *
 *     <last played>\t<seconds played>\t<times started>\t<name>\n
 *
 * <last played> is the console's local clock (plat_wallclock: seconds since
 * 1970 as the Vita's own clock shows them), <name> the game list's name for
 * the game (rom_entry.name: the path under roms/, or the whole path of a
 * game in an extra folder) -- the key favourites.txt and last_rom use.
 * A line that does not read as one is dropped.
 *
 * The game loop counts a session (play_log_add) when a game starts and adds
 * the time actually played -- not menus, not a suspended console -- as it
 * goes; the game list stamps every entry from the table (play_log_get) for
 * its Recent tab, its orders and the game page.
 *
 * STATIC, no heap, like the game list: the heap is gone once a game runs.
 * Written as favourites.txt is (tmp, then .old aside, then renamed in; the
 * .old is put back at load if a power cut fell between). */
#ifndef PLAY_LOG_H
#define PLAY_LOG_H

#include <stdint.h>
#include <stddef.h>

#define PLAY_LOG_MAX   2048   /* the game list's own cap; full, the game
                               * played longest ago makes room */
#define PLAY_LOG_POOL  (128 * 1024)

typedef struct
{
   uint32_t last;       /* local seconds since 1970; 0 = never */
   uint32_t secs;       /* time played */
   uint32_t count;      /* times started */
} play_rec;

/* Read `path` (and remember it for play_log_save).  0 = read or absent;
 * -1 = unreadable (the table is empty, and saving is refused so a list that
 * could not be read is never written over). */
int  play_log_load(const char *path);
/* 0 = written (or nothing changed); -1 = not written. */
int  play_log_save(void);
/* The record of `name`, or NULL. */
const play_rec *play_log_get(const char *name);
/* A session of `name` began (`started` = 1: one more start, last played
 * = now) and/or `secs` more were played.  0 = recorded; -1 = the table is
 * full or the name does not fit. */
int  play_log_add(const char *name, uint32_t now, uint32_t secs, int started);
/* Forget everything (tests). */
void play_log_reset(void);
unsigned play_log_size(void);
/* Bumped by every change: a list that stamped itself at generation g is
 * still right while this returns g. */
uint32_t play_log_generation(void);

/* "today", "yesterday", "3 days ago", "in March" -- when `last` was, seen
 * from `now`; and "2 h 15 min" / "45 min" / "under a minute" for `secs`. */
void play_log_when(uint32_t last, uint32_t now, char *out, size_t cap);
void play_log_duration(uint32_t secs, char *out, size_t cap);

#endif
