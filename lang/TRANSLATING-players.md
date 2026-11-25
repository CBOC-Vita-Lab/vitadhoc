# Translating the players

## Players

The two players (the SNES player and the Sega player) and the library they share (playerkit) read their texts from
`lang/pk_<code>.txt`. This is a separate set of files from the main app's catalogues, with its own keys, so a
translator can do one without the other. English is the source and is compiled in: the players work with no
catalogue at all, and any key a catalogue lacks shows in English.

### Files

| Language | File |
| --- | --- |
| English (source, compiled in) | `lang/pk_en.txt` |
| Spanish | `lang/pk_es.txt` |
| French | `lang/pk_fr.txt` |
| German | `lang/pk_de.txt` |
| Italian | `lang/pk_it.txt` |
| Portuguese (Brazil) | `lang/pk_pt-BR.txt` |
| Japanese | `lang/pk_ja.txt` |
| Simplified Chinese | `lang/pk_zh-Hans.txt` |

Copy `pk_en.txt`, translate the text after the tab on each line, keep the keys. You may leave a line out (it
shows in English) but do not leave one empty.

### Format

- UTF-8, one entry per line: `key<TAB>text`. A byte order mark and Windows line endings are accepted.
- A line starting with `#` is a comment. The `# context:` line above a key says where the text shows.
- `\n` is a line break, `\t` a tab, `\\` a backslash. Nothing else needs escaping.
- Keys are never translated and never changed. Keys that start with `snes.` or `md.` belong to one player; the
  others (`menu.`, `input.`, `player.`, ...) are shared.
- Keep every `%` conversion of the English text: the same conversions, in the same order (`%d`, `%s`, `%u`, ...).
  The players have no positional arguments, so word order that needs the values swapped cannot be written with
  them: reword the sentence instead. A translated line whose conversions differ from the English one is ignored
  and the English shows.
- A line is at most 4096 characters, a file at most 1 MiB.

### Dates

A state's date is day, month name, year (`3 Oct 2026`). The month names are `states.jan` to `states.dec`.
`states.date_order` is not shown: set it to `dmy`, `mdy` (Oct 3, 2026) or `ymd` (2026 Oct 3) to match the
language, and nothing else. A literal percent sign is written `%%`; a lone `20 % fast` (a percent sign
followed by a space) is read as text, not as a conversion.

### Length

The screen is small and a text is never shrunk to fit: a long one is wrapped or ends in an ellipsis. Keep
translations about as long as the English; the longest ones (help lines under a row, notices) are the first to be cut.
Names of buttons (Cross, Circle, L, R, Start, Select) stay as the console prints them.

### Trying it

Settings > Interface > Language picks the language (Automatic follows the Vita's system language; an unsupported
system language gives English). The choice is saved in `display.ini` as `language=` and is shared with the main app.
The pseudo-language `language=xx-long` (not in the picker) makes every text about 35% longer with accented
letters, to find what does not fit; the tests run every screen in it.

### Checking

`make -C playerkit i18n-check` checks that the compiled English table matches `lang/pk_en.txt`. After adding or
changing a key in `pk_en.txt` run `python3 playerkit/tools/gen_strings.py` to rebuild
`playerkit/src/pk_strings_en.inc`. Keys stay sorted (the loader searches them with a binary search).

### What is not translated

The log (`log.txt`), file names, config keys and the text the cores print stay in English on purpose, so that
reports are readable by everyone.
