/* play_log.c -- play_log.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "play_log.h"
#include "fe_evt.h"

#define PL_SLOTS 4096                 /* hash slots: twice the records */

typedef struct
{
   play_rec rec;
   uint32_t hash;
   uint32_t off;                      /* name in the pool */
} pl_ent;

static struct
{
   pl_ent   e[PLAY_LOG_MAX];
   unsigned n;
   char     pool[PLAY_LOG_POOL];
   unsigned used;
   short    slot[PL_SLOTS];           /* entry + 1, 0 = empty */
   char     path[512];
   int      unread;                   /* load failed: never save over it */
   int      dirty;
   uint32_t gen;
} P;

static uint32_t pl_hash(const char *s)
{
   uint32_t h = 2166136261u;
   while (*s)
      h = (h ^ (unsigned char)*s++) * 16777619u;
   return h;
}

void play_log_reset(void)
{
   P.n = 0;
   P.used = 0;
   memset(P.slot, 0, sizeof(P.slot));
   P.dirty = 0;
   P.gen++;
}

unsigned play_log_size(void)
{
   return P.n;
}

uint32_t play_log_generation(void)
{
   return P.gen;
}

static pl_ent *pl_find(const char *name, uint32_t h, unsigned *slot_out)
{
   unsigned s = h & (PL_SLOTS - 1);
   for (;;)
   {
      int k = P.slot[s];
      if (!k)
      {
         if (slot_out)
            *slot_out = s;
         return NULL;
      }
      if (P.e[k - 1].hash == h && !strcmp(P.pool + P.e[k - 1].off, name))
         return &P.e[k - 1];
      s = (s + 1) & (PL_SLOTS - 1);
   }
}

/* Full: the game played longest ago makes room (its name leaves the pool,
 * the others move down, the hash table is built again). */
static void pl_evict_oldest(void)
{
   unsigned i, v = 0, w = 0, used = 0;
   if (!P.n)
      return;
   for (i = 1; i < P.n; i++)
      if (P.e[i].rec.last < P.e[v].rec.last)
         v = i;
   for (i = 0; i < P.n; i++)
   {
      unsigned len;
      if (i == v)
         continue;
      len = (unsigned)strlen(P.pool + P.e[i].off) + 1;
      memmove(P.pool + used, P.pool + P.e[i].off, len);   /* offsets rise */
      P.e[w] = P.e[i];
      P.e[w].off = used;
      used += len;
      w++;
   }
   P.n = w;
   P.used = used;
   memset(P.slot, 0, sizeof(P.slot));
   for (i = 0; i < P.n; i++)
   {
      unsigned s = P.e[i].hash & (PL_SLOTS - 1);
      while (P.slot[s])
         s = (s + 1) & (PL_SLOTS - 1);
      P.slot[s] = (short)(i + 1);
   }
}

static pl_ent *pl_insert(const char *name)
{
   uint32_t h = pl_hash(name);
   unsigned slot, len = (unsigned)strlen(name) + 1;
   pl_ent *e = pl_find(name, h, &slot);
   if (e)
      return e;
   if (len < 2 || len > 1024 || strpbrk(name, "\t\r\n"))
      return NULL;
   while (P.n && (P.n >= PLAY_LOG_MAX || P.used + len > PLAY_LOG_POOL))
   {
      pl_evict_oldest();
      (void)pl_find(name, h, &slot);    /* the table was built again */
   }
   if (P.n >= PLAY_LOG_MAX || P.used + len > PLAY_LOG_POOL)
      return NULL;
   e = &P.e[P.n];
   memset(e, 0, sizeof(*e));
   e->hash = h;
   e->off = P.used;
   memcpy(P.pool + P.used, name, len);
   P.used += len;
   P.slot[slot] = (short)(++P.n);
   return e;
}

const play_rec *play_log_get(const char *name)
{
   pl_ent *e = name ? pl_find(name, pl_hash(name), NULL) : NULL;
   return e ? &e->rec : NULL;
}

int play_log_add(const char *name, uint32_t now, uint32_t secs, int started)
{
   pl_ent *e = name ? pl_insert(name) : NULL;
   if (!e)
      return -1;
   if (started)
   {
      e->rec.count++;
      e->rec.last = now;
   }
   else if (secs && now > e->rec.last)
      e->rec.last = now;          /* still playing: the end of the session */
   e->rec.secs += secs;
   P.dirty = 1;
   P.gen++;
   return 0;
}

