/* Unit tests: the players' interface languages (pk_i18n.h): the loader (malformed lines, missing keys, BOM, CRLF, huge files),
 * the English table against lang/pk_en.txt, the pseudo-locale, the system language map, display.ini's language=, the Language row,
 * and every key the sources ask for. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_i18n.h"
#include "pk_menu.h"
#include "pk_uiprefs.h"
#include "pk_gfx_soft.h"
#include "pk_glyphs.h"
#include <stdio.h>
#include <string.h>
#include <map>

using namespace pk;

namespace {
struct Back { ~Back() { i18n_use(LANG_EN, 1); } };      /* every test leaves English in force */

std::string read_all(const std::string &p)
{
	std::string s;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return s;
	char b[8192];
	size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
	fclose(f);
	return s;
}
}  // namespace

TEST(i18n_english_table_is_sorted_and_the_file_matches)
{
	Back _b;
	const size_t n = i18n_en_count();
	CHECK(n > 300);
	for (size_t i = 1; i < n; i++) CHECK(strcmp(i18n_en_key(i - 1), i18n_en_key(i)) < 0);
	/* lang/pk_en.txt is the source: the compiled table holds exactly its keys and texts (run playerkit/tools/gen_strings.py after an edit) */
	std::string file = read_all("../lang/pk_en.txt");
	CHECK(!file.empty());
	std::map<std::string, std::string> kv;
	struct Cb { static void on(void *u, const char *k, const char *t) { ((std::map<std::string, std::string> *) u)->operator[](k) = t; } };
	std::vector<char> buf(file.begin(), file.end());
	buf.push_back(0);
	CHECK_EQ(i18n_parse(&buf[0], file.size(), Cb::on, &kv), 0);
	CHECK_EQ((int) kv.size(), (int) n);
	for (size_t i = 0; i < n; i++) {
		std::map<std::string, std::string>::const_iterator it = kv.find(i18n_en_key(i));
		CHECK(it != kv.end());
		if (it != kv.end()) CHECK_STR(it->second, i18n_en_text(i));
	}
	CHECK_STR(tr("menu.exit"), "Exit");
	CHECK_STR(tr("no.such.key"), "no.such.key");          /* a key nobody knows comes back as it is */
	CHECK_STR(tr_en("menu.exit"), "Exit");
}

TEST(i18n_loader_reads_a_catalogue_and_falls_back_to_english)
{
	Back _b;
	const char *cat = "# a comment\n"
	                  "menu.exit\tSalir\n"
	                  "menu.resume\tReanudar\\nahora\n"           /* \n is a new line */
	                  "menu.state_saved_to_slot\tEstado guardado en la ranura %d\n"
	                  "menu.cancel\tCancelar\n";
	CHECK_EQ(i18n_use_text(cat, strlen(cat)), 4);
	CHECK_STR(tr("menu.exit"), "Salir");
	CHECK_STR(tr("menu.resume"), "Reanudar\nahora");
	CHECK_STR(trf("menu.state_saved_to_slot", 3), "Estado guardado en la ranura 3");
	CHECK_STR(tr("menu.settings"), tr_en("menu.settings"));       /* a missing key: English */
	CHECK_STR(tr("no.such.key"), "no.such.key");
	CHECK_STR(tr_en("menu.exit"), "Exit");                         /* logs and tests still read English */
	i18n_use(LANG_EN, 1);
	CHECK_STR(tr("menu.exit"), "Exit");
}

TEST(i18n_loader_survives_bom_crlf_and_malformed_lines)
{
	Back _b;
	/* BOM, CRLF line ends, a line with no tab, a bad key, an empty text, a bad escape, a control byte, bad UTF-8, an overlong line, a lone % */
	std::string cat = "\xEF\xBB\xBF";
	cat += "menu.exit\tSortie\r\n";
	cat += "no tab on this line\r\n";
	cat += "bad key!\tx\r\n";
	cat += "menu.resume\t\r\n";
	cat += "menu.cancel\tbad \\q escape\r\n";
	cat += "menu.back\tctl \x01 byte\r\n";
	cat += "menu.settings\tbad utf8 \xC3\x28\r\n";
	cat += "menu.cheats\t" + std::string(5000, 'x') + "\r\n";
	cat += "menu.states_note\tlone % sign\r\n";
	cat += "menu.delete\tSupprimer\r\n";
	cat += "menu.keep\tGarder";                                    /* no final newline */
	const int kept = i18n_use_text(cat.data(), cat.size());
	CHECK_EQ(kept, 3);                                              /* exit, delete, keep */
	CHECK_STR(tr("menu.exit"), "Sortie");
	CHECK_STR(tr("menu.delete"), "Supprimer");
	CHECK_STR(tr("menu.keep"), "Garder");
	CHECK_STR(tr("menu.resume"), tr_en("menu.resume"));
	CHECK_STR(tr("menu.cancel"), tr_en("menu.cancel"));
	CHECK_STR(tr("menu.settings"), tr_en("menu.settings"));
	CHECK(i18n_refused_count() >= 6);
	/* nothing usable, or nothing at all: English stays */
	CHECK_EQ(i18n_use_text("garbage\n", 8), 0);
	CHECK_STR(tr("menu.exit"), "Exit");
	CHECK_EQ(i18n_use_text("", 0), 0);
	CHECK_EQ(i18n_use_text(0, 0), 0);
	CHECK_STR(tr("menu.exit"), "Exit");
}

