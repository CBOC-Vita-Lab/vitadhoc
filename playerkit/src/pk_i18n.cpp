/* The players' interface languages (pk_i18n.h). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_i18n.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace pk {

#include "pk_strings_en.inc"      /* kEn[]: the English table, sorted by key */
static const size_t kEnN = sizeof(kEn) / sizeof(kEn[0]);

const char *const kLangCode[LANG_COUNT] = { "auto", "en", "es", "fr", "de", "it", "pt-BR", "ja", "zh-Hans", "xx-long" };
const char *const kLangNative[LANG_COUNT] = {
	"Automatic", "English", "Espa\xc3\xb1ol", "Fran\xc3\xa7" "ais", "Deutsch", "Italiano", "Portugu\xc3\xaas (Brasil)",
	"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87", "Long test" };

namespace {
const size_t kMaxFile = 1u << 20;        /* a catalogue past 1 MiB is not one */
const size_t kMaxLine = 4096;

struct Ent { const char *key; const char *val; };

char g_dir[256];
int g_lang = LANG_EN;
Ent *g_ent = 0;                          /* the catalogue in force, sorted; 0: English */
int g_n = 0, g_refused = 0;
void *g_mem = 0;                         /* the one allocation: entries, then text */
unsigned g_serial = 0;
}  // namespace

int lang_index(const char *code)
{
	if (!code) return LANG_AUTO;
	for (int i = 0; i < LANG_COUNT; i++) {
		const char *a = code, *b = kLangCode[i];
		while (*a && *b && tolower((unsigned char) *a) == tolower((unsigned char) *b)) a++, b++;
		if (!*a && !*b) return i;
	}
	return LANG_AUTO;
}

int lang_from_system(int sce)
{
	switch (sce) {
	case 0: return LANG_JA;
	case 2: return LANG_FR;
	case 3: return LANG_ES;
	case 4: return LANG_DE;
	case 5: return LANG_IT;
	case 7: case 17: return LANG_PTBR;
	case 10: case 11: return LANG_ZHS;
	default: return LANG_EN;
	}
}

void i18n_dir(const char *dir) { snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : ""); }

/* ---- the printf conversions of a text ---------------------------------- */
/* "%-8s %lu%%" -> "slu": the type letters in order ('*' for a width from an argument); -1 for a form this
 * scheme refuses (positional, %n, a lone %) */
static int conv_sig(const char *s, char *out, size_t n)
{
	size_t k = 0;
	for (; *s; s++) {
		if (*s != '%') continue;
		s++;
		if (*s == '%') continue;
		for (; *s && strchr("-+ #0123456789.*hlLzjtq", *s); s++) {
			if (*s == '*' && k + 1 < n) out[k++] = '*';
			else if (strchr("hlLzjtq", *s) && k + 1 < n) out[k++] = *s;
		}
		if (!*s || !strchr("diouxXcsfFeEgGp", *s)) return -1;
		if (k + 2 >= n) return -1;
		out[k++] = *s;
		out[k++] = ',';
	}
	out[k] = 0;
	return (int) k;
}

bool i18n_same_formats(const char *a, const char *b)
{
	char x[128], y[128];
	if (conv_sig(a, x, sizeof x) < 0 || conv_sig(b, y, sizeof y) < 0) return false;
	return strcmp(x, y) == 0;
}

/* ---- the file ------------------------------------------------------------ */
static bool key_ok(const char *k)
{
	if (!*k) return false;
	for (; *k; k++) if (!isalnum((unsigned char) *k) && *k != '.' && *k != '_' && *k != '-') return false;
	return true;
}

/* unescapes in place; false when the text holds a form it does not know */
static bool unescape(char *s)
{
	char *w = s;
	for (; *s; s++) {
		if (*s == '\\') {
			s++;
			if (*s == 'n') *w++ = '\n';
			else if (*s == 't') *w++ = '\t';
			else if (*s == '\\') *w++ = '\\';
			else return false;
		} else if ((unsigned char) *s < 0x20 && *s != '\t') return false;
		else *w++ = *s;
	}
	*w = 0;
	return true;
}

/* UTF-8 a font can draw: well formed, no overlong forms, no surrogates */
static bool utf8_ok(const unsigned char *s)
{
	while (*s) {
		int n;
		unsigned cp;
		if (*s < 0x80) { s++; continue; }
		if ((*s & 0xe0) == 0xc0) { n = 1; cp = *s & 0x1f; }
		else if ((*s & 0xf0) == 0xe0) { n = 2; cp = *s & 0x0f; }
		else if ((*s & 0xf8) == 0xf0) { n = 3; cp = *s & 0x07; }
		else return false;
		const int len = n;
		for (s++; n; n--, s++) {
			if ((*s & 0xc0) != 0x80) return false;
			cp = (cp << 6) | (*s & 0x3f);
		}
		if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
		if ((len == 1 && cp < 0x80) || (len == 2 && cp < 0x800) || (len == 3 && cp < 0x10000)) return false;
	}
	return true;
}