int play_log_load(const char *path)
{
   FILE *f;
   char line[700], old[530];
   play_log_reset();
   P.unread = 0;
   snprintf(P.path, sizeof(P.path), "%s", path ? path : "");
   if (!P.path[0])
      return -1;
   f = fopen(P.path, "rb");
   if (!f)
   {
      /* A save interrupted between "the list aside" and "the new one in":
       * the list aside is the list. */
      snprintf(old, sizeof(old), "%s.old", P.path);
      if (rename(old, P.path) == 0)
         f = fopen(P.path, "rb");
   }
   if (!f)
   {
      fe_evt("play_log_load n=0 absent=1");
      return 0;
   }
   while (fgets(line, sizeof(line), f))
   {
      char *t1 = strchr(line, '\t'), *t2, *t3, *nl, *end;
      unsigned long last, secs, count;
      pl_ent *e;
      if (!t1 || !(t2 = strchr(t1 + 1, '\t')) || !(t3 = strchr(t2 + 1, '\t')))
         continue;
      last = strtoul(line, &end, 10);
      if (end != t1) continue;
      secs = strtoul(t1 + 1, &end, 10);
      if (end != t2) continue;
      count = strtoul(t2 + 1, &end, 10);
      if (end != t3) continue;
      if ((nl = strpbrk(t3 + 1, "\r\n")) != NULL)
         *nl = 0;
      else if (!feof(f))
         continue;                /* a name longer than the line: dropped */
      if (!(e = pl_insert(t3 + 1)))
         continue;
      e->rec.last = (uint32_t)last;
      e->rec.secs = (uint32_t)secs;
      e->rec.count = (uint32_t)count;
   }
   if (ferror(f))
   {
      play_log_reset();
      P.unread = 1;
   }
   fclose(f);
   P.dirty = 0;
   fe_evt("play_log_load n=%u unread=%d", P.n, P.unread);
   return P.unread ? -1 : 0;
}

int play_log_save(void)
{
   char tmp[530], old[530];
   FILE *f;
   unsigned i;
   int rc = 0;
   if (!P.dirty)
      return 0;
   if (P.unread || !P.path[0])
   {
      fe_evt("play_log_save rc=-1 reason=%s", P.unread ? "unread" : "no_path");
      return -1;
   }
   snprintf(tmp, sizeof(tmp), "%s.tmp", P.path);
   snprintf(old, sizeof(old), "%s.old", P.path);
   f = fopen(tmp, "wb");
   if (!f)
      rc = -1;
   else
   {
      for (i = 0; i < P.n && rc == 0; i++)
         if (fprintf(f, "%u\t%u\t%u\t%s\n", (unsigned)P.e[i].rec.last,
                     (unsigned)P.e[i].rec.secs, (unsigned)P.e[i].rec.count,
                     P.pool + P.e[i].off) < 0)
            rc = -1;
      if (fclose(f) != 0)
         rc = -1;
   }
   if (rc == 0)
   {
      /* rename() over an existing file is refused on the Vita's card: the
       * list goes aside, the new one in, then the old one goes. */
      FILE *have = fopen(P.path, "rb");
      int had = have != NULL;
      if (have)
         fclose(have);
      remove(old);
      if (had && rename(P.path, old) != 0)
         rc = -1;
      else if (rename(tmp, P.path) != 0)
      {
         if (had)
            rename(old, P.path);
         rc = -1;
      }
      else
         remove(old);
   }
   if (rc != 0)
      remove(tmp);
   else
      P.dirty = 0;
   fe_evt("play_log_save n=%u rc=%d", P.n, rc);
   return rc;
}

void play_log_when(uint32_t last, uint32_t now, char *out, size_t cap)
{
   static const char *const month[12] = {
      "January", "February", "March", "April", "May", "June", "July",
      "August", "September", "October", "November", "December" };
   long days;
   if (!last)
   {
      snprintf(out, cap, "never");
      return;
   }
   days = (long)(now / 86400u) - (long)(last / 86400u);
   if (days <= 0)
      snprintf(out, cap, "today");
   else if (days == 1)
      snprintf(out, cap, "yesterday");
   else if (days < 14)
      snprintf(out, cap, "%ld days ago", days);
   else if (days < 60)
      snprintf(out, cap, "%ld weeks ago", days / 7);
   else
   {
      /* civil month of `last` (days since 1970 -> y/m/d, Hinnant) */
      long z = (long)(last / 86400u) + 719468;
      long era = z / 146097, doe = z - era * 146097;
      long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      long mp = (5 * doy + 2) / 153;
      long m = mp < 10 ? mp + 3 : mp - 9;
      long y = yoe + era * 400 + (m <= 2);
      long ny;
      {
         long z2 = (long)(now / 86400u) + 719468;
         long era2 = z2 / 146097, doe2 = z2 - era2 * 146097;
         long yoe2 = (doe2 - doe2 / 1460 + doe2 / 36524 - doe2 / 146096) / 365;
         long doy2 = doe2 - (365 * yoe2 + yoe2 / 4 - yoe2 / 100);
         long mp2 = (5 * doy2 + 2) / 153;
         long m2 = mp2 < 10 ? mp2 + 3 : mp2 - 9;
         ny = yoe2 + era2 * 400 + (m2 <= 2);
      }
      if (y == ny)
         snprintf(out, cap, "in %s", month[m - 1]);
      else
         snprintf(out, cap, "in %s %ld", month[m - 1], y);
   }
}

void play_log_duration(uint32_t secs, char *out, size_t cap)
{
   unsigned h = secs / 3600u, m = (secs % 3600u) / 60u;
   if (secs < 60)
      snprintf(out, cap, "under a minute");
   else if (!h)
      snprintf(out, cap, "%u min", m);
   else if (h < 10 && m)
      snprintf(out, cap, "%u h %u min", h, m);
   else
      snprintf(out, cap, "%u h", h);
}
