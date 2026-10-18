/* fe_linkmode.c -- see fe_linkmode.h. */
#include "fe_linkmode.h"
#include "link_games.h"

#include <string.h>

static unsigned lm_hash(const char *s)
{
   unsigned h = 2166136261u;
   while (*s)
      h = (h ^ (unsigned char)*s++) * 16777619u;
   return h;
}

void fe_linkmode_clear(fe_linkmode *m)
{
   if (m)
      m->n = 0;
}

static int lm_find(const fe_linkmode *m, const char *name, unsigned h)
{
   unsigned i;
   for (i = 0; i < m->n; i++)
      if (m->e[i].hash == h && strcmp(m->e[i].name, name) == 0)
         return (int)i;
   return -1;
}

int fe_linkmode_get(const fe_linkmode *m, const char *name)
{
   int i;
   if (!m || !name || !name[0])
      return FE_LM_AUTO;
   i = lm_find(m, name, lm_hash(name));
   return i < 0 ? FE_LM_AUTO : m->e[i].mode;
}

int fe_linkmode_set(fe_linkmode *m, const char *name, int mode)
{
   size_t len;
   unsigned h;
   int i;
   if (!m || !name || !(len = strlen(name)) || len >= FE_LINKMODE_NAME)
      return -1;
   h = lm_hash(name);
   i = lm_find(m, name, h);
   if (mode != FE_LM_CABLE && mode != FE_LM_ADAPTER)
   {
      if (i >= 0)
      {
         /* keep the file's order: shift the rest down */
         memmove(&m->e[i], &m->e[i + 1],
                 (m->n - (unsigned)i - 1) * sizeof(m->e[0]));
         m->n--;
      }
      return 0;
   }
   if (i < 0)
   {
      if (m->n >= FE_LINKMODE_MAX)
         return -1;
      i = (int)m->n++;
      m->e[i].hash = h;
      memcpy(m->e[i].name, name, len + 1);
   }
   m->e[i].mode = (unsigned char)mode;
   return 0;
}

static int lm_blank(char c)
{
   return c == ' ' || c == '\t' || c == '\r';
}

unsigned fe_linkmode_parse(fe_linkmode *m, const char *text, size_t len)
{
   size_t pos = 0;
   if (!m)
      return 0;
   fe_linkmode_clear(m);
   while (text && pos < len)
   {
      size_t a = pos, b, eq, nb, ne, vb, ve;
      int mode;
      char name[FE_LINKMODE_NAME];
      while (pos < len && text[pos] != '\n')
         pos++;
      b = pos++;                         /* line is [a, b) */
      while (a < b && lm_blank(text[a]))
         a++;
      if (a >= b || text[a] == '#')
         continue;
      for (eq = b; eq > a && text[eq - 1] != '='; eq--)
         ;
      if (eq == a)
         continue;                       /* no '=' */
      eq--;                              /* text[eq] == '=' */
      nb = a;
      ne = eq;
      while (ne > nb && lm_blank(text[ne - 1]))
         ne--;
      vb = eq + 1;
      ve = b;
      while (vb < ve && lm_blank(text[vb]))
         vb++;
      while (ve > vb && lm_blank(text[ve - 1]))
         ve--;
      if (ne == nb || ne - nb >= FE_LINKMODE_NAME)
         continue;
      if (ve - vb == 5 && !memcmp(text + vb, "cable", 5))
         mode = FE_LM_CABLE;
      else if (ve - vb == 7 && !memcmp(text + vb, "adapter", 7))
         mode = FE_LM_ADAPTER;
      else
         continue;
      memcpy(name, text + nb, ne - nb);
      name[ne - nb] = '\0';
      (void)fe_linkmode_set(m, name, mode);
   }
   return m->n;
}

size_t fe_linkmode_serialise(const fe_linkmode *m, char *out, size_t cap)
{
   static const char head[] = "# GBAdhoc link mode, one game per line: "
                              "<rom> = cable|adapter\n";
   size_t need = 0, i;
   if (cap)
      out[0] = '\0';
#define LM_PUT(str, n)                                         \
   do {                                                        \
      size_t k_ = (n);                                         \
      if (need + k_ < cap)                                     \
         memcpy(out + need, (str), k_);                        \
      else if (need < cap)                                     \
         memcpy(out + need, (str), cap - 1 - need);            \
      need += k_;                                              \
   } while (0)
   if (!m)
      return 0;
   LM_PUT(head, sizeof(head) - 1);
   for (i = 0; i < m->n; i++)
   {
      const char *v = m->e[i].mode == FE_LM_CABLE ? " = cable\n"
                                                  : " = adapter\n";
      LM_PUT(m->e[i].name, strlen(m->e[i].name));
      LM_PUT(v, strlen(v));
   }
#undef LM_PUT
   if (cap)
      out[need < cap ? need : cap - 1] = '\0';
   return need;
}

int fe_linkmode_effective(int table_mode, int override_mode)
{
   if (override_mode == FE_LM_CABLE)
      return LINK_CABLE;
   if (override_mode == FE_LM_ADAPTER)
      return LINK_ADAPTER;
   return table_mode == LINK_ADAPTER || table_mode == LINK_BOTH
             ? table_mode : LINK_CABLE;
}