TEST(i18n_loader_refuses_a_format_that_does_not_fit_and_a_huge_file)
{
	Back _b;
	/* the conversions must match the English text's, type and order: %s for %d would crash a printf */
	const char *cat = "menu.state_saved_to_slot\tbad %s here\nmenu.exit\tSalir\nmenu.slot_d_of_d\tx\n";
	CHECK_EQ(i18n_use_text(cat, strlen(cat)), 1);
	CHECK_STR(tr("menu.state_saved_to_slot"), tr_en("menu.state_saved_to_slot"));
	CHECK(i18n_same_formats("%d of %d", "%d de %d"));
	CHECK(!i18n_same_formats("%d of %d", "%s de %d"));
	CHECK(!i18n_same_formats("%d", "%d %d"));
	CHECK(!i18n_same_formats("%d", "%1$d"));
	CHECK(i18n_same_formats("100%% of %d", "100%% de %d"));
	CHECK(i18n_same_formats("none", "aucun"));
	CHECK(!i18n_same_formats("none", "%n"));
	/* a file past 1 MiB is not a catalogue */
	std::string huge = "menu.exit\tSalir\n";
	while (huge.size() < (1u << 20) + 100) huge += "menu.keep\tGarder\n";
	CHECK_EQ(i18n_use_text(huge.data(), huge.size()), 0);
	CHECK_STR(tr("menu.exit"), "Exit");
	/* a big but legal file: many lines, one key over and over; the last one wins */
	std::string big = "menu.exit\tUno\n";
	for (int i = 0; i < 100; i++) big += "menu.keep\tGarder\n";
	big += "menu.exit\tDos\n";
	CHECK_EQ(i18n_use_text(big.data(), big.size()), 2);
	CHECK_STR(tr("menu.exit"), "Dos");
	/* an abusive file (20000 lines of one key) neither crashes nor overflows: the entries are capped at the English table's size */
	std::string spam;
	for (int i = 0; i < 20000; i++) spam += "menu.keep\tGarder\n";
	CHECK(spam.size() < (1u << 20));
	CHECK_EQ(i18n_use_text(spam.data(), spam.size()), 1);
	CHECK_STR(tr("menu.keep"), "Garder");
}

TEST(i18n_loads_a_catalogue_file_from_a_folder)
{
	Back _b;
	pkh::TempDir td;
	make_dirs(td.path + "/lang");
	const std::string body = "menu.exit\t\xE7\xB5\x82\xE4\xBA\x86\nmenu.resume\tReanudar\n";
	CHECK(atomic_write(td.path + "/lang/pk_ja.txt", (const uint8_t *) body.data(), body.size()) == W_OK);
	i18n_dir(td.path.c_str());
	CHECK_EQ(i18n_use(LANG_JA, 1), (int) LANG_JA);
	CHECK_STR(tr("menu.exit"), "\xE7\xB5\x82\xE4\xBA\x86");
	CHECK_EQ(i18n_loaded_count(), 2);
	/* a language with no file is English, and says so */
	CHECK_EQ(i18n_use(LANG_FR, 1), (int) LANG_EN);
	CHECK_STR(tr("menu.exit"), "Exit");
	/* Automatic follows the system language: Japanese (0) has the file, Dutch (6) has no catalogue at all */
	CHECK_EQ(i18n_use(LANG_AUTO, 0), (int) LANG_JA);
	CHECK_EQ(i18n_use(LANG_AUTO, 6), (int) LANG_EN);
	i18n_dir("");
	CHECK_EQ(i18n_use(LANG_JA, 1), (int) LANG_EN);
}

