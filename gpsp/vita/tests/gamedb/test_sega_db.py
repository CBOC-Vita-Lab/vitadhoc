"""assets/gamedb.txt's Sega records (2.0): exact counts, every record consistent,
and, when the No-Intro catalogues are at hand, every catalogue CRC resolved.

    python3 test_sega_db.py [DAT_DIR]

DAT_DIR (or $GAMEDB_DAT_DIR) holds libretro-database's metadat/no-intro/*.dat
files (the repository does not carry them); without it that part is skipped
and said so.
"""
import os
import re
import sys
import unittest
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
VITA = HERE.parents[1]
sys.path.insert(0, str(VITA / 'tools'))
import make_gamedb as mg  # noqa: E402

CONSOLES = {  # letter: (catalogue, games, N records)
    'I': ('Sega - Mega Drive - Genesis', 2800, 1625),
    'J': ('Sega - Master System - Mark III', 1057, 650),
    'K': ('Sega - Game Gear', 607, 467),
    'L': ('Sega - SG-1000', 211, 99),
}
# the same ROM in two catalogues: the first console's name is kept (the checksum
# names one game); the others are found by file name and by their N records
SHARED = {'07301F83': 'Phantasy Star Fukkokuban (Japan)',   # Genesis and Master System
          '5E7B18C8': 'Wonder Kid (Korea) (En)'}           # Master System and Game Gear
DAT_DIR = (sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-')
           else os.environ.get('GAMEDB_DAT_DIR'))


def records():
    out = {}
    with open(VITA / 'assets' / 'gamedb.txt', encoding='utf-8') as f:
        for line in f:
            k, v = line.rstrip('\n').split('\t', 1)
            out[k] = v
    return out


class SegaDb(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rec = records()

    def test_record_counts_are_exact(self):
        c = Counter(k[0] for k in self.rec)
        self.assertEqual(c['C'], 23933)
        self.assertEqual(c['N'], 14327)
        for letter, (_, _, n) in CONSOLES.items():
            self.assertEqual(sum(1 for k in self.rec if k[:2] == 'N' + letter), n, letter)
        # the earlier consoles' records are as they were (1.4: 19260 C, 11486 N)
        self.assertEqual(c['C'] - 4673, 19260)
        self.assertEqual(c['N'] - 1625 - 650 - 467 - 99, 11486)

    def test_loose_names_are_catalogue_names(self):
        names = {v for k, v in self.rec.items() if k[0] == 'C'}
        for k, v in self.rec.items():
            if k[0] == 'N' and k[1] in CONSOLES:
                self.assertIn(v, names, k)

    def test_loose_key_is_the_squashed_title(self):
        for k, v in self.rec.items():
            if k[0] == 'N' and k[1] in CONSOLES:
                self.assertEqual(k[2:], mg.squash(mg.title_key(mg.base_title(v))), k)

    @unittest.skipUnless(DAT_DIR, 'no catalogues (set GAMEDB_DAT_DIR to libretro-database/metadat/no-intro)')
    def test_every_catalogue_crc_resolves_to_its_name(self):
        total = 0
        for letter, (dat, games, _) in CONSOLES.items():
            entries = mg.parse_dat(Path(DAT_DIR, dat + '.dat').read_text(encoding='utf-8'))
            self.assertEqual(len(entries), games, dat)
            for g in entries:
                want = SHARED.get(g['crc'], g['name'])
                self.assertEqual(self.rec.get('C' + g['crc']), want, (dat, g['name']))
                total += 1
        self.assertEqual(total, 4675)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
