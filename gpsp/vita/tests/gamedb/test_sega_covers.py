"""The Sega consoles' cover art through the real artfetch worker (2.0): the
thumbnail folder names, libretro-thumbnails' special-character rule, and the
checksum rules (SMD de-interleaved, a copier header left out) that name a file.

    python3 test_sega_covers.py

Reuses tests/artfetch/test_artfetch.py's runner and local HTTP server.
"""
import importlib.util
import os
import re
import shutil
import sys
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
VITA = HERE.parents[1]
spec = importlib.util.spec_from_file_location('test_artfetch', VITA / 'tests' / 'artfetch' / 'test_artfetch.py')
af = importlib.util.module_from_spec(spec)
sys.modules['test_artfetch'] = af
spec.loader.exec_module(af)

MD = '/Sega - Mega Drive - Genesis/'
SMS = '/Sega - Master System - Mark III/'
GG = '/Sega - Game Gear/'
SG = '/Sega - SG-1000/'
SPECIAL = '&*/:`<>?\\|"'


def libretro_name(name):
    """libretro-thumbnails: & * / : ` < > ? \\ | " become '_'"""
    return ''.join('_' if c in SPECIAL else c for c in name)


def smd_of(plain):
    out = bytearray(512)
    out[0], out[1], out[8], out[9], out[10] = len(plain) // 16384, 3, 0xAA, 0xBB, 6
    for b in range(len(plain) // 16384):
        blk = plain[b * 16384:(b + 1) * 16384]
        out += blk[1::2] + blk[0::2]       # odd bytes (1, 3, ...) first, then even
    return bytes(out)


def catalogue_names(letter):
    """The No-Intro names of one console from the shipped gamedb: its C records
    cannot be told by console, so N records (loose names) stand in for them"""
    out = set()
    for line in (VITA / 'assets' / 'gamedb.txt').read_text(encoding='utf-8').splitlines():
        k, v = line.split('\t', 1)
        if k[:2] == 'N' + letter:
            out.add(v)
    return sorted(out)


class SegaCovers(unittest.TestCase):
    setUpClass = classmethod(af.ArtworkWorkerTests.setUpClass.__func__)
    tearDownClass = classmethod(af.ArtworkWorkerTests.tearDownClass.__func__)
    setUp = af.ArtworkWorkerTests.setUp
    tearDown = af.ArtworkWorkerTests.tearDown
    run_worker = af.ArtworkWorkerTests.run_worker

    def with_db(self, extra=()):
        text = (VITA / 'assets' / 'gamedb.txt').read_text(encoding='utf-8')
        lines = text.splitlines() + list(extra)
        (self.root / 'gamedb.txt').write_text('\n'.join(sorted(lines)) + '\n', encoding='utf-8')

    def boxart(self, name, route, size=0, paths=None):
        self.with_db()
        stats, requests = self.run_worker([name], {route: (200, af.PNG)}, titles=False, size=size, paths=paths)
        return stats, requests

    def test_each_console_asks_its_own_thumbnail_folder(self):
        for name, folder in [('Sonic The Hedgehog (USA, Europe).md', MD),
                             ('Sonic The Hedgehog (USA, Europe).gen', MD),
                             ('Sonic The Hedgehog (USA, Europe).smd', MD),
                             ('Alex Kidd in Miracle World (USA, Europe).sms', SMS),
                             ('Sonic The Hedgehog 2 (World).gg', GG),
                             ('Girl\'s Garden (Japan, Australia) (Ja).sg', SG)]:
            with self.subTest(name=name):
                self.tearDown()
                self.setUp()
                stem = Path(name).stem
                _, req = self.boxart(name, folder + 'Named_Boxarts/' + libretro_name(stem) + '.png')
                self.assertTrue((self.root / 'boxart' / (stem + '.png')).is_file(), req)
                self.assertTrue(all(r.startswith(folder) for r in req), req)

    def test_bin_is_asked_of_the_genesis_folder(self):
        name = 'Streets of Rage 2 (USA).bin'
        _, req = self.boxart(name, MD + 'Named_Boxarts/Streets of Rage 2 (USA).png')
        self.assertTrue((self.root / 'boxart' / 'Streets of Rage 2 (USA).png').is_file(), req)

    def test_special_characters_become_underscores_for_every_console(self):
        seen = 0
        for letter, folder, ext in [('I', MD, '.md'), ('J', SMS, '.sms'), ('K', GG, '.gg'), ('L', SG, '.sg')]:
            names = [n for n in catalogue_names(letter) if any(c in n for c in SPECIAL)]
            self.assertTrue(names or letter == 'L', letter)
            for n in names[:3]:
                self.tearDown()
                self.setUp()
                safe = ''.join('_' if c in '/\\:*?"<>|' else c for c in n)   # a file name cannot hold these
                _, req = self.boxart(safe + ext, folder + 'Named_Boxarts/' + libretro_name(n) + '.png')
                self.assertTrue(any(r == folder + 'Named_Boxarts/' + libretro_name(n) + '.png' for r in req), (n, req))
                seen += 1
        self.assertGreaterEqual(seen, 6)

    def test_ampersand_name(self):
        self.assertEqual(libretro_name('Sonic & Knuckles (World)'), 'Sonic _ Knuckles (World)')
        self.assertEqual(libretro_name('Who Framed Roger Rabbit? "x" a/b: c'), 'Who Framed Roger Rabbit_ _x_ a_b_ c')
        _, req = self.boxart('Sonic & Knuckles (World).md', MD + 'Named_Boxarts/Sonic _ Knuckles (World).png')
        self.assertIn(MD + 'Named_Boxarts/Sonic _ Knuckles (World).png', req)
        self.assertTrue((self.root / 'boxart' / 'Sonic & Knuckles (World).png').is_file())

    def test_smd_file_is_named_by_the_plain_roms_checksum(self):
        plain = bytearray(os.urandom(3 * 16384))
        plain[0x100:0x110] = b'SEGA MEGA DRIVE '
        crc = zlib.crc32(bytes(plain)) & 0xFFFFFFFF
        self.with_db(['C%08X\tMy Homebrew (World)' % crc])
        path = self.root / 'roms' / 'whatever.smd'
        path.write_bytes(smd_of(bytes(plain)))
        route = MD + 'Named_Boxarts/My Homebrew (World).png'
        _, req = self.run_worker(['whatever.smd'], {route: (200, af.PNG)}, titles=False)
        self.assertIn(route, req)
        self.assertTrue((self.root / 'boxart' / 'whatever.png').is_file())

    def test_smd_named_bin_is_named_by_the_plain_roms_checksum(self):
        plain = bytearray(os.urandom(2 * 16384))
        crc = zlib.crc32(bytes(plain)) & 0xFFFFFFFF
        self.with_db(['C%08X\tOther Homebrew (Japan)' % crc])
        (self.root / 'roms' / 'x.bin').write_bytes(smd_of(bytes(plain)))
        route = MD + 'Named_Boxarts/Other Homebrew (Japan).png'
        _, req = self.run_worker(['x.bin'], {route: (200, af.PNG)}, titles=False)
        self.assertIn(route, req)

    def test_sms_file_with_a_copier_header_is_named_by_the_cartridges_checksum(self):
        body = os.urandom(32768)
        crc = zlib.crc32(body) & 0xFFFFFFFF
        self.with_db(['C%08X\tSome Cart (USA)' % crc])
        (self.root / 'roms' / 'headed.sms').write_bytes(b'\x55' * 512 + body)
        route = SMS + 'Named_Boxarts/Some Cart (USA).png'
        _, req = self.run_worker(['headed.sms'], {route: (200, af.PNG)}, titles=False)
        self.assertIn(route, req)

    def test_sms_file_may_be_a_game_gear_title(self):
        # a ".sms" the Game Gear catalogue lists: Master System's folder first, then Game Gear's
        body = os.urandom(16384)
        crc = zlib.crc32(body) & 0xFFFFFFFF
        self.with_db(['C%08X\tGG In SMS Mode (USA)' % crc])
        (self.root / 'roms' / 'a.sms').write_bytes(body)
        route = GG + 'Named_Boxarts/GG In SMS Mode (USA).png'
        _, req = self.run_worker(['a.sms'], {route: (200, af.PNG)}, titles=False)
        self.assertEqual(req[0], SMS + 'Named_Boxarts/GG In SMS Mode (USA).png')
        self.assertIn(route, req)
        self.assertTrue((self.root / 'boxart' / 'a.png').is_file())

    def test_loose_name_is_asked_under_its_own_console(self):
        self.with_db()
        _, req = self.run_worker(['Sonic the Hedgehog.md'], {}, titles=False)
        self.assertIn(MD + 'Named_Boxarts/Sonic The Hedgehog (USA, Europe).png', req)
        self.assertFalse(any(r.startswith(GG) or r.startswith(SMS) for r in req), req)


if __name__ == '__main__':
    unittest.main(verbosity=2)
