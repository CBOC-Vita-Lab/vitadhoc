/* test_ini_doc.c -- CONFIG.INI read once, written once (fe_ini_doc, fe_util.h).
 *
 * Built against the production fe_util.c, config_psp.c and ctl_map.c, TWICE:
 * normally, and with -DFE_INI_DOC_LEGACY, which makes fe_ini_doc_bind a
 * no-op -- so the very same pcfg_load / pcfg_save run on the old path, every
 * key its own file open and every set its own read-modify-rewrite.  Then:
 *
 *   unit                  (normal build only)
 *     1. doc_get == fe_ini_get for every key of every fixture and of random
 *        files (CR LF, comments, sections, duplicates, blanks around '=',
 *        lines longer than fgets' 511 bytes, no final newline).
 *     2. doc_set == fe_ini_set byte for byte over random edit sequences on
 *        the same files; an unchanged document is never rewritten.
 *     3. Overflow: a file over the cap does not load; a set that would not
 *        fit is refused and the document is unchanged; nothing truncated.
 *     4. Atomic save + recovery: every state a power cut can leave.
 *     5. Open counts: a bound pcfg_load opens the file once.
 *   dump CONFIG OUT       pcfg_load CONFIG, write every g_pcfg / g_pdisp
 *                         byte to OUT; then change settings, pcfg_save to
 *                         CONFIG (a copy), and the shell compares the two
 *                         builds' OUT and saved files (run_host_tests.py). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctl_map.h"
#include "config_psp.h"
#include "fe_util.h"

#ifndef GPSP_PLAYABLE
void fe_evt(const char *fmt, ...) { (void)fmt; }
#endif
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "t"; }
const char *vid_filter_name(int mode) { (void)mode; return "t"; }

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
   exit(1); } } while (0)

static void put(const char *path, const char *data, size_t n)
{
   FILE *f = fopen(path, "wb");
   CHECK(f && fwrite(data, 1, n, f) == n && fclose(f) == 0);
}

static size_t get(const char *path, char *buf, size_t cap)
{
   FILE *f = fopen(path, "rb");
   size_t n;
   if (!f)
      return (size_t)-1;
   n = fread(buf, 1, cap, f);
   fclose(f);
   return n;
}

static int exists(const char *p)
{
   return access(p, F_OK) == 0;
}

static unsigned rng = 12345;
static unsigned rnd(unsigned n)
{
   rng = rng * 1103515245u + 12345u;
   return (rng >> 8) % n;
}

static const char *keys[] = {
   "scale", "filter", "theme", "console", "group", "nick", "last_rom",
   "bind_a", "scale_gb", "net_session_fps", "me_mode", "x", "scale_gba",
   "gb_palette", "gb_palette_gb", "menu_button", "unknown_key",
};
#define NKEYS (sizeof(keys) / sizeof(keys[0]))

/* A random INI with every trap the old reader and writer have opinions on. */
static size_t random_ini(char *out, size_t cap)
{
   size_t o = 0;
   int lines = 1 + (int)rnd(40), i;
   cap = cap < 8192 ? cap : 8192;
   for (i = 0; i < lines && o + 700 < cap; i++)
   {
      const char *k = keys[rnd(NKEYS)];
      const char *eol = rnd(4) == 0 ? "\r\n" : "\n";
      switch (rnd(9))
      {
      case 0: o += (size_t)sprintf(out + o, "# %s = comment%s", k, eol); break;
      case 1: o += (size_t)sprintf(out + o, "[section]%s", eol); break;
      case 2: o += (size_t)sprintf(out + o, "%s", eol); break;
      case 3: o += (size_t)sprintf(out + o, "  %s\t=  %u  %s", k, rnd(99), eol); break;
      case 4:
      {
         /* longer than fgets' 511: the old reader saw it as pieces */
         int n = 500 + (int)rnd(120), j;
         o += (size_t)sprintf(out + o, "%s = ", k);
         for (j = 0; j < n; j++)
            out[o++] = (char)('a' + rnd(26));
         o += (size_t)sprintf(out + o, "%s", eol);
         break;
      }
      case 5: o += (size_t)sprintf(out + o, "%s=%u%s", k, rnd(9), eol); break;
      case 6: o += (size_t)sprintf(out + o, "%sx = 1%s", k, eol); break;
      case 7: o += (size_t)sprintf(out + o, ";%s = 3%s", k, eol); break;
      default: o += (size_t)sprintf(out + o, "%s = %u%s", k, rnd(1000), eol); break;
      }
   }
   if (o && rnd(3) == 0)
      while (o && (out[o - 1] == '\n' || out[o - 1] == '\r'))
         o--;                                 /* no final newline */
   out[o] = '\0';
   return o;
}

static const char *const A = "/tmp/ini-doc-a.ini";
static const char *const B = "/tmp/ini-doc-b.ini";