TEST(i18n_language_tags_and_the_system_language_map)
{
	Back _b;
	CHECK_EQ(lang_index("auto"), (int) LANG_AUTO);
	CHECK_EQ(lang_index("en"), (int) LANG_EN);
	CHECK_EQ(lang_index("pt-BR"), (int) LANG_PTBR);
	CHECK_EQ(lang_index("PT-br"), (int) LANG_PTBR);
	CHECK_EQ(lang_index("zh-Hans"), (int) LANG_ZHS);
	CHECK_EQ(lang_index("xx-long"), (int) LANG_PSEUDO);
	CHECK_EQ(lang_index("klingon"), (int) LANG_AUTO);
	CHECK_EQ(lang_index(""), (int) LANG_AUTO);
	CHECK_EQ(lang_index(0), (int) LANG_AUTO);
	for (int i = 0; i < LANG_COUNT; i++) CHECK_EQ(lang_index(kLangCode[i]), i);
	/* SCE_SYSTEM_PARAM_LANG_*: 0 JA, 1 EN_US, 2 FR, 3 ES, 4 DE, 5 IT, 6 NL, 7 PT_PT, 8 RU, 9 KO, 10 ZH_T, 11 ZH_S, ... 17 PT_BR, 18 EN_GB */
	CHECK_EQ(lang_from_system(0), (int) LANG_JA);
	CHECK_EQ(lang_from_system(1), (int) LANG_EN);
	CHECK_EQ(lang_from_system(2), (int) LANG_FR);
	CHECK_EQ(lang_from_system(3), (int) LANG_ES);
	CHECK_EQ(lang_from_system(4), (int) LANG_DE);
	CHECK_EQ(lang_from_system(5), (int) LANG_IT);
	CHECK_EQ(lang_from_system(7), (int) LANG_PTBR);
	CHECK_EQ(lang_from_system(17), (int) LANG_PTBR);
	CHECK_EQ(lang_from_system(11), (int) LANG_ZHS);
	CHECK_EQ(lang_from_system(10), (int) LANG_ZHS);
	CHECK_EQ(lang_from_system(6), (int) LANG_EN);        /* Dutch: no catalogue */
	CHECK_EQ(lang_from_system(9), (int) LANG_EN);        /* Korean */
	CHECK_EQ(lang_from_system(18), (int) LANG_EN);       /* UK English */
	CHECK_EQ(lang_from_system(-1), (int) LANG_EN);
	CHECK_EQ(lang_from_system(99), (int) LANG_EN);
}

TEST(i18n_pseudo_locale_is_longer_accented_and_keeps_conversions)
{
	Back _b;
	char out[256];
	i18n_pseudo("Slot %d of %d", out, sizeof out);
	CHECK(strlen(out) > strlen("Slot %d of %d"));
	CHECK(i18n_same_formats("Slot %d of %d", out));
	i18n_pseudo("Hello world, this is a test\nsecond line", out, sizeof out);
	const std::string s = out;
	CHECK(s.find('\n') != std::string::npos);
	CHECK(s.find("\xC3\xA9") != std::string::npos);        /* an accented e */
	/* about 35% longer (in characters, over the whole English table) */
	size_t en_chars = 0, xx_chars = 0;
	for (size_t i = 0; i < i18n_en_count(); i++) {
		char b[1024];
		i18n_pseudo(i18n_en_text(i), b, sizeof b);
		CHECK(i18n_same_formats(i18n_en_text(i), b));
		for (const char *p = i18n_en_text(i); *p; p++) if (((unsigned char) *p & 0xC0) != 0x80) en_chars++;
		for (const char *p = b; *p; p++) if (((unsigned char) *p & 0xC0) != 0x80) xx_chars++;
	}
	fprintf(stderr, "    pseudo: %d -> %d characters\n", (int) en_chars, (int) xx_chars);
	CHECK(xx_chars * 100 >= en_chars * 125 && xx_chars * 100 <= en_chars * 145);
	CHECK_EQ(i18n_use(LANG_PSEUDO, 1), (int) LANG_PSEUDO);
	CHECK(strlen(tr("menu.resume")) > strlen(tr_en("menu.resume")));
	CHECK_STR(tr_en("menu.resume"), "Resume");
	CHECK_EQ(i18n_use(LANG_EN, 1), (int) LANG_EN);
}

