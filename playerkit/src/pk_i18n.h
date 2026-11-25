/*
 * The players' interface languages.
 *
 * One catalogue per language, lang/pk_<code>.txt in the package (app0:lang/): UTF-8, one `key<TAB>text` per
 * line, `\n` `\t` `\\` as escapes, `#` lines are notes for the translator. The keys of playerkit's menus are
 * plain (menu.*, link.*, ...); the two players' own texts start with snes. and md. English is ALSO compiled
 * in (pk_strings_en.inc, generated from lang/pk_en.txt by playerkit/tools/gen_strings.py), so a missing or
 * damaged file, or a missing key, shows English and never blank text.
 *
 * The same scheme as the main app's i18n (lang/<code>.txt there): the language is the `language=` line of
 * display.ini (auto|en|es|fr|de|it|pt-BR|ja|zh-Hans), Automatic is the Vita's system language, one the app
 * has no catalogue for is English. The players' own files are separate (pk_ prefix), so the two sets never
 * collide.
 *
 * The catalogue is one allocation, read when the language is chosen (at start, and when the setting
 * changes); a lookup is a binary search over sorted keys. Nothing is read or allocated per frame.
 *
 * A text with a % conversion is used as a printf format by its caller, so a translation whose conversions
 * (types and order) differ from the English one is dropped when the file is loaded: the English shows for
 * that key. (No positional %1$s: write whole sentences with %s / %d, as in English.)
 *
 * No platform calls and no GPL code: the players link this file. Log lines are never translated.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_I18N_H
#define PK_I18N_H

#include <stddef.h>
#include <string>

namespace pk {

/* The values of display.ini's `language=`, in the order of Settings > Interface > Language. Index 0 is "auto"
 * (the system's language); LANG_PSEUDO is the test language xx-long, not in the cycle. */
enum { LANG_AUTO = 0, LANG_EN, LANG_ES, LANG_FR, LANG_DE, LANG_IT, LANG_PTBR, LANG_JA, LANG_ZHS, LANG_PSEUDO, LANG_COUNT };
enum { LANG_CHOICES = LANG_PSEUDO };      /* what the Language row offers (the pseudo-locale is a test aid) */
extern const char *const kLangCode[LANG_COUNT];     /* "auto" "en" "es" ... "zh-Hans" "xx-long" */
extern const char *const kLangNative[LANG_COUNT];   /* "Automatic" "English" "Espa\xc3\xb1ol" ...: each in itself (Automatic is translated by the caller: lang.auto) */

/* The index of a display.ini value (any case); LANG_AUTO when it is not one of them. */
int lang_index(const char *code);
/* The language of a system setting (SceSystemParamLang 0..19); English for one with no catalogue. Portugal's
 * Portuguese reads the Brazilian text, Traditional Chinese the simplified. */
int lang_from_system(int sce_lang);

/* The folder that holds lang/ ("app0:" on the console, the repository root on the twin). Call before i18n_use. */
void i18n_dir(const char *dir);
/* Chooses the language (a LANG_* index; LANG_AUTO resolves with `system`, the SceSystemParamLang value). Loads
 * the file; returns the language now in force (English when the file is missing or unreadable). Frees the
 * previous catalogue: pointers tr() gave before are void. Bumps i18n_serial(). */
int i18n_use(int lang, int system);
int i18n_current();                 /* never LANG_AUTO */
unsigned i18n_serial();             /* counts i18n_use calls: a screen that cached texts rebuilds when it changes */
int i18n_loaded_count();            /* texts of the catalogue in force (0 for English: the compiled table) */
int i18n_refused_count();           /* lines of the file refused or dropped */

/* The text of a key in the language in force; English when it has none; the key itself for a key nobody knows.
 * Never NULL. */
const char *tr(const char *key);
/* The English text of a key (logs and tests read English whatever the setting). */
const char *tr_en(const char *key);
/* The text of a key as a printf format with the arguments filled in. */
std::string trf(const char *key, ...);      /* (no format attribute: the key is not the format; the loader checks the conversions) */
/* A key in a static table: the key itself, to be passed to tr() when the table is used (tools read these). */
#define PK_TR_KEY(k) (k)

/* tr() or tr_en() (en: log lines say English whatever the setting) */
inline const char *trl(const char *key, bool en) { return en ? tr_en(key) : tr(key); }

/* The English table, for the tests and the generator's check. */
size_t i18n_en_count();
const char *i18n_en_key(size_t i);
const char *i18n_en_text(size_t i);

/* The loader on its own: parses `len` bytes of a catalogue the way a file is read (BOM, CRLF, malformed lines)
 * and calls back for each good line with the unescaped text. buf must have one spare byte after len (it is
 * edited in place). Returns the number of lines refused. */
typedef void (*i18n_line_fn)(void *user, const char *key, const char *text);
int i18n_parse(char *buf, size_t len, i18n_line_fn fn, void *user);
/* The pseudo-locale's text for an English one: about 35% longer, with accents (conversions untouched). */
void i18n_pseudo(const char *en, char *out, size_t n);
/* true when two texts hold the same printf conversions in the same order */
bool i18n_same_formats(const char *a, const char *b);
/* Loads a catalogue from memory (a copy is parsed), as i18n_use does a file; for the tests. Returns the
 * number of texts kept (0: not loaded, English stays). */
int i18n_use_text(const char *data, size_t len, int lang = LANG_ES);

}  // namespace pk

#endif
