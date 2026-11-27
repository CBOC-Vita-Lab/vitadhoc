/* i18n_test.c -- the catalogue loader (../i18n.c): BOM, CRLF, malformed
 * lines, missing keys, a huge file, formats, the pseudo-locale.
 * Usage: i18n_test <scratch dir>.  Exit 0 = all agree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../i18n.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static char dir[300];

static void put(const char *code, const void *data, size_t n)
{
   char p[400];
   FILE *f;
   snprintf(p, sizeof(p), "%s/lang", dir);
   mkdir(p, 0755);
   snprintf(p, sizeof(p), "%s/lang/%s.txt", dir, code);
   f = fopen(p, "wb");
   fwrite(data, 1, n, f);
   fclose(f);
}
#define PUT(code, s) put(code, s, sizeof(s) - 1)

static size_t utf8_chars(const char *s)
{
   size_t n = 0;
   for (; *s; s++)
      if (((unsigned char)*s & 0xc0) != 0x80)
         n++;
   return n;
}

int main(int argc, char **argv)
{
   char *h;
   size_t i;
   if (argc < 2)
      return 2;
   snprintf(dir, sizeof(dir), "%s", argv[1]);
   i18n_dir(dir);

   /* English: the compiled table, no file */
   CHECK(i18n_use(I18N_EN, 1) == I18N_EN);
   CHECK(!strcmp(tr("settings.language"), "Language"));
   CHECK(!strcmp(tr("no.such.key"), "no.such.key"));       /* the key itself, never NULL or blank */
   CHECK(!strcmp(tr_en("settings.about"), "ABOUT"));

   /* a good file with a BOM and CRLF line ends, a comment, a blank line and an escape */
   PUT("es", "\xef\xbb\xbf# context: the row\r\nsettings.language\tIdioma\r\n\r\nsettings.about\tAutom\xc3\xa1tico\\n2\r\n");
   CHECK(i18n_use(I18N_ES, 1) == I18N_ES);
   CHECK(!strcmp(tr("settings.language"), "Idioma"));
   CHECK(!strcmp(tr("settings.about"), "Autom\xc3\xa1tico\n2"));
   CHECK(i18n_loaded_count() == 2 && i18n_refused_count() == 0);
   CHECK(!strcmp(tr_en("settings.language"), "Language"));  /* logs and tests keep English */

   /* malformed lines are refused one by one; the good ones stay; a missing key shows English */
   PUT("fr", "no tab here\n\tno key\nbad key\twith space\nsettings.about\t\nsettings.language\tLangue\\q\n"
             "settings.language\tLangue\n"            /* good */
             "unknown.key\tinconnu\n"                 /* not a key of this build: dropped */
             "settings.about\tbad \xff utf8\n"
             "settings.about\tctl \x01 char\n");
   CHECK(i18n_use(I18N_FR, 1) == I18N_FR);
   CHECK(!strcmp(tr("settings.language"), "Langue"));
   CHECK(!strcmp(tr("settings.about"), "ABOUT"));          /* its only lines were bad: English */
   CHECK(i18n_loaded_count() == 1 && i18n_refused_count() >= 6);

   /* a file with no usable line, a missing file, a binary file, a huge one: English, no crash */
   PUT("de", "# only comments\n\n");
   CHECK(i18n_use(I18N_DE, 1) == I18N_EN && !strcmp(tr("settings.language"), "Language"));
   CHECK(i18n_use(I18N_IT, 1) == I18N_EN && !strcmp(tr("settings.language"), "Language"));   /* it.txt does not exist */
   { unsigned char b[3000]; for (i = 0; i < sizeof(b); i++) b[i] = (unsigned char)(i * 7 + 1); put("pt-BR", b, sizeof(b)); }
   CHECK(i18n_use(I18N_PTBR, 1) == I18N_EN && !strcmp(tr("settings.language"), "Language"));
   h = malloc(2u << 20);
   memset(h, 'a', 2u << 20);
   memcpy(h, "settings.language\tX", 19);
   h[19] = '\n';
   put("ja", h, 2u << 20);
   CHECK(i18n_use(I18N_JA, 1) == I18N_EN && !strcmp(tr("settings.language"), "Language"));
   /* a very long line, no end of line at the end of the file */
   memset(h, 'b', 5000);
   memcpy(h, "settings.language\t", 18);
   memcpy(h + 5000, "\nsettings.about\tAuto", 20);
   put("zh-Hans", h, 5020);
   CHECK(i18n_use(I18N_ZHS, 1) == I18N_ZHS && !strcmp(tr("settings.language"), "Language") &&
         !strcmp(tr("settings.about"), "Auto") && i18n_refused_count() == 1);
   free(h);

   /* dates in the language's order; Japanese and Chinese month names (10月) take 年 and 日 */
   {
      char d[64];
      static const char ymd[] = "ui.date_order\tymd\nui.oct\t10\xe6\x9c\x88\n";
      static const char mdy[] = "ui.date_order\tmdy\nui.oct\tOkt\n";
      i18n_date(d, sizeof(d), 2026, 10, 3);
      CHECK(!strcmp(d, "3 Oct 2026"));
      put("ja", ymd, sizeof(ymd) - 1);
      CHECK(i18n_use(I18N_JA, 1) == I18N_JA);
      i18n_date(d, sizeof(d), 2026, 10, 3);
      CHECK(!strcmp(d, "2026\xe5\xb9\xb4" "10\xe6\x9c\x88" "3\xe6\x97\xa5"));
      put("de", mdy, sizeof(mdy) - 1);
      CHECK(i18n_use(I18N_DE, 1) == I18N_DE);
      i18n_date(d, sizeof(d), 2026, 10, 3);
      CHECK(!strcmp(d, "Okt 3, 2026"));
      CHECK(i18n_use(I18N_EN, 1) == I18N_EN);
   }

   /* Automatic follows the system's language; a language with no catalogue is English */
   CHECK(i18n_use(I18N_AUTO, 3) == I18N_ES);                           /* the Vita's Spanish */
   CHECK(i18n_use(I18N_AUTO, 8) == I18N_EN);                           /* Russian: no catalogue */
   CHECK(i18n_from_system(0) == I18N_JA && i18n_from_system(2) == I18N_FR && i18n_from_system(4) == I18N_DE &&
         i18n_from_system(5) == I18N_IT && i18n_from_system(17) == I18N_PTBR && i18n_from_system(7) == I18N_PTBR &&
         i18n_from_system(11) == I18N_ZHS && i18n_from_system(18) == I18N_EN && i18n_from_system(-5) == I18N_EN &&
         i18n_from_system(9999) == I18N_EN);
   CHECK(i18n_index("pt-br") == I18N_PTBR && i18n_index("ZH-HANS") == I18N_ZHS && i18n_index("xx-long") == I18N_PSEUDO &&
         i18n_index("auto") == I18N_AUTO && i18n_index("klingon") == I18N_AUTO && i18n_index(NULL) == I18N_AUTO &&
         i18n_index("") == I18N_AUTO && i18n_index("en") == I18N_EN);

   /* formats: the same conversions in the same order, or the English text stays */
   CHECK(i18n_same_formats("%s of %d", "%s von %d"));
   CHECK(!i18n_same_formats("%s of %d", "%d von %s"));
   CHECK(!i18n_same_formats("%s", "%s %s"));
   CHECK(!i18n_same_formats("%d", "%1$d"));
   CHECK(!i18n_same_formats("%d", "%n"));
   CHECK(!i18n_same_formats("%d", "100%"));
   CHECK(i18n_same_formats("100%% of %lu", "100%% de %lu"));
   CHECK(!i18n_same_formats("%d", "%ld"));
   CHECK(i18n_same_formats("none", "keine"));
   CHECK(!i18n_same_formats("none", "%s"));

   /* the pseudo-locale: every text ~35% longer, accents, conversions untouched */
   CHECK(i18n_use(I18N_PSEUDO, 1) == I18N_PSEUDO);
   {
      const char *en = tr_en("settings.language"), *ps = tr("settings.language");
      CHECK(strcmp(en, ps) != 0 && utf8_chars(ps) > utf8_chars(en));
   }
   {
      char o[200];
      i18n_pseudo("Saved %d of %s (100%%)\nOK", o, sizeof(o));
      CHECK(strstr(o, "%d") && strstr(o, "%s") && strstr(o, "100%%") && strchr(o, '\n') && i18n_same_formats("Saved %d of %s (100%%)\nOK", o));
      i18n_pseudo("Rewind length setting is here", o, sizeof(o));
      CHECK(utf8_chars(o) >= 29 * 125 / 100 && utf8_chars(o) <= 29 * 150 / 100);
      i18n_pseudo("A long text that cannot fit", o, 10);       /* a short buffer: cut, ended */
      CHECK(strlen(o) < 10);
   }
   CHECK(i18n_use(I18N_EN, 1) == I18N_EN && !strcmp(tr("settings.language"), "Language"));

   if (fails)
      return 1;
   printf("ok i18n\n");
   return 0;
}