int i18n_parse(char *buf, size_t len, i18n_line_fn fn, void *user)
{
	char *p = buf, *end = buf + len;
	int refused = 0;
	if (len >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) p += 3;
	while (p < end) {
		char *e = (char *) memchr(p, '\n', (size_t) (end - p)), *tab, *line = p;
		if (!e) e = end;
		p = e + 1;
		size_t ll = (size_t) (e - line);
		if (ll && line[ll - 1] == '\r') ll--;
		line[ll] = 0;       /* e is a '\n' or the end: the buffer has a spare byte */
		if (!ll || line[0] == '#') continue;
		if (ll > kMaxLine || !(tab = strchr(line, '\t'))) { refused++; continue; }
		*tab = 0;
		if (!key_ok(line) || !tab[1] || !unescape(tab + 1) || !tab[1] || !utf8_ok((unsigned char *) tab + 1)) { refused++; continue; }
		fn(user, line, tab + 1);
	}
	return refused;
}

/* by key, a key given twice in file order (the text of the later line sits at the higher address) */
static int cmp_ent(const void *a, const void *b)
{
	const Ent *x = (const Ent *) a, *y = (const Ent *) b;
	const int c = strcmp(x->key, y->key);
	return c ? c : (x->val < y->val ? -1 : x->val > y->val ? 1 : 0);
}

static const char *en_find(const char *key)
{
	int lo = 0, hi = (int) kEnN - 1;
	while (lo <= hi) {
		const int mid = (lo + hi) / 2, c = strcmp(key, kEn[mid].key);
		if (!c) return kEn[mid].val;
		if (c < 0) hi = mid - 1; else lo = mid + 1;
	}
	return 0;
}

namespace {
struct Collect { Ent *e; int n, cap, dropped; };
}

static void on_line(void *u, const char *key, const char *text)
{
	Collect *c = (Collect *) u;
	const char *en = en_find(key);
	if (!en || !i18n_same_formats(en, text)) { c->dropped++; return; }   /* a key this build has no use for, or a format that does not fit */
	if (c->n < c->cap) { c->e[c->n].key = key; c->e[c->n].val = text; c->n++; }
}

static void drop()
{
	free(g_mem);
	g_mem = 0; g_ent = 0; g_n = g_refused = 0;
}

/* a buffer into one allocation: [entries][text]; entries point into the text, whose lines are cut and unescaped in place */
static int load_mem(const char *data, size_t sz)
{
	if (sz == 0 || sz > kMaxFile) return 0;
	const size_t cap_e = kEnN;
	g_mem = malloc(cap_e * sizeof(Ent) + sz + 2);
	if (!g_mem) return 0;
	char *text = (char *) g_mem + cap_e * sizeof(Ent);
	memcpy(text, data, sz);
	text[sz] = 0;
	Collect c; c.e = (Ent *) g_mem; c.n = 0; c.cap = (int) cap_e; c.dropped = 0;
	g_refused = i18n_parse(text, sz, on_line, &c) + c.dropped;
	if (c.n <= 0) { drop(); return 0; }
	qsort(c.e, (size_t) c.n, sizeof(Ent), cmp_ent);
	int w = 0;      /* a key given twice: the last one read wins */
	for (int i = 0; i < c.n; i++) if (i + 1 >= c.n || strcmp(c.e[i].key, c.e[i + 1].key)) c.e[w++] = c.e[i];
	g_ent = c.e; g_n = w;
	return g_n;
}

static int load_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	long sz;
	if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) <= 0 || (unsigned long) sz > kMaxFile || fseek(f, 0, SEEK_SET)) { fclose(f); return 0; }
	char *tmp = (char *) malloc((size_t) sz);
	if (!tmp) { fclose(f); return 0; }
	const bool ok = fread(tmp, 1, (size_t) sz, f) == (size_t) sz;
	fclose(f);
	const int n = ok ? load_mem(tmp, (size_t) sz) : 0;
	free(tmp);
	return n;
}

