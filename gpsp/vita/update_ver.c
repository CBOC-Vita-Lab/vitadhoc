/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Copyright (c) 2026 VitAdhoc contributors */
/* update_ver.c -- see update_ver.h. */
#include "update_ver.h"

#include <string.h>

int upd_ver_parse(const char *s, unsigned v[4], char *shown, size_t cap)
{
   unsigned n = 0, i;
   const char *start, *p;
   size_t len;

   v[0] = v[1] = v[2] = v[3] = 0;
   if (!s)
      return 0;
   /* the words ahead of the first digit: letters, spaces, '-' and '_' only */
   for (p = s; *p && (*p < '0' || *p > '9'); p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            *p == ' ' || *p == '-' || *p == '_'))
         return 0;
   if (!*p || p - s > 24)
      return 0;
   start = p;
   for (;;)
   {
      unsigned val = 0, digits = 0;
      while (*p >= '0' && *p <= '9')
      {
         if (++digits > 5)
            return 0;
         val = val * 10 + (unsigned)(*p - '0');
         p++;
      }
      if (!digits || n >= 4)
         return 0;
      v[n++] = val;
      if (*p == '.')
      {
         p++;
         continue;
      }
      break;
   }
   if (*p)
      return 0;                    /* 2.1-rc1, 2.1b, 2.1.x, 2.1 beta */
   len = (size_t)(p - start);
   if (shown && cap)
   {
      if (len >= cap)
         return 0;
      for (i = 0; i < len; i++)
         shown[i] = start[i];
      shown[len] = '\0';
   }
   return 1;
}

int upd_ver_cmp(const unsigned a[4], const unsigned b[4])
{
   int i;
   for (i = 0; i < 4; i++)
      if (a[i] != b[i])
         return a[i] < b[i] ? -1 : 1;
   return 0;
}

int upd_ver_newer(const char *tag, const char *mine)
{
   unsigned a[4], b[4];
   if (!upd_ver_parse(tag, a, NULL, 0) || !upd_ver_parse(mine, b, NULL, 0))
      return 0;
   return upd_ver_cmp(a, b) > 0;
}

/* ---- the answer ---------------------------------------------------------- */

typedef struct
{
   const char *p, *end;
} jr;

static void ws(jr *r)
{
   while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' ||
                            *r->p == '\r'))
      r->p++;
}

/* A string at r->p (the opening quote).  Copies up to cap - 1 bytes of its
 * raw text to out (NULL: skips it); *overflow when it did not fit.  A
 * backslash escape is kept as written (a tag with one is not a version, and
 * the other strings are only skipped).  0 = not a well-formed string. */
static int jstr(jr *r, char *out, size_t cap, int *overflow)
{
   size_t n = 0;
   if (r->p >= r->end || *r->p != '"')
      return 0;
   r->p++;
   while (r->p < r->end)
   {
      char c = *r->p++;
      if (c == '"')
      {
         if (out && cap)
            out[n] = '\0';
         return 1;
      }
      if ((unsigned char)c < 0x20)
         return 0;
      if (c == '\\')
      {
         if (r->p >= r->end)
            return 0;
         if (out && n + 2 < cap)
         {
            out[n++] = c;
            out[n++] = *r->p;
         }
         else if (out)
            *overflow = 1;
         r->p++;
         continue;
      }
      if (out && n + 1 < cap)
         out[n++] = c;
      else if (out)
         *overflow = 1;
   }
   return 0;
}

/* A value to step over: a string, a number or literal, or a balanced object
 * or array (strings inside it respected).  0 = broken. */
static int jskip(jr *r, int depth)
{
   ws(r);
   if (r->p >= r->end || depth > 32)
      return 0;
   if (*r->p == '"')
      return jstr(r, NULL, 0, NULL);
   if (*r->p == '{' || *r->p == '[')
   {
      char close = *r->p == '{' ? '}' : ']';
      int obj = *r->p == '{';
      r->p++;
      ws(r);
      if (r->p < r->end && *r->p == close)
      {
         r->p++;
         return 1;
      }
      for (;;)
      {
         ws(r);
         if (obj)
         {
            if (!jstr(r, NULL, 0, NULL))
               return 0;
            ws(r);
            if (r->p >= r->end || *r->p++ != ':')
               return 0;
         }
         if (!jskip(r, depth + 1))
            return 0;
         ws(r);
         if (r->p >= r->end)
            return 0;
         if (*r->p == ',')
         {
            r->p++;
            continue;
         }
         if (*r->p == close)
         {
            r->p++;
            return 1;
         }
         return 0;
      }
   }
   /* number, true, false, null */
   {
      const char *s = r->p;
      while (r->p < r->end && *r->p != ',' && *r->p != '}' && *r->p != ']' &&
             *r->p != ' ' && *r->p != '\n' && *r->p != '\r' && *r->p != '\t')
         r->p++;
      return r->p > s;
   }
}

int upd_release_parse(const char *json, size_t len, char *shown, size_t cap)
{
   jr r;
   char tag[64], key[32], lit[8];
   int have_tag = 0, draft = 0, pre = 0, tag_over = 0, key_over;
   unsigned v[4];

   if (!json || !len)
      return 0;
   r.p = json;
   r.end = json + len;
   ws(&r);
   if (r.p >= r.end || *r.p++ != '{')
      return 0;
   ws(&r);
   if (r.p < r.end && *r.p == '}')
      return 0;
   for (;;)
   {
      ws(&r);
      key_over = 0;
      if (!jstr(&r, key, sizeof(key), &key_over))
         return 0;
      ws(&r);
      if (r.p >= r.end || *r.p++ != ':')
         return 0;
      ws(&r);
      if (!key_over && !strcmp(key, "tag_name") && r.p < r.end && *r.p == '"')
      {
         if (!jstr(&r, tag, sizeof(tag), &tag_over))
            return 0;
         have_tag = 1;
      }
      else if (!key_over && (!strcmp(key, "draft") || !strcmp(key, "prerelease")))
      {
         size_t n = 0;
         while (r.p < r.end && n + 1 < sizeof(lit) && *r.p >= 'a' && *r.p <= 'z')
            lit[n++] = *r.p++;
         lit[n] = '\0';
         if (!strcmp(lit, "true"))
         {
            if (key[0] == 'd')
               draft = 1;
            else
               pre = 1;
         }
         else if (strcmp(lit, "false") != 0)
            return 0;              /* "prerelease": "yes" is not an answer */
      }
      else if (!jskip(&r, 0))
         return 0;
      ws(&r);
      if (r.p >= r.end)
         return 0;
      if (*r.p == ',')
      {
         r.p++;
         continue;
      }
      if (*r.p == '}')
         break;
      return 0;
   }
   if (!have_tag || tag_over || draft || pre)
      return 0;
   return upd_ver_parse(tag, v, shown, cap);
}
