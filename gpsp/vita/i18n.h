/* i18n.h -- the app's interface languages.
 *
 * One catalogue per language, lang/<code>.txt in the package (app0:lang/):
 * UTF-8, one `key<TAB>text` per line, `\n` `\t` `\\` as escapes, `#` lines
 * are comments for the translator.  English is ALSO compiled in (lang_en.h,
 * generated from lang/en.txt by tools/i18n/gen_lang_h.py), so a missing or
 * damaged file, or a missing key, shows English and never blank text.
 *
 * The catalogue is one allocation, read when the language is chosen (at
 * start, and when the setting changes); a lookup is a binary search over
 * sorted keys.  Nothing is read or allocated per frame.
 *
 * A text with a % conversion is used as a printf format by its caller, so
 * a translation whose conversions (types and order) differ from the English
 * one is dropped when the file is loaded: the English shows for that key.
 * (No positional %1$s: write whole sentences with %s / %d, as in English.)
 *
 * Plain C, no platform calls: the unit test (hostcore/i18n_test.c) builds it
 * alone.
 */
#ifndef GBADHOC_I18N_H
#define GBADHOC_I18N_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The language codes of display.ini's `language=`, in the order of
 * Settings > Interface > Language.  Index 0 is "auto" (the system's
 * language); I18N_LANG_PSEUDO is a test language, not in the cycle. */
enum { I18N_AUTO, I18N_EN, I18N_ES, I18N_FR, I18N_DE, I18N_IT, I18N_PTBR,
       I18N_JA, I18N_ZHS, I18N_PSEUDO, I18N_LANGS };
extern const char *const i18n_code[I18N_LANGS];   /* "auto" "en" ... "xx-long" */
extern const char *const i18n_native[I18N_LANGS]; /* "Automatic" "English" "Español" ... */

/* Index of a display.ini value (any case, "pt-br" too); I18N_AUTO when it
 * is not one of them. */
int  i18n_index(const char *code);
/* The language of a system setting (SceSystemParamLang, 0..19); English for
 * one this app has no catalogue for. */
int  i18n_from_system(int sce_lang);

/* Where the catalogues are: <dir>/lang/<code>.txt.  Call before i18n_use. */
void i18n_dir(const char *dir);
/* Chooses the language (an I18N_* index; I18N_AUTO resolves with `system`,
 * the SceSystemParamLang value).  Loads the file; returns the language now
 * in force (English when the file is missing or unreadable).  Frees the
 * previous catalogue: pointers tr() gave before are void. */
int  i18n_use(int lang, int system);
/* The language in force (never I18N_AUTO). */
int  i18n_current(void);
/* How many texts the catalogue in force holds (0 for English: it is the
 * compiled table), and how many lines of the file were refused. */
int  i18n_loaded_count(void);
int  i18n_refused_count(void);

/* The text of a key in the language in force; English when it has none; the
 * key itself for a key nobody knows.  Never NULL, never empty for a key the
 * English table holds. */
const char *tr(const char *key);
/* The English text of a key (logs and tests read English whatever the
 * setting). */
const char *tr_en(const char *key);
/* The twin's audit (GBADHOC_TR_AUDIT=1 in the environment): a text drawn that
 * is a key of the English table (a table that kept TR_KEY() and was drawn
 * without tr()) is reported on stderr as "EVT i18n: key shown <key>".  The
 * text layers call i18n_audit() only when i18n_audit_on is set. */
extern int i18n_audit_on;
void i18n_audit(const char *drawn);
/* ... and a drawn text with half a character or a byte that is no UTF-8 in it
 * is reported as "EVT i18n: cut character drawn ..." / "bad UTF-8 drawn ...";
 * a code point no font has, as "EVT i18n: glyph missing U+XXXX", one that found no
 * free cell in its face's atlas as "no free glyph cell for U+XXXX" (the layers
 * call i18n_audit_glyph on the way to their '?'). */
void i18n_audit_glyph(unsigned cp, int why);   /* why 0: no font has it, 1: no free cell */

/* The English text of a text tr() gave (by pointer; any other text comes back
 * as it is): for the lines the tests read, which stay English. */
const char *tr_english_of(const char *text);
/* A key marker for a static table: the key itself, to be passed to tr() when
 * the table is used.  (tools/i18n/extract.py reads these too.) */
#define TR_KEY(k) (k)

/* The loader on its own, for the unit test: parses `len` bytes of a
 * catalogue the way a file is read (BOM, CRLF, malformed lines) and calls
 * back for each good line with the unescaped text.  Returns the number of
 * refused lines. */
typedef void (*i18n_line_fn)(void *user, const char *key, const char *text);
int  i18n_parse(char *buf, size_t len, i18n_line_fn fn, void *user);
/* The pseudo-locale's text for an English one, ~35% longer with accents;
 * written to out (at most n bytes, always terminated). */
void i18n_pseudo(const char *en, char *out, size_t n);
/* 1 when two texts hold the same printf conversions in the same order. */
int  i18n_same_formats(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif
