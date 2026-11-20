# Sega cheat codes in cheats.db (Genesis, Master System, Game Gear, SG-1000)

What `tools/make_cheatdb.py` writes for the four Sega consoles and what the
player's cheat hook (`md_host_cheat_*`) must do with it. The test vectors are in
`sega_vectors.txt`; `test_sega_codes.py` runs them.

## The database

- System numbers in the game table: 6 Genesis / Mega Drive, 7 Master System,
  8 Game Gear, 9 SG-1000. Name-key letters: `I`, `J`, `K`, `L` (the game
  database's system letters, `gamedb.h`): `cheatdb_find_name('I', path, &exact)`.
- A game is found by the CRC-32 of the cartridge as No-Intro lists it (for
  Genesis the plain ROM: an SMD file is de-interleaved first,
  `gamedb_md_crc_cancel`; Master System, Game Gear and SG-1000 the whole image
  without a 512-byte copier header, `gamedb_sms_crc_cancel`), then by name.
- Source: libretro-database `cht/<console>/*.cht` at revision d5bae90 (the pin
  of the other consoles), No-Intro lists of the same revision.
- A cheat's code is one or more parts joined by `+`; every part is canonical
  (upper-case hex, no spaces).

## Part forms

| console | part | meaning |
|---|---|---|
| Genesis | `AAAAAA:VVVV` | 24-bit address, 16-bit value |
| Master System, Game Gear, SG-1000 | `AAAA:VV` | Z80 address, 8-bit value |
| | `AAAA:VV:CC` | the same, applied only while the byte there is `CC` |

The generator reads these inputs (all case-insensitive, parts separated by `+`
or white space):

- Genesis Game Genie `XXXX-XXXX` (letters `ABCDEFGHJKLMNPRSTVWXYZ0123456789`):
  `md_gg_decode` in `make_cheatdb.py` (a copy of Genesis Plus GX's
  `decode_cheat`) gives address and word. Written `AAAAAA:VVVV`.
- Genesis Pro Action Replay `AAAAAA:VVVV`, and `AAAAAA:VV` (a byte, only for a
  work-RAM address, written `AAAAAA:00VV`).
- Master System / Game Gear Game Genie `DDA-AAB` and `DDA-AAB-RRr`:
  data `DD`, address `A-AA` with the high nibble `B` inverted, compare byte from
  the first and third digit of the last group (2-bit right rotation, then XOR
  `BA`). `sms_gg_decode`.
- Pro Action Replay `00AA-AAVV` (the type byte is always `00` in the lists),
  Fusion RAM `AAAA:VV`, Fusion ROM `RRAAAA:VV` (compare `RR`).

## What the hook does

Genesis (`AAAAAA:VVVV`):
- Address `FF0000`-`FFFFFF`: work RAM, written every frame. If the high byte of
  the value is 0 it is a byte store at exactly that address (`address & 0xFFFF`
  of the 64 KiB work RAM; odd addresses allowed); otherwise a big-endian word
  store at `address & 0xFFFE`.
- Address below `400000`: a 16-bit ROM patch, applied when the cheat is switched
  on and undone when it is switched off: the big-endian word at
  `address & 0xFFFFFE` becomes the value. Ignored when the address is not inside
  the ROM.
- Anything in between never shipped.

Master System / Game Gear / SG-1000 (`AAAA:VV[:CC]`):
- Address `C000`-`FFFF`: work RAM, written every frame at `address & 0x1FFF`
  (8 KiB RAM, `E000`-`FFFF` mirrors it).
- Address below `C000`: a ROM patch on the Z80 address as the mapper shows it
  now, so it follows bank switches (applied again after every bank change, as
  Genesis Plus GX's `ROMCheatUpdate` does), and with `:CC` only while the ROM byte
  there is `CC`. Genesis Plus GX treats a compare of `00` as no compare
  (`decode_cheat` stores it in the same field as "none"); 160 parts in the
  database have `:00`. Follow Genesis Plus GX unless there is a reason not to.
- Genesis Plus GX keeps at most 150 cheats (`MAX_CHEATS`) for the whole game, one
  per part.

## Where this differs from Genesis Plus GX (checked against its source)

- `AAAAAA:VV` (two digits): `decode_cheat` reads it as `VV00` from the digits
  after the colon and whatever follows in memory (single-line cases give `VV00`:
  a word store of `VV00` at the even address). The lists mean a byte, so the
  database writes `AAAAAA:00VV`, which Genesis Plus GX reads as the byte. 2,156
  distinct parts are affected.
- Lower-case hex: `decode_cheat` refuses it (`strchr` on upper-case digits), the
  lists have some. The database upper-cases.
- Every other part that ships decodes to the same address, data and compare as
  `decode_cheat` gives the original text (46,631 distinct parts of the d5bae90
  lists; `check_gpgx.py` in the working notes does it against the real function).

## What is dropped (counted in cheats.db.txt)

An entry is shipped whole or not at all.
- `not a code`: a form that is none of the above (five-digit addresses, no colon,
  a byte store of ROM, Game Genie letters that do not exist, 8-digit lines,
  Xploder-like forms, text from a broken file, "N/A").
- `placeholder`: `?`, `*` or `X` where the player would choose a value.
- `unreachable address` (Genesis): between `400000` and `FEFFFF`.
- Entries with no code, duplicates (equal after canonical form), and lists over
  1000 cheats are left out as for the other consoles.
