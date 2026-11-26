# Translations: what native speakers should check

The seven catalogues (`<code>.txt` for the app, `pk_<code>.txt` for the Super NES and Sega players) are machine
translations. They were checked for fit on every page the tests walk (PC test twin, Normal and Large text), not for
style. Below, per language, the words and choices worth a native eye. Fixes are welcome as plain edits to the files;
run `python3 tools/i18n/check_catalogues.py` afterwards.

## Every language

- The terms of each language's glossary (save, save state, slot, cheat, rewind, fast-forward, room, host, join,
  relay, link, cover, starred): are they the words the Vita and common games use?
- Texts added late, with no translator of their own (written by the integrator from the glossaries): the Game Boy core
  row, its values (Standard / Accurate) and help lines, the toasts about a state of the other Game Boy core, link and
  two-cartridge play with the Accurate core, the Settings values that were English (On/Off everywhere, Theme
  Light/Dark, Game list look Home / Marquee / Shelf, Button mapping Default/Custom, Reverse, Level Set, Export save
  Plain .sav, Extra CPU core In use / Not available, Shown/Hidden), the help lines of Export save, Camera, Overlay and
  CPU speed, the Save states slot words (saved, empty, old), the Play together words (quick, fine, signal, chosen,
  full, now: yours / the partner's, OFF), the cheat list words (none, unsupported, yours, NAME, results), the Button
  test stick and rear-panel labels, the rewind chip (`<< REWIND 0:12`, at most about 23 characters) and rewind
  notices, the smoothing note, "played 3 days ago / in October / 2 h 10 min" on the home screen, the empty game list
  line and the camera toasts. Keys: see `en.txt`, they carry a `# context:` line.
- Shortened to fit the screen (please check they still read well): the home screen's Settings hint, the in-game
  menu's Settings card and some card descriptions, the Cheats, Add a code, Button mapping, Save states and code pad
  hint lines (see the language sections).
- The players' PAL speed help (`player.european_games_run_at_50_hz`) was rewritten in every language once "20 %"
  became plain text.

## Spanish (es)

- Neutral Spanish with tú. "partida" for a save file, "estado guardado" for a save state, "ranura" for a slot.
- "Nearby" was kept in English; "Cable Link" / "Adaptador inalámbrico" for the GBA accessories.
- Shortened: `hint.x_on_off_tri_add_*` (act/des, START memoria), `hint.x_type_sq_delete_l_keyboard` (letra, listo),
  `hint.x_rebind_sq_none_tri_default` (vacío = sin uso), `menu.save_or_load_the_game_of`,
  `menu.save_the_last_ten_seconds_as`, `ui.rewind_chip_oldest` ("tope").
- "una Game Boy" (feminine) is used throughout.

## French (fr, France)

- Tu form. "état" for a save state, "emplacement" for a slot, "Retour arrière" for rewind, "Codes de triche".
- Section titles À propos, Bibliothèque and Réglages de liaison are written in sentence case (a workaround for an old
  bug, now fixed); the other titles are in capitals. Either is fine now; pick one.
- Shortened: `home.settings` and `menu.settings` (Réglages), `hint.x_on_off_tri_add_*` (on/off, SELECT trouver,
  START mémoire), `menu.save_the_last_ten_seconds_as`, `ui.rewind_chip_oldest` ("limite").
- Units Ko / Mo / i/s; "UP/DOWN" became HAUT/BAS in hints.

## German (de)

- Du form. "Savestate", "Spielstand", "Zurückspulen", "Vorspulen", "Raum".
- Shortened: `home.settings` and `menu.settings` (Optionen instead of the Vita's Einstellungen), `hint.x_on_off_tri_add_*`
  ({tri} Neu), `hint.x_type_sq_delete_l_keyboard` (Entf., OK), `hint.dpad_letter_move_x_pad_sq` (Zeichen, Cursor,
  {tri} Los), `hint.x_add_remove_sq_rename_tri` (Hinzuf./Entf.), `hint.x_load_sq_save_tri_delete` (Sichern, Sperre,
  Rückg.), `menu.link_up_over_wi_fi_ad`, `menu.save_the_last_ten_seconds_as`, the empty game list line.
- `ui.save_badge` "SPIELST." (the Shelf look's badge) is an abbreviation: a better short word is welcome.

## Italian (it)

- Tu form; On/Off kept as the Vita does. "stato salvato", "trucco", "Stanza", "Riavvolgi".
- Shortened: `home.settings` (Opzioni), `hint.x_rebind_sq_none_tri_default` (predef., vuoto = off).
- `ui.save_badge` "SALV." is an abbreviation.

## Brazilian Portuguese (pt-BR)

- "você"; "Estado salvo", "Slot", "Cheat", "Sala", "Retransmissão"; "Voltar no tempo" for rewind (Rebobinar in hints).
- Shortened: `menu.settings` (Opções), `hint.x_on_off_tri_add_*` ({tri} novo, START memória),
  `menu.save_the_last_ten_seconds_as`, `ui.rewind_chip_oldest` ("limite").
- "um Game Boy" (masculine) is used throughout.

## Japanese (ja)

- System-menu tone; full-width punctuation; button glyphs kept, and added in a few texts where English names the button
  in words ("{o}がA").
- Dates read 2026年10月3日 (order ymd, months 1月..12月).
- Shortened: `ui.nothing_saved_here` (何も保存されていません), `ui.slot_saved` (スロット%dにセーブ), the empty game
  list line, help lines and the clip card label (by the translator).
- The PAL speed help uses the full-width ％.
- Super NES is スーパーファミコン in prose; check the Game Boy core texts (高精度 / 標準) and スーパーゲームボーイ.

## Simplified Chinese (zh-Hans)

- 即时存档 for a save state, 存档位 for a slot, 金手指 for cheats, 倒带 for rewind, 联机 for link.
- Dates read 2026年10月3日 (order ymd).
- The PAL speed help uses the full-width ％.
- Values chosen by the integrator: 大图 (Marquee look), 书架 (Shelf look), 已关闭 (Mystery Gift off): please check.
- Traditional Chinese users get this text for now.