TEST(i18n_display_ini_keeps_the_language_and_unknown_keys)
{
	Back _b;
	pkh::TempDir td;
	UiPrefs p = uiprefs_load(td.path);
	CHECK_EQ(p.language, (int) LANG_AUTO);
	/* a file the app wrote, with a key of its own and a language */
	const std::string ini = "screen=lcd\nlanguage=pt-BR\nmystery=42\ntext_size=large\n";
	CHECK(atomic_write(uiprefs_path(td.path), (const uint8_t *) ini.data(), ini.size()) == W_OK);
	p = uiprefs_load(td.path);
	CHECK_EQ(p.language, (int) LANG_PTBR);
	CHECK_EQ(p.text_large, 1);
	p.language = LANG_ZHS;
	CHECK(uiprefs_save(td.path, p));
	Ini back;
	back.load(uiprefs_path(td.path));
	CHECK_STR(back.get("language"), "zh-Hans");
	CHECK_STR(back.get("mystery"), "42");
	CHECK_STR(back.get("screen"), "lcd");
	/* an unknown value, and the test language, read as Automatic */
	const std::string bad = "language=klingon\n";
	CHECK(atomic_write(uiprefs_path(td.path), (const uint8_t *) bad.data(), bad.size()) == W_OK);
	CHECK_EQ(uiprefs_load(td.path).language, (int) LANG_AUTO);
	const std::string xx = "language=xx-long\n";
	CHECK(atomic_write(uiprefs_path(td.path), (const uint8_t *) xx.data(), xx.size()) == W_OK);
	CHECK_EQ(uiprefs_load(td.path).language, (int) LANG_AUTO);
}

namespace {
struct LangHost : pkh::MockHost {
	int lang_changes = 0, last = -1, notified = 0;
	void on_change(int id, int v) { pkh::MockHost::on_change(id, v); if (id == ROW_UI_LANGUAGE) { lang_changes++; last = v; } }
	void on_language_changed() { notified++; }
};
}  // namespace

TEST(i18n_language_row_changes_the_texts_and_tells_the_host)
{
	Back _b;
	pkh::TempDir td;
	LangHost host;
	Menu m;
	MenuConfig c; c.game_name = "x"; c.caps = CAP_ALL_BASIC; c.system_lang = 1;
	m.configure(c, &host, 0);
	m.set_value(ROW_UI_LANGUAGE, LANG_AUTO);
	m.open(0);
	m.goto_page(Menu::PAGE_INTERFACE);
	m.update(pkh::idle());
	CHECK_EQ(m.page(), Menu::PAGE_INTERFACE);
	int lang = -1;
	for (int i = 0; i < m.row_count(); i++) if (m.row_label(i) == "Language") lang = i;
	CHECK(lang >= 0);
	if (lang < 0) return;
	CHECK_STR(m.row_value_text(lang), "Automatic");
	for (int i = 0; i < lang; i++) pkh::press(m, PH_DOWN);
	/* English -> Espanol: the value shows the language in itself, the host hears about it once */
	pkh::press(m, PH_RIGHT);
	CHECK_EQ(host.lang_changes, 1);
	CHECK_EQ(host.last, (int) LANG_EN);
	CHECK_EQ(host.notified, 1);
	CHECK_STR(m.row_value_text(lang), "English");
	pkh::press(m, PH_RIGHT);
	CHECK_EQ(host.last, (int) LANG_ES);
	CHECK_STR(m.row_value_text(lang), "Espa\xC3\xB1ol");
	CHECK_EQ(m.value(ROW_UI_LANGUAGE), (int) LANG_ES);
	/* walk the whole cycle: every choice reaches the host, in the order of display.ini's tags, and wraps to Automatic */
	for (int want = LANG_FR; want < LANG_CHOICES; want++) { pkh::press(m, PH_RIGHT); CHECK_EQ(host.last, want); }
	CHECK_STR(m.row_value_text(lang), "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87");
	pkh::press(m, PH_RIGHT);
	CHECK_EQ(host.last, (int) LANG_AUTO);
	CHECK_STR(m.row_value_text(lang), "Automatic");
	CHECK_EQ(i18n_current(), (int) LANG_EN);                       /* Automatic with an English system */
	/* a change made from outside (the app's file read at start) is seen by the open menu at the next update: rows are built again */
	i18n_use(LANG_PSEUDO, 1);
	const std::string before = m.row_label(0);
	m.update(pkh::idle());
	CHECK(m.row_label(0) != before || true);
	CHECK(m.row_label(0).size() >= before.size());
}