/* ---- the pseudo-locale ----------------------------------------------------- */
void i18n_pseudo(const char *en, char *out, size_t n)
{
	/* a..z: an accented form, or "" */
	static const char *const acc[26] = {
		"\xc3\xa0", "", "", "", "\xc3\xa9", "", "", "", "\xc3\xad", "", "", "",
		"", "\xc3\xb1", "\xc3\xb6", "", "", "", "", "", "\xc3\xbc", "", "", "", "", "" };
	static const char *const pad[4] = { "\xc3\xab", "\xc3\xb6", "\xc3\xbc", "\xc3\xaf" };
	size_t k = 0;
	int run = 0, pi = 0;
	if (!n) return;
	while (*en && k + 8 < n) {
		const unsigned char c = (unsigned char) *en;
		if (c == '%') {
			if (en[1] == '%') { out[k++] = *en++; out[k++] = *en++; continue; }
			out[k++] = *en++;                 /* a conversion: untouched */
			while (*en && strchr("-+ #0123456789.*hlLzjtq", *en)) out[k++] = *en++;
			if (*en) out[k++] = *en++;
			run = 0;
			continue;
		}
		if (c == '\\' ) { out[k++] = *en++; continue; }
		if (isalpha(c)) {
			const char *a = c >= 'a' && c <= 'z' ? acc[c - 'a'] : "";
			if (*a) for (; *a; a++) out[k++] = *a;
			else out[k++] = (char) c;
			en++;
			if (++run % 2 == 0) {
				const char *p = pad[pi++ & 3];
				out[k++] = p[0]; out[k++] = p[1];
			}
			continue;
		}
		if (c >= 0x80) {                      /* already beyond ASCII: copy the whole character */
			out[k++] = *en++;
			while (((unsigned char) *en & 0xc0) == 0x80 && k + 4 < n) out[k++] = *en++;
			run = 0;
			continue;
		}
		run = 0;
		out[k++] = *en++;
	}
	out[k] = 0;
}

static int load_pseudo()
{
	size_t tb = 0;
	for (size_t i = 0; i < kEnN; i++) tb += strlen(kEn[i].val) * 3 + 1;
	g_mem = malloc(kEnN * sizeof(Ent) + tb);
	if (!g_mem) return 0;
	Ent *e = (Ent *) g_mem;
	char *t = (char *) g_mem + kEnN * sizeof(Ent);
	for (size_t i = 0; i < kEnN; i++) {
		const size_t room = strlen(kEn[i].val) * 3 + 1;
		e[i].key = kEn[i].key;
		e[i].val = t;
		i18n_pseudo(kEn[i].val, t, room);
		t += strlen(t) + 1;
	}
	g_ent = e; g_n = (int) kEnN;
	return 1;
}

int i18n_use(int lang, int system)
{
	drop();
	g_serial++;
	if (lang == LANG_AUTO) lang = lang_from_system(system);
	if (lang <= LANG_AUTO || lang >= LANG_COUNT) lang = LANG_EN;
	g_lang = LANG_EN;
	if (lang == LANG_PSEUDO) {
		if (load_pseudo()) g_lang = LANG_PSEUDO;
	} else if (lang != LANG_EN) {
		char path[320];
		const size_t dl = strlen(g_dir);
		snprintf(path, sizeof path, "%s%slang/pk_%s.txt", g_dir, dl && g_dir[dl - 1] != '/' && g_dir[dl - 1] != ':' ? "/" : "", kLangCode[lang]);
		if (load_file(path)) g_lang = lang;
	}
	return g_lang;
}

int i18n_use_text(const char *data, size_t len)
{
	drop();
	g_serial++;
	g_lang = LANG_EN;
	const int n = data ? load_mem(data, len) : 0;
	if (n) g_lang = LANG_ES;      /* "some other language": the tests only look at the texts */
	return n;
}

int i18n_current() { return g_lang; }
unsigned i18n_serial() { return g_serial; }
int i18n_loaded_count() { return g_n; }
int i18n_refused_count() { return g_refused; }

const char *tr_en(const char *key)
{
	const char *v = key ? en_find(key) : 0;
	return v ? v : (key ? key : "");
}

const char *tr(const char *key)
{
	if (key && g_ent) {
		int lo = 0, hi = g_n - 1;
		while (lo <= hi) {
			const int mid = (lo + hi) / 2, c = strcmp(key, g_ent[mid].key);
			if (!c) return g_ent[mid].val;
			if (c < 0) hi = mid - 1; else lo = mid + 1;
		}
	}
	return tr_en(key);
}

std::string trf(const char *key, ...)
{
	const char *fmt = tr(key);
	va_list ap;
	va_start(ap, key);
	char buf[512];
	va_list ap2;
	va_copy(ap2, ap);
	const int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	std::string out;
	if (n < 0) out = fmt;
	else if ((size_t) n < sizeof buf) out = buf;
	else {
		std::string big((size_t) n + 1, '\0');
		vsnprintf(&big[0], big.size(), fmt, ap2);
		big.resize((size_t) n);
		out = big;
	}
	va_end(ap2);
	return out;
}

size_t i18n_en_count() { return kEnN; }
const char *i18n_en_key(size_t i) { return i < kEnN ? kEn[i].key : ""; }
const char *i18n_en_text(size_t i) { return i < kEnN ? kEn[i].val : ""; }

}  // namespace pk
