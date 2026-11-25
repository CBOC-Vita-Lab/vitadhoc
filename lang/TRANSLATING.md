# Translating VitAdhoc

One file per language in this folder: `lang/<code>.txt`. `en.txt` is the source and is
also compiled into the app; it is the fallback for any key a language leaves out.

| Code | Language | Code | Language |
|---|---|---|---|
| `es` | Spanish | `it` | Italian |
| `fr` | French | `pt-BR` | Brazilian Portuguese |
| `de` | German | `ja` | Japanese |
| `zh-Hans` | Simplified Chinese | | |

Traditional Chinese readers get the simplified text for now.

## File format

- One line per text: `key<TAB>text`. Copy the key exactly; never translate or reorder it.
- Lines starting with `#` are comments. `en.txt` has a `# context:` line above each key
  telling where the text is used. Keep or drop them in your file; they are ignored.
- `\n` inside a text is a line break; `\\` is a backslash. Nothing else is escaped.
- UTF-8 only, no byte-order mark needed (one is tolerated). LF or CRLF line ends both work.
- A missing key shows the English text. A bad line (no tab, bad escape, broken UTF-8,
  control characters) is skipped and the rest of the file still loads.
- Sorting does not matter to the app, but keep the keys in the same order as `en.txt`
  so a diff shows what changed.

## Placeholders

- Keep every `%s` and `%d` (and `%u`, `%c`, `%.1f`, ...) exactly as in English, in the same
  order. There are no positional forms (`%1$s`), so reword the sentence around them rather
  than swapping them. A line whose conversions differ from English is dropped at load time.
  Write a literal percent sign as `%%`.
- Button glyphs are written `{x}` `{o}` `{tri}` `{sq}` `{star}` `{staro}`. Keep them as they are;
  the app draws the console symbol. Put them where the English has them.
- `START`, `SELECT`, `L`, `R`, `DPAD` in hint lines are button names printed on the console.
  Keep them in capitals and in Latin letters in every language.

## Hint lines (the footers)

Hints such as `{x} open   {o} back` are split by runs of two or more spaces. Keep the
two (or more) spaces between hints and a single space inside one. Each hint must keep its
button (glyph or button word) at the start. Do not add a full stop.

## Tone

Short, plain and friendly, like the English. Talk to the player directly ("you"), use the
informal form where the language has one that is normal in games (tu, du, tu/voce). Prefer
everyday words over technical ones. No exclamation marks unless English has one. Sentence
case, not Title Case. Section headers in capitals in English (`ABOUT`) are capitals in
languages that have them; in Japanese and Chinese just translate them.

## Names that stay as they are

VitAdhoc, Vita, PS Vita, PSTV, Wi-Fi, RetroAchievements, Game Boy, GBA, SNES, NES, Mega Drive,
Genesis, WonderSwan, Zapper, DualShock, the names of cores, shaders, and emulators, file
names and file extensions, and anything inside quotes that comes from the player (a game
name, a cheat name).

## Length limits

The screen is 960x544 and text is not shrunk. Long text is wrapped or cut with an ellipsis,
which loses meaning, so aim for the English length or less. To test, set `language=xx-long`
(about 35 % longer, accented) in `display.ini`.

| Key family | Aim for (characters) |
|---|---|
| `hint.*`, footers (the whole line) | the English length, never above 1.2x |
| `settings.*` row labels | 24 or fewer |
| `settings.*` values | 14 or fewer |
| `settings.*` help (one line under a row) | 70 or fewer |
| `toast.*` | 44 or fewer, one line |
| `notice.*` | 2 lines of 48 |
| `play.*` (the Play together pages) | the English length; lines wrap at about 50 |
| `home.*`, `cover.*`, `lib.*`, `list.*` (cards, tiles, rows) | 22 or fewer for a title, 40 for a line |
| `ui.*` | the English length |
| `cheats.*`, `share.*`, `online.*`, `link.*` | the English length, 1.2x at most |

Chinese and Japanese are about half as wide per character as the limits above count;
that is fine.

## Plurals

Counts come as a pair: `key_one` (exactly 1) and `key_many` (any other number, 0 included).
Translate both. Languages with one form use the same text twice.

## Fragments

A few texts finish a sentence the app builds, for example "(the host)" or the text after a
game name. They are marked in the `# context:` line. Keep them short and make them work in
the position described.

## Checking a file

1. Copy it next to `en.txt`, set `language=<code>` in `display.ini` (or choose it in
   Settings > Interface > Language), and walk the screens.
2. A string you left in English, or a key that is wrong, is easy to spot; the test build can
   list them (`GBADHOC_TR_AUDIT=1`).
3. The log lines of the app stay in English and are not part of the catalogue.