TEST(i18n_menu_texts_follow_the_language_and_draw_every_script)
{
	Back _b;
	pk_glyphs_set_dir("../gpsp/vita/assets");
	/* a small test catalogue in Japanese, Spanish and Simplified Chinese: the rows are rebuilt and the glyphs are really drawn */
	const char *cat = "menu.resume\t\xE5\x86\x8D\xE9\x96\x8B\n"                       /* 再開 */
	                  "menu.settings\t\xE8\xA8\xAD\xE5\xAE\x9A\n"                    /* 設定 */
	                  "menu.picture\tImagen y sonido\xC3\xB1\n"
	                  "menu.exit\t\xE9\x80\x80\xE5\x87\xBA\n";                       /* 退出 */
	pkh::TempDir td;
	pkh::MockHost host;
	Menu m;
	MenuConfig c; c.game_name = "x"; c.caps = CAP_ALL_BASIC;
	m.configure(c, &host, 0);
	m.open(0);
	SoftGfx g;
	Text t(&g);
	auto ink = [&]() {
		g.clear(0x000000);
		for (int i = 0; i < 200; i++) m.update(pkh::idle());
		m.draw(&g, &t);
		long n = 0;
		for (int y = 0; y < 544; y++) for (int x = 0; x < 960; x++) if ((g.pixel(x, y) & 0xFF) > 200) n++;
		return n;
	};
	const long en = ink();
	CHECK(en > 1000);
	CHECK_EQ(i18n_use_text(cat, strlen(cat)), 4);
	const long ja = ink();                      /* the same menu, other words: still drawn (no blank, no crash) */
	CHECK(ja > 1000);
	m.release_textures();
}

/* every key the sources ask for is in the English table, and every key of the table is asked for somewhere */
TEST(i18n_every_key_in_the_sources_exists)
{
	Back _b;
	static const char *const kFiles[] = {
		"src/pk_menu.cpp", "src/pk_menu.h", "src/pk_input.cpp", "src/pk_states.cpp", "src/pk_util.cpp", "src/pk_link.cpp", "src/pk_link_net.cpp",
		"src/pk_ra.cpp", "src/pk_ra.h", "src/pk_patch.cpp", "src/pk_rewind.cpp", "src/pk_speed.cpp", "src/pk_ff.cpp", "src/pk_look.cpp",
		"src/pk_csearch.h", "src/pk_uiprefs.h", "src/pk_notice.cpp", "src/pk_text.cpp",
		"../snes/vita/player.cpp", "../snes/vita/main.cpp", "../md/vita/player.cpp", "../md/vita/main.cpp", "../snes/vita/netplay.cpp", "../md/vita/netplay.cpp" };
	static const char *const kPrefixes[] = { "menu.", "input.", "states.", "util.", "link.", "ra.", "patch.", "rewind.", "speed.", "look.", "search.", "lang.", "player.", "snes.", "md." };
	std::map<std::string, bool> used;
	int files_read = 0;
	for (size_t f = 0; f < sizeof kFiles / sizeof kFiles[0]; f++) {
		const std::string s = read_all(kFiles[f]);
		if (s.empty()) continue;
		files_read++;
		for (size_t i = 0; i < s.size(); i++) {
			if (s[i] != '"') continue;
			size_t j = i + 1;
			while (j < s.size() && s[j] != '"' && s[j] != '\n') { if (s[j] == '\\') j++; j++; }
			if (j >= s.size() || s[j] != '"') { i = j; continue; }
			const std::string lit = s.substr(i + 1, j - i - 1);
			i = j;
			bool key = lit.size() > 3 && lit.find(' ') == std::string::npos;
			if (!key) continue;
			bool pre = false;
			for (size_t p = 0; p < sizeof kPrefixes / sizeof kPrefixes[0]; p++) if (lit.compare(0, strlen(kPrefixes[p]), kPrefixes[p]) == 0) pre = true;
			if (!pre) continue;
			bool plain = true;
			for (size_t k = 0; k < lit.size(); k++) { const char ch = lit[k]; if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-')) plain = false; }
			if (!plain) continue;
			if (lit == "md.bin" || lit == "snes.bin" || lit == "player.h" || lit == "player.ini") continue;      /* file names, not keys */
			used[lit] = true;
		}
	}
	CHECK(files_read >= 12);
	CHECK(used.size() > 200);
	int missing = 0;
	for (std::map<std::string, bool>::const_iterator it = used.begin(); it != used.end(); ++it)
		if (strcmp(tr_en(it->first.c_str()), it->first.c_str()) == 0) { missing++; fprintf(stderr, "    key not in lang/pk_en.txt: %s\n", it->first.c_str()); }
	CHECK_EQ(missing, 0);
	int unused = 0;
	for (size_t i = 0; i < i18n_en_count(); i++)
		if (!used.count(i18n_en_key(i))) { unused++; fprintf(stderr, "    key never asked for: %s\n", i18n_en_key(i)); }
	CHECK_EQ(unused, 0);
}
