/*
 * The settings every console shares, whichever program is running: Screen (the colour profile), Text size and
 * High contrast. They live in display.ini in the app's data folder (the folder of config.ini): the app writes it
 * from Settings, snes.bin and md.bin read it at start and write it when their menu changes it. Plain key=value
 * lines through the safe write; unknown keys stay.
 *
 *   screen=auto|oled|lcd|pstv     text_size=normal|large     contrast=off|on
 *   language=auto|en|es|fr|de|it|pt-BR|ja|zh-Hans      (pk_i18n.h; the app has the same line)
 *
 * Header only (pk_util's Ini is all it needs).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_UIPREFS_H
#define PK_UIPREFS_H

#include "pk_i18n.h"
#include "pk_screen.h"
#include "pk_util.h"
#include <string>

namespace pk {

struct UiPrefs {
	int screen;        /* PK_SCREEN_AUTO .. PK_SCREEN_PSTV */
	int text_large;    /* 0 Normal, 1 Large */
	int contrast;      /* 0 Off, 1 On */
	int language;      /* LANG_AUTO .. LANG_ZHS (pk_i18n.h) */
	UiPrefs() : screen(0), text_large(0), contrast(0), language(0) {}
};

inline std::string uiprefs_path(const std::string &root) { return path_join(root, "display.ini"); }

inline UiPrefs uiprefs_parse(const Ini &ini)
{
	UiPrefs p;
	const std::string s = lower(ini.get("screen", "auto"));
	for (int i = 0; i < PK_SCREEN_CHOICES; i++) if (s == pk_screen_tag(i)) p.screen = i;
	p.text_large = lower(ini.get("text_size", "normal")) == "large" ? 1 : 0;
	const std::string c = lower(ini.get("contrast", "off"));
	p.contrast = c == "on" || c == "1" ? 1 : 0;
	{
		const int l = lang_index(ini.get("language", "auto").c_str());
		p.language = l >= LANG_CHOICES ? LANG_AUTO : l;      /* an unknown value, or the test language: Automatic */
	}
	return p;
}

inline UiPrefs uiprefs_load(const std::string &root)
{
	Ini ini;
	ini.load(uiprefs_path(root));
	return uiprefs_parse(ini);
}

/* false (and nothing changed on the card) when the write failed */
inline bool uiprefs_save(const std::string &root, const UiPrefs &p)
{
	Ini ini;
	ini.load(uiprefs_path(root));
	ini.set("screen", pk_screen_tag(pk_screen_clamp(p.screen)));
	ini.set("text_size", p.text_large ? "large" : "normal");
	ini.set("contrast", p.contrast ? "on" : "off");
	ini.set("language", kLangCode[p.language < 0 || p.language >= LANG_CHOICES ? LANG_AUTO : p.language]);
	return ini_save(uiprefs_path(root), ini) == W_OK;
}

}  // namespace pk

#endif
