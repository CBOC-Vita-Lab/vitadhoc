"""assets/gamedb.txt's descriptions (2.0): the NES, WonderSwan, Genesis, Master
System, Game Gear and SG-1000 have them now (Sega CD's titles are in too).

    python3 test_descriptions.py

Counts are exact (a change to the data must change them here on purpose); every
T / U record points at a W record; the text rules of the existing descriptions
hold for all of them (ASCII, whole sentences, at most ~420 characters, no
leftover romanisation bracket); the attribution list is complete; and a few
well-known games get the article of THEIR console.
"""
import re
import subprocess
import sys
import unittest
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
VITA = HERE.parents[1]
sys.path.insert(0, str(VITA / 'tools'))
import make_gamedb as mg  # noqa: E402

# the lowest share of a console's games (N records) with a description:
# measured at 2.0 (NES 851 of 2985 ...), a drop means the matching broke
PARTNER = {'B': 'C', 'C': 'B', 'G': 'H', 'H': 'G', 'F': 'E', 'J': 'K'}   # gamedb.c
MIN_GAMES = {'A': 800, 'B': 340, 'C': 270, 'D': 850, 'E': 830, 'F': 30, 'G': 6, 'H': 3,
             'I': 650, 'J': 200, 'K': 110, 'L': 25}


def records():
    out = {}
    with open(VITA / 'assets' / 'gamedb.txt', encoding='utf-8') as f:
        for line in f:
            k, v = line.rstrip('\n').split('\t', 1)
            out[k] = v
    return out


class Descriptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rec = records()

    def blurb(self, tag, name):
        k = mg.title_key(mg.base_title(name))
        for t in [tag] + list(PARTNER.get(tag, '')):
            for more in ('', ' version', ' edition'):
                if 'U' + t + k + more in self.rec:
                    return self.rec['W' + self.rec['U' + t + k + more]]
        return None

    def test_counts(self):
        c = Counter(k[0] for k in self.rec)
        self.assertEqual((c['T'], c['U'], c['W']), (0, 4360, 3219))
        # the names and checksums are as they were
        self.assertEqual((c['C'], c['N'], c['S'], c['Z']), (23933, 14327, 2756, 2756))
        per = Counter(k[1] for k in self.rec if k[0] == 'U')
        self.assertEqual(dict(per), {'A': 847, 'B': 359, 'C': 275, 'D': 874, 'E': 864, 'G': 7,
                                     'H': 4, 'I': 676, 'J': 205, 'K': 116, 'L': 27, 'M': 106})

    def test_every_title_has_its_text(self):
        for k, v in self.rec.items():
            if k[0] in 'TU':
                self.assertIn('W' + v, self.rec, k)

    def test_no_text_nobody_can_reach(self):
        used = {v for k, v in self.rec.items() if k[0] == 'U'}
        for k in self.rec:
            if k[0] == 'W':
                self.assertIn(k[1:], used, k)

    def test_no_series_text(self):
        for k, v in self.rec.items():
            if k[0] == 'W':
                self.assertFalse(mg.about_a_series(v), (k, v))

    def test_text_rules(self):
        for k, v in self.rec.items():
            if k[0] != 'W':
                continue
            self.assertRegex(v, r'^[ -~]{40,}$', k)
            self.assertLessEqual(len(v), 430, k)
            self.assertNotRegex(v, r'\((?:,|;) ', k)    # a romanisation left over
            self.assertNotIn('()', v, k)
            self.assertRegex(v, r'[.!?)"\']$', k)

    def test_u_records_name_their_console(self):
        for k, v in self.rec.items():
            if k[0] == 'U':
                self.assertIn(k[1], 'ABCDEGHIJKLM', k)
                self.assertIn('W' + v, self.rec, k)

    def test_coverage_per_console(self):
        for tag, least in MIN_GAMES.items():
            names = [v for k, v in self.rec.items() if k[:2] == 'N' + tag]
            n = sum(1 for nm in names if self.blurb(tag, nm))
            self.assertGreaterEqual(n, least, (tag, n, len(names)))

    def test_each_console_gets_its_own_game(self):
        cases = [('E', 'Strider (USA)', 'Nintendo Entertainment System'),
                 ('I', 'Strider (USA)', 'arcades'),
                 ('I', 'Streets of Rage 2 (USA)', 'Sega Genesis'),
                 ('I', 'Sonic the Hedgehog (USA, Europe)', 'Sega Genesis'),
                 ('J', 'Sonic the Hedgehog (Europe)', 'Master System'),
                 ('K', 'Sonic the Hedgehog (USA, Europe)', 'Game Gear'),
                 ('J', 'Alex Kidd in Miracle World (USA, Europe)', 'Master System'),
                 ('E', 'Mega Man 2 (USA)', 'Nintendo Entertainment System'),
                 ('E', 'Super Mario Bros. (World)', 'Nintendo Entertainment System'),
                 ('G', 'Buffers Evolution (Japan)', 'WonderSwan'),
                 ('H', 'Judgement Silversword (World) (Ja) (WonderWitch)', 'WonderSwan Color'),
                 ('L', 'Flicky (Japan) (Rev 1)', 'Flicky')]
        for tag, name, word in cases:
            b = self.blurb(tag, name)
            self.assertIsNotNone(b, (tag, name))
            self.assertIn(word, b, (tag, name))

    def test_earlier_consoles_keep_their_text(self):
        self.assertIn('Game Boy Advance', self.blurb('A', 'Pokemon - Emerald Version (USA, Europe)'))

    def test_a_description_is_about_the_game_of_its_console(self):
        # the cases a title shared with another console's game used to get wrong
        self.assertIsNone(self.blurb('H', 'Final Fantasy II (Japan)'))
        self.assertIsNone(self.blurb('K', 'Tempo Jr. (World)'))
        self.assertIsNone(self.blurb('J', 'Golden Axe (USA, Europe, Brazil) (En)'))
        self.assertIsNone(self.blurb('I', 'Alex Kidd in Miracle World (USA, Europe)'))
        self.assertIn('Game Boy', self.blurb('C', 'Tetris (World) (Rev 1)'))   # the partner console

    def test_attribution_list_is_complete(self):
        r = subprocess.run([sys.executable, str(VITA / 'tools' / 'make_wiki_attribution.py'), '--check'])
        self.assertEqual(r.returncode, 0, 'run tools/make_wiki_attribution.py')

    def test_article_rank_prefers_the_game(self):
        self.assertLess(mg.article_rank('Sonic the Hedgehog (1991 video game)'),
                        mg.article_rank('Sonic the Hedgehog'))
        self.assertLess(mg.article_rank('Sonic the Hedgehog'),
                        mg.article_rank('Sonic the Hedgehog (series)'))
        self.assertLess(mg.article_rank('Metroid (video game)'), mg.article_rank('Metroid'))
        self.assertTrue(mg.about_a_series('Golden Axe is a series of side-scrolling beat em up games.'))
        self.assertTrue(mg.about_a_series('Battletoads is a video game franchise by Rare.'))
        self.assertFalse(mg.about_a_series('Scooby-Doo! Unmasked is a platform game based on the Scooby-Doo franchise.'))
        self.assertFalse(mg.about_a_series('Super Mario Bros. is a 1985 platform game, the first of the series.'))

    def test_tidy(self):
        self.assertEqual(mg.tidy('Albert Odyssey (, Arubato Odessei) is a game.'),
                         'Albert Odyssey is a game.')
        self.assertEqual(mg.tidy('X (; Y (Z)) is a game.'), 'X is a game.')
        self.assertEqual(mg.tidy('Bad News, released as Gekito!! Stadium (, Gekito; lit. "Fierce'),
                         'Bad News, released as Gekito!! Stadium.')
        self.assertEqual(mg.tidy('Plain text (1991) is fine.'), 'Plain text (1991) is fine.')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