static void same_gets(const char *path, const fe_ini_doc *d)
{
   size_t k;
   for (k = 0; k < NKEYS; k++)
   {
      char x[700], y[700];
      int fx, fy;
      memset(x, 1, sizeof(x)); memset(y, 1, sizeof(y));
      fx = fe_ini_get(path, keys[k], x, sizeof(x));
      fy = fe_ini_doc_get(d, keys[k], y, sizeof(y));
      CHECK(fx == fy);
      if (fx)
         CHECK(strcmp(x, y) == 0);
      CHECK(fe_ini_get_int(path, keys[k], -7) == fe_ini_doc_get_int(d, keys[k], -7));
   }
}

static void unit_get_set(const char *fixture)
{
   static char text[8192], fa[16384], fb[16384];
   int round;
   for (round = 0; round < 3000; round++)
   {
      fe_ini_doc d;
      size_t n, na, nb;
      int e, edits = (int)rnd(12);
      if (fixture && round == 0)
         n = get(fixture, text, sizeof(text) - 1);
      else
         /* inside the old writer's 4095-byte input limit, so its answer is
          * the reference (above it the old writer refused every set) */
         n = random_ini(text, 3800);
      CHECK(n != (size_t)-1);
      put(A, text, n);
      put(B, text, n);
      CHECK(fe_ini_doc_load(&d, B) == 0);
      same_gets(A, &d);
      for (e = 0; e < edits && get(A, fa, sizeof(fa)) < 4000; e++)
      {
         const char *k = keys[rnd(NKEYS)];
         char v[64];
         snprintf(v, sizeof(v), rnd(2) ? "%u" : "v%u x", rnd(500));
         CHECK(fe_ini_set(A, k, v) == fe_ini_doc_set(&d, k, v));
      }
      CHECK(fe_ini_doc_save(&d, B) == 0);
      na = get(A, fa, sizeof(fa));
      nb = get(B, fb, sizeof(fb));
      CHECK(na == nb && memcmp(fa, fb, na) == 0);
      CHECK(!exists("/tmp/ini-doc-b.ini.tmp") && !exists("/tmp/ini-doc-b.ini.bak"));
      CHECK(fe_ini_doc_load(&d, B) == 0);
      same_gets(A, &d);
   }
}

static void unit_unchanged_not_written(void)
{
   fe_ini_doc d;
   char buf[64];
   put(A, "a = 1\n", 6);
   CHECK(fe_ini_doc_load(&d, A) == 0);
   CHECK(fe_ini_doc_set(&d, "a", "1") == 0);       /* same bytes */
   CHECK(!d.changed);
   unlink(A);                                       /* a write would recreate */
   CHECK(fe_ini_doc_save(&d, A) == 0);
   CHECK(!exists(A));
   /* absent file: an empty document, saved on the first set */
   CHECK(fe_ini_doc_load(&d, A) == 0 && !d.existed && d.len == 0);
   CHECK(fe_ini_doc_set_int(&d, "theme", 1) == 0);
   CHECK(fe_ini_doc_save(&d, A) == 0);
   CHECK(get(A, buf, sizeof(buf)) == 10 && memcmp(buf, "theme = 1\n", 10) == 0);
}

static void unit_overflow(void)
{
   static char big[FE_INI_DOC_CAP + 64], back[FE_INI_DOC_CAP + 64];
   fe_ini_doc d;
   size_t n, i;
   /* exactly the cap - 1 bytes loads; one more does not */
   for (i = 0; i < FE_INI_DOC_CAP - 1; i++)
      big[i] = (i % 40 == 39) ? '\n' : 'k';
   put(A, big, FE_INI_DOC_CAP - 1);
   CHECK(fe_ini_doc_load(&d, A) == 0 && d.len == FE_INI_DOC_CAP - 1);
   /* full: a new key is refused, the document unchanged, the file intact */
   CHECK(fe_ini_doc_set(&d, "theme", "1") == -1 && d.overflow && !d.changed);
   CHECK(d.len == FE_INI_DOC_CAP - 1 && memcmp(d.buf, big, d.len) == 0);
   CHECK(fe_ini_doc_save(&d, A) == -1);
   n = get(A, back, sizeof(back));
   CHECK(n == FE_INI_DOC_CAP - 1 && memcmp(back, big, n) == 0);
   put(A, big, FE_INI_DOC_CAP);
   CHECK(fe_ini_doc_load(&d, A) == -1 && !d.ok);
   CHECK(fe_ini_doc_set(&d, "a", "1") == -1 && fe_ini_doc_save(&d, A) == -1);
   CHECK(get(A, back, sizeof(back)) == FE_INI_DOC_CAP);
   /* a value too long for one line is refused, not cut */
   {
      char v[1100];
      memset(v, 'x', sizeof(v) - 1); v[sizeof(v) - 1] = '\0';
      put(A, "a = 1\n", 6);
      CHECK(fe_ini_doc_load(&d, A) == 0);
      CHECK(fe_ini_doc_set(&d, "a", v) == -1 && !d.changed);
      CHECK(d.len == 6 && memcmp(d.buf, "a = 1\n", 6) == 0);
   }
}

