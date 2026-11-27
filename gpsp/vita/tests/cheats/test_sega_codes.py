"""The Sega cheat decoders of make_cheatdb.py, the shipped cheats.db's Sega part,
and (with LIBRETRO_DB set) the database rebuilt byte for byte.

    python3 test_sega_codes.py

LIBRETRO_DB is a checkout of libretro-database at revision d5bae90 (the pin in
notices/THIRD-PARTY.md); without it the rebuild test is skipped and says so.
"""
import hashlib
import os
import random
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
VITA = HERE.parents[1]
sys.path.insert(0, str(VITA / 'tools'))
import cheatdb_read as reader  # noqa: E402
import make_cheatdb as m  # noqa: E402

DB = VITA / 'assets' / 'cheats.db'
# sha256 of the 1.4 cheats.db (GBA, GB, GBC, NES, WonderSwan, WonderSwan Color from
# the d5bae90 lists): the generator must still make it with --systems 0,1,2,3,4,5
SHA_14 = 'a73da9f303abb8a08ec0b48fdca369362b5e86c195f117ca08a110b86cf012f6'


def vectors():
    for line in (HERE / 'sega_vectors.txt').read_text(encoding='utf-8').splitlines():
        if line and not line.startswith('#'):
            yield tuple(line.split('\t'))


class Decoders(unittest.TestCase):
    def test_vectors(self):
        n = 0
        for kind, code, want in vectors():
            conv = m.convert_md if kind == 'MD' else m.convert_sms
            out, why = conv(code)
            self.assertEqual(out if out else 'DROP:' + why, want, (kind, code))
            n += 1
        self.assertEqual(n, 103)

    def test_genesis_game_genie_round_trip(self):
        rnd = random.Random(1)
        for _ in range(5000):
            address, data = rnd.randrange(1 << 24), rnd.randrange(1 << 16)
            code = m.md_gg_encode(address, data)
            self.assertRegex(code, r'^[A-Z0-9]{4}-[A-Z0-9]{4}$')
            self.assertEqual(m.md_gg_decode(code), (address, data), code)

    def test_master_system_game_genie_round_trip(self):
        rnd = random.Random(2)
        for _ in range(5000):
            address, data = rnd.randrange(1 << 16), rnd.randrange(256)
            ref = rnd.choice([None, rnd.randrange(256)])
            code = m.sms_gg_encode(address, data, ref)
            self.assertEqual(m.sms_gg_decode(code), (address, data, ref), code)

    def test_published_codes_from_the_lists_round_trip(self):
        # codes of the libretro lists (vector table): decode, encode again, same letters
        n = 0
        for kind, code, want in vectors():
            if want.startswith('DROP'):
                continue
            for part in m.split_sega(code):
                if kind == 'MD' and re.fullmatch(r'[A-Z0-9]{4}-[A-Z0-9]{4}', part):
                    self.assertEqual(m.md_gg_encode(*m.md_gg_decode(part)), part)
                    n += 1
                elif kind == 'SMS' and re.fullmatch(r'[0-9A-F]{3}-[0-9A-F]{3}(-[0-9A-F]{3})?', part):
                    a, d, r = m.sms_gg_decode(part)
                    enc = m.sms_gg_encode(a, d, r)
                    self.assertEqual(m.sms_gg_decode(enc), (a, d, r))
                    self.assertEqual(enc[:7], part[:7])       # the compare's middle digit is free
                    n += 1
        self.assertGreaterEqual(n, 14)

    def test_a_published_genesis_game_genie_code(self):
        # a code of the libretro list (Sonic The Hedgehog): a word patch of ROM
        self.assertEqual(m.convert_md('ATGA-AA56'), ('000C7C:6004', None))


