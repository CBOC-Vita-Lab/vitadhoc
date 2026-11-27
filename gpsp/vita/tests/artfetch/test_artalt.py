#!/usr/bin/env python3
"""Cover-name fallbacks (artalt.h): synthetic cases, and the match rate on the real No-Intro lists when they are given.

Real data (optional, offline): AF_NOINTRO_DIR holds <tag>.dat (libretro-database metadat/no-intro files) and <tag>.tree (git ls-tree
-r --name-only of the libretro-thumbnails repo of that system) for tag in gba, snes.  Without it only the synthetic cases run.
"""
import os, re, subprocess, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
VITA = HERE.parents[1]


def sanitize(n):
    return re.sub(r'[&*/:`<>?\\|"]', '_', n)


class AltNames(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='artalt-')
        cls.cli = Path(cls.tmp.name) / 'alt_cli'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(VITA),
                        str(HERE / 'alt_cli.c'), '-o', str(cls.cli)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def alts(self, names):
        out = subprocess.run([str(self.cli)], input='\n'.join(names) + '\n', capture_output=True, text=True, check=True).stdout
        return [line.split('\t') if line else [] for line in out.split('\n')[:len(names)]]

    def one(self, name):
        return self.alts([name])[0]

    def test_world_release_asks_for_usa_then_europe(self):
        a = self.one('Super Example (World)')
        self.assertEqual(a[:2], ['Super Example (USA)', 'Super Example (Europe)'])

    def test_usa_release_tries_usa_europe_and_europe(self):
        a = self.one('Game Name (USA)')
        self.assertEqual(a[:2], ['Game Name (USA, Europe)', 'Game Name (Europe)'])

    def test_language_list_is_dropped_and_kept(self):
        a = self.one('Game (Europe) (En,Fr,De)')
        self.assertEqual(a[0], 'Game (Europe)')
        self.assertIn('Game (USA) (En,Fr,De)', a)
        self.assertIn('Game (USA)', a)

    def test_revision_and_version_tags_are_dropped(self):
        self.assertEqual(self.one('Game (USA) (Rev 1)')[0], 'Game (USA)')
        self.assertEqual(self.one('Game (USA) (En,Fr) (v1.1) (Beta)')[0], 'Game (USA) (En,Fr)')

    def test_subtitle_dash_is_also_tried_as_a_space(self):
        a = self.one('Game - The Sequel (USA)')
        self.assertIn('Game The Sequel (USA)', a)

    def test_no_region_or_unknown_first_group_asks_for_nothing_more(self):
        for n in ['Plain Name', 'Homebrew (Unl)', 'Thing (Proto)', 'Thing (Beta) (USA)', '(USA)']:
            self.assertEqual(self.one(n), [], n)

    def test_never_repeats_the_exact_name_and_is_capped(self):
        for n in ['A (USA)', 'A (World)', 'A - B (Europe) (En,Fr,De,Es,It)', 'A (Japan, USA)']:
            a = self.one(n)
            self.assertNotIn(n, a)
            self.assertEqual(len(a), len(set(a)))
            self.assertLessEqual(len(a), 7)

    def test_title_with_parentheses_inside_keeps_them(self):
        a = self.one('Pocket (Mini) Game (USA)')
        self.assertEqual(a[0] if a else '', 'Pocket (Mini) Game (USA, Europe)')

    def test_synthetic_lists_match_rate_rises(self):
        dat = ['A (World)', 'B (USA)', 'C (Europe) (En,Fr)', 'D (USA) (Rev 2)', 'E - F (USA)', 'G (Japan)', 'H (USA)', 'Z (Proto)']
        thumbs = {'A (USA)', 'B (USA)', 'C (Europe)', 'D (USA)', 'E F (USA)', 'G (USA)', 'H (Europe)'}
        before = sum(sanitize(n) in thumbs for n in dat)
        after = sum(any(sanitize(x) in thumbs for x in [n] + a) for n, a in zip(dat, self.alts(dat)))
        self.assertEqual((before, after), (1, 7))

    def test_real_lists(self):
        base = os.environ.get('AF_NOINTRO_DIR')
        if not base:
            self.skipTest('AF_NOINTRO_DIR not set')
        for tag in ('gba', 'snes'):
            dat = Path(base) / (tag + '.dat')
            tree = Path(base) / (tag + '.tree')
            if not dat.exists() or not tree.exists():
                self.skipTest(f'{tag} files missing')
            names = [m.group(1) for line in dat.open(encoding='utf-8', errors='replace')
                     if (m := re.match(r'^\tname "(.*)"$', line.rstrip('\n')))][1:]
            thumbs = {line[len('Named_Boxarts/'):-4] for line in tree.read_text(encoding='utf-8').splitlines()
                      if line.startswith('Named_Boxarts/') and line.endswith('.png')}
            before = sum(sanitize(n) in thumbs for n in names)
            alts = self.alts(names)
            after = 0
            reqs = 0
            for n, a in zip(names, alts):
                hit = False
                for x in [n] + a:
                    reqs += 1
                    if sanitize(x) in thumbs:
                        hit = True
                        break
                after += hit
            print(f'COVER_MATCH {tag} games={len(names)} before={before} ({100 * before / len(names):.1f}%) '
                  f'after={after} ({100 * after / len(names):.1f}%) mean_requests={reqs / len(names):.2f}', flush=True)
            self.assertGreater(after, before)


if __name__ == '__main__':
    unittest.main()