/* Every state fe_ini_doc_save can be cut in (see its comment). */
static void unit_recovery(void)
{
   const char *P = "/tmp/ini-doc-r.ini", *T = "/tmp/ini-doc-r.ini.tmp",
              *K = "/tmp/ini-doc-r.ini.bak";
   fe_ini_doc d;
   char v[16];
   unlink(P); unlink(T); unlink(K);
   /* between 3 and 4: old moved to .bak, new complete in .tmp -> new */
   put(K, "theme = 0\n", 10); put(T, "theme = 1\n", 10);
   CHECK(fe_ini_doc_load(&d, P) == 0 && d.existed);
   CHECK(fe_ini_doc_get(&d, "theme", v, sizeof(v)) && !strcmp(v, "1"));
   CHECK(exists(P) && !exists(T) && !exists(K));
   /* only the .bak (tmp lost) -> old */
   unlink(P); put(K, "theme = 0\n", 10);
   CHECK(fe_ini_doc_load(&d, P) == 0 && d.existed);
   CHECK(fe_ini_doc_get(&d, "theme", v, sizeof(v)) && !strcmp(v, "0"));
   CHECK(exists(P) && !exists(K));
   /* a first save cut short: .tmp alone may be a fragment -> never used */
   unlink(P); put(T, "nd_rto_min_us = 5", 17);
   CHECK(fe_ini_doc_load(&d, P) == 0 && !d.existed && d.len == 0);
   CHECK(!exists(T) && !exists(P));
   /* the file is there: leftovers are ignored, and the next save leaves
    * neither (a stale .bak is removed before the old file moves aside) */
   put(P, "theme = 0\n", 10); put(T, "junk", 4); put(K, "old", 3);
   CHECK(fe_ini_doc_load(&d, P) == 0);
   CHECK(fe_ini_doc_get(&d, "theme", v, sizeof(v)) && !strcmp(v, "0"));
   CHECK(fe_ini_doc_set(&d, "theme", "1") == 0 && fe_ini_doc_save(&d, P) == 0);
   CHECK(!exists(T) && !exists(K));
   CHECK(fe_ini_doc_load(&d, P) == 0 &&
         fe_ini_doc_get(&d, "theme", v, sizeof(v)) && !strcmp(v, "1"));
   unlink(P);
}

static void unit_open_count(const char *fixture)
{
   unsigned o0, o1, m;
   static char text[8192];
   size_t n = get(fixture, text, sizeof(text));
   put(A, text, n);
   fe_ini_stats(&o0, &m);
   pcfg_load(A);
   fe_ini_stats(&o1, &m);
   CHECK(o1 - o0 == 1);
   fe_ini_stats(&o0, &m);
   g_pcfg.theme ^= 1;
   pcfg_save();
   fe_ini_stats(&o1, &m);
   CHECK(o1 - o0 == 1);                    /* one read; the write is one */
}

static void dump(const char *cfg, const char *out)
{
   FILE *f;
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   memset(g_pdisp, 0, sizeof(g_pdisp));
   pcfg_load(cfg);
   f = fopen(out, "wb");
   CHECK(f);
   CHECK(fwrite(&g_pcfg, sizeof(g_pcfg), 1, f) == 1);
   CHECK(fwrite(g_pdisp, sizeof(g_pdisp), 1, f) == 1);
   CHECK(fclose(f) == 0);
   /* Then a save with a few settings changed and one profile edited, the
    * way Settings closes; the shell compares the two builds' files. */
   g_pcfg.theme ^= 1;
   g_pcfg.ff_hold ^= 1;
   snprintf(g_pcfg.group, sizeof(g_pcfg.group), "GPSP42");
   pcfg_display_select(FE_CONSOLE_GB);
   g_pcfg.scale = 2;
   pcfg_save();
   CHECK(pcfg_remember_rom("Some Game (USA).gba") == 0);
   CHECK(pcfg_remember_console(FE_CONSOLE_GBC) == 0);
}

int main(int argc, char **argv)
{
   if (argc == 4 && !strcmp(argv[1], "dump"))
   {
      dump(argv[2], argv[3]);
      return 0;
   }
#ifdef FE_INI_DOC_LEGACY
   (void)unit_get_set; (void)unit_unchanged_not_written;
   (void)unit_overflow; (void)unit_recovery; (void)unit_open_count;
   fprintf(stderr, "the legacy build only dumps\n");
   return 2;
#else
   if (argc >= 2)
   {
      int i;
      for (i = 1; i < argc; i++)
         unit_get_set(argv[i]);
      unit_open_count(argv[1]);
   }
   unit_get_set(NULL);
   unit_unchanged_not_written();
   unit_overflow();
   unit_recovery();
   unlink(A); unlink(B);
   printf("PASS ini_doc: get/set identical to the per-key reader/writer, "
          "overflow refused, atomic save + recovery, 1 open per load/save "
          "(%d checks)\n", checks);
   return 0;
#endif
}