class Shipped(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.games, cls.crcs, cls.names = reader.read_db(str(DB))

    def test_counts_are_exact(self):
        by = {}
        for g in self.games:
            c = by.setdefault(g['sys'], [0, 0])
            c[0] += 1
            c[1] += len(g['cheats'])
        self.assertEqual(by, {0: [511, 6909], 1: [970, 14406], 2: [709, 10603],
                              3: [2037, 56916], 4: [1, 1],
                              6: [1586, 49722], 7: [621, 6016], 8: [544, 7294],
                              9: [26, 69]})
        self.assertEqual(len(self.games), 7005)
        self.assertEqual(sum(len(g['cheats']) for g in self.games), 151936)
        # the 1.4 consoles come first, unchanged in number
        self.assertTrue(all(g['sys'] < 6 for g in self.games[:4228]))
        self.assertTrue(all(g['sys'] >= 6 for g in self.games[4228:]))

    def test_every_sega_code_is_canonical(self):
        for g in self.games:
            if g['sys'] < 6:
                continue
            form = (r'[0-9A-F]{6}:[0-9A-F]{4}' if g['sys'] == 6 else
                    r'[0-9A-F]{4}:[0-9A-F]{2}(:[0-9A-F]{2})?')
            for title, code in g['cheats']:
                self.assertTrue(title and len(title.encode()) <= m.NAME_CAP)
                for part in code.split('+'):
                    self.assertRegex(part, '^' + form + '$', (g['name'], title))

    def test_games_are_found_by_crc_and_by_name(self):
        def find(crc):
            return self.games[self.crcs[crc]]
        sonic = find(0xF9394E97)                         # Sonic The Hedgehog (USA, Europe)
        self.assertEqual((sonic['name'], sonic['sys']), ('Sonic The Hedgehog (USA, Europe)', 6))
        self.assertIn(('MASTER CODE--MUST BE ENTERED', '000338:6004'), sonic['cheats'])
        alex = find(0x17A40E29)                          # Alex Kidd in Miracle World (USA, Europe)
        self.assertEqual((alex['name'], alex['sys']), ('Alex Kidd in Miracle World (USA, Europe)', 7))
        gg = find(0x95A18EC7)                            # Sonic The Hedgehog 2 (World), Game Gear
        self.assertEqual((gg['name'], gg['sys']), ('Sonic The Hedgehog 2 (World)', 8))
        self.assertIn(('Enable Stage Select', '2543:00:10'), gg['cheats'])
        self.assertEqual(self.games[self.names['Ibsonicthehedgehog']]['sys'], 6)
        self.assertEqual(self.games[self.names['Jbalexkiddinmiracleworld']]['sys'], 7)
        self.assertEqual(self.games[self.names['Kbsonicthehedgehog2']]['sys'], 8)
        self.assertEqual(self.games[self.names['Lbgirlsgarden']]['sys'], 9)


class Rebuild(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('LIBRETRO_DB'), 'no libretro-database checkout (set LIBRETRO_DB)')
    def test_database_is_rebuilt_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as t:
            out = Path(t) / 'cheats.db'
            subprocess.run([sys.executable, str(VITA / 'tools' / 'make_cheatdb.py'), '--db',
                            os.environ['LIBRETRO_DB'], '--out', str(out)], check=True,
                           stdout=subprocess.DEVNULL)
            self.assertEqual(out.read_bytes(), DB.read_bytes())
            self.assertEqual(out.with_name('cheats.db.txt').read_bytes(),
                             (VITA / 'assets' / 'cheats.db.txt').read_bytes())
            old = Path(t) / 'old.db'
            subprocess.run([sys.executable, str(VITA / 'tools' / 'make_cheatdb.py'), '--db',
                            os.environ['LIBRETRO_DB'], '--systems', '0,1,2,3,4,5', '--out',
                            str(old)], check=True, stdout=subprocess.DEVNULL)
            self.assertEqual(hashlib.sha256(old.read_bytes()).hexdigest(), SHA_14)


if __name__ == '__main__':
    unittest.main(verbosity=2)
